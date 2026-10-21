// Trim Video: preview + timeline with start/end handles, frame stepping, exact (re-encode) or fast keyframe cut.
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

class TrimVideoDialog : public VideoToolDialog {
 public:
  explicit TrimVideoDialog(const Selection& sel) : VideoToolDialog(sel) {
    a_ = 0; b_ = Dur();
    result_ = std::make_shared<media::TrimResult>();
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Trim Video"; o.widthDip = 648; o.heightDip = 720; o.resizable = true; o.minWidthDip = 540; o.minHeightDip = 600;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "trim-video"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["fast"] = fast_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("fast")) fast_ = j.Get("fast").AsBool();
    const double d = Dur();
    if (j.Has("start")) a_ = std::clamp(j.Get("start").AsNumber(), 0.0, std::max(0.0, d - 0.05));
    if (j.Has("end")) { const double e = j.Get("end").AsNumber(); if (e > a_ + 0.04) b_ = std::min(e, d); }
  }
  void TestApply() override { MaybeSelfTest(); StartExport(); }
  bool AcceptsFileDrop() const override { return false; }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float below = 10 + 34 + 4 + 44 + 4 + 30 + 12 + 26 + 46 + 8;
    const float wellH = std::max(170.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    SinkOptions so; so.end = b_; so.from = a_;
    DrawWellSink(ui, g, so);

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, b_, a_);
    y += 34 + 4;

    // ---- timeline with the two trim handles
    {
      Rect tl{well.x + 10, y, well.w - 20, 44};
      double pos = view_.IsOpen() ? view_.Pos() : 0.0;
      const double a0 = a_, b0 = b_;
      TimelineResult r = Timeline(ui, tl, Dur(), pos, &a_, &b_, nullptr, nullptr, 1.0 / Fps());
      if (r.rangeChanged) {
        const double fr = 1.0 / Fps();
        a_ = std::round(a_ / fr) * fr;
        b_ = b_ >= Dur() - 0.5 * fr ? Dur() : std::round(b_ / fr) * fr;
        if (b_ - a_ < fr) { a_ = a0; b_ = b0; }
        if (view_.IsOpen()) { if (view_.Playing()) view_.Pause(); view_.Seek(r.handle == 1 ? a_ : std::max(a_, b_ - fr), true); }
      } else if (r.seeked && view_.IsOpen()) view_.Seek(pos, r.scrubbing);
    }
    y += 44 + 4;

    // ---- start / duration / end readout with "set to playhead" buttons
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(40), Fixed(78), Fixed(46), Flex(1), Fixed(46), Fixed(34), Fixed(78)}, 6);
      TextStyle lab; lab.size = 13; lab.align = Align::Start;
      TextStyle labEnd = lab; labEnd.align = Align::End;
      const double pos = view_.IsOpen() ? view_.Pos() : 0.0;
      const double fr = 1.0 / Fps();
      p.Text(c[0], L"Start", pal.textSecondary, lab);
      p.Text(c[1], FormatTime(a_, true), pal.text, MonoStyle(14.5f, 600));
      ui.PushId(0x7101);
      if (ui.Button({c[2].x, row.cy() - 12, c[2].w, 24}, L"Set", ButtonStyle::Secondary, view_.IsOpen() && pos < b_ - fr)) a_ = std::round(pos / fr) * fr;
      ui.PopId();
      TextStyle mid; mid.size = 13; mid.align = Align::Center; mid.weight = 500;
      p.Text(c[3], FormatTime(std::max(0.0, b_ - a_), true) + L" selected", pal.textSecondary, mid);
      ui.PushId(0x7102);
      if (ui.Button({c[4].x, row.cy() - 12, c[4].w, 24}, L"Set", ButtonStyle::Secondary, view_.IsOpen() && pos > a_ + fr)) b_ = pos >= Dur() - 0.5 * fr ? Dur() : std::round(pos / fr) * fr;
      ui.PopId();
      p.Text(c[5], L"End", pal.textSecondary, labEnd);
      p.Text(c[6], FormatTime(b_, true), pal.text, MonoStyle(14.5f, 600, Align::End));
    }
    y += 30 + 12;

    // ---- exact vs fast cut
    ui.Checkbox({well.x, y, 260, 26}, L"Fast cut (keyframes)", fast_, !Busy());
    y += 26;
    TextStyle ex; ex.size = 12.5f; ex.wrap = true; ex.ellipsis = false; ex.vcenter = false;
    p.Text({well.x + 24, y + 2, well.w - 24, 42},
           fast_ ? L"Instant and lossless: the video is copied, not re-encoded. A cut can only begin on a keyframe, so the clip may start a little before your start point."
                 : L"Exact cut: the video is re-encoded so the clip begins and ends on the frames you picked. Slower, but frame accurate.",
           pal.textSecondary, ex);

    // ---- footer
    FooterInfo fi;
    fi.action = L"Export";
    const bool valid = Usable() && b_ - a_ >= 0.04;
    fi.actionEnabled = valid && !Busy();
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else fi.status = FormatTime(a_) + L" - " + FormatTime(b_) + L"  (" + FormatTime(b_ - a_) + L")  -  " + (fast_ ? L"fast cut" : L"exact cut");
    const bool enter = ConsumeEnter();
    FooterResult fr = DrawFooter(ui, content, fi);
    if ((fr.action || enter) && fi.actionEnabled) StartExport();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && result_->ok) {
      std::vector<std::wstring> lines;
      lines.push_back(L"Saved range " + FormatTime(result_->actualStart) + L" - " + FormatTime(result_->actualEnd) + L"  (" + FormatTime(result_->outputDuration > 0 ? result_->outputDuration : result_->actualEnd - result_->actualStart) + L")");
      lines.push_back(result_->snappedToKeyframes ? L"Fast cut started at the previous keyframe (you asked for " + FormatTime(result_->requestedStart) + L")." : (fast_ ? L"Copied without re-encoding." : L"Frame-accurate cut."));
      DrawDoneNote(ui, content, lines);
    }
  }

 private:
  void StartExport() {
    media::TrimVideoRequest req;
    const double d = Dur();
    req.start = a_;
    req.end = b_ >= d - 0.001 ? 0 : b_;
    req.copyMode = fast_;
    Settings st = SettingsStore::Get().Snapshot();
    req.hardware = st.hardwareAcceleration; req.preserveMetadata = st.preserveMetadata; req.crf = 0;
    auto res = result_ = std::make_shared<media::TrimResult>();
    const fs::path in = path_;
    RunJob(L"Trimming video...", [in, req, res](JobContext& ctx) -> JobResult {
      *res = media::TrimVideo(in, req, ctx);
      return static_cast<JobResult>(*res);
    });
  }

  double a_ = 0, b_ = 0;
  bool fast_ = false;
  std::shared_ptr<media::TrimResult> result_;
};

} // namespace

ToolDialog* MakeTrimVideo(const Selection& sel) { return new TrimVideoDialog(sel); }

} // namespace filo::tools
