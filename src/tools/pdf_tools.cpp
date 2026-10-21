// PDF tools: Merge PDFs, Split PDF, Organize PDF pages, and Create PDF (from images).
// All the real work lives in engines/pdf/pdf_ops.h; these dialogs collect the inputs and run the operations as jobs.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/output.h"
#include "core/settings.h"
#include "engines/pdf/pdf_ops.h"
#include "registry/filetype.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

bool IsRtl() { return SettingsStore::Get().Snapshot().language == L"ar"; }
std::wstring Plural(int n, const wchar_t* one, const wchar_t* many) { return std::to_wstring(n) + L" " + (n == 1 ? one : many); }

// ---------------------------------------------------------------------------------------------------- shared list UI
struct ListItem {
  fs::path path;
  std::wstring info;           // "12 pages  -  1.2 MB" or an error sentence
  bool ok = true;
  bool needsPassword = false;
  std::wstring password;
  int pages = 0;
};

// First page of a PDF as a thumbnail (the shell often has no PDF thumbnail handler and returns the viewer's app icon instead).
bool FirstPageThumb(const fs::path& path, const std::wstring& pw, pdf::Bitmap& out) {
  auto th = pdf::PageThumbnailer::Open(path, pw);
  return th && th->PageCount() > 0 && th->Render(0, 220, out);
}

void MoveItem(std::vector<ListItem>& v, int from, int to) { if (from >= 0 && to >= 0 && from < (int)v.size() && to < (int)v.size()) std::swap(v[from], v[to]); }

