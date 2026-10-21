// Change Speed: multiplier slider + presets, resulting length preview, pitch option and a live preview at the chosen speed.
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

constexpr double kPresets[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
constexpr int kNumPresets = 8;

std::wstring FmtSpeed(double f) {
  wchar_t b[32];
  if (std::fabs(f - std::round(f)) < 0.005) swprintf_s(b, L"%d\x00D7", (int)std::lround(f));
  else if (std::fabs(f * 10 - std::round(f * 10)) < 0.005) swprintf_s(b, L"%.1f\x00D7", f);
  else swprintf_s(b, L"%.2f\x00D7", f);
  return b;
}

class ChangeSpeedDialog : public VideoToolDialog {
 public:
  explicit ChangeSpeedDialog(const Selection& sel) : VideoToolDialog(sel) { SetFactor(2.0); result_ = std::make_shared<media::SpeedResult>(); }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Change Speed"; o.widthDip = 648; o.heightDip = 780; o.resizable = true; o.minWidthDip = 560; o.minHeightDip = 660;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "change-speed"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["factor"] = f_; j["preservePitch"] = preservePitch_; j["smooth"] = smooth_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("factor")) SetFactor(std::clamp(j.Get("factor").AsNumber(2.0), 0.05, 100.0));
    if (j.Has("preservePitch")) preservePitch_ = j.Get("preservePitch").AsBool(true);
    if (j.Has("smooth")) smooth_ = j.Get("smooth").AsBool(false);
  }
  void TestApply() override { StartExport(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float below = 10 + 34 + 2 + 34 + 8 + 46 + 4 + 34 + 6 + 28 + 12 + 60 + 12 + 30 + 4 + 30 + 8;
    const float wellH = std::max(150.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    SinkOptions so;
    DrawWellSink(ui, g, so);

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, -1, 0);
    y += 34 + 2;
    DrawScrubber(ui, {well.x + 10, y, well.w - 20, 34});
    y += 34 + 8;
    const bool en = Usable() && !Busy();

    // ---- big readout with slower / faster
    {
      Rect row{well.x, y, well.w, 46};
      auto c = ui.Cols(row, {Flex(1), Fixed(100), Fixed(150), Fixed(100), Flex(1)}, 10);
      ui.PushId(0x7E01);
      if (ui.Button({c[1].x, row.cy() - 15, c[1].w, 30}, L"Slower", ButtonStyle::Secondary, en && f_ > kPresets[0] + 1e-6, "chevronleft")) SetFactor(StepPreset(-1));
      ui.PopId();
      TextStyle big; big.size = 30; big.weight = 700; big.align = Align::Center;
      p.Text(c[2], FmtSpeed(f_), pal.heading, big);
      ui.PushId(0x7E02);
      if (ui.Button({c[3].x, row.cy() - 15, c[3].w, 30}, L"Faster", ButtonStyle::Secondary, en && f_ < kPresets[kNumPresets - 1] - 1e-6, "chevronright")) SetFactor(StepPreset(1));
      ui.PopId();
    }
    y += 46 + 4;

    // ---- multiplier slider (logarithmic: 0.25x .. 4x with 1x in the middle)
    {
      float t = (float)std::log2(std::clamp(f_, kPresets[0], kPresets[kNumPresets - 1]));
      if (ui.Slider({well.x, y, well.w, 34}, t, -2.f, 2.f, 0.f, 0.f, en)) {
        double f = std::pow(2.0, (double)t);
        for (double pr : kPresets) if (std::fabs(f / pr - 1.0) < 0.035) f = pr;
        SetFactor(f);
      }
    }
    y += 34 + 6;

    // ---- presets
    {
      std::vector<std::wstring> labels;
      int sel = -1;
      for (int i = 0; i < kNumPresets; ++i) { labels.push_back(FmtSpeed(kPresets[i])); if (std::fabs(f_ - kPresets[i]) < 1e-6) sel = i; }
      const int ns = ui.Segmented({well.x, y, well.w, 28}, labels, sel, en);
      if (ns != sel && ns >= 0) SetFactor(kPresets[ns]);
    }
    y += 28 + 12;

    // ---- resulting duration
    {
      Rect card{well.x, y, well.w, 60};
      p.FillRoundRect(card, control::listRowRadius, pal.fieldFill);
      p.StrokeRoundRect(card, control::listRowRadius, pal.divider, 1.f);
      const double d = Dur(), nd = media::SpeedDurationPreview(d, f_);
      auto c = ui.Cols(card.Inset(18, 0), {Flex(1), Fixed(30), Flex(1), Fixed(130)}, 8);
      TextStyle lab; lab.size = 12; lab.align = Align::Start;
      p.Text({c[0].x, card.y + 9, c[0].w, 16}, L"Original length", pal.textTertiary, lab);
      p.Text({c[0].x, card.y + 26, c[0].w, 26}, FormatTime(d, true), pal.text, MonoStyle(17.f, 600));
      {
        const float ax = c[1].cx(), ay = card.cy() + 5, dir = ui.Rtl() ? -1.f : 1.f;
        p.Line(ax - 9 * dir, ay, ax + 9 * dir, ay, pal.textTertiary, 1.8f, true);
        p.Line(ax + 9 * dir, ay, ax + 4 * dir, ay - 5, pal.textTertiary, 1.8f, true);
        p.Line(ax + 9 * dir, ay, ax + 4 * dir, ay + 5, pal.textTertiary, 1.8f, true);
      }
      p.Text({c[2].x, card.y + 9, c[2].w, 16}, L"New length", pal.textTertiary, lab);
      p.Text({c[2].x, card.y + 26, c[2].w, 26}, FormatTime(nd, true), accent::base, MonoStyle(17.f, 700));
      TextStyle pc; pc.size = 12.5f; pc.align = Align::End;
      wchar_t b[64]; swprintf_s(b, L"%.0f%% of the original", 100.0 / std::max(f_, 1e-6));
      p.Text({c[3].x, card.y, c[3].w, card.h}, b, pal.textSecondary, pc);
    }
    y += 60 + 12;

    // ---- options
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(44), Flex(1)}, 10);
      ui.PushId(0x7E10);
      ui.Toggle(c[0], preservePitch_, en && info_.hasAudio);
      ui.PopId();
      TextStyle ls; ls.size = dialog::bodyFontSize;
      const bool hasAudio = info_.hasAudio;
      p.Text(c[1], hasAudio ? L"Keep the audio pitch" : L"Keep the audio pitch (this video has no audio)", hasAudio ? pal.text : pal.textDisabled, ls);
    }
    y += 30 + 4;
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(44), Flex(1)}, 10);
      const bool slow = f_ < 1.0 - 1e-6;
      ui.PushId(0x7E11);
      ui.Toggle(c[0], smooth_, en && slow);
      ui.PopId();
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[1], L"Smooth slow motion (blends frames, slower to export)", slow ? pal.text : pal.textDisabled, ls);
    }

    // ---- footer
    FooterInfo fi;
    fi.action = L"Change Speed";
    fi.actionEnabled = en && info_.ok;
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else fi.status = FmtSpeed(f_) + L"  -  " + FormatTime(Dur(), false) + L" to " + FormatTime(media::SpeedDurationPreview(Dur(), f_), false) +
                     (info_.hasAudio ? (preservePitch_ ? L"  -  pitch kept" : L"  -  pitch follows speed") : L"  -  no audio");
    const bool enter = ConsumeEnter();
    FooterResult fr = DrawFooter(ui, content, fi);
    if ((fr.action || enter) && fi.actionEnabled) StartExport();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && result_->ok)
      DrawDoneNote(ui, content, {L"New length " + FormatTime(result_->outputDuration, true) + L" at " + FmtSpeed(f_),
                                 L"Expected " + FormatTime(result_->expectedDuration, true) + L" (" + FormatTime(Dur(), true) + L" original)"});
  }

 private:
  void SetFactor(double f) { f_ = f; ApplySpeedToView(std::clamp(f, 0.1, 16.0)); }
  double StepPreset(int dir) const {
    if (dir < 0) { for (int i = kNumPresets - 1; i >= 0; --i) if (kPresets[i] < f_ - 1e-6) return kPresets[i]; return kPresets[0]; }
    for (int i = 0; i < kNumPresets; ++i) if (kPresets[i] > f_ + 1e-6) return kPresets[i];
    return kPresets[kNumPresets - 1];
  }

  void StartExport() {
    media::SpeedRequest req;
    req.factor = f_;
    req.changePitch = info_.hasAudio && !preservePitch_;
    req.interpolate = smooth_ && f_ < 1.0;
    Settings st = SettingsStore::Get().Snapshot();
    req.hardware = st.hardwareAcceleration; req.preserveMetadata = st.preserveMetadata; req.crf = 0;
    auto res = result_ = std::make_shared<media::SpeedResult>();
    const fs::path in = path_;
    RunJob(L"Changing the speed...", [in, req, res](JobContext& ctx) -> JobResult {
      *res = media::ChangeSpeed(in, req, ctx);
      return static_cast<JobResult>(*res);
    });
  }

  double f_ = 2.0;
  bool preservePitch_ = true, smooth_ = false;
  std::shared_ptr<media::SpeedResult> result_;
};

} // namespace

ToolDialog* MakeChangeSpeed(const Selection& sel) { return new ChangeSpeedDialog(sel); }

} // namespace filo::tools
