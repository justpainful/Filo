#include "engines/pdf/pdf_ops.h"
#include "engines/pdf/pdf_outline.h"
#include "core/output.h"
#include "core/settings.h"
#include <algorithm>
#include <cwctype>

namespace filo::pdf {

namespace {

OverwritePolicy Policy() { return SettingsStore::Get().Snapshot().overwritePolicy; }

OpResult Fail(std::wstring msg) { OpResult r; r.error = std::move(msg); return r; }
OpResult Cancelled() { OpResult r; r.cancelled = true; r.error = L"Cancelled"; return r; }

std::wstring NameOf(const fs::path& p) { return p.filename().wstring(); }

// Opens a source and turns a failure into a sentence naming the file.
bool OpenSource(const PdfInput& in, std::unique_ptr<PdfDocument>& doc, std::wstring& err) {
  auto r = PdfDocument::Open(in.path, in.password);
  if (!r.ok()) { err = L"\"" + NameOf(in.path) + L"\": " + r.message; return false; }
  doc = std::move(r.doc);
  return true;
}

bool VerifyPageCount(const fs::path& file, int expected, std::wstring* why) {
  auto r = PdfDocument::Open(file);
  if (!r.ok()) { if (why) *why = L"The result could not be re-opened: " + r.message; return false; }
  if (r.doc->PageCount() != expected) { if (why) *why = L"The result has an unexpected number of pages."; return false; }
  return true;
}

std::vector<int> Iota(int a, int b) { std::vector<int> v; for (int i = a; i <= b; ++i) v.push_back(i); return v; }

} // namespace

// ---- ranges -----------------------------------------------------------------------------------------------------------

bool ParsePageRanges(const std::wstring& specIn, int pageCount, std::vector<std::pair<int, int>>& out, std::wstring* err) {
  out.clear();
  auto fail = [&](const std::wstring& m) { if (err) *err = m; return false; };
  std::wstring spec = ToLower(specIn);
  std::vector<std::wstring> parts;
  std::wstring cur;
  for (wchar_t c : spec) {
    if (c == L',' || c == L';' || c == 0x060C) { parts.push_back(cur); cur.clear(); }
    else if (!iswspace(c)) cur += c;
  }
  parts.push_back(cur);
  auto num = [&](const std::wstring& s, int& v) -> bool {
    if (s == L"last" || s == L"end") { v = pageCount; return true; }
    if (s == L"first") { v = 1; return true; }
    if (s.empty() || s.size() > 7) return false;
    for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
    v = std::stoi(s);
    return true;
  };
  for (auto& p : parts) {
    if (p.empty()) continue;
    int a = 0, b = 0;
    size_t dash = p.find_first_of(L"-–—");
    if (dash == std::wstring::npos) {
      if (!num(p, a)) return fail(L"\"" + p + L"\" is not a valid page number.");
      b = a;
    } else {
      std::wstring l = p.substr(0, dash), r = p.substr(dash + 1);
      if (l.empty() && r.empty()) return fail(L"\"" + p + L"\" is not a valid page range.");
      if (l.empty()) a = 1; else if (!num(l, a)) return fail(L"\"" + p + L"\" is not a valid page range.");
      if (r.empty()) b = pageCount; else if (!num(r, b)) return fail(L"\"" + p + L"\" is not a valid page range.");
      if (a > b) return fail(L"The page range " + p + L" is backwards.");
    }
    if (a < 1 || b < 1 || a > pageCount || b > pageCount)
      return fail(L"Page " + std::to_wstring(a < 1 || a > pageCount ? a : b) + L" is outside the document (it has " + std::to_wstring(pageCount) + L" page" + (pageCount == 1 ? L"" : L"s") + L").");
    out.push_back({a - 1, b - 1});
  }
  if (out.empty()) return fail(L"Enter the pages to use, for example 1-3,5,8-.");
  return true;
}

// ---- merge ------------------------------------------------------------------------------------------------------------

OpResult MergePdfs(const std::vector<fs::path>& ordered, const fs::path& desiredOutput, JobContext& ctx,
                   const std::map<fs::path, std::wstring>& passwords, const MergeOptions& opt) {
  if (ordered.empty()) return Fail(L"Add at least one PDF to merge.");
  OpResult res;
  OutputTransaction tx(desiredOutput, Policy());
  auto dest = PdfDocument::CreateNew();
  if (!dest) return Fail(L"The PDF engine could not be started.");

  struct Group { std::wstring name; int firstPage; std::vector<OutlineNode> nodes; };
  std::vector<Group> groups;
  int offset = 0;
  bool anyOutline = false;
  const size_t n = ordered.size();
  for (size_t i = 0; i < n; ++i) {
    if (ctx.Cancelled()) return Cancelled();
    PdfInput in{ordered[i], {}};
    auto it = passwords.find(ordered[i]);
    if (it != passwords.end()) in.password = it->second;
    std::unique_ptr<PdfDocument> src;
    std::wstring err;
    if (!OpenSource(in, src, err)) return Fail(err);
    int pc = src->PageCount();
    if (src->IsEncrypted()) res.warnings.push_back(L"\"" + NameOf(in.path) + L"\" was password-protected; the merged file is not.");
    std::wstring ierr;
    if (!dest->ImportPages(*src, Iota(0, pc - 1), -1, &ierr)) return Fail(L"\"" + NameOf(in.path) + L"\": " + ierr);
    if (opt.keepOutlines) {
      Group g{in.path.stem().wstring(), offset, ReadOutline(*src, offset)};
      if (!g.nodes.empty()) anyOutline = true;
      groups.push_back(std::move(g));
    }
    offset += pc;
    ctx.SetProgress(0.9 * (i + 1) / n);
  }
  if (ctx.Cancelled()) return Cancelled();
  std::wstring serr;
  if (!dest->Save(tx.TempPath(), &serr, ctx.CancelFlag(), true)) {
    if (ctx.Cancelled()) return Cancelled();
    return Fail(serr);
  }
  dest.reset();
  if (opt.keepOutlines && anyOutline) {
    std::vector<OutlineNode> roots;
    for (auto& g : groups) {
      OutlineNode top; top.title = g.name; top.page = g.firstPage;
      top.children = std::move(g.nodes);
      roots.push_back(std::move(top));
    }
    std::wstring oerr;
    if (!AppendOutlinesAndInfo(tx.TempPath(), roots, {}, &oerr)) res.warnings.push_back(L"The bookmarks of the source files could not be kept (" + oerr + L").");
  }
  ctx.SetProgress(0.97);
  std::wstring cerr;
  if (!tx.Commit(&cerr, [&](const fs::path& p, std::wstring* why) { return VerifyPageCount(p, offset, why); })) return Fail(cerr);
  res.ok = true; res.pages = offset; res.outputs = {tx.FinalPath()};
  ctx.SetProgress(1.0);
  return res;
}

// ---- split ------------------------------------------------------------------------------------------------------------

OpResult SplitPdf(const fs::path& input, const fs::path& desiredDir, const SplitOptions& opt, JobContext& ctx) {
  PdfInput in{input, opt.password};
  std::unique_ptr<PdfDocument> src;
  std::wstring err;
  if (!OpenSource(in, src, err)) return Fail(err);
  int pc = src->PageCount();
  std::vector<std::pair<int, int>> ranges;
  switch (opt.mode) {
    case SplitMode::SinglePages: for (int i = 0; i < pc; ++i) ranges.push_back({i, i}); break;
    case SplitMode::EveryN: {
      int n = std::max(1, opt.everyN);
      for (int i = 0; i < pc; i += n) ranges.push_back({i, std::min(pc, i + n) - 1});
      break;
    }
    case SplitMode::Ranges:
      if (!ParsePageRanges(opt.ranges, pc, ranges, &err)) return Fail(err);
      break;
  }
  if (ranges.empty()) return Fail(L"There are no pages to split.");
  OpResult res;
  if (src->IsEncrypted()) res.warnings.push_back(L"The source was password-protected; the split files are not.");
  OutputDirTransaction tx(desiredDir);
  int width = std::max<int>(3, (int)std::to_wstring(ranges.size()).size());
  std::wstring stem = input.stem().wstring();
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (ctx.Cancelled()) return Cancelled();
    auto out = PdfDocument::CreateNew();
    if (!out) return Fail(L"The PDF engine could not be started.");
    std::wstring e;
    if (!out->ImportPages(*src, Iota(ranges[i].first, ranges[i].second), -1, &e)) return Fail(e);
    wchar_t idx[32]; swprintf_s(idx, L"%0*d", width, (int)i + 1);
    fs::path file = tx.TempDir() / (stem + L"_" + idx + L".pdf");
    if (!out->Save(file, &e, ctx.CancelFlag(), true)) { if (ctx.Cancelled()) return Cancelled(); return Fail(e); }
    out.reset();
    if (!VerifyPageCount(file, ranges[i].second - ranges[i].first + 1, &e)) return Fail(e);
    ctx.SetProgress((i + 1.0) / ranges.size());
  }
  std::wstring cerr;
  if (!tx.Commit(&cerr)) return Fail(cerr);
  res.ok = true; res.pages = (int)ranges.size(); res.outputs = {tx.FinalDir()};
  return res;
}

