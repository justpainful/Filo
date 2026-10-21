// Trim Audio - reference window 514 x 432 dip: "Drag the ends to trim" + "0:41.74 selected", the accent-bordered waveform box with end
// blocks, monospace Start / End readouts, "Trim silent ends automatically", "Play Selected Range", footer with the small Apply button.
#include "tools/audio/audio_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace audio;

namespace {

class TrimAudioDialog : public AudioDialog {
 public:
  explicit TrimAudioDialog(const Selection& sel) { selection_ = sel; }

 protected:
  ui::DialogOptions Options() const override {
    ui::DialogOptions o; o.title = L"Trim Audio"; o.widthDip = 514; o.heightDip = 432; o.rtl = RtlSetting();
    return o;
  }
  std::string ToolId() const override { return "trim-audio"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["silent"] = silent_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("start")) start_ = std::max(0.0, j.Get("start").AsNumber());
    if (j.Has("end")) end_ = j.Get("end").AsNumber();
    if (j.Has("silent")) { silent_ = j.Get("silent").AsBool(); silentPending_ = silent_; }
    if (j.Has("copy")) copy_ = j.Get("copy").AsBool();
    if (dur_ > 0) Clamp();
  }

  void Begin() override {
    LoadWave(900);
    PreparePlayer(Path());
  }
  void OnWaveLoaded() override {
    dur_ = wave_.wf.duration;
    Clamp();
    if (silentPending_) { silentPending_ = false; DetectSilence(); }
  }

  void TestPlay() override { PlayRange(start_, end_, 0, L"range"); }
  bool CanApply() const override { return !Busy() && waveError_.empty() && (dur_ <= 0 || end_ - start_ >= 0.05); }

  void Apply() override {
    media::TrimAudioRequest req;
    req.start = start_;
    req.end = (end_ > start_ && (dur_ <= 0 || end_ < dur_ - 0.004)) ? end_ : 0;
    req.trimSilentEnds = silent_;
    req.copyMode = copy_;
    req.preserveMetadata = SettingsStore::Get().Snapshot().preserveMetadata;
    const fs::path in = Path();
    StopPlayback();
    RunJob(L"Trimming audio...", [in, req](JobContext& ctx) -> JobResult { return media::TrimAudio(in, req, ctx); });
  }

