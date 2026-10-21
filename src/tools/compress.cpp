// Compress - images, video, audio and PDF (single file or a whole batch with the same settings).
#include "tools/tools.h"
#include "core/output.h"
#include "engines/image/image_compress.h"
#include "engines/media/media_ops.h"
#include "engines/pdf/pdf_ops.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_COMPRESS_EST = WM_APP + 0x62;

struct CompressParams {
  int mode = 0;               // 0 preset, 1 target size, 2 percent
  int preset = 1;             // 0 light, 1 balanced, 2 strong
  double targetMB = 1.0;
  float percent = 50;
  int maxSide = 0;            // 0 = keep (images/video: longer side / height)
  bool keepMetadata = true;
};

struct OneResult { bool ok = false, notSmaller = false; std::wstring error, line; fs::path out; uint64_t inB = 0, outB = 0; std::vector<std::wstring> warnings; };

std::wstring Percent(uint64_t in, uint64_t out) {
  if (!in) return L"";
  const int p = (int)std::lround((1.0 - (double)out / (double)in) * 100.0);
  return (p >= 0 ? L"\x2212" : L"+") + std::to_wstring(std::abs(p)) + L"%";
}

OneResult CompressOne(const FileKind& k, const CompressParams& cp, JobContext& ctx) {
  OneResult r; r.inB = k.size;
  Settings st = SettingsStore::Get().Snapshot();
  const std::wstring name = k.path.filename().wstring();
  auto done = [&](bool ok, uint64_t outB, const fs::path& out, bool notSmaller, const std::wstring& err) {
    r.ok = ok; r.outB = outB; r.out = out; r.notSmaller = notSmaller; r.error = err;
    if (ok) r.line = name + L": " + FormatBytes(r.inB) + L" \x2192 " + FormatBytes(outB) + L" (" + Percent(r.inB, outB) + L")";
    else if (notSmaller) r.line = name + L": already as small as it gets";
    else r.line = name + L": " + err;
    return r;
  };
  if (k.group == Group::Image) {
    img::CompressRequest rq;
    rq.input = k.path;
    rq.mode = cp.mode == 0 ? img::CompressMode::Preset : (cp.mode == 1 ? img::CompressMode::TargetBytes : img::CompressMode::PercentReduction);
    rq.preset = cp.preset == 0 ? img::CompressPreset::Low : (cp.preset == 1 ? img::CompressPreset::Medium : img::CompressPreset::High);
    rq.targetBytes = (uint64_t)(cp.targetMB * 1e6);
    rq.percentReduction = (int)std::lround(cp.percent);
    rq.maxSide = cp.maxSide; rq.preserveMetadata = cp.keepMetadata;
    fs::path desired = DesiredOutputPath(k.path, k.format.empty() ? ExtOf(k.path) : k.format, st, L" (compressed)");
    OutputTransaction tx(desired, st.overwritePolicy);
    rq.output = tx.TempPath();
    img::CompressResult cr = img::CompressImage(rq, ctx);
    for (auto& w : cr.warnings) r.warnings.push_back(name + L": " + w);
    if (cr.notSmaller) return done(false, 0, {}, true, L"");
    if (!cr.ok) return done(false, 0, {}, false, cr.error);
    std::wstring e; if (!tx.Commit(&e)) return done(false, 0, {}, false, e);
    return done(true, cr.outputBytes, tx.FinalPath(), false, L"");
  }
  if (k.group == Group::Video || k.group == Group::Audio) {
    media::CompressRequest rq;
    rq.mode = cp.mode == 0 ? media::CompressMode::Preset : (cp.mode == 1 ? media::CompressMode::TargetSize : media::CompressMode::Percent);
    rq.preset = cp.preset == 0 ? media::CompressPreset::High : (cp.preset == 1 ? media::CompressPreset::Balanced : media::CompressPreset::Small);
    rq.targetBytes = (uint64_t)(cp.targetMB * 1e6);
    rq.percentSmaller = cp.percent;
    rq.maxHeight = k.group == Group::Video ? cp.maxSide : 0;
    rq.hardware = st.hardwareAcceleration; rq.preserveMetadata = cp.keepMetadata;
    rq.outputPath = DesiredOutputPath(k.path, k.format.empty() ? ExtOf(k.path) : k.format, st, L" (compressed)");
    media::CompressResult cr = media::CompressMedia(k.path, rq, ctx);
    if (cr.notSmaller) return done(false, 0, {}, true, L"");
    if (!cr.ok) return done(false, 0, {}, false, cr.error);
    if (cr.missedTarget) r.warnings.push_back(name + L": could not reach the target size");
    return done(true, cr.outputBytes, cr.outputs.empty() ? fs::path() : cr.outputs[0], false, L"");
  }
  if (k.group == Group::Pdf) {
    pdf::CompressOptions po;
    po.preset = cp.preset == 0 ? pdf::CompressPreset::Low : (cp.preset == 1 ? pdf::CompressPreset::Medium : pdf::CompressPreset::High);
    if (cp.mode == 1) po.targetBytes = (uint64_t)(cp.targetMB * 1e6);
    if (cp.mode == 2) po.targetPercent = std::max(1, (int)std::lround(100.0 - cp.percent));
    fs::path desired = DesiredOutputPath(k.path, L"pdf", st, L" (compressed)");
    pdf::OpResult cr = pdf::CompressPdf(k.path, desired, po, ctx);
    for (auto& w : cr.warnings) r.warnings.push_back(name + L": " + w);
    if (cr.notSmaller) return done(false, 0, {}, true, L"");
    if (!cr.ok) return done(false, 0, {}, false, cr.error);
    return done(true, cr.outBytes, cr.outputs.empty() ? fs::path() : cr.outputs[0], false, L"");
  }
  return done(false, 0, {}, false, L"This type of file cannot be compressed.");
}

