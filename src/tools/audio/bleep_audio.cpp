// Bleep Audio - reference window 515 x 515 dip: "Bleeps 0", Bleep / Audio lanes with a time scale, the drag hint, a Preview row
// (Original / With Bleeps), footer status "Add at least one bleep range." and the Save Bleeped Copy button.
#include "tools/audio/audio_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace audio;

namespace {

class BleepAudioDialog : public AudioDialog {
 public:
  explicit BleepAudioDialog(const Selection& sel) { selection_ = sel; }

 protected:
  ui::DialogOptions Options() const override {
    ui::DialogOptions o; o.title = L"Bleep Audio"; o.widthDip = 515; o.heightDip = 515; o.rtl = RtlSetting();
    return o;
  }
  std::string ToolId() const override { return "bleep-audio"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject(); j["kind"] = kind_ == media::BleepKind::Tone ? "tone" : "silence"; j["toneHz"] = toneHz_; j["toneDb"] = toneDb_; return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("kind")) kind_ = j.Get("kind").AsString() == "silence" ? media::BleepKind::Silence : media::BleepKind::Tone;
    if (j.Has("toneHz")) toneHz_ = std::clamp(j.Get("toneHz").AsNumber(1000), 50.0, 8000.0);
    if (j.Has("toneDb")) toneDb_ = std::clamp(j.Get("toneDb").AsNumber(-12), -40.0, 0.0);
    if (j.Has("ranges")) {
      ranges_.clear();
      for (const Json& r : j.Get("ranges").Elements()) {
        if (r.IsArray() && r.Size() >= 2 && r.At(1).AsNumber() > r.At(0).AsNumber()) ranges_.push_back({r.At(0).AsNumber(), r.At(1).AsNumber()});
      }
    }
  }

  void Begin() override {
    LoadWave(1000);
  }
  void OnWaveLoaded() override {
    dur_ = wave_.wf.duration;
    for (auto& r : ranges_) { r.start = std::clamp(r.start, 0.0, dur_); r.end = std::clamp(r.end, 0.0, dur_); }
  }

  void TestPlay() override { Preview(true); }
  bool CanApply() const override { return !Busy() && !ranges_.empty() && waveError_.empty(); }

  void Apply() override {
    media::BleepRequest req = Request();
    const fs::path in = Path();
    StopPlayback();
    RunJob(L"Adding bleeps...", [in, req](JobContext& ctx) -> JobResult { return media::BleepAudio(in, req, ctx); });
  }

  void Draw(Ui& ui, const Rect& content, const Rect& body) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const bool en = !Locked();
    const float X = body.x, Y = body.y, W = body.w;
    auto M = [&](Rect r) { return Mir(ui, body, r); };
    TextStyle ts; ts.size = dialog::bodyFontSize;
    TextStyle tb = ts; tb.weight = 600;

    // ---- header: "Bleeps  N" (+ selection readout and Remove on the right)
    p.Text(M({X + kInset, Y + 64.f, 120.f, 22.f}), L"Bleeps", pal.heading, tb);
    const float bwid = p.TextWidth(L"Bleeps", tb);
    p.Text(M({X + kInset + bwid + 11.f, Y + 64.f, 40.f, 22.f}), std::to_wstring(ranges_.size()), Info(pal), ts);
    const bool haveSel = bs_.selected >= 0 && bs_.selected < (int)ranges_.size();
    ui.PushId(0xB1E0);
    if (haveSel) {
      const auto& g = ranges_[bs_.selected];
      TextStyle ms; ms.size = 12.5f; ms.mono = true; ms.align = Align::End;
      p.Text(M({body.right() - kInset - 86.f - 190.f, Y + 65.f, 190.f, 22.f}), FormatTime(g.start, true) + L" - " + FormatTime(g.end, true), Info(pal), ms);
      if (ui.Button(M({body.right() - kInset - 78.f, Y + 63.f, 78.f, 26.f}), L"Remove", ButtonStyle::Quiet, en)) { ranges_.erase(ranges_.begin() + bs_.selected); bs_.selected = -1; StopPlayback(); }
    }
    ui.PopId();

    // ---- lanes
    const float laneX = X + 86.f, laneR = body.right() - kInset;
    p.Text(M({X + kInset, Y + 95.f, 68.f, 35.f}), L"Bleep", pal.heading, tb);
    p.Text(M({X + kInset, Y + 140.f, 68.f, 81.5f}), L"Audio", pal.heading, tb);
    const Rect rl = M({laneX, Y + 95.f, laneR - laneX, 35.f}), al = M({laneX, Y + 140.f, laneR - laneX, 81.5f});
    double ph = playhead_;
    if (Playing()) ph = std::clamp(PlayPos(), 0.0, dur_);
    ui.PushId(0xB1E1);
    BleepEdit be = BleepLanes(ui, rl, al, wave_, dur_, ranges_, bs_, dur_ > 0 ? ph : 0.0, en && dur_ > 0);
    ui.PopId();
    if (be.changed) { if (Playing()) StopPlayback(); }
    if (be.seek >= 0) { playhead_ = be.seek; if (Playing()) StopPlayback(); }
    if (dur_ <= 0) {
      TextStyle ls; ls.size = 13.f; ls.align = Align::Center;
      const bool bad = !waveError_.empty();
      p.Text(al, bad ? waveError_ : L"Reading the waveform...", bad ? Color::Hex(0xC2410C) : Info(pal), ls);
    }
    // time scale (monospace labels, first one starts 10 dip inside the lane like the reference)
    if (dur_ > 0) {
      TextStyle ms; ms.size = 13.f; ms.mono = true;
      const int n = 4;
      for (int k = 0; k < n; ++k) {
        const float x = rl.x + rl.w * k / n + 10.f;
        p.Text({x, Y + 228.f, 70.f, 22.f}, FormatTime(dur_ * k / n, true), Info(pal), ms);
      }
    }