// ---------------------------------------------------------------------------------------------------- Merge PDFs
class MergePdfs : public ToolDialog {
 public:
  explicit MergePdfs(const Selection& sel) { selection_ = sel; for (auto& i : sel.items) AddFile(i.path); }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Merge PDFs"; o.widthDip = 520; o.heightDip = 560; o.resizable = true; o.minWidthDip = 440; o.minHeightDip = 380; o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "merge-pdf"; }
  void TestApply() override { Start(); }
  bool AcceptsFileDrop() const override { return !Busy(); }
  void OnFilesDropped(const std::vector<fs::path>& f) override { for (auto& p : f) AddFile(p); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    Rect vp{body.x + pad, body.y + 12, body.w - 2 * pad, body.h - 22};
    const float rowH = 66, gap = 8;
    const float totalH = items_.size() * (rowH + gap);
    const float off = ui.BeginScroll(vp, totalH, ui.Id("list"));
    int up = -1, down = -1, rem = -1;
    for (int i = 0; i < (int)items_.size(); ++i) {
      Rect row{vp.x + 2, vp.y + i * (rowH + gap) - off, vp.w - 4 - (totalH > vp.h ? 10.f : 0.f), rowH};
      if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
      ui.PushId(i);
      ListItem& it = items_[i];
      p.FillRoundRect(row, control::listRowRadius, pal.fieldFill);
      p.StrokeRoundRect(row, control::listRowRadius, pal.divider, 1.f);
      auto X = [&](float x, float w) { return ui.Rtl() ? row.right() - (x - row.x) - w : x; };
      TextStyle ns; ns.size = 15; ns.weight = 600; ns.align = Align::Center;
      p.Text({X(row.x + 10, 26), row.y, 26, row.h}, std::to_wstring(i + 1), pal.textSecondary, ns);
      Rect th{X(row.x + 40, 40), row.y + 9, 40, row.h - 18};
      p.FillRoundRect(th, 6, Color(1, 1, 1, 0.85f));
      if (ID2D1Bitmap* bmp = Thumb(it.path, 160)) {
        auto sz = bmp->GetSize(); const float s = std::min(th.w / sz.width, th.h / sz.height);
        p.Bitmap(bmp, {th.cx() - sz.width * s / 2, th.cy() - sz.height * s / 2, sz.width * s, sz.height * s});
      } else p.Icon("document", th.cx(), th.cy(), 18, pal.textTertiary, 1.5f);
      const float tx = row.x + 90, tw = row.w - 90 - 92;
      TextStyle nm; nm.size = 13.5f; nm.weight = 500;
      p.Text({X(tx, tw), row.y + 9, tw, 22}, NameOf(it.path), pal.text, nm);
      TextStyle sub; sub.size = 12;
      if (it.needsPassword) {
        ui.PushId(0x9A55);
        if (ui.TextInput({X(tx, std::min(tw, 190.f)), row.y + 34, std::min(tw, 190.f), 24}, it.password, L"Password")) {}
        if (!it.password.empty() && ui.Button({X(tx + std::min(tw, 190.f) + 6, 56), row.y + 34, 56, 24}, L"Unlock", ButtonStyle::Secondary)) Probe(it);
        ui.PopId();
      } else {
        p.Text({X(tx, tw), row.y + 34, tw, 20}, it.info, it.ok ? pal.textTertiary : Color::Hex(0xC2410C), sub);
      }
      const float bx = X(row.right() - 12 - 22, 22);
      if (ui.IconButton({bx, row.y + 8, 22, 22}, "chevronup", i > 0 && !Busy(), L"Move up", true)) up = i;
      if (ui.IconButton({bx, row.y + row.h - 30, 22, 22}, "chevrondown", i + 1 < (int)items_.size() && !Busy(), L"Move down", true)) down = i;
      if (ui.IconButton({X(row.right() - 12 - 22 - 28, 22), row.cy() - 11, 22, 22}, "close", !Busy(), L"Remove", false)) rem = i;
      ui.PopId();
    }
    ui.EndScroll();
    if (up > 0) MoveItem(items_, up, up - 1);
    if (down >= 0) MoveItem(items_, down, down + 1);
    if (rem >= 0) items_.erase(items_.begin() + rem);
    if (items_.empty()) {
      TextStyle es; es.size = 14; es.align = Align::Center;
      p.Text({vp.x, vp.cy() - 12, vp.w, 24}, L"Drop PDF files here.", pal.textTertiary, es);
    }
    int total = 0; bool allOk = items_.size() >= 2;
    for (auto& i : items_) { total += i.pages; if (!i.ok || i.needsPassword) allOk = false; }
    FooterInfo fi;
    fi.action = L"Merge"; fi.actionEnabled = allOk && !Busy();
    fi.status = items_.size() < 2 ? L"Add at least two PDFs." : (allOk ? Plural(total, L"page", L"pages") + L" in the merged file." : L"Fix the files marked in red first.");
    fi.statusIsError = items_.size() < 2;
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  void Probe(ListItem& it) {
    pdf::PdfFileInfo info = pdf::GetPdfFileInfo(it.path, it.password);
    it.needsPassword = info.status == pdf::OpenStatus::NeedsPassword || info.status == pdf::OpenStatus::WrongPassword;
    it.ok = info.status == pdf::OpenStatus::Ok;
    it.pages = info.pageCount;
    it.info = it.ok ? Plural(info.pageCount, L"page", L"pages") + L"  \x00B7  " + FormatBytes(info.sizeBytes) : pdf::OpenStatusMessage(info.status);
    { pdf::Bitmap b; if (it.ok && FirstPageThumb(it.path, it.password, b)) SetThumbOverride(it.path, b.width, b.height, b.bgra.data()); }
  }
  void AddFile(const fs::path& path) {
    if (DetectFile(path).group != Group::Pdf) return;
    for (auto& i : items_) if (i.path == path) return;
    ListItem it; it.path = path; Probe(it); items_.push_back(std::move(it));
  }
  void Start() {
    if (items_.size() < 2 || Busy()) return;
    std::vector<fs::path> order; std::map<fs::path, std::wstring> pw;
    for (auto& i : items_) { order.push_back(i.path); if (!i.password.empty()) pw[i.path] = i.password; }
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(order[0], L"pdf", st, L" (merged)");
    RunJob(L"Merging PDFs...", [order, pw, desired, st](JobContext& ctx) -> JobResult {
      pdf::OpResult r = pdf::MergePdfs(order, desired, ctx, pw);
      if (!r.ok) return JobResult::Fail(r.cancelled ? L"Cancelled" : r.error);
      JobResult jr = JobResult::Ok(r.outputs); jr.warnings = r.warnings; return jr;
    });
  }
  std::vector<ListItem> items_;
};

// ---------------------------------------------------------------------------------------------------- Create PDF from images
class ImagesToPdf : public ToolDialog {
 public:
  explicit ImagesToPdf(const Selection& sel) { selection_ = sel; for (auto& i : sel.items) AddFile(i.path); }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Create PDF"; o.widthDip = 520; o.heightDip = 660; o.resizable = true; o.minWidthDip = 440; o.minHeightDip = 480; o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "images-to-pdf"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["size"] = size_; j["orient"] = orient_; j["fit"] = fit_; j["margin"] = margin_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("size")) size_ = std::clamp((int)j.Get("size").AsInt(), 0, 5);
    if (j.Has("orient")) orient_ = std::clamp((int)j.Get("orient").AsInt(), 0, 2);
    if (j.Has("fit")) fit_ = std::clamp((int)j.Get("fit").AsInt(), 0, 2);
    if (j.Has("margin")) margin_ = j.Get("margin").AsNumber(0);
  }
  void TestApply() override { Start(); }
  bool AcceptsFileDrop() const override { return !Busy(); }
  void OnFilesDropped(const std::vector<fs::path>& f) override { for (auto& p : f) AddFile(p); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float optH = 150;
    Rect vp{body.x + pad, body.y + 12, body.w - 2 * pad, std::max(120.f, body.h - optH - 22)};
    const float rowH = 58, gap = 6;
    const float totalH = items_.size() * (rowH + gap);
    const float off = ui.BeginScroll(vp, totalH, ui.Id("list"));
    int up = -1, down = -1, rem = -1;
    for (int i = 0; i < (int)items_.size(); ++i) {
      Rect row{vp.x + 2, vp.y + i * (rowH + gap) - off, vp.w - 4 - (totalH > vp.h ? 10.f : 0.f), rowH};
      if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
      ui.PushId(i);
      const ListItem& it = items_[i];
      p.FillRoundRect(row, control::listRowRadius, pal.fieldFill);
      p.StrokeRoundRect(row, control::listRowRadius, pal.divider, 1.f);
      auto X = [&](float x, float w) { return ui.Rtl() ? row.right() - (x - row.x) - w : x; };
      TextStyle ns; ns.size = 14; ns.weight = 600; ns.align = Align::Center;
      p.Text({X(row.x + 8, 24), row.y, 24, row.h}, std::to_wstring(i + 1), pal.textSecondary, ns);
      Rect th{X(row.x + 36, 48), row.y + 7, 48, row.h - 14};
      p.FillRoundRect(th, 6, Color(0, 0, 0, 0.10f));
      if (ID2D1Bitmap* bmp = Thumb(it.path, 160)) {
        auto sz = bmp->GetSize(); const float s = std::max(th.w / sz.width, th.h / sz.height);
        p.PushClipRoundRect(th, 6); p.Bitmap(bmp, {th.cx() - sz.width * s / 2, th.cy() - sz.height * s / 2, sz.width * s, sz.height * s}); p.PopClip();
      }
      const float tx = row.x + 92, tw = row.w - 92 - 92;
      TextStyle nm; nm.size = 13.5f; nm.weight = 500;
      p.Text({X(tx, tw), row.y + 8, tw, 22}, NameOf(it.path), pal.text, nm);
      TextStyle sub; sub.size = 12;
      p.Text({X(tx, tw), row.y + 30, tw, 20}, it.info, pal.textTertiary, sub);
      const float bx = X(row.right() - 12 - 22, 22);
      if (ui.IconButton({bx, row.y + 6, 22, 22}, "chevronup", i > 0 && !Busy(), L"Move up", true)) up = i;
      if (ui.IconButton({bx, row.y + row.h - 28, 22, 22}, "chevrondown", i + 1 < (int)items_.size() && !Busy(), L"Move down", true)) down = i;
      if (ui.IconButton({X(row.right() - 12 - 22 - 28, 22), row.cy() - 11, 22, 22}, "close", !Busy(), L"Remove", false)) rem = i;
      ui.PopId();
    }
    ui.EndScroll();
    if (up > 0) MoveItem(items_, up, up - 1);
    if (down >= 0) MoveItem(items_, down, down + 1);
    if (rem >= 0) items_.erase(items_.begin() + rem);
    if (items_.empty()) { TextStyle es; es.size = 14; es.align = Align::Center; p.Text({vp.x, vp.cy() - 12, vp.w, 24}, L"Drop images here.", pal.textTertiary, es); }

    // ---- options
    float y = vp.bottom() + 12; const float inset = 24;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    static const std::vector<std::wstring> sizes{L"A4", L"A3", L"A5", L"Letter", L"Legal", L"Same as the image"};
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(96), Fixed(190), Flex(1)}, 8);
      p.Text(c[0], L"Page size", pal.text, ls); ui.PushId(0x51);
      ui.Dropdown(c[1], sizes, size_); ui.PopId();
    }
    y += 34;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(96), Fixed(260), Flex(1)}, 8);
      p.Text(c[0], L"Orientation", pal.text, ls); ui.PushId(0x52);
      orient_ = ui.Segmented(c[1], {L"Auto", L"Portrait", L"Landscape"}, orient_, size_ != 5); ui.PopId();
    }
    y += 34;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(96), Fixed(260), Flex(1)}, 8);
      p.Text(c[0], L"Image", pal.text, ls); ui.PushId(0x53);
      fit_ = ui.Segmented(c[1], {L"Fit", L"Fill", L"Stretch"}, fit_, size_ != 5); ui.PopId();
    }
    y += 34;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(96), Fixed(120), Flex(1)}, 8);
      p.Text(c[0], L"Margin", pal.text, ls); ui.PushId(0x54);
      ui.Stepper(c[1], margin_, 0, 50, 1, 0, L"mm", size_ != 5); ui.PopId();
    }
    FooterInfo fi;
    fi.action = L"Create PDF"; fi.actionEnabled = !items_.empty() && !Busy();
    fi.status = items_.empty() ? L"Add at least one image." : Plural((int)items_.size(), L"page", L"pages") + L". JPEG photos are embedded without recompression.";
    fi.statusIsError = items_.empty();
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  void AddFile(const fs::path& path) {
    FileKind k = DetectFile(path);
    if (k.group != Group::Image) return;
    for (auto& i : items_) if (i.path == path) return;
    ListItem it; it.path = path; it.info = FormatBytes(k.size); items_.push_back(std::move(it));
  }
  void Start() {
    if (items_.empty() || Busy()) return;
    std::vector<fs::path> files; for (auto& i : items_) files.push_back(i.path);
    pdf::ImagesToPdfOptions o;
    static const pdf::PageSizeKind kinds[] = {pdf::PageSizeKind::A4, pdf::PageSizeKind::A3, pdf::PageSizeKind::A5, pdf::PageSizeKind::Letter, pdf::PageSizeKind::Legal, pdf::PageSizeKind::FitImage};
    o.size = kinds[std::clamp(size_, 0, 5)];
    o.orientation = orient_ == 1 ? pdf::PageOrientation::Portrait : (orient_ == 2 ? pdf::PageOrientation::Landscape : pdf::PageOrientation::Auto);
    o.fit = fit_ == 1 ? pdf::FitMode::Fill : (fit_ == 2 ? pdf::FitMode::Stretch : pdf::FitMode::Fit);
    o.marginPt = margin_ * 72.0 / 25.4;
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(files[0], L"pdf", st, files.size() > 1 ? L" (PDF)" : L"");
    RunJob(L"Creating PDF...", [files, o, desired](JobContext& ctx) -> JobResult {
      pdf::OpResult r = pdf::CreatePdfFromImages(files, desired, o, ctx);
      if (!r.ok) return JobResult::Fail(r.cancelled ? L"Cancelled" : r.error);
      JobResult jr = JobResult::Ok(r.outputs); jr.warnings = r.warnings; return jr;
    });
  }
  std::vector<ListItem> items_;
  int size_ = 0, orient_ = 0, fit_ = 0;
  double margin_ = 0;
};