class CompressDialog : public ToolDialog {
 public:
  explicit CompressDialog(const Selection& sel) {
    selection_ = sel;
    for (auto& i : sel.items) { total_ += i.size; hasImage_ |= i.group == Group::Image; hasVideo_ |= i.group == Group::Video; hasAudio_ |= i.group == Group::Audio; hasPdf_ |= i.group == Group::Pdf; }
    shared_ = std::make_shared<Shared>();
  }
  ~CompressDialog() override { shared_->cancel = true; }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Compress"; o.widthDip = 470; o.heightDip = 500; o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "compress"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject(); j["mode"] = cp_.mode; j["preset"] = cp_.preset; j["targetMB"] = cp_.targetMB; j["percent"] = cp_.percent; j["maxSide"] = cp_.maxSide; return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("mode")) cp_.mode = (int)j.Get("mode").AsInt(); if (j.Has("preset")) cp_.preset = (int)j.Get("preset").AsInt();
    if (j.Has("targetMB")) cp_.targetMB = j.Get("targetMB").AsNumber(1.0); if (j.Has("percent")) cp_.percent = (float)j.Get("percent").AsNumber(50);
    if (j.Has("maxSide")) cp_.maxSide = (int)j.Get("maxSide").AsInt(); estDirty_ = true;
  }
  void TestApply() override { Start(); }

  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_COMPRESS_EST) { Invalidate(); SetHandled(true); return 0; }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = 24;
    ui.BeginColumn({body.x + pad, body.y + 20, body.w - 2 * pad, body.h - 30}, 12);
    // ---- what is being compressed
    {
      Rect r = ui.Take(54);
      p.FillRoundRect(r, 14, pal.fieldFill);
      TextStyle t; t.size = dialog::bodyFontSize; t.weight = 600;
      std::wstring title = selection_.count() == 1 ? NameOf(selection_.items[0].path) : std::to_wstring(selection_.count()) + L" files";
      p.Text({r.x + 14, r.y + 6, r.w - 28, 24}, title, pal.text, t);
      TextStyle s; s.size = dialog::smallFontSize;
      p.Text({r.x + 14, r.y + 28, r.w - 28, 20}, FormatBytes(total_) + EstimateText(), pal.textSecondary, s);
    }
    bool changed = false;
    {
      Rect row = ui.Take(30);
      auto c = ui.Cols(row, {Fixed(96), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Mode", pal.text, ls);
      int m = ui.Segmented(c[1], {L"Preset", L"Target size", L"Percentage"}, cp_.mode);
      if (m != cp_.mode) { cp_.mode = m; changed = true; }
    }
    if (cp_.mode == 0) {
      Rect row = ui.Take(30);
      auto c = ui.Cols(row, {Fixed(96), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Compression", pal.text, ls);
      int v = ui.Segmented(c[1], {L"Light", L"Balanced", L"Strong"}, cp_.preset);
      if (v != cp_.preset) { cp_.preset = v; changed = true; }
      TextStyle hs; hs.size = dialog::smallFontSize; hs.wrap = true; hs.vcenter = false;
      Rect h = ui.Take(34);
      const wchar_t* tips[] = {L"Best quality, smallest savings.", L"A good balance for sharing.", L"Smallest files; some quality is lost."};
      p.Text(h, tips[std::clamp(cp_.preset, 0, 2)], pal.textTertiary, hs);
    } else if (cp_.mode == 1) {
      Rect row = ui.Take(30);
      auto c = ui.Cols(row, {Fixed(96), Fixed(140), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Target size", pal.text, ls);
      double v = cp_.targetMB;
      if (ui.Stepper(c[1], v, 0.05, 20000, v < 5 ? 0.1 : 1.0, 2, L"MB")) { cp_.targetMB = v; changed = true; }
      Rect h = ui.Take(34);
      TextStyle hs; hs.size = dialog::smallFontSize; hs.wrap = true; hs.vcenter = false;
      p.Text(h, L"Each file is compressed to at most this size when possible.", pal.textTertiary, hs);
    } else {
      float v = cp_.percent;
      if (ui.SliderRow(ui.Take(30), L"Smaller by", v, 5, 95, 1, 50, [](float x) { return std::to_wstring((int)std::lround(x)) + L"%"; }, true, 96.f, 56.f)) { cp_.percent = v; changed = true; }
      ui.Take(8);
    }
    if (hasImage_ || hasVideo_) {
      Rect row = ui.Take(30);
      auto c = ui.Cols(row, {Fixed(96), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], hasVideo_ && !hasImage_ ? L"Resolution" : L"Max size", pal.text, ls);
      const bool vid = hasVideo_ && !hasImage_;
      std::vector<std::wstring> items = vid ? std::vector<std::wstring>{L"Original", L"1440p", L"1080p", L"720p", L"480p"} : std::vector<std::wstring>{L"Original", L"4096 px", L"2560 px", L"1920 px", L"1280 px"};
      const int vals_img[] = {0, 4096, 2560, 1920, 1280}, vals_vid[] = {0, 1440, 1080, 720, 480};
      const int* vals = vid ? vals_vid : vals_img;
      int cur = 0; for (int i = 0; i < 5; ++i) if (vals[i] == cp_.maxSide) cur = i;
      int nv = cur;
      if (ui.Dropdown(c[1], items, nv) && nv != cur) { cp_.maxSide = vals[nv]; changed = true; }
    }
    {
      Rect row = ui.Take(26);
      if (ui.Checkbox(row, L"Keep metadata (date, camera, tags)", cp_.keepMetadata)) changed = true;
    }
    ui.EndColumn();
    if (changed) { estDirty_ = true; est_.clear(); }
    if (estDirty_) { estDirty_ = false; StartEstimate(); }

    FooterInfo fi;
    fi.action = selection_.count() > 1 ? L"Compress All" : L"Compress";
    fi.actionEnabled = !Busy() && selection_.count() > 0;
    fi.status = hasPdf_ && cp_.mode == 1 ? L"PDF target sizes are approximate." : L"The original file is never changed.";
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  struct Shared { std::mutex m; std::wstring text; uint64_t seq = 0; std::atomic<bool> cancel{false}; };

  std::wstring EstimateText() const { std::lock_guard lk(shared_->m); return shared_->text.empty() ? L"" : L"   \x00B7   " + shared_->text; }

  void StartEstimate() {
    if (selection_.count() != 1) return;
    const FileKind k = selection_.items[0];
    const CompressParams cp = cp_;
    auto sh = shared_; HWND hw = Hwnd();
    uint64_t seq; { std::lock_guard lk(sh->m); seq = ++sh->seq; sh->text.clear(); }
    std::thread([sh, k, cp, seq, hw] {
      Sleep(250);                                                    // debounce slider drags
      { std::lock_guard lk(sh->m); if (sh->seq != seq || sh->cancel) return; }
      std::wstring text;
      if (k.group == Group::Image) {
        img::CompressRequest rq; rq.input = k.path;
        rq.mode = cp.mode == 0 ? img::CompressMode::Preset : (cp.mode == 1 ? img::CompressMode::TargetBytes : img::CompressMode::PercentReduction);
        rq.preset = cp.preset == 0 ? img::CompressPreset::Low : (cp.preset == 1 ? img::CompressPreset::Medium : img::CompressPreset::High);
        rq.targetBytes = (uint64_t)(cp.targetMB * 1e6); rq.percentReduction = (int)std::lround(cp.percent); rq.maxSide = cp.maxSide;
        img::SizeEstimate e = img::EstimateCompressedSize(rq);
        if (e.ok) text = L"\x2192 about " + FormatBytes(e.bytes) + L" (" + Percent(k.size, e.bytes) + L")";
      } else if (k.group == Group::Video || k.group == Group::Audio) {
        media::CompressRequest rq;
        rq.mode = cp.mode == 0 ? media::CompressMode::Preset : (cp.mode == 1 ? media::CompressMode::TargetSize : media::CompressMode::Percent);
        rq.preset = cp.preset == 0 ? media::CompressPreset::High : (cp.preset == 1 ? media::CompressPreset::Balanced : media::CompressPreset::Small);
        rq.targetBytes = (uint64_t)(cp.targetMB * 1e6); rq.percentSmaller = cp.percent; rq.maxHeight = k.group == Group::Video ? cp.maxSide : 0;
        media::SizeEstimate e = media::EstimateCompressedSize(k.path, rq);
        if (e.bytes) text = (e.feasible ? L"\x2192 about " : L"\x2192 target too small: about ") + FormatBytes(e.bytes) + L" (" + Percent(k.size, e.bytes) + L")";
      }
      { std::lock_guard lk(sh->m); if (sh->seq == seq) sh->text = text; }
      if (hw) PostMessageW(hw, WM_COMPRESS_EST, 0, 0);
    }).detach();
  }

  void Start() {
    if (Busy() || selection_.empty()) return;
    const std::vector<FileKind> files = selection_.items;
    const CompressParams cp = cp_;
    RunJob(files.size() > 1 ? L"Compressing files..." : L"Compressing...", [files, cp](JobContext& ctx) -> JobResult {
      JobResult jr; jr.ok = false;
      int okCount = 0; std::wstring firstErr;
      for (size_t i = 0; i < files.size(); ++i) {
        if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
        ctx.SetProgressWindow((double)i / files.size(), (double)(i + 1) / files.size());
        ctx.SetStage(JobState::Running, files[i].path.filename().wstring());
        OneResult r = CompressOne(files[i], cp, ctx);
        jr.warnings.insert(jr.warnings.begin() + std::min(jr.warnings.size(), (size_t)okCount), r.line);   // summary lines first
        for (auto& w : r.warnings) jr.warnings.push_back(w);
        if (r.ok) { ++okCount; jr.outputs.push_back(r.out); }
        else if (!r.notSmaller && firstErr.empty()) firstErr = r.error;
      }
      ctx.SetProgressWindow(0, 1);
      jr.ok = okCount > 0 || firstErr.empty();       // "already optimal" everywhere is not a failure
      if (!jr.ok) jr.error = firstErr;
      if (okCount == 0 && jr.ok) jr.error.clear();
      return jr;
    });
  }

  CompressParams cp_;
  std::shared_ptr<Shared> shared_;
  std::wstring est_;
  bool estDirty_ = true;
  uint64_t total_ = 0;
  bool hasImage_ = false, hasVideo_ = false, hasAudio_ = false, hasPdf_ = false;
};

} // namespace

ToolDialog* MakeCompress(const Selection& sel) { return new CompressDialog(sel); }

} // namespace filo::tools
