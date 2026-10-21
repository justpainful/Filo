// Audio Visualizer - turns an audio file into an MP4 with an animated waveform / spectrum / vectorscope. The preview at the top is drawn
// live from the REAL peaks of the audio (it moves with the playback position); the final frames are rendered by ffmpeg
// (media::AudioVisualizer), so the preview is an approximation of their look, not a pixel copy.
#include "tools/audio/audio_common.h"
#include "engines/image/bitmap.h"
#include <algorithm>
#include <cmath>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace filo::tools {

using namespace audio;

namespace {

const uint32_t kWaveColors[6] = {0x2DD4BF, 0x3B8BFF, 0xFFFFFF, 0xFB923C, 0xF472B6, 0xA3E635};
const uint32_t kBgColors[6] = {0x0B1220, 0x000000, 0x1E1B4B, 0x14532D, 0x4C0519, 0xF5F5F7};

std::wstring HexText(uint32_t c) { wchar_t b[16]; swprintf_s(b, L"#%06X", c & 0xFFFFFF); return b; }
bool ParseHex(const std::wstring& s, uint32_t& out) {
  std::wstring t = Trim(s);
  if (!t.empty() && t[0] == L'#') t.erase(0, 1);
  if (t.size() != 6) return false;
  uint32_t v = 0;
  for (wchar_t ch : t) {
    int d = (ch >= L'0' && ch <= L'9') ? ch - L'0' : (ch >= L'a' && ch <= L'f') ? ch - L'a' + 10 : (ch >= L'A' && ch <= L'F') ? ch - L'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + (uint32_t)d;
  }
  out = v; return true;
}
float Hash01(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return (x & 0xFFFFFF) / 16777215.f; }

class VisualizerDialog : public AudioDialog {
 public:
  explicit VisualizerDialog(const Selection& sel) {
    selection_ = sel;
    waveHex_ = HexText(waveColor_); bgHex_ = HexText(bgColor_);
  }

 protected:
  ui::DialogOptions Options() const override {
    ui::DialogOptions o; o.title = L"Audio Visualizer"; o.widthDip = 515; o.heightDip = 612; o.rtl = RtlSetting();
    return o;
  }
  std::string ToolId() const override { return "audio-visualizer"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject();
    j["layout"] = layout_; j["style"] = style_; j["fps"] = fps_;
    j["waveColor"] = Narrow(waveHex_); j["background"] = Narrow(bgHex_); j["bgMode"] = bgMode_;
    return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("layout")) layout_ = (int)std::clamp<int64_t>(j.Get("layout").AsInt(0), 0, 2);
    if (j.Has("style")) style_ = (int)std::clamp<int64_t>(j.Get("style").AsInt(0), 0, 2);
    if (j.Has("fps")) fps_ = (int)std::clamp<int64_t>(j.Get("fps").AsInt(30), 10, 60);
    uint32_t c = 0;
    if (j.Has("waveColor") && ParseHex(Widen(j.Get("waveColor").AsString()), c)) { waveColor_ = c; waveHex_ = HexText(c); }
    if (j.Has("background") && ParseHex(Widen(j.Get("background").AsString()), c)) { bgColor_ = c; bgHex_ = HexText(c); }
    if (j.Has("position")) parked_ = std::max(0.0, j.Get("position").AsNumber());
    if (j.Has("bgMode")) bgMode_ = (int)std::clamp<int64_t>(j.Get("bgMode").AsInt(0), 0, 2);
    if (j.Has("image")) { bgImage_ = Widen(j.Get("image").AsString()); if (!bgImage_.empty()) { bgMode_ = 1; LoadBgImage(); } }
  }