// ---- organize ---------------------------------------------------------------------------------------------------------

OpResult OrganizePdf(const std::vector<PdfInput>& sources, const std::vector<PageRef>& pages, const fs::path& desiredOutput,
                     JobContext& ctx) {
  if (pages.empty()) return Fail(L"There are no pages left to save.");
  if (sources.empty()) return Fail(L"No source document was given.");
  OpResult res;
  // expand "duplicate"
  struct Item { int src, page, rot; };
  std::vector<Item> seq;
  for (auto& p : pages) {
    if (p.sourceDoc < 0 || p.sourceDoc >= (int)sources.size()) return Fail(L"A page refers to a document that is not in the list.");
    if (p.rotationDelta % 90 != 0) return Fail(L"Pages can only be rotated in steps of 90 degrees.");
    int rot = ((p.rotationDelta % 360) + 360) % 360;
    seq.push_back({p.sourceDoc, p.pageIndex, rot});
    if (p.duplicate) seq.push_back({p.sourceDoc, p.pageIndex, rot});
  }
  // open only the sources that are used
  std::vector<std::unique_ptr<PdfDocument>> docs(sources.size());
  for (auto& it : seq) {
    if (docs[it.src]) continue;
    std::wstring err;
    if (!OpenSource(sources[it.src], docs[it.src], err)) return Fail(err);
    if (docs[it.src]->IsEncrypted()) res.warnings.push_back(L"\"" + NameOf(sources[it.src].path) + L"\" was password-protected; the result is not.");
  }
  for (auto& it : seq)
    if (it.page < 0 || it.page >= docs[it.src]->PageCount()) return Fail(L"A page number is outside its document.");

  OutputTransaction tx(desiredOutput, Policy());
  auto dest = PdfDocument::CreateNew();
  if (!dest) return Fail(L"The PDF engine could not be started.");
  // one import call per run of consecutive pages from the same source (shared fonts/images are copied once per call)
  size_t i = 0;
  while (i < seq.size()) {
    if (ctx.Cancelled()) return Cancelled();
    size_t j = i;
    std::vector<int> idx;
    while (j < seq.size() && seq[j].src == seq[i].src) { idx.push_back(seq[j].page); ++j; }
    std::wstring e;
    if (!dest->ImportPages(*docs[seq[i].src], idx, -1, &e)) return Fail(e);
    i = j;
    ctx.SetProgress(0.8 * i / seq.size());
  }
  for (size_t k = 0; k < seq.size(); ++k)
    if (seq[k].rot && !dest->RotatePage((int)k, seq[k].rot)) return Fail(L"A page could not be rotated.");
  ctx.SetProgress(0.85);
  std::wstring serr;
  if (!dest->Save(tx.TempPath(), &serr, ctx.CancelFlag(), true)) { if (ctx.Cancelled()) return Cancelled(); return Fail(serr); }
  std::wstring cerr;
  int expected = (int)seq.size();
  if (!tx.Commit(&cerr, [&](const fs::path& p, std::wstring* why) { return VerifyPageCount(p, expected, why); })) return Fail(cerr);
  res.ok = true; res.pages = expected; res.outputs = {tx.FinalPath()};
  ctx.SetProgress(1.0);
  return res;
}

} // namespace filo::pdf
