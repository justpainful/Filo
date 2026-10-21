// Convert Audio Channels - reference window 515 x 515 dip: "Output channel preview" with two waveform rows (each with a play triangle),
// divider, Mono | Stereo segmented control, two gain sliders with percent readouts, "Play Output", footer with Apply.
#include "tools/audio/audio_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace audio;

namespace {

class ConvertChannelsDialog : public AudioDialog {
 public:
  explicit ConvertChannelsDialog(const Selection& sel) { selection_ = sel; }

 protected:
  ui::DialogOptions Options() const override {
    ui::DialogOptions o; o.title = L"Convert Audio Channels"; o.widthDip = 515; o.heightDip = 515; o.rtl = RtlSetting();
    return o;
  }
  std::string ToolId() const override { return "convert-channels"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject(); j["channels"] = outCh_; j["left"] = (double)gl_; j["right"] = (double)gr_; return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("channels")) outCh_ = j.Get("channels").AsInt(2) == 1 ? 1 : 2;
    if (j.Has("left")) gl_ = (float)std::clamp(j.Get("left").AsNumber(1.0), 0.0, 4.0);
    if (j.Has("right")) gr_ = (float)std::clamp(j.Get("right").AsNumber(1.0), 0.0, 4.0);
    boost_ = gl_ > 1.0001f || gr_ > 1.0001f;
  }

  void Begin() override {
    waveLoading_ = true;
    const fs::path p = Path();
    auto cancel = cancel_;
    RunAsync([this, p, cancel]() -> std::function<void()> {
      media::WaveformPerChannel w = media::ComputeWaveformPerChannel(p, 360, cancel.get());
      return [this, w = std::move(w)]() mutable {
        waveLoading_ = false;
        if (!w.ok || w.peaks.empty()) { waveError_ = w.error.empty() ? L"This audio file cannot be read." : w.error; return; }
        dur_ = w.duration; srcCh_ = w.channels;
        pl_ = w.peaks[0]; pr_ = w.peaks.size() > 1 ? w.peaks[1] : w.peaks[0];
        norm_ = 0.02f;
        for (float v : pl_) norm_ = std::max(norm_, v);
        for (float v : pr_) norm_ = std::max(norm_, v);
      };
    });
  }

  void TestPlay() override { PlayOutput(); }
  bool CanApply() const override { return !Busy() && waveError_.empty(); }

  void Apply() override {
    media::ChannelsRequest req = Request(gl_, gr_);
    req.preserveMetadata = SettingsStore::Get().Snapshot().preserveMetadata;
    const fs::path in = Path();
    StopPlayback();
    RunJob(outCh_ == 1 ? L"Converting to mono..." : L"Converting to stereo...", [in, req](JobContext& ctx) -> JobResult { return media::ConvertChannels(in, req, ctx); });
  }

  void Draw(Ui& ui, const Rect& content, const Rect& body) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const bool en = !Locked();
    const float X = body.x, Y = body.y, W = body.w;
    auto M = [&](Rect r) { return Mir(ui, body, r); };
    TextStyle ts; ts.size = dialog::bodyFontSize;

    p.Text(M({X + kInner, Y + 72, W - 2 * kInner, 22}), L"Output channel preview", pal.text, ts);

    // ---- two preview rows with play triangles
    const bool mono = outCh_ == 1;
    const wchar_t* tags[2] = {L"L", L"R"};
    for (int i = 0; i < 2; ++i) {
      const float ry = Y + (i == 0 ? 108.5f : 180.5f);
      Rect row = M({X + kInner, ry, W - kInner - 64.f, 56.f});
      p.FillRoundRect(row, 10.f, Color(pal.text.r, pal.text.g, pal.text.b, 0.06f));
      const Rect wr = row.Inset(10.f, 8.f);
      if (!pl_.empty()) {
        std::vector<float> mix;
        const std::vector<float>* src = &pl_;
        float scale = i == 0 ? gl_ : gr_;
        if (!i) src = &pl_; else src = &pr_;
        if (mono) {
          mix.resize(pl_.size());
          for (size_t k = 0; k < mix.size(); ++k) mix[k] = 0.5f * (gl_ * pl_[k] + gr_ * pr_[std::min(k, pr_.size() - 1)]);
          src = &mix; scale = 1.f;
        }
        DrawPeakBars(p, Rect{row.x + 30.f, wr.y, row.w - 34.f, wr.h}, *src, scale, norm_, Color(pal.text.r, pal.text.g, pal.text.b, 0.50f), BarStyle{}, 0.86f);
      } else {
        TextStyle ls; ls.size = 13.f; ls.align = Align::Center;
        const bool bad = !waveError_.empty();
        p.Text(row, bad ? waveError_ : L"Reading the waveform...", bad ? Color::Hex(0xC2410C) : Info(pal), ls);
      }
      TextStyle lt; lt.size = 11.5f; lt.weight = 600;
      p.Text({row.x + 10.f, row.y + 6.f, 24.f, 16.f}, mono ? L"M" : tags[i], pal.textTertiary, lt);
      // playhead while this row (or the full output) plays
      if (Playing() && dur_ > 0 && (PlayingTag(L"out") || PlayingTag(tags[i]))) {
        const Rect pr{row.x + 30.f, wr.y, row.w - 34.f, wr.h};
        const float px = pr.x + (float)std::clamp(PlayPos() / std::min(dur_, 60.0), 0.0, 1.0) * pr.w;
        p.FillRoundRect({px - 1.f, row.y + 4.f, 2.f, row.h - 8.f}, 1.f, pal.text);
      }
      ui.PushId(100 + i);
      const bool on = PlayingTag(tags[i]);
      if (GlyphOnly(ui, M({body.right() - 37.5f - 16.f, ry + 28.f - 16.f, 32.f, 32.f}), on ? "stop" : "play", on ? 18.f : 22.f, en && dur_ > 0 && !Rendering())) {
        if (on) StopPlayback(); else PlayRow(i);
      }
      ui.PopId();
    }

