// Normalize Volume - EBU R128 loudness normalisation. Shows the measured loudness of the input, a target (presets or custom), the
// true-peak limit and a comparison of the input with the predicted (and after running, the re-measured) result.
#include "tools/audio/audio_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace audio;

namespace {

struct Preset { double lufs; const wchar_t* hint; };
const Preset kPresets[3] = {
  {-14, L"Streaming (Spotify, YouTube, Apple Music)"},
  {-16, L"Podcasts and general listening"},
  {-23, L"Broadcast television and radio (EBU R128)"},
};

class NormalizeDialog : public AudioDialog {
 public:
  explicit NormalizeDialog(const Selection& sel) { selection_ = sel; res_ = std::make_shared<media::NormalizeResult>(); }

 protected:
  ui::DialogOptions Options() const override {
    ui::DialogOptions o; o.title = L"Normalize Volume"; o.widthDip = 515; o.heightDip = 640; o.rtl = RtlSetting();
    return o;
  }
  std::string ToolId() const override { return "normalize-volume"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject(); j["target"] = target_; j["truePeak"] = (double)tp_; j["lra"] = (double)lra_; return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("target")) SetTarget(std::clamp(j.Get("target").AsNumber(-16), -50.0, -5.0));
    if (j.Has("truePeak")) tp_ = (float)std::clamp(j.Get("truePeak").AsNumber(-1.5), -9.0, 0.0);
    if (j.Has("lra")) lra_ = (float)std::clamp(j.Get("lra").AsNumber(11), 1.0, 20.0);
  }

  void Begin() override {
    measuring_ = true;
    const fs::path p = Path();
    RunAsync([this, p]() -> std::function<void()> {
      media::MediaSummary ms = media::SummarizeMedia(p);
      std::wstring err;
      media::LoudnessInfo li = media::MeasureLoudness(p, nullptr, &err);
      return [this, ms, li, err]() {
        measuring_ = false; info_ = ms; infoReady_ = true;
        if (li.valid) in_ = li; else measureError_ = err.empty() ? L"The loudness could not be measured." : err;
      };
    });
  }

  bool CanApply() const override { return !Busy() && !measuring_; }
  void TestApply() override { if (!Busy()) Apply(); }      // headless runs do not wait for the measurement (the job measures itself)

  void Apply() override {
    media::NormalizeRequest req;
    req.targetLufs = target_; req.truePeakDb = tp_; req.lra = lra_;
    req.preserveMetadata = SettingsStore::Get().Snapshot().preserveMetadata;
    const fs::path in = Path();
    auto res = res_; *res = media::NormalizeResult{};
    RunJob(L"Normalizing the volume...", [in, req, res](JobContext& ctx) -> JobResult {
      media::NormalizeResult r = media::NormalizeVolume(in, req, ctx);
      *res = r;
      return r;
    });
  }

  void Draw(Ui& ui, const Rect& content, const Rect& body) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const bool en = !Locked();
    const float X = body.x, Y = body.y, W = body.w;
    const float aw = W - 2 * kInset;
    auto M = [&](Rect r) { return Mir(ui, body, r); };
    TextStyle ts; ts.size = dialog::bodyFontSize;

    // ---- file card
    {
      const Rect card = M({X + kInset, Y + 64.f, aw, 48.f});
      p.FillRoundRect(card, 14.f, pal.fieldFill);
      p.StrokeRoundRect(card, 14.f, pal.divider, 1.f);
      const Rect tile = M({X + kInset + 10.f, Y + 72.f, 32.f, 32.f});
      p.FillRoundRect(tile, 9.f, accent::base.WithAlpha(0.16f));
      p.Icon(infoReady_ && info_.hasVideo ? "video" : "audio", tile.cx(), tile.cy(), 18.f, accent::base, 1.7f);
      TextStyle nm; nm.size = 13.5f; nm.weight = 600;
      p.Text(M({X + kInset + 54.f, Y + 67.f, aw - 64.f, 22.f}), NameOf(Path()), pal.text, nm);
      TextStyle sm; sm.size = 12.f;
      std::wstring line = infoReady_ ? (info_.ok ? info_.Describe() : info_.error) : L"Reading the file...";
      p.Text(M({X + kInset + 54.f, Y + 88.f, aw - 64.f, 20.f}), line, pal.textSecondary, sm);
    }