// ---------------------------------------------------------------------------------------------------- Split PDF
class SplitPdfDialog : public ToolDialog {
 public:
  explicit SplitPdfDialog(const Selection& sel) {
    selection_ = sel;
    if (!sel.items.empty()) { path_ = sel.items[0].path; Probe(); }
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Split PDF"; o.widthDip = 470; o.heightDip = 430; o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "split-pdf"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["mode"] = mode_; j["every"] = every_; j["ranges"] = Narrow(ranges_); return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("mode")) mode_ = std::clamp((int)j.Get("mode").AsInt(), 0, 2);
    if (j.Has("every")) every_ = std::clamp(j.Get("every").AsNumber(2), 1.0, 9999.0);
    if (j.Has("ranges")) ranges_ = Widen(j.Get("ranges").AsString());
  }
  void TestApply() override { Start(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float inset = 24;
    float y = body.y + 16;
    Rect card{body.x + inset, y, body.w - 2 * inset, 84};
    p.FillRoundRect(card, 16, pal.fieldFill); p.StrokeRoundRect(card, 16, pal.divider, 1.f);
    Rect th{card.x + 12, card.y + 10, 50, 64};
    p.FillRoundRect(th, 6, Color(1, 1, 1, 0.85f));
    if (ID2D1Bitmap* bmp = Thumb(path_, 160)) {
      auto sz = bmp->GetSize(); const float s = std::min(th.w / sz.width, th.h / sz.height);
      p.Bitmap(bmp, {th.cx() - sz.width * s / 2, th.cy() - sz.height * s / 2, sz.width * s, sz.height * s});
    }
    TextStyle nm; nm.size = 14.5f; nm.weight = 600;
    p.Text({card.x + 74, card.y + 14, card.w - 86, 24}, NameOf(path_), pal.text, nm);
    TextStyle sub; sub.size = 12.5f;
    p.Text({card.x + 74, card.y + 42, card.w - 86, 22}, info_, ok_ ? pal.textSecondary : Color::Hex(0xC2410C), sub);
    y = card.bottom() + 18;
    if (needsPw_) {
      Rect row{body.x + inset, y, body.w - 2 * inset, 30}; auto c = ui.Cols(row, {Flex(1), Fixed(90)}, 8);
      ui.PushId(0x9A);
      ui.TextInput(c[0], password_, L"Password");
      if (ui.Button(c[1], L"Unlock", ButtonStyle::Primary, !password_.empty())) Probe();
      ui.PopId();
      y += 40;
    }
    TextStyle ls; ls.size = dialog::bodyFontSize;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(70), Flex(1)}, 8);
      p.Text(c[0], L"Split", pal.text, ls); ui.PushId(0x51);
      mode_ = ui.Segmented(c[1], {L"Every page", L"Page ranges", L"Every N pages"}, mode_, ok_); ui.PopId();
    }
    y += 40;
    if (mode_ == 1) {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28};
      ui.PushId(0x52); ui.TextInput(row, ranges_, L"1-3, 4-6, 7-"); ui.PopId();
      TextStyle hs; hs.size = dialog::smallFontSize; hs.wrap = true;
      p.Text({row.x, y + 32, row.w, 34}, L"Each range becomes its own file. Example: 1-3, 4-6, 7- (to the end).", pal.textTertiary, hs);
      y += 70;
    } else if (mode_ == 2) {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(120), Fixed(110), Flex(1)}, 8);
      p.Text(c[0], L"Pages per file", pal.text, ls); ui.PushId(0x53);
      ui.Stepper(c[1], every_, 1, std::max(1, pages_), 1, 0); ui.PopId();
      y += 40;
    }
    std::wstring err; const int n = FileCount(&err);
    TextStyle rs; rs.size = 13; rs.wrap = true;
    p.Text({body.x + inset, y, body.w - 2 * inset, 40}, !err.empty() ? err : (n > 0 ? L"Creates " + Plural(n, L"file", L"files") + L" in a new folder." : L""), err.empty() ? pal.textSecondary : Color::Hex(0xB26A00), rs);
    FooterInfo fi;
    fi.action = L"Split"; fi.actionEnabled = ok_ && err.empty() && n > 0 && !Busy();
    fi.status = L"The original PDF is never changed.";
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  void Probe() {
    pdf::PdfFileInfo i = pdf::GetPdfFileInfo(path_, password_);
    needsPw_ = i.status == pdf::OpenStatus::NeedsPassword || i.status == pdf::OpenStatus::WrongPassword;
    ok_ = i.status == pdf::OpenStatus::Ok; pages_ = i.pageCount;
    info_ = ok_ ? Plural(i.pageCount, L"page", L"pages") + L"  \x00B7  " + FormatBytes(i.sizeBytes) : pdf::OpenStatusMessage(i.status);
    { pdf::Bitmap b; if (ok_ && FirstPageThumb(path_, password_, b)) SetThumbOverride(path_, b.width, b.height, b.bgra.data()); }
  }
  int FileCount(std::wstring* err) const {
    if (!ok_ || pages_ <= 0) return 0;
    if (mode_ == 0) return pages_;
    if (mode_ == 2) return (pages_ + std::max(1, (int)every_) - 1) / std::max(1, (int)every_);
    std::vector<std::pair<int, int>> r;
    if (ranges_.empty()) return 0;
    if (!pdf::ParsePageRanges(ranges_, pages_, r, err)) return 0;
    return (int)r.size();
  }
  void Start() {
    if (!ok_ || Busy()) return;
    pdf::SplitOptions o;
    o.mode = mode_ == 0 ? pdf::SplitMode::SinglePages : (mode_ == 1 ? pdf::SplitMode::Ranges : pdf::SplitMode::EveryN);
    o.ranges = ranges_; o.everyN = std::max(1, (int)every_); o.password = password_;
    Settings st = SettingsStore::Get().Snapshot();
    fs::path dir = DesiredOutputPath(path_, L"x", st, L" (split)"); dir.replace_extension();
    fs::path in = path_;
    RunJob(L"Splitting PDF...", [in, dir, o](JobContext& ctx) -> JobResult {
      pdf::OpResult r = pdf::SplitPdf(in, dir, o, ctx);
      if (!r.ok) return JobResult::Fail(r.cancelled ? L"Cancelled" : r.error);
      JobResult jr = JobResult::Ok(r.outputs); jr.warnings = r.warnings; return jr;
    });
  }
  fs::path path_;
  std::wstring info_, password_, ranges_;
  bool ok_ = false, needsPw_ = false;
  int pages_ = 0, mode_ = 0;
  double every_ = 2;
};