    p.FillRect({X + kInner, Y + 253.f, W - 2 * kInner, 1.f}, pal.divider);

    // ---- Mono | Stereo
    p.Text(M({X + kInner, Y + 269.5f, 140.f, kBtnH}), L"Output", pal.text, ts);
    ui.PushId(0xC0A1);
    int sel = ui.Segmented(M({body.right() - kInner - 137.5f, Y + 269.5f, 137.5f, kBtnH}), {L"Mono", L"Stereo"}, outCh_ == 1 ? 0 : 1, en);
    ui.PopId();
    if (sel != (outCh_ == 1 ? 0 : 1)) { outCh_ = sel == 0 ? 1 : 2; if (Playing()) StopPlayback(); }

    // ---- gains
    const float hi = boost_ ? 4.f : 1.f;
    const wchar_t* names[2] = {L"Left channel", L"Right channel"};
    for (int i = 0; i < 2; ++i) {
      const float cy = Y + (i == 0 ? 320.f : 354.5f);
      p.Text(M({X + kInner, cy - 13.f, 120.f, 26.f}), names[i], pal.text, ts);
      float v = std::min(i == 0 ? gl_ : gr_, hi);
      ui.PushId(0x600 + i);
      if (ui.Slider(M({X + kInner + 112.f, cy - 13.f, 428.f - kInner - 112.f, 26.f}), v, 0.f, hi, 0.01f, NAN, en)) { (i == 0 ? gl_ : gr_) = v; if (Playing()) StopPlayback(); }
      ui.PopId();
      TextStyle vs = ts; vs.align = Align::End;
      wchar_t b[24]; swprintf_s(b, L"%d%%", (int)std::lround((i == 0 ? gl_ : gr_) * 100));
      p.Text(M({X + 439.5f, cy - 13.f, 52.f, 26.f}), b, Info(pal), vs);
    }

    // ---- Play Output
    ui.PushId(0xC0A2);
    const bool po = PlayingTag(L"out");
    if (GlyphButton(ui, M({body.right() - kInner - 127.5f, Y + 380.f, 127.5f, kBtnH}), po ? L"Stop" : L"Play Output", po ? "stop" : "play", en && dur_ > 0 && (!Rendering() || po))) {
      if (po) StopPlayback(); else PlayOutput();
    }
    ui.PopId();

    // ---- boost option + notices (the reference leaves this band empty)
    ui.PushId(0xC0A3);
    bool bo = boost_;
    if (ui.Checkbox(M({X + kInner, Y + 410.f, 300.f, 24.f}), L"Allow boost up to 400%", bo, en)) { boost_ = bo; if (!boost_) { gl_ = std::min(gl_, 1.f); gr_ = std::min(gr_, 1.f); } }
    ui.PopId();
    std::wstring note = Rendering() ? L"Rendering the preview..." : notice_;
    if (!note.empty()) {
      TextStyle ns; ns.size = 12.5f; ns.ellipsis = true; ns.align = Align::End;
      p.Text(M({X + kInner + 200.f, Y + 410.f, W - 2 * kInner - 200.f, 24.f}), note, notice_.empty() ? Info(pal) : Color::Hex(0xC2410C), ns);
    }

    if (Footer(ui, body, L"", false, L"Apply", CanApply())) Apply();
    DrawJobPanel(ui, content);
  }

 private:
  int outCh_ = 2;
  float gl_ = 1.f, gr_ = 1.f;
  bool boost_ = false;
  double dur_ = 0;
  int srcCh_ = 0;
  float norm_ = 1.f;
  std::vector<float> pl_, pr_;

  media::ChannelsRequest Request(float l, float r) const {
    media::ChannelsRequest q; q.outChannels = outCh_; q.leftGain = l; q.rightGain = r; return q;
  }
  double PreviewLen() const { return dur_ > 0 ? std::min(dur_, 60.0) : 60.0; }

  void StartPreview(const std::wstring& tag, media::ChannelsRequest q) {
    const fs::path in = Path(); const double len = PreviewLen();
    RenderAndPlay(tag, 0.0, [in, q, len](JobContext& ctx, const fs::path& wav) { return media::PreviewChannelsToWav(in, q, 0.0, len, wav, ctx); });
  }
  void PlayOutput() { StartPreview(L"out", Request(gl_, gr_)); }
  // One output channel on its own: the other channel is silenced, so it is heard only on its side.
  void PlayRow(int i) {
    if (outCh_ == 1) { StartPreview(i == 0 ? L"L" : L"R", Request(gl_, gr_)); return; }
    StartPreview(i == 0 ? L"L" : L"R", Request(i == 0 ? gl_ : 0.f, i == 0 ? 0.f : gr_));
  }
};

} // namespace

ToolDialog* MakeConvertChannels(const Selection& sel) { return new ConvertChannelsDialog(sel); }

} // namespace filo::tools