    // ---- measured loudness
    ui.SectionTitle(M({X + kInset, Y + 124.f, aw, 22.f}), L"Measured loudness");
    {
      const float tw = (aw - 20.f) / 3.f;
      const struct { const wchar_t* label; const wchar_t* unit; double v; } t[3] = {
        {L"Integrated", L"LUFS", in_.integratedLufs}, {L"Loudness range", L"LU", in_.lra}, {L"True peak", L"dBTP", in_.truePeakDb}};
      for (int i = 0; i < 3; ++i) {
        Rect r = M({X + kInset + i * (tw + 10.f), Y + 150.f, tw, 58.f});
        p.FillRoundRect(r, 14.f, pal.fieldFill);
        p.StrokeRoundRect(r, 14.f, pal.divider, 1.f);
        TextStyle ls; ls.size = 12.f;
        p.Text({r.x + 14.f, r.y + 6.f, r.w - 20.f, 18.f}, t[i].label, pal.textSecondary, ls);
        TextStyle vs; vs.size = 22.f; vs.weight = 600;
        TextStyle us; us.size = 12.f;
        if (in_.valid) {
          wchar_t b[32]; swprintf_s(b, L"%.1f", t[i].v);
          const float w = p.TextWidth(b, vs);
          p.Text({r.x + 14.f, r.y + 24.f, w + 4.f, 30.f}, b, pal.text, vs);
          p.Text({r.x + 14.f + w + 5.f, r.y + 30.f, 60.f, 22.f}, t[i].unit, pal.textSecondary, us);
        } else {
          p.Text({r.x + 14.f, r.y + 24.f, r.w - 20.f, 30.f}, measuring_ ? L"Measuring..." : L"-", measuring_ ? pal.textTertiary : pal.textSecondary, measuring_ ? us : vs);
        }
      }
      if (!measureError_.empty()) {
        TextStyle es; es.size = 12.f; es.ellipsis = true;
        p.Text(M({X + kInset, Y + 209.f, aw, 18.f}), measureError_, Color::Hex(0xC2410C), es);
      }
    }

    // ---- target
    ui.SectionTitle(M({X + kInset, Y + 226.f, aw, 22.f}), L"Target loudness");
    ui.PushId(0x4041);
    int sel = ui.Segmented(M({X + kInset, Y + 252.f, aw, kBtnH}), {L"-14 LUFS", L"-16 LUFS", L"-23 LUFS", L"Custom"}, presetIdx_, en);
    ui.PopId();
    if (sel != presetIdx_) { presetIdx_ = sel; if (sel < 3) target_ = kPresets[sel].lufs; }
    if (presetIdx_ < 3) {
      TextStyle hs; hs.size = 12.5f;
      p.Text(M({X + kInset + 2.f, Y + 282.f, aw, 24.f}), kPresets[presetIdx_].hint, pal.textSecondary, hs);
    } else {
      p.Text(M({X + kInset + 2.f, Y + 281.f, 130.f, 28.f}), L"Target", pal.text, ts);
      double v = target_;
      ui.PushId(0x4042);
      if (ui.Stepper(M({X + kInset + 120.f, Y + 282.f, 150.f, kBtnH}), v, -50, -5, 0.5, 1, L"LUFS", en)) target_ = v;
      ui.PopId();
    }
    ui.PushId(0x4043);
    ui.SliderRow(M({X + kInset, Y + 316.f, aw, 30.f}), L"True peak limit", tp_, -9.f, 0.f, 0.1f, -1.5f, [](float v) { wchar_t b[24]; swprintf_s(b, L"%.1f dB", v); return std::wstring(b); }, en);
    ui.SliderRow(M({X + kInset, Y + 346.f, aw, 30.f}), L"Loudness range", lra_, 1.f, 20.f, 0.5f, 11.f, [](float v) { wchar_t b[24]; swprintf_s(b, L"%.1f LU", v); return std::wstring(b); }, en);
    ui.PopId();

    // ---- comparison
    DrawComparison(ui, body, Y + 392.f);