// ---------------------------------------------------------------------------------------------------- Organize PDF
constexpr UINT WM_ORG_THUMB = WM_APP + 0x74;

class OrganizePdfDialog : public ToolDialog {
 public:
  explicit OrganizePdfDialog(const Selection& sel) {
    selection_ = sel;
    shared_ = std::make_shared<Shared>();
    if (!sel.items.empty()) { path_ = sel.items[0].path; Open(); }
  }
  ~OrganizePdfDialog() override { if (shared_) shared_->stop = true; }

 protected:
  struct Shared {
    std::mutex m; std::atomic<bool> stop{false};
    std::unique_ptr<pdf::PageThumbnailer> th;
    std::vector<pdf::Bitmap> thumbs;       // by source page index
    std::vector<char> ready;
  };
  struct Pg { int src = 0; int rot = 0; };
  struct Cached { ComPtr<ID2D1Bitmap> bmp; int rot = -1; };

  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Organize PDF"; o.widthDip = 640; o.heightDip = 640; o.resizable = true; o.minWidthDip = 460; o.minHeightDip = 420; o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "organize-pdf"; }
  void LoadPreset(const Json& j) override {      // automation: {"delete":[0-based pages], "rotate":{"<page>":deg}, "move":[from,to]}
    if (j.Has("rotate")) for (auto& kv : j.Get("rotate").Items()) { int pg = atoi(kv.first.c_str()); for (auto& g : pages_) if (g.src == pg) g.rot = (int)kv.second.AsInt() % 360; }
    if (j.Has("move") && j.Get("move").Size() == 2) Move((int)j.Get("move").At(0).AsInt(), (int)j.Get("move").At(1).AsInt());
    if (j.Has("delete")) { std::vector<int> del; for (auto& e : j.Get("delete").Elements()) del.push_back((int)e.AsInt());
      pages_.erase(std::remove_if(pages_.begin(), pages_.end(), [&](const Pg& g) { return std::find(del.begin(), del.end(), g.src) != del.end(); }), pages_.end()); }
  }
  void TestApply() override { Start(); }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_ORG_THUMB) { Invalidate(); return 0; }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    Rect well{body.x + pad, body.y + 12, body.w - 2 * pad, body.h - 22};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (needsPw_) {
      Rect row{well.cx() - 160, well.cy() - 15, 320, 30}; auto c = ui.Cols(row, {Flex(1), Fixed(86)}, 8);
      TextStyle ts; ts.size = 13.5f; ts.align = Align::Center; p.Text({well.x, row.y - 36, well.w, 28}, L"This PDF is password protected.", Color(1, 1, 1, 0.8f), ts);
      ui.PushId(0x9A); ui.TextInput(c[0], password_, L"Password");
      if (ui.Button(c[1], L"Unlock", ButtonStyle::Primary, !password_.empty())) Open();
      ui.PopId();
    } else if (!err_.empty()) {
      TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true; p.Text(well.Inset(30), err_, Color(1, 1, 1, 0.7f), es);
    } else {
      const float cw = 128, ch = 176, gap = 12;
      const Rect vp = well.Inset(14, 12);
      const int cols = std::max(1, (int)((vp.w + gap) / (cw + gap)));
      const int rows = ((int)pages_.size() + cols - 1) / cols;
      const float totalH = rows * (ch + gap);
      const float off = ui.BeginScroll(vp, totalH, ui.Id("grid"));
      const float x0 = vp.x + std::max(0.f, (vp.w - (cols * cw + (cols - 1) * gap)) / 2 - (totalH > vp.h ? 4.f : 0.f));
      int rotI = -1, delI = -1, mvFrom = -1, mvTo = -1;
      for (int i = 0; i < (int)pages_.size(); ++i) {
        Rect t{x0 + (i % cols) * (cw + gap), vp.y + (i / cols) * (ch + gap) - off, cw, ch};
        if (t.bottom() < vp.y - 2 || t.y > vp.bottom() + 2) continue;
        ui.PushId(i);
        Interaction it = ui.Behavior(t, ui.Id("card"), true, false);
        const float h = ui.Anim(ui.Id("h"), it.hot ? 1.f : 0.f);
        p.FillRoundRect(t, 12, Color(1, 1, 1, 0.07f + 0.07f * h));
        Rect img{t.x + 8, t.y + 8, t.w - 16, t.h - 40};
        if (ID2D1Bitmap* b = PageBitmap(pages_[i])) {
          auto sz = b->GetSize(); const float s = std::min(img.w / sz.width, img.h / sz.height);
          Rect d{img.cx() - sz.width * s / 2, img.cy() - sz.height * s / 2, sz.width * s, sz.height * s};
          p.Shadow(d, 2, 6, Color(0, 0, 0, 0.35f), 1); p.Bitmap(b, d);
        } else { TextStyle ts; ts.size = 12; ts.align = Align::Center; p.Text(img, L"...", Color(1, 1, 1, 0.5f), ts); }
        TextStyle ns; ns.size = 12; ns.align = Align::Center;
        p.Text({t.x, t.bottom() - 28, t.w, 22}, std::to_wstring(i + 1), Color(1, 1, 1, 0.75f), ns);
        if (h > 0.02f) {
          const float by = t.bottom() - 30;
          if (ui.IconButton({t.x + 6, by, 22, 22}, "chevronleft", i > 0, L"Move earlier", true)) { mvFrom = i; mvTo = i - 1; }
          if (ui.IconButton({t.right() - 28, by, 22, 22}, "chevronright", i + 1 < (int)pages_.size(), L"Move later", true)) { mvFrom = i; mvTo = i + 1; }
          if (ui.IconButton({t.cx() - 26, by, 22, 22}, "rotate", true, L"Rotate", true)) rotI = i;
          if (ui.IconButton({t.cx() + 4, by, 22, 22}, "trash", pages_.size() > 1, L"Delete page", true)) delI = i;
        }
        ui.PopId();
      }
      ui.EndScroll();
      if (rotI >= 0) pages_[rotI].rot = (pages_[rotI].rot + 90) % 360;
      if (delI >= 0) pages_.erase(pages_.begin() + delI);
      if (mvFrom >= 0) Move(mvFrom, mvTo);
    }
    FooterInfo fi;
    fi.action = L"Save a Copy"; fi.actionEnabled = !pages_.empty() && !Busy() && !needsPw_ && err_.empty() && Changed();
    fi.secondary = L"Reset"; fi.secondaryEnabled = Changed();
    fi.status = Changed() ? Plural((int)pages_.size(), L"page", L"pages") + L" in the new file." : L"Rotate, delete or reorder pages; the original is never changed.";
    FooterResult fr = DrawFooter(ui, content, fi);
    if (fr.secondary) { pages_.clear(); for (int i = 0; i < total_; ++i) pages_.push_back({i, 0}); }
    if (fr.action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  bool Changed() const {
    if ((int)pages_.size() != total_) return true;
    for (int i = 0; i < (int)pages_.size(); ++i) if (pages_[i].src != i || pages_[i].rot) return true;
    return false;
  }
  void Move(int from, int to) { if (from >= 0 && to >= 0 && from < (int)pages_.size() && to < (int)pages_.size()) std::swap(pages_[from], pages_[to]); }

  void Open() {
    pdf::OpenStatus st; std::wstring msg;
    auto th = pdf::PageThumbnailer::Open(path_, password_, &st, &msg);
    needsPw_ = st == pdf::OpenStatus::NeedsPassword || st == pdf::OpenStatus::WrongPassword;
    if (!th) { if (!needsPw_) err_ = msg.empty() ? pdf::OpenStatusMessage(st) : msg; return; }
    err_.clear(); needsPw_ = false;
    total_ = th->PageCount();
    pages_.clear(); for (int i = 0; i < total_; ++i) pages_.push_back({i, 0});
    cache_.assign(total_, {});
    { std::lock_guard lk(shared_->m); shared_->th = std::move(th); shared_->thumbs.assign(total_, {}); shared_->ready.assign(total_, 0); }
    auto sh = shared_; HWND hw = Hwnd(); const int n = total_;
    std::thread([sh, hw, n]() {
      for (int i = 0; i < n && !sh->stop; ++i) {
        pdf::Bitmap b;
        if (sh->th->Render(i, 260, b)) { std::lock_guard lk(sh->m); sh->thumbs[i] = std::move(b); sh->ready[i] = 1; }
        if (!sh->stop && hw) PostMessageW(hw, WM_ORG_THUMB, (WPARAM)i, 0);
      }
    }).detach();
  }

  ID2D1Bitmap* PageBitmap(const Pg& g) {
    if (!dc_ || g.src < 0 || g.src >= (int)cache_.size()) return nullptr;
    Cached& c = cache_[g.src];
    if (c.bmp && c.rot == g.rot) return c.bmp.Get();
    std::vector<uint8_t> px; int w = 0, h = 0;
    {
      std::lock_guard lk(shared_->m);
      if (!shared_->ready[g.src]) return nullptr;
      const pdf::Bitmap& b = shared_->thumbs[g.src];
      w = b.width; h = b.height;
      const int rot = g.rot;
      const int ow = (rot == 90 || rot == 270) ? h : w, oh = (rot == 90 || rot == 270) ? w : h;
      px.resize((size_t)ow * oh * 4);
      for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        int nx = x, ny = y;
        if (rot == 90) { nx = h - 1 - y; ny = x; } else if (rot == 180) { nx = w - 1 - x; ny = h - 1 - y; } else if (rot == 270) { nx = y; ny = w - 1 - x; }
        memcpy(&px[((size_t)ny * ow + nx) * 4], &b.bgra[((size_t)y * w + x) * 4], 4);
      }
      w = ow; h = oh;
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b1;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &b1))) return nullptr;
    c.bmp = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b1.Get())); c.rot = g.rot;
    return c.bmp.Get();
  }

  void Start() {
    if (pages_.empty() || Busy()) return;
    std::vector<pdf::PageRef> refs;
    for (auto& g : pages_) { pdf::PageRef r; r.sourceDoc = 0; r.pageIndex = g.src; r.rotationDelta = g.rot; refs.push_back(r); }
    pdf::PdfInput src{path_, password_};
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(path_, L"pdf", st, L" (organized)");
    RunJob(L"Saving PDF...", [src, refs, desired](JobContext& ctx) -> JobResult {
      pdf::OpResult r = pdf::OrganizePdf({src}, refs, desired, ctx);
      if (!r.ok) return JobResult::Fail(r.cancelled ? L"Cancelled" : r.error);
      JobResult jr = JobResult::Ok(r.outputs); jr.warnings = r.warnings; return jr;
    });
  }

  fs::path path_;
  std::wstring err_, password_;
  bool needsPw_ = false;
  int total_ = 0;
  std::vector<Pg> pages_;
  std::vector<Cached> cache_;
  std::shared_ptr<Shared> shared_;
};

}  // namespace

