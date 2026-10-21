// Save Frames: find the moments (frame stepping), mark them into a thumbnail strip and export them as PNG at native resolution.
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

constexpr float kThumbW = 118, kThumbH = 66, kThumbGap = 10;

struct Mark {
  int frame = 0;
  double t = 0;
  std::vector<uint8_t> px;          // BGRA thumbnail (premultiplied == straight, opaque)
  int w = 0, h = 0;
  ComPtr<ID2D1Bitmap1> bmp;
};

class SaveFramesDialog : public VideoToolDialog {
 public:
  explicit SaveFramesDialog(const Selection& sel) : VideoToolDialog(sel) {}

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Save Frames"; o.widthDip = 648; o.heightDip = 740; o.resizable = true; o.minWidthDip = 540; o.minHeightDip = 600;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "save-frames"; }
  void LoadPreset(const Json& j) override {
    if (j.Has("times")) {
      marks_.clear();
      const bool thumbs = j.Get("thumbs").AsBool(false);      // preset / test hook: decode the thumbnails right away (blocks briefly)
      for (const auto& t : j.Get("times").Elements()) {
        const double tt = std::clamp(t.AsNumber(), 0.0, std::max(0.0, Dur() - 0.05));
        if (thumbs) {
          media::FrameImage fi = media::ExtractFrame(path_, tt, 640, 360, true);
          if (fi.ok) { Mark th = MakeThumb(fi.bgra.data(), fi.width, fi.height); AddMarkAt(tt, &th); continue; }
        }
        AddMarkAt(tt, nullptr);
      }
    }
  }
  void TestApply() override { StartExport(); }
  void OnDeviceReset(bool rebuilt) override { VideoToolDialog::OnDeviceReset(rebuilt); for (auto& m : marks_) m.bmp.Reset(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float stripH = kThumbH + 28;
    const float below = 10 + 34 + 2 + 40 + 8 + 30 + 8 + stripH + 8;
    const float wellH = std::max(150.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    SinkOptions so;
    DrawWellSink(ui, g, so);
    const bool en = Usable() && !Busy() && view_.IsOpen();

    // a pending mark waits until the still for the playhead has arrived
    if (pendingMark_ && !view_.StillPending()) { pendingMark_ = false; CommitMark(); }
    if (en && SinkFocused(ui)) for (const auto& k : ui.In().keys) if (k.vk == 'M') RequestMark();

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, -1, 0);
    y += 34 + 2;
    std::vector<double> mk; for (auto& m : marks_) mk.push_back(m.t);
    DrawScrubber(ui, {well.x + 10, y, well.w - 20, 40}, &mk);
    y += 40 + 8;

    // ---- actions
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(170), Fixed(130), Flex(1)}, 8);
      if (ui.Button({c[0].x, row.cy() - 13, c[0].w, 26}, L"Mark frame", ButtonStyle::Secondary, en, "add")) RequestMark();
      if (ui.Button({c[1].x, row.cy() - 13, c[1].w, 26}, L"Clear all", ButtonStyle::Secondary, !Busy() && !marks_.empty())) marks_.clear();
      TextStyle hs; hs.size = 12.5f; hs.align = Align::End;
      p.Text(c[2], marks_.empty() ? L"Step to a frame, then mark it (M)" : std::to_wstring(marks_.size()) + (marks_.size() == 1 ? L" frame marked" : L" frames marked"), pal.textTertiary, hs);
    }
    y += 30 + 8;

    DrawStrip(ui, {well.x - 4, y, well.w + 8, stripH});

    // ---- footer
    FooterInfo fi;
    fi.action = marks_.empty() ? L"Export Frames" : L"Export " + std::to_wstring(marks_.size()) + (marks_.size() == 1 ? L" Frame" : L" Frames");
    fi.actionEnabled = Usable() && !Busy() && !marks_.empty();
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else fi.status = marks_.empty() ? L"Mark at least one frame to export." : L"PNG, " + std::to_wstring(SrcW()) + L" \x00D7 " + std::to_wstring(SrcH()) + L" px (full resolution)";
    const bool enter = ConsumeEnter();
    FooterResult fr = DrawFooter(ui, content, fi);
    if ((fr.action || enter) && fi.actionEnabled) StartExport();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && LastResult().ok)
      DrawDoneNote(ui, content, {std::to_wstring(exported_) + (exported_ == 1 ? L" PNG file" : L" PNG files") + L", " + std::to_wstring(SrcW()) + L" \x00D7 " + std::to_wstring(SrcH()) + L" px",
                                 L"Folder: " + (LastResult().outputs.empty() ? std::wstring() : LastResult().outputs[0].filename().wstring())});
  }

 private:
  void RequestMark() {
    if (!view_.IsOpen()) return;
    if (view_.Playing()) { CommitMark(); return; }
    pendingMark_ = true; pendingT_ = view_.Pos();
  }

  void CommitMark() {
    const double t = view_.Playing() ? view_.Pos() : pendingT_;
    const int w = view_.FrameW(), h = view_.FrameH();
    const std::vector<uint8_t>& px = view_.Pixels();
    if (px.size() < (size_t)w * h * 4 || w <= 0) { AddMarkAt(t, nullptr); return; }
    Mark th = MakeThumb(px.data(), w, h);
    AddMarkAt(t, &th);
  }

  static Mark MakeThumb(const uint8_t* px, int w, int h) {
    Mark th;
    const int th2 = 132, tw2 = std::max(2, (int)std::lround((double)w * th2 / h));
    th.w = tw2; th.h = th2; th.px.resize((size_t)tw2 * th2 * 4);
    for (int y = 0; y < th2; ++y) {
      const int y0 = (int)((int64_t)y * h / th2), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / th2));
      for (int x = 0; x < tw2; ++x) {
        const int x0 = (int)((int64_t)x * w / tw2), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / tw2));
        int s[3] = {0, 0, 0}, n = 0;
        for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { const uint8_t* q = &px[((size_t)yy * w + xx) * 4]; s[0] += q[0]; s[1] += q[1]; s[2] += q[2]; ++n; }
        uint8_t* o = &th.px[((size_t)y * tw2 + x) * 4];
        o[0] = (uint8_t)(s[0] / n); o[1] = (uint8_t)(s[1] / n); o[2] = (uint8_t)(s[2] / n); o[3] = 255;
      }
    }
    return th;
  }

  void AddMarkAt(double t, Mark* thumb) {
    const int frame = view_.IsOpen() ? view_.FrameIndex(t) : (int)std::floor(t * Fps() + 1e-3);
    for (auto& m : marks_) if (m.frame == frame) return;
    Mark m;
    if (thumb) m = std::move(*thumb);
    m.frame = frame; m.t = frame / Fps();
    marks_.push_back(std::move(m));
    std::sort(marks_.begin(), marks_.end(), [](const Mark& a, const Mark& b) { return a.frame < b.frame; });
  }

  ID2D1Bitmap* ThumbBitmap(Mark& m) {
    if (m.bmp) return m.bmp.Get();
    if (m.px.empty() || !dc_) return nullptr;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    dc_->CreateBitmap(D2D1::SizeU(m.w, m.h), m.px.data(), m.w * 4, &bp, &m.bmp);
    return m.bmp.Get();
  }

  void DrawStrip(Ui& ui, const Rect& vp) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    if (marks_.empty()) {
      p.FillRoundRect(vp.Inset(4, 0), control::listRowRadius, Color(pal.text.r, pal.text.g, pal.text.b, 0.045f));
      p.StrokeRoundRect(vp.Inset(4, 0), control::listRowRadius, pal.divider, 1.f);
      TextStyle es; es.size = 13; es.align = Align::Center; es.wrap = true;
      p.Icon("frames", vp.cx(), vp.y + vp.h * 0.36f, 22, pal.textTertiary, 1.6f);
      p.Text({vp.x + 30, vp.y + vp.h * 0.36f + 16, vp.w - 60, 36}, L"Marked frames appear here", pal.textTertiary, es);
      return;
    }
    const float itemW = kThumbW + kThumbGap;
    const float contentW = marks_.size() * itemW + 8;
    const float maxOff = std::max(0.f, contentW - vp.w);
    const InputState& in = ui.In();
    if (in.inWindow && vp.Contains(in.mx, in.my) && in.wheel != 0) { stripOff_ -= in.wheel * 60.f; ui.RequestFrame(); }
    stripOff_ = std::clamp(stripOff_, 0.f, maxOff);
    p.PushClip(vp);
    int removeIdx = -1;
    for (int i = 0; i < (int)marks_.size(); ++i) {
      Mark& m = marks_[i];
      const float x = vp.x + 8 + i * itemW - stripOff_;
      Rect th{x, vp.y + 4, kThumbW, kThumbH};
      if (th.right() < vp.x - 4 || th.x > vp.right() + 4) continue;
      ui.PushId(i);
      const bool hoverItem = in.inWindow && th.Contains(in.mx, in.my) && vp.Contains(in.mx, in.my);
      Rect xr{th.right() - 24, th.y + 4, 20, 20};
      Interaction ix = ui.Behavior(xr, ui.Id("rm"), !Busy() && hoverItem, false);
      Interaction it = ui.Behavior(th, ui.Id("thumb"), true, false);
      const float hov = ui.Anim(ui.Id("hov"), (it.hot || ix.hot) ? 1.f : 0.f);
      p.FillRoundRect(th, 10, Color(0, 0, 0, 0.30f));
      if (ID2D1Bitmap* b = ThumbBitmap(m)) {
        const float s = std::min(th.w / m.w, th.h / m.h);
        Rect ir{th.cx() - m.w * s / 2, th.cy() - m.h * s / 2, m.w * s, m.h * s};
        p.PushClipRoundRect(th, 10);
        p.Bitmap(b, ir, 1.f, 0.f);
        p.PopClip();
      } else p.Icon("video", th.cx(), th.cy(), 20, Color(1, 1, 1, 0.45f), 1.5f);
      const bool atMark = view_.IsOpen() && view_.FrameIndex(view_.Pos()) == m.frame;
      p.StrokeRoundRect(th, 10, Mix(Color(0, 0, 0, 0.18f), accent::base, std::max(hov, atMark ? 1.f : 0.f)), atMark ? 2.5f : 1.5f);
      TextStyle ts = MonoStyle(12.f, 500, Align::Center);
      p.Text({th.x, th.bottom() + 3, th.w, 18}, FormatTime(m.t, true), pal.textSecondary, ts);
      if (hoverItem) {
        p.FillCircle(xr.cx(), xr.cy(), 10, ix.hot ? Color(0.9f, 0.2f, 0.2f, 0.95f) : Color(0, 0, 0, 0.62f));
        p.Icon("close", xr.cx(), xr.cy(), 11, Color(1, 1, 1, 0.95f), 2.f);
      }
      if (ix.clicked) removeIdx = i;
      else if (it.clicked && view_.IsOpen()) view_.Seek(m.t, false);
      ui.PopId();
    }
    p.PopClip();
    if (removeIdx >= 0) marks_.erase(marks_.begin() + removeIdx);
  }

  void StartExport() {
    media::SaveFramesRequest req;
    const double fr = 1.0 / Fps();
    for (auto& m : marks_) req.timestamps.push_back(std::max(0.0, m.t - std::min(0.003, 0.25 * fr)));
    exported_ = (int)req.timestamps.size();
    const fs::path in = path_;
    RunJob(L"Saving frames...", [in, req](JobContext& ctx) -> JobResult { return media::SaveVideoFrames(in, req, ctx); });
  }

  std::vector<Mark> marks_;
  bool pendingMark_ = false;
  double pendingT_ = 0;
  float stripOff_ = 0;
  int exported_ = 0;
};

} // namespace

ToolDialog* MakeSaveFrames(const Selection& sel) { return new SaveFramesDialog(sel); }

} // namespace filo::tools