    if (Footer(ui, body, L"", false, L"Normalize", CanApply())) Apply();
    DrawJobPanel(ui, content);
  }

 private:
  double target_ = -16;
  int presetIdx_ = 1;
  float tp_ = -1.5f, lra_ = 11.f;
  bool measuring_ = false, infoReady_ = false;
  media::LoudnessInfo in_;
  media::MediaSummary info_;
  std::wstring measureError_;
  std::shared_ptr<media::NormalizeResult> res_;

  void SetTarget(double v) {
    target_ = v; presetIdx_ = 3;
    for (int i = 0; i < 3; ++i) if (std::fabs(kPresets[i].lufs - v) < 0.01) presetIdx_ = i;
  }

  void DrawComparison(Ui& ui, const Rect& body, float y) {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const float X = body.x, W = body.w, aw = W - 2 * kInset;
    auto M = [&](Rect r) { return Mir(ui, body, r); };
    const bool done = Phase() == JobPhase::Done && res_ && res_->output.valid;
    ui.SectionTitle(M({X + kInset, y, aw, 22.f}), done ? L"Input and result" : L"Input and predicted result");
    const float colIn = 130.f, colOut = 150.f;       // right-aligned numeric columns
    TextStyle hs; hs.size = 12.f; hs.align = Align::End;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    TextStyle vs = ls; vs.align = Align::End;
    const float hy = y + 28.f;
    p.Text(M({X + W - kInset - 14.f - colOut - colIn, hy, colIn, 20.f}), L"Input", pal.textSecondary, hs);
    p.Text(M({X + W - kInset - 14.f - colOut, hy, colOut, 20.f}), done ? L"Output (measured)" : L"Predicted", pal.textSecondary, hs);
    p.FillRect({X + kInset, hy + 22.f, aw, 1.f}, pal.divider);

    const media::LoudnessInfo& i0 = in_;
    const media::LoudnessInfo* o0 = done ? &res_->output : nullptr;
    const double gain = i0.valid ? target_ - i0.integratedLufs : 0;
    const double predTp = i0.valid ? i0.truePeakDb + gain : 0;
    auto fmt = [](double v, const wchar_t* unit) { wchar_t b[40]; swprintf_s(b, L"%.1f %s", v, unit); return std::wstring(b); };
    struct Row { const wchar_t* label; std::wstring in, out; };
    std::vector<Row> rows;
    rows.push_back({L"Integrated loudness", i0.valid ? fmt(i0.integratedLufs, L"LUFS") : L"-", o0 ? fmt(o0->integratedLufs, L"LUFS") : (i0.valid ? fmt(target_, L"LUFS") : L"-")});
    rows.push_back({L"True peak", i0.valid ? fmt(i0.truePeakDb, L"dBTP") : L"-", o0 ? fmt(o0->truePeakDb, L"dBTP") : (i0.valid ? fmt(std::min(predTp, (double)tp_), L"dBTP") : L"-")});
    rows.push_back({L"Loudness range", i0.valid ? fmt(i0.lra, L"LU") : L"-", o0 ? fmt(o0->lra, L"LU") : (i0.valid ? fmt(std::min(i0.lra, (double)lra_), L"LU") : L"-")});
    std::wstring gtxt = L"-";
    if (done) { wchar_t b[40]; swprintf_s(b, L"%+.1f dB%s", res_->gainDb, res_->linear ? L"" : L" (dynamic)"); gtxt = b; }
    else if (i0.valid) { wchar_t b[40]; swprintf_s(b, L"%+.1f dB", gain); gtxt = b; }
    rows.push_back({L"Gain change", L"", gtxt});
    float ry = hy + 27.f;
    for (auto& r : rows) {
      p.Text(M({X + kInset + 2.f, ry, 200.f, 24.f}), r.label, pal.text, ls);
      p.Text(M({X + W - kInset - 14.f - colOut - colIn, ry, colIn, 24.f}), r.in, pal.textSecondary, vs);
      p.Text(M({X + W - kInset - 14.f - colOut, ry, colOut, 24.f}), r.out, pal.text, vs);
      ry += 25.f;
    }
    if (!done && i0.valid && predTp > tp_ + 0.05) {
      TextStyle ns; ns.size = 12.f; ns.ellipsis = true;
      p.Text(M({X + kInset + 2.f, ry + 1.f, aw, 18.f}), L"Peaks would pass the limit: the limiter and dynamic mode will keep them below it.", Info(pal), ns);
    }
  }
};

} // namespace

ToolDialog* MakeNormalizeVolume(const Selection& sel) { return new NormalizeDialog(sel); }

} // namespace filo::tools