  void Begin() override {
    LoadWave(1600);
    PreparePlayer(Path());
    const fs::path p = Path();
    RunAsync([this, p]() -> std::function<void()> {
      media::MediaSummary ms = media::SummarizeMedia(p);
      media::FrameImage fi;
      if (ms.ok && ms.hasCoverArt) fi = media::ExtractFrame(p, 0, 960, 960, false);
      return [this, ms, fi]() mutable {
        dur_ = ms.ok ? ms.duration : dur_;
        hasCover_ = ms.ok && ms.hasCoverArt;
        if (fi.ok && fi.width > 0) { coverPx_ = fi.bgra; coverW_ = fi.width; coverH_ = fi.height; coverBmp_.Reset(); }
        if (!hasCover_ && bgMode_ == 2) bgMode_ = 0;
        if (hasCover_ && bgImage_.empty() && !userPickedBg_) bgMode_ = 2;
      };
    });
  }
  void OnWaveLoaded() override {
    dur_ = std::max(dur_, wave_.wf.duration);
    // idle preview frame: the loudest moment, so the picture is never an empty line
    const auto& pk = wave_.wf.peaks; size_t bi = 0;
    for (size_t i = 1; i < pk.size(); ++i) if (pk[i] > pk[bi]) bi = i;
    if (!pk.empty()) peakT_ = (double)bi / pk.size() * wave_.wf.duration;
  }
  void OnDeviceReset(bool rebuilt) override { AudioDialog::OnDeviceReset(rebuilt); if (!rebuilt) { bgBmp_.Reset(); coverBmp_.Reset(); } }

  void TestPlay() override { PlayRange(parked_, dur_, 0, L"viz"); }
  bool CanApply() const override { return !Busy() && waveError_.empty(); }

  void Apply() override {
    media::VisualizerRequest req;
    req.layout = layout_ == 1 ? media::VizLayout::Portrait : (layout_ == 2 ? media::VizLayout::Square : media::VizLayout::Landscape);
    req.style = style_ == 1 ? media::VizStyle::Spectrum : (style_ == 2 ? media::VizStyle::Vectorscope : media::VizStyle::Waveform);
    req.waveColor = waveColor_; req.backgroundColor = bgColor_;
    req.fps = fps_;
    if (bgMode_ == 1 && !bgImage_.empty()) req.backgroundImage = bgImage_;
    req.useCoverArt = bgMode_ == 2;
    Settings st = SettingsStore::Get().Snapshot();
    req.crf = st.videoCrf; req.audioBitrateKbps = st.audioBitrateKbps; req.hardware = st.hardwareAcceleration;
    const fs::path in = Path();
    StopPlayback();
    RunJob(L"Rendering the visualizer video...", [in, req](JobContext& ctx) -> JobResult { return media::AudioVisualizer(in, req, ctx); });
  }

  void Draw(Ui& ui, const Rect& content, const Rect& body) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const bool en = !Locked();
    const float X = body.x, Y = body.y, W = body.w, aw = W - 2 * kInset;
    auto M = [&](Rect r) { return Mir(ui, body, r); };
    TextStyle ts; ts.size = dialog::bodyFontSize;

    DrawPreview(ui, body, M({X + kInset, Y + 64.f, aw, 240.f}), en);

    // ---- option rows
    const float x0 = X + kInset + 140.f, cw = aw - 140.f;
    auto label = [&](float y, const wchar_t* t) { p.Text(M({X + kInset + 2.f, y, 134.f, kBtnH}), t, pal.text, ts); };
    float y = Y + 322.f;
    label(y, L"Layout");
    ui.PushId(0x5101);
    { int s = ui.Segmented(M({x0, y, cw, kBtnH}), {L"Landscape", L"Portrait", L"Square"}, layout_, en); layout_ = s; }
    ui.PopId();
    y += 38.f; label(y, L"Style");
    ui.PushId(0x5102);
    { int s = ui.Segmented(M({x0, y, cw, kBtnH}), {L"Waveform", L"Spectrum", L"Vectorscope"}, style_, en); style_ = s; }
    ui.PopId();
    y += 38.f; label(y, L"Waveform colour");
    ui.PushId(0x5103);
    ColorRow(ui, body, x0, y, kWaveColors, waveColor_, waveHex_, en);
    ui.PopId();
    y += 38.f; label(y, L"Background");
    ui.PushId(0x5104);
    {
      std::vector<std::wstring> items = {L"Colour", L"Image"};
      if (hasCover_) items.push_back(L"Cover art");
      int s = ui.Segmented(M({x0, y, hasCover_ ? 270.f : 190.f, kBtnH}), items, std::min(bgMode_, (int)items.size() - 1), en);
      if (s != bgMode_) { bgMode_ = s; userPickedBg_ = true; }
    }
    ui.PopId();
    y += 36.f;
    ui.PushId(0x5105);
    if (bgMode_ == 0) ColorRow(ui, body, x0, y, kBgColors, bgColor_, bgHex_, en);
    else if (bgMode_ == 1) {
      TextStyle ns; ns.size = 13.f;
      const float bw = 140.f;
      if (ui.Button(M({x0, y, bw, kBtnH}), bgImage_.empty() ? L"Choose Image..." : L"Change Image...", ButtonStyle::Secondary, en)) ChooseImage();
      std::wstring nm = bgImage_.empty() ? L"No image: the colour is used" : bgImage_.filename().wstring();
      p.Text(M({x0 + bw + 12.f, y, cw - bw - 12.f - (bgImage_.empty() ? 0.f : 36.f), kBtnH}), nm, bgImage_.empty() ? pal.textTertiary : pal.textSecondary, ns);
      if (!bgImage_.empty() && ui.IconButton(M({x0 + cw - 28.f, y, 28.f, kBtnH}), "close", en, L"Remove the image", true)) { bgImage_.clear(); bgBmp_.Reset(); bgPx_.clear(); }
    } else {
      TextStyle ns; ns.size = 13.f;
      p.Text(M({x0, y, cw, kBtnH}), L"The cover art embedded in the file fills the frame", pal.textSecondary, ns);
    }
    ui.PopId();
    y += 38.f; label(y, L"Frame rate");
    ui.PushId(0x5106);
    { int cur = fps_ <= 24 ? 0 : (fps_ >= 60 ? 2 : 1); int s = ui.Segmented(M({x0, y, 210.f, kBtnH}), {L"24 fps", L"30 fps", L"60 fps"}, cur, en); if (s != cur) fps_ = s == 0 ? 24 : (s == 2 ? 60 : 30); }
    ui.PopId();

