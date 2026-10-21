// Split Video: markers on a timeline (add at the playhead, select, remove), equal parts or custom segments.
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

class SplitVideoDialog : public VideoToolDialog {
 public:
  explicit SplitVideoDialog(const Selection& sel) : VideoToolDialog(sel) {
    a_ = 0; b_ = Dur();
    result_ = std::make_shared<media::SplitResult>();
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Split Video"; o.widthDip = 648; o.heightDip = 780; o.resizable = true; o.minWidthDip = 560; o.minHeightDip = 660;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "split-video"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["copy"] = copy_; j["mode"] = mode_; j["parts"] = parts_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("copy")) copy_ = j.Get("copy").AsBool(false);
    if (j.Has("mode")) mode_ = std::clamp((int)j.Get("mode").AsInt(0), 0, 2);
    if (j.Has("parts")) parts_ = std::clamp((int)j.Get("parts").AsInt(3), 2, 50);
    if (j.Has("markers")) { markers_.clear(); for (const auto& m : j.Get("markers").Elements()) markers_.push_back(m.AsNumber()); std::sort(markers_.begin(), markers_.end()); }
    if (j.Has("segments")) { segs_.clear(); for (const auto& s : j.Get("segments").Elements()) segs_.push_back({s.Get("start").AsNumber(), s.Get("end").AsNumber()}); }
  }
  void TestApply() override { StartSplit(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float listH = 118;
    const float below = 10 + 34 + 2 + 46 + 8 + 28 + 8 + 30 + 6 + listH + 8 + 28 + 2 + 20 + 8;
    const float wellH = std::max(140.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    SinkOptions so;
    DrawWellSink(ui, g, so);
    const bool en = Usable() && !Busy();
    const double dur = Dur(), fr = 1.0 / Fps();

    // keyboard: M = add marker, Delete = remove the selected one
    if (en && SinkFocused(ui) && mode_ == 0) {
      for (const auto& k : ui.In().keys) { if (k.vk == 'M') AddMarker(); else if (k.vk == VK_DELETE) RemoveMarker(); }
    }

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, -1, 0);
    y += 34 + 2;

    // ---- timeline
    std::vector<double> shown = ShownMarkers();
    {
      Rect tl{well.x + 10, y, well.w - 20, 46};
      double pos = view_.IsOpen() ? view_.Pos() : 0.0;
      TimelineResult r;
      if (mode_ == 2) r = Timeline(ui, tl, dur, pos, &a_, &b_, nullptr, nullptr, fr);
      else r = Timeline(ui, tl, dur, pos, nullptr, nullptr, &shown, nullptr, fr);
      if (r.rangeChanged) {
        a_ = std::round(a_ / fr) * fr; b_ = b_ >= dur - 0.5 * fr ? dur : std::round(b_ / fr) * fr;
        if (view_.IsOpen()) { if (view_.Playing()) view_.Pause(); view_.Seek(r.handle == 1 ? a_ : std::max(a_, b_ - fr), true); }
      } else if (r.markerClicked >= 0 && mode_ == 0) {
        selMarker_ = r.markerClicked;
        if (view_.IsOpen()) view_.Seek(markers_[selMarker_], false);
      } else if (r.seeked && view_.IsOpen()) view_.Seek(pos, r.scrubbing);
      // highlight the selected marker; show the saved segments under the track
      if (mode_ == 0 && selMarker_ >= 0 && selMarker_ < (int)markers_.size()) {
        const float x = tl.x + (float)(markers_[selMarker_] / dur) * tl.w;
        p.FillRoundRect({x - 3.5f, tl.y + 3, 7, tl.h - 6}, 3.5f, Color(1, 1, 1, 0.95f));
        p.FillRoundRect({x - 1.5f, tl.y + 4, 3, tl.h - 8}, 1.5f, Color::Hex(0xFFB020));
      }
      if (mode_ == 2) for (size_t i = 0; i < segs_.size(); ++i) {
        const float x0 = tl.x + (float)(segs_[i].start / dur) * tl.w, x1 = tl.x + (float)(segs_[i].end / dur) * tl.w;
        p.FillRoundRect({x0, tl.bottom() - 7, std::max(3.f, x1 - x0), 5}, 2.5f, (int)i == selSeg_ ? Color::Hex(0xFFB020) : Color::Hex(0xFFB020, 0.60f));
      }
    }
    y += 46 + 8;

    // ---- mode
    {
      Rect row{well.x, y, well.w, 28};
      auto c = ui.Cols(row, {Fixed(64), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Split by", pal.text, ls);
      const int nm = ui.Segmented(c[1], {L"Markers", L"Equal parts", L"Custom segments"}, mode_, en);
      if (nm != mode_) { mode_ = nm; selMarker_ = -1; selSeg_ = -1; }
    }
    y += 28 + 8;

    // ---- mode specific controls
    const double pos = view_.IsOpen() ? view_.Pos() : 0.0;
    {
      Rect row{well.x, y, well.w, 30};
      TextStyle hs; hs.size = 12.5f; hs.align = Align::End;
      if (mode_ == 0) {
        auto c = ui.Cols(row, {Fixed(150), Fixed(130), Fixed(80), Flex(1)}, 8);
        if (ui.Button({c[0].x, row.cy() - 13, c[0].w, 26}, L"Add marker", ButtonStyle::Secondary, en, "add")) AddMarker();
        if (ui.Button({c[1].x, row.cy() - 13, c[1].w, 26}, L"Remove", ButtonStyle::Secondary, en && selMarker_ >= 0, "remove")) RemoveMarker();
        if (ui.Button({c[2].x, row.cy() - 13, c[2].w, 26}, L"Clear", ButtonStyle::Secondary, en && !markers_.empty())) { markers_.clear(); selMarker_ = -1; }
        p.Text(c[3], markers_.empty() ? L"Add markers at the playhead (M)" : std::to_wstring(markers_.size()) + (markers_.size() == 1 ? L" marker, " : L" markers, ") + std::to_wstring(markers_.size() + 1) + L" parts", pal.textTertiary, hs);
      } else if (mode_ == 1) {
        auto c = ui.Cols(row, {Fixed(120), Fixed(110), Flex(1)}, 10);
        TextStyle ls; ls.size = dialog::bodyFontSize;
        p.Text(c[0], L"Number of parts", pal.text, ls);
        double v = parts_;
        ui.PushId(0x7D01);
        if (ui.Stepper({c[1].x, row.cy() - 13, c[1].w, 26}, v, 2, 50, 1, 0, L"", en)) parts_ = std::clamp((int)std::lround(v), 2, 50);
        ui.PopId();
        p.Text(c[2], L"each " + FormatTime(dur / parts_, true), pal.textTertiary, hs);
      } else {
        auto c = ui.Cols(row, {Fixed(150), Fixed(130), Fixed(80), Flex(1)}, 8);
        const bool okSeg = b_ - a_ >= 0.1;
        if (ui.Button({c[0].x, row.cy() - 13, c[0].w, 26}, L"Add segment", ButtonStyle::Secondary, en && okSeg, "add")) { segs_.push_back({a_, b_}); selSeg_ = (int)segs_.size() - 1; }
        if (ui.Button({c[1].x, row.cy() - 13, c[1].w, 26}, L"Remove", ButtonStyle::Secondary, en && selSeg_ >= 0, "remove")) { segs_.erase(segs_.begin() + selSeg_); selSeg_ = -1; }
        if (ui.Button({c[2].x, row.cy() - 13, c[2].w, 26}, L"Clear", ButtonStyle::Secondary, en && !segs_.empty())) { segs_.clear(); selSeg_ = -1; }
        p.Text(c[3], L"Drag the blue handles, then add the segment", pal.textTertiary, hs);
      }
    }
    y += 30 + 6;
    (void)pos;

    // ---- resulting parts
    DrawParts(ui, {well.x - 4, y, well.w + 8, listH});
    y += listH + 8;

    // ---- copy mode + output folder
    {
      Rect row{well.x, y, well.w, 28};
      auto c = ui.Cols(row, {Fixed(44), Flex(1)}, 10);
      ui.PushId(0x7D10);
      ui.Toggle(c[0], copy_, en);
      ui.PopId();
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[1], L"Fast copy (no re-encoding; cuts snap to keyframes)", pal.text, ls);
    }
    y += 28 + 2;
    {
      TextStyle os; os.size = 12.5f;
      p.Text({well.x, y, well.w, 20}, L"Saves into  " + OutputFolderText(), pal.textTertiary, os);
    }

    // ---- footer
    FooterInfo fi;
    fi.action = L"Split";
    const std::vector<media::TimeRange> parts = Parts();
    fi.actionEnabled = en && info_.ok && !parts.empty();
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else if (parts.empty()) { fi.status = mode_ == 0 ? L"Add at least one marker." : L"Add at least one segment."; fi.statusIsError = true; }
    else fi.status = std::to_wstring(parts.size()) + (parts.size() == 1 ? L" file" : L" files") + (copy_ ? L"  -  fast copy" : L"  -  re-encoded, frame accurate");
    const bool enter = ConsumeEnter();
    FooterResult fres = DrawFooter(ui, content, fi);
    if ((fres.action || enter) && fi.actionEnabled) StartSplit();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && result_->ok) {
      std::vector<std::wstring> lines;
      double total = 0; for (auto& s : result_->segments) total += s.end - s.start;
      lines.push_back(std::to_wstring(result_->segments.size()) + L" parts, " + FormatTime(total, true) + L" in total");
      lines.push_back(L"Folder: " + result_->folder.filename().wstring());
      DrawDoneNote(ui, content, lines);
    }
  }

 private:
  std::vector<double> ShownMarkers() const {
    if (mode_ == 0) return markers_;
    std::vector<double> m;
    if (mode_ == 1) for (int i = 1; i < parts_; ++i) m.push_back(Dur() * i / parts_);
    return m;
  }

  std::vector<media::TimeRange> Parts() const {
    std::vector<media::TimeRange> out;
    const double d = Dur();
    if (mode_ == 0) {
      std::vector<double> m;
      for (double v : markers_) if (v > 0.04 && v < d - 0.04) m.push_back(v);
      if (m.empty()) return out;
      double prev = 0;
      for (double v : m) { out.push_back({prev, v}); prev = v; }
      out.push_back({prev, d});
    } else if (mode_ == 1) {
      for (int i = 0; i < parts_; ++i) out.push_back({d * i / parts_, d * (i + 1) / parts_});
    } else {
      out = segs_;
    }
    return out;
  }

  void AddMarker() {
    if (!view_.IsOpen()) return;
    const double fr = 1.0 / Fps(), d = Dur();
    const double t = std::round(view_.Pos() / fr) * fr;
    if (t < 0.05 || t > d - 0.05) return;
    for (double m : markers_) if (std::fabs(m - t) < 0.04) return;
    markers_.push_back(t);
    std::sort(markers_.begin(), markers_.end());
    selMarker_ = (int)(std::find(markers_.begin(), markers_.end(), t) - markers_.begin());
  }
  void RemoveMarker() {
    if (selMarker_ >= 0 && selMarker_ < (int)markers_.size()) { markers_.erase(markers_.begin() + selMarker_); selMarker_ = -1; }
  }

  std::wstring OutputFolderText() const {
    Settings st = SettingsStore::Get().Snapshot();
    fs::path dir = ResolveOutputDir(path_, st);
    return (dir / (path_.stem().wstring() + L" (split)")).wstring();
  }

  void DrawParts(Ui& ui, const Rect& vp) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    const std::vector<media::TimeRange> parts = Parts();
    const float rowH = 34, gap = 6;
    const float contentH = parts.size() * (rowH + gap);
    if (parts.empty()) {
      p.FillRoundRect(vp.Inset(4, 0), control::listRowRadius, Color(pal.text.r, pal.text.g, pal.text.b, 0.04f));
      TextStyle es; es.size = 13; es.align = Align::Center; es.wrap = true;
      p.Text(vp.Inset(24, 8), mode_ == 0 ? L"No parts yet. Markers cut the video into parts." : L"No segments yet. Pick a range and add it.", pal.textTertiary, es);
      return;
    }
    const float off = ui.BeginScroll(vp, contentH, ui.Id("parts"));
    for (int i = 0; i < (int)parts.size(); ++i) {
      Rect row{vp.x + 4, vp.y + i * (rowH + gap) - off, vp.w - 8 - (contentH > vp.h ? 8.f : 0.f), rowH};
      if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
      ui.PushId(i);
      const bool sel = mode_ == 2 && i == selSeg_;
      Interaction it = ui.Behavior(row, ui.Id("row"), !Busy(), false);
      const float hov = ui.Anim(ui.Id("hov"), it.hot ? 1.f : 0.f);
      p.FillRoundRect(row, 12, Mix(pal.fieldFill, accent::base.WithAlpha(sel ? 0.22f : 0.12f), std::max(hov, sel ? 1.f : 0.f)));
      p.StrokeRoundRect(row, 12, Mix(pal.divider, accent::base.WithAlpha(0.45f), std::max(hov, sel ? 1.f : 0.f)), 1.f);
      TextStyle ns; ns.size = 13; ns.weight = 700; ns.align = Align::Center;
      p.Text({row.x + 6, row.y, 28, row.h}, std::to_wstring(i + 1), pal.textSecondary, ns);
      const double len = parts[i].end - parts[i].start;
      p.Text({row.x + 40, row.y, 220, row.h}, FormatTime(parts[i].start, true) + L"  -  " + FormatTime(parts[i].end, true), pal.text, MonoStyle(13.5f, 500));
      TextStyle ls; ls.size = 12.5f; ls.align = Align::End;
      p.Text({row.right() - 150 - (mode_ == 2 ? 34.f : 12.f), row.y, 150, row.h}, FormatTime(len, true), pal.textSecondary, ls);
      bool removed = false;
      if (mode_ == 2 && ui.IconButton({row.right() - 30, row.cy() - 12, 24, 24}, "close", !Busy(), L"Remove", false)) { segs_.erase(segs_.begin() + i); selSeg_ = -1; removed = true; }
      if (!removed && it.clicked && !Busy()) {
        if (mode_ == 2) { selSeg_ = i; a_ = parts[i].start; b_ = parts[i].end; }
        if (view_.IsOpen()) view_.Seek(parts[i].start, false);
      }
      ui.PopId();
      if (removed) break;
    }
    ui.EndScroll();
  }

  void StartSplit() {
    media::SplitRequest req;
    req.mode = mode_ == 0 ? media::SplitMode::Markers : (mode_ == 1 ? media::SplitMode::EqualParts : media::SplitMode::Segments);
    req.markers = markers_; req.parts = parts_; req.segments = segs_;
    req.copyMode = copy_;
    Settings st = SettingsStore::Get().Snapshot();
    req.hardware = st.hardwareAcceleration; req.preserveMetadata = st.preserveMetadata; req.crf = 0;
    auto res = result_ = std::make_shared<media::SplitResult>();
    const fs::path in = path_;
    RunJob(L"Splitting video...", [in, req, res](JobContext& ctx) -> JobResult {
      *res = media::SplitVideo(in, req, ctx);
      return static_cast<JobResult>(*res);
    });
  }

  int mode_ = 0, parts_ = 3, selMarker_ = -1, selSeg_ = -1;
  bool copy_ = false;
  double a_ = 0, b_ = 0;
  std::vector<double> markers_;
  std::vector<media::TimeRange> segs_;
  std::shared_ptr<media::SplitResult> result_;
};

} // namespace

ToolDialog* MakeSplitVideo(const Selection& sel) { return new SplitVideoDialog(sel); }

} // namespace filo::tools