    // ---- hint
    TextStyle hs = ts; hs.wrap = true; hs.vcenter = false; hs.ellipsis = false;
    p.Text(M({X + kInner, Y + 270.5f, 395.f, 46.f}), L"Drag across the Bleep lane to add a range; drag a range edge to adjust it.", Info(pal), hs);
    p.FillRect({X + kInner, Y + 327.f, W - 2 * kInner, 1.f}, pal.divider);

    // ---- preview row
    p.Text(M({X + kInner, Y + 343.f, 120.f, 26.f}), L"Preview", Info(pal), ts);
    const bool canPrev = en && dur_ > 0 && !Rendering();
    ui.PushId(0xB1E2);
    const bool po = PlayingTag(L"orig"), pb = PlayingTag(L"bleeps");
    const float rEdge = body.right() - kInner;
    if (GlyphButton(ui, M({rEdge - 129.5f, Y + 343.5f, 129.5f, kBtnH}), pb ? L"Stop" : L"With Bleeps", pb ? "stop" : "play", canPrev || pb)) {
      if (pb) StopPlayback(); else Preview(true);
    }
    if (GlyphButton(ui, M({rEdge - 129.5f - 16.f - 78.f, Y + 343.5f, 78.f, kBtnH}), po ? L"Stop" : L"Original", "", canPrev || po)) {
      if (po) StopPlayback(); else Preview(false);
    }
    ui.PopId();

    // ---- bleep sound
    p.Text(M({X + kInner, Y + 379.f, 140.f, 26.f}), L"Bleep sound", Info(pal), ts);
    ui.PushId(0xB1E3);
    int sk = ui.Segmented(M({rEdge - 150.f, Y + 379.f, 150.f, kBtnH}), {L"Tone", L"Silence"}, kind_ == media::BleepKind::Tone ? 0 : 1, en);
    ui.PopId();
    if (sk != (kind_ == media::BleepKind::Tone ? 0 : 1)) { kind_ = sk == 0 ? media::BleepKind::Tone : media::BleepKind::Silence; if (Playing()) StopPlayback(); }

    // ---- footer
    std::wstring status; bool err = false;
    if (!notice_.empty() || Rendering()) { status = Rendering() ? L"Rendering the preview..." : notice_; err = !Rendering(); }
    else if (ranges_.empty()) status = L"Add at least one bleep range.";
    else status = Summary();
    if (Footer(ui, body, status, err, L"Save Bleeped Copy", CanApply())) Apply();
    DrawJobPanel(ui, content);
  }

 private:
  std::vector<media::BleepRange> ranges_;
  BleepState bs_;
  media::BleepKind kind_ = media::BleepKind::Tone;
  double toneHz_ = 1000, toneDb_ = -12, dur_ = 0, playhead_ = 0;

  media::BleepRequest Request() const {
    media::BleepRequest r;
    r.ranges = ranges_; r.kind = kind_; r.toneHz = toneHz_; r.toneLevelDb = toneDb_;
    r.preserveMetadata = SettingsStore::Get().Snapshot().preserveMetadata;
    return r;
  }

  std::wstring Summary() const {
    // merged length of all ranges
    std::vector<media::BleepRange> v = ranges_;
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.start < b.start; });
    double total = 0, cur = -1, curEnd = -1;
    for (auto& r : v) { if (r.start > curEnd) { if (curEnd > cur) total += curEnd - cur; cur = r.start; curEnd = r.end; } else curEnd = std::max(curEnd, r.end); }
    if (curEnd > cur) total += curEnd - cur;
    wchar_t b[160];
    swprintf_s(b, L"%d bleep range%s, %.2f s %s", (int)ranges_.size(), ranges_.size() == 1 ? L"" : L"s", total, kind_ == media::BleepKind::Tone ? L"replaced by a tone" : L"muted");
    return b;
  }

  // Renders from the playhead (at most two minutes) with or without the bleeps, then plays it.
  void Preview(bool withBleeps) {
    const double t0 = std::clamp(playhead_, 0.0, std::max(0.0, dur_ - 0.2));
    const double len = std::min(120.0, std::max(0.2, dur_ - t0));
    media::BleepRequest req = Request();
    if (!withBleeps) req.ranges.clear();
    const fs::path in = Path();
    RenderAndPlay(withBleeps ? L"bleeps" : L"orig", t0, [in, req, t0, len](JobContext& ctx, const fs::path& wav) { return media::RenderBleepPreview(in, req, t0, len, wav, ctx); });
  }
};

} // namespace

ToolDialog* MakeBleepAudio(const Selection& sel) { return new BleepAudioDialog(sel); }

} // namespace filo::tools