    if (!notice_.empty()) {
      TextStyle es; es.size = 12.5f; es.ellipsis = true;
      p.Text(M({X + kInset + 2.f, Y + body.h - 67.f - 26.f, aw, 22.f}), notice_, Color::Hex(0xC2410C), es);
    }
    if (Footer(ui, body, L"", false, L"Export MP4", CanApply())) Apply();
    DrawJobPanel(ui, content);
  }

 private:
  int layout_ = 0, style_ = 0, fps_ = 30, bgMode_ = 0;
  uint32_t waveColor_ = 0x2DD4BF, bgColor_ = 0x0B1220;
  std::wstring waveHex_, bgHex_;
  fs::path bgImage_;
  bool hasCover_ = false, userPickedBg_ = false;
  double dur_ = 0, parked_ = 0, peakT_ = 0;
  std::vector<uint8_t> bgPx_, coverPx_;
  int bgW_ = 0, bgH_ = 0, coverW_ = 0, coverH_ = 0;
  ComPtr<ID2D1Bitmap> bgBmp_, coverBmp_;
  bool seekDrag_ = false;

  void ColorRow(Ui& ui, const Rect& body, float x0, float y, const uint32_t* presets, uint32_t& color, std::wstring& hex, bool en) {
    for (int i = 0; i < 6; ++i) {
      ui.PushId(i);
      const Color c = Color::Hex(presets[i]);
      Rect r = Mir(ui, body, {x0 + i * 34.f, y, kBtnH, kBtnH});
      if (ui.Swatch(r, c, Mix(c, Color(0, 0, 0, 1), 0.18f), color == presets[i], HexText(presets[i])) && en) { color = presets[i]; hex = HexText(color); }
      ui.PopId();
    }
    ui.PushId(77);
    std::wstring h = hex;
    if (ui.TextInput(Mir(ui, body, {x0 + 6 * 34.f + 8.f, y, 104.f, kBtnH}), h, L"#RRGGBB", false, en)) {
      hex = h;
      uint32_t c; if (ParseHex(h, c)) color = c;
    }
    ui.PopId();
  }

  void ChooseImage() {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = Hwnd();
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) { bgImage_ = buf; userPickedBg_ = true; LoadBgImage(); }
  }

  void LoadBgImage() {
    bgPx_.clear(); bgBmp_.Reset();
    const fs::path p = bgImage_;
    if (p.empty() || Hwnd() == nullptr) return;
    RunAsync([this, p]() -> std::function<void()> {
      img::Bitmap bm; img::LoadOptions lo; lo.maxSide = 960; std::wstring err;
      const bool ok = img::LoadBitmap(p, bm, lo, &err) && !bm.Empty();
      if (ok) {
        for (size_t i = 0; i < (size_t)bm.width * bm.height; ++i) { uint8_t* q = &bm.px[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
      }
      auto px = std::make_shared<img::Bitmap>(std::move(bm));
      return [this, ok, px, p, err]() {
        if (p != bgImage_) return;
        if (!ok) { notice_ = err.empty() ? L"This image cannot be opened." : err; bgImage_.clear(); return; }
        notice_.clear();
        bgPx_ = std::move(px->px); bgW_ = px->width; bgH_ = px->height; bgBmp_.Reset();
      };
    });
  }

  ID2D1Bitmap* MakeBitmap(ComPtr<ID2D1Bitmap>& slot, const std::vector<uint8_t>& px, int w, int h, bool premultiplied) {
    if (slot || px.empty() || !dc_) return slot.Get();
    std::vector<uint8_t> tmp;
    const uint8_t* src = px.data();
    if (!premultiplied) {   // ExtractFrame returns opaque BGRA: already valid premultiplied data
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(w, h), src, w * 4, &bp, &b))) slot = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
    return slot.Get();
  }

  // ---- live preview ------------------------------------------------------------------------------------------------------
  void DrawPreview(Ui& ui, const Rect& body, const Rect& well, bool en) {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    p.FillRoundRect(well, 22.f, pal.darkCanvas);
    const float ctlH = 34.f;
    const Rect area{well.x + 14.f, well.y + 24.f, well.w - 28.f, well.h - 38.f - ctlH};
    const float aspect = layout_ == 1 ? 9.f / 16.f : (layout_ == 2 ? 1.f : 16.f / 9.f);
    float fw = area.w, fh = fw / aspect;
    if (fh > area.h) { fh = area.h; fw = fh * aspect; }
    const Rect f{area.cx() - fw / 2, area.cy() - fh / 2, fw, fh};
    const Color bg = Color::Hex(bgColor_);
    p.PushClipRoundRect(f, 6.f);
    p.FillRect(f, bg);
    auto cover = [&](ID2D1Bitmap* b) {
      if (!b) return;
      auto sz = b->GetSize();
      const float s = std::max(f.w / sz.width, f.h / sz.height);
      p.Bitmap(b, {f.cx() - sz.width * s / 2, f.cy() - sz.height * s / 2, sz.width * s, sz.height * s});
    };
    if (bgMode_ == 1 && !bgPx_.empty()) cover(MakeBitmap(bgBmp_, bgPx_, bgW_, bgH_, true));
    else if (bgMode_ == 2 && !coverPx_.empty()) cover(MakeBitmap(coverBmp_, coverPx_, coverW_, coverH_, true));
    double pos = Playing() ? std::clamp(PlayPos(), 0.0, std::max(dur_, 0.1)) : (parked_ > 0 ? parked_ : peakT_);
    DrawVisual(p, f, pos);
    p.PopClip();
    p.StrokeRoundRect(f, 6.f, Color(1, 1, 1, 0.10f), 1.f);

    if (!wave_.Ok()) {
      TextStyle ls; ls.size = 12.5f; ls.align = Align::Center;
      const bool bad = !waveError_.empty();
      p.Text(f, bad ? waveError_ : L"Reading the audio...", bad ? Color::Hex(0xFB923C) : Color(1, 1, 1, 0.6f), ls);
    }
    TextStyle hs; hs.size = 11.f;
    p.Text({well.x + 18.f, well.y + 5.f, well.w - 36.f, 14.f}, L"Preview approximates the exported video", Color(1, 1, 1, 0.38f), hs);

    // ---- transport row: play / seek bar / time
    const float cy = well.bottom() - ctlH / 2 - 4.f;
    ui.PushId(0x5199);
    const bool playing = Playing();
    const bool canPlay = en && PlayerReady() && dur_ > 0;
    Rect pb = Mir(ui, body, {well.x + 14.f, cy - 13.f, 26.f, 26.f});
    if (ui.IconButton(pb, playing ? "pause" : "play", canPlay, playing ? L"Pause" : L"Play the preview", false)) {
      if (playing) { parked_ = PlayPos(); StopPlayback(); }
      else PlayRange(parked_ >= dur_ - 0.05 ? 0.0 : parked_, dur_, 0, L"viz");
    }
    TextStyle tt; tt.size = 12.f; tt.mono = true; tt.align = Align::End;
    const float tw = 118.f;
    p.Text(Mir(ui, body, {well.right() - 14.f - tw, cy - 11.f, tw, 22.f}), FormatTime(pos, false) + L" / " + FormatTime(dur_, false), Color(1, 1, 1, 0.62f), tt);
    Rect bar = Mir(ui, body, {well.x + 50.f, cy - 10.f, well.w - 50.f - 14.f - tw - 10.f, 20.f});
    Interaction it = ui.Behavior(bar, ui.Id("seek"), canPlay, true);
    if (it.hot && canPlay) ui.SetCursor((int)(intptr_t)IDC_HAND);
    if (canPlay && (it.pressed || (it.active && !it.released))) {
      const double t = std::clamp((double)((ui.In().mx - bar.x) / std::max(1.f, bar.w)), 0.0, 1.0) * dur_;
      parked_ = t;
      if (it.pressed && playing) PlayRange(t, dur_, 0, L"viz");
    }
    p.FillRoundRect({bar.x, bar.cy() - 2.f, bar.w, 4.f}, 2.f, Color(1, 1, 1, 0.18f));
    const float fx = bar.x + bar.w * (float)(dur_ > 0 ? std::clamp(pos / dur_, 0.0, 1.0) : 0.0);
    p.FillRoundRect({bar.x, bar.cy() - 2.f, std::max(4.f, fx - bar.x), 4.f}, 2.f, accent::base);
    p.FillCircle(fx, bar.cy(), 5.f, Color(1, 1, 1, 1));
    ui.PopId();
  }

  // Approximation of ffmpeg's showwaves (cline) / showfreqs (bar) / avectorscope, driven by the real peaks around `pos`.
  void DrawVisual(Painter& p, const Rect& f, double pos) {
    if (!wave_.Ok()) return;
    const Color wc = Color::Hex(waveColor_);
    const uint32_t seed = (uint32_t)(pos * 24.0);
    if (style_ == 0) {
      const int cols = (int)(f.w / 2.2f);
      const float cw = f.w / cols;
      const double span = 0.35;                       // seconds of audio across the frame
      const float mid = f.cy(), amp = f.h / 3.f * 0.5f * 1.9f;
      for (int i = 0; i < cols; ++i) {
        const double t = pos + ((double)i / cols - 0.5) * span;
        const float a = wave_.PeakAt(std::max(0.0, t));
        const float carrier = std::fabs(std::sin((float)(t * 2.0 * 3.14159265 * 52.0)) * 0.78f + (Hash01(i * 2654435761u + seed) - 0.5f) * 0.45f);
        const float h = std::max(0.6f, a * carrier * amp);
        p.FillRect({f.x + i * cw, mid - h, std::max(1.2f, cw - 0.4f), 2 * h}, wc);
      }
      if (!Playing() || true) p.FillRect({f.x, mid - 0.5f, f.w, 1.f}, wc.WithAlpha(0.35f));
    } else if (style_ == 1) {
      const int n = 44;
      const Rect r{f.x, f.cy() - f.h / 4, f.w, f.h / 2};
      const float bw = r.w / n;
      for (int k = 0; k < n; ++k) {
        const double t = pos + ((int)(Hash01(k * 977u) * 9.f) - 4) * 0.012;
        const float a = wave_.PeakAt(std::max(0.0, t));
        const float tilt = 0.35f + 0.65f * std::pow(1.f - (float)k / n, 1.6f);
        const float wob = 0.72f + 0.28f * std::sin((float)(pos * 9.0) + k * 0.9f) * (0.5f + Hash01(k * 31u + seed));
        const float h = std::max(1.5f, a * tilt * wob * r.h);
        p.FillRect({r.x + k * bw + 1.f, r.bottom() - h, std::max(1.f, bw - 2.f), h}, wc);
      }
    } else {
      const float sz = std::min(f.w, f.h) * 0.75f;
      const float a = std::min(1.f, wave_.PeakAt(pos) * 1.1f + 0.04f);
      const int n = 220;
      float px = 0, py = 0;
      for (int i = 0; i <= n; ++i) {
        const float th = (float)i / n * 6.2831853f;
        const float x = std::sin(th * 3.f + (float)pos * 2.f), y = std::sin(th * 2.f + 0.6f * std::sin((float)pos * 3.f));
        const float sx = f.cx() + x * a * sz / 2, sy = f.cy() - y * a * sz / 2;
        if (i) p.Line(px, py, sx, sy, wc, 1.6f, true);
        px = sx; py = sy;
      }
    }
  }
};

} // namespace

ToolDialog* MakeAudioVisualizer(const Selection& sel) { return new VisualizerDialog(sel); }

} // namespace filo::tools