  void Draw(Ui& ui, const Rect& content, const Rect& body) override {
    (void)content;
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const bool en = !Locked();
    const float X = body.x, Y = body.y, W = body.w;
    auto M = [&](Rect r) { return Mir(ui, body, r); };

    // ---- header line
    TextStyle ts; ts.size = 14.f;
    p.Text(M({X + kInset, Y + 64, W - 2 * kInset, 22}), L"Drag the ends to trim", Info(pal), ts);
    TextStyle te = ts; te.align = Align::End;
    p.Text(M({X + kInset, Y + 64, W - 2 * kInset, 22}), FormatTime(std::max(0.0, end_ - start_), true) + L" selected", Info(pal), te);

    // ---- waveform box
    const Rect box{X + kInset, Y + 95.5f, W - 2 * kInset, 123.5f};
    double ph = start_;
    if (Playing()) ph = std::clamp(PlayPos(), start_, end_);
    else if (parked_ >= start_ && parked_ <= end_) ph = parked_;
    ui.PushId(0x7121);
    TrimEdit te2 = TrimBox(ui, box, wave_, dur_, start_, end_, ph, en && dur_ > 0, trim_);
    ui.PopId();
    if (te2.changed) { silent_ = false; parked_ = -1; if (Playing()) StopPlayback(); }
    if (te2.seek >= 0 && PlayerReady()) { parked_ = te2.seek; PlayRange(te2.seek, end_, 0, L"range"); }
    if (dur_ <= 0) {
      TextStyle ls; ls.size = 13.5f; ls.align = Align::Center;
      const bool bad = !waveError_.empty();
      p.Text(box.Inset(40, 0), bad ? waveError_ : L"Reading the waveform...", bad ? Color::Hex(0xC2410C) : Info(pal), ls);
    }

    // ---- Start / End readouts (monospace)
    // the waveform keeps its left-to-right time axis in RTL, so Start stays under the left handle and End under the right one
    TextStyle ms; ms.size = 13.5f; ms.mono = true; ms.align = ui.Rtl() ? Align::End : Align::Start;
    TextStyle me = ms; me.align = ui.Rtl() ? Align::Start : Align::End;
    p.Text({X + kInset, Y + 227, W - 2 * kInset, 22}, L"Start " + FormatTime(start_, true), Info(pal), ms);
    p.Text({X + kInset, Y + 227, W - 2 * kInset, 22}, L"End " + FormatTime(dur_ > 0 ? end_ : 0, true), Info(pal), me);

    // ---- trim silent ends
    ui.PushId(0x7122);
    bool sil = silent_;
    if (ui.Checkbox(M({X + kInner, Y + 266.5f, 330, 24}), L"Trim silent ends automatically", sil, en && dur_ > 0 && !detecting_)) SetSilent(sil);
    ui.PopId();

    // ---- play
    const bool playing = PlayingTag(L"range");
    const bool canPlay = en && PlayerReady() && dur_ > 0;
    ui.PushId(0x7123);
    if (GlyphButton(ui, M({body.right() - 22.f - 187.f, Y + 302.f, 187.f, kBtnH}), playing ? L"Stop" : L"Play Selected Range", playing ? "stop" : "play", canPlay || playing)) {
      if (playing) StopPlayback();
      else { parked_ = -1; PlayRange(start_, end_, 0, L"range"); }
    }
    ui.PopId();

    // ---- notice (errors / progress), only when needed
    std::wstring note = !notice_.empty() ? notice_ : (detecting_ ? L"Looking for silence at both ends..." : L"");
    if (!note.empty()) {
      TextStyle ns; ns.size = 12.5f; ns.wrap = true; ns.ellipsis = true;
      p.Text(M({X + kInner, Y + 336.f, W - 2 * kInner, 22}), note, notice_.empty() ? Info(pal) : Color::Hex(0xC2410C), ns);
    }

    if (Footer(ui, body, L"", false, L"Apply", CanApply())) Apply();
    DrawJobPanel(ui, content);
  }

 private:
  double start_ = 0, end_ = 0, dur_ = 0, parked_ = -1;
  double savedStart_ = 0, savedEnd_ = 0;
  bool silent_ = false, silentPending_ = false, detecting_ = false, copy_ = false;
  TrimState trim_;

  void Clamp() {
    start_ = std::clamp(start_, 0.0, std::max(0.0, dur_ - 0.05));
    if (end_ <= start_ || end_ > dur_) end_ = dur_;
    if (end_ - start_ < 0.05) { start_ = 0; end_ = dur_; }
  }

  void SetSilent(bool on) {
    if (!on) { silent_ = false; if (dur_ > 0) { start_ = savedStart_; end_ = savedEnd_; Clamp(); } return; }
    silent_ = true;
    savedStart_ = start_; savedEnd_ = end_;
    DetectSilence();
  }

  void DetectSilence() {
    detecting_ = true; notice_.clear();
    const fs::path p = Path();
    auto cancel = cancel_;
    RunAsync([this, p, cancel]() -> std::function<void()> {
      media::SilenceInfo si = media::DetectSilentEnds(p, -50, 0.15, cancel.get());
      return [this, si]() {
        detecting_ = false;
        if (!si.ok) { notice_ = si.error.empty() ? L"The silence could not be measured." : si.error; silent_ = false; return; }
        if (si.allSilent) { notice_ = L"This audio is silent: nothing to keep."; silent_ = false; return; }
        if (!silent_) return;
        if (dur_ <= 0) dur_ = si.duration;
        start_ = si.start; end_ = si.end;
        parked_ = -1;
        Clamp();
        if (si.leadingSilence < 0.05 && si.trailingSilence < 0.05) notice_ = L"There is no silence at either end.";
      };
    });
  }
};

} // namespace

ToolDialog* MakeTrimAudio(const Selection& sel) { return new TrimAudioDialog(sel); }

} // namespace filo::tools