ToolDialog* MakeMergePdfs(const Selection& s) { return new MergePdfs(s); }
ToolDialog* MakeImagesToPdf(const Selection& s) { return new ImagesToPdf(s); }
ToolDialog* MakeSplitPdf(const Selection& s) { return new SplitPdfDialog(s); }
ToolDialog* MakeOrganizePdf(const Selection& s) { return new OrganizePdfDialog(s); }

}  // namespace filo::tools

using namespace filo;
using namespace filo::tools;

FILO_TOOL_GROUP(Pdf) {
  ToolInfo m;
  m.id = "merge-pdf"; m.label = L"Merge PDFs"; m.wheelLabel = L"Merge"; m.icon = "merge"; m.category = "pdf";
  m.accepts = [](const Selection& s, std::wstring* why) {
    if (!SelAllGroup(s, Group::Pdf)) { SelReason(why, L"Select PDF files."); return false; }
    if (s.count() < 2) { SelReason(why, L"Select two or more PDFs."); return false; }
    return true;
  };
  add(m, &MakeMergePdfs);

  ToolInfo o;
  o.id = "organize-pdf"; o.label = L"Organize PDF"; o.wheelLabel = L"Organize"; o.icon = "organize"; o.category = "pdf";
  o.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Pdf)) { SelReason(why, L"Select one PDF."); return false; }
    return true;
  };
  add(o, &MakeOrganizePdf);

  ToolInfo sp;
  sp.id = "split-pdf"; sp.label = L"Split PDF"; sp.wheelLabel = L"Split"; sp.icon = "split"; sp.category = "pdf";
  sp.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Pdf)) { SelReason(why, L"Select one PDF."); return false; }
    return true;
  };
  add(sp, &MakeSplitPdf);
}
