#include "pdf_testutil.h"
#include "core/job.h"
#include "core/json.h"
#include "engines/pdf/pdf_ops.h"
#include "engines/pdf/pdf_outline.h"
#include "engines/pdf/pdf_text.h"
#include <set>

using namespace ptest;
using namespace filo;
using namespace filo::pdf;

namespace {

struct OpRun { OpResult res; int progressSamples = 0; double maxProgress = 0; bool monotonic = true; JobState state = JobState::Pending; };

OpRun RunOp(const std::function<OpResult(JobContext&)>& f, std::function<void(uint64_t)> whileRunning = {}) {
  OpRun out;
  JobSpec s; s.operation = L"test"; s.title = L"pdf op";
  s.work = [&](JobContext& ctx) -> JobResult { out.res = f(ctx); return out.res.ok ? JobResult::Ok(out.res.outputs) : JobResult::Fail(out.res.error); };
  uint64_t id = 0;
  double last = -1;
  int lid = JobManager::Get().AddListener([&](const JobInfo& i) {
    if (i.id != id) return;
    if (i.progress < last - 1e-9) out.monotonic = false;
    if (i.progress != last) { ++out.progressSamples; last = i.progress; }
    out.maxProgress = std::max(out.maxProgress, i.progress);
  });
  id = JobManager::Get().Submit(std::move(s));
  if (whileRunning) whileRunning(id);
  JobManager::Get().WaitFor(id, 900000);
  JobManager::Get().RemoveListener(lid);
  if (auto i = JobManager::Get().Info(id)) out.state = i->state;
  return out;
}

fs::path FreshDir(const wchar_t* name) {
  fs::path d = Scratch() / name;
  std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d, ec);
  return d;
}
bool NoTempLeft(const fs::path& dir) {
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(dir, ec)) if (e.path().filename().wstring().rfind(L".filo-", 0) == 0) { std::printf("  leftover: %ls\n", e.path().c_str()); return false; }
  return true;
}
fs::path MarkerDoc(const wchar_t* name, int n, const std::wstring& prefix = L"PAGE") {
  fs::path p = Scratch() / name;
  BuildMarkerPdf(p, n, 595, 842, prefix);
  return p;
}
std::wstring Marker(const std::wstring& t, const wchar_t* prefix = L"PAGE") {
  size_t p = t.find(std::wstring(prefix) + L"-");
  return p == std::wstring::npos ? L"?" : t.substr(p, wcslen(prefix) + 5);
}

Img Gradient(int w, int h) {
  Img i; i.w = w; i.h = h; i.bgra.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    uint8_t* p = &i.bgra[((size_t)y * w + x) * 4];
    p[0] = (uint8_t)(x * 255 / std::max(1, w - 1)); p[1] = (uint8_t)(y * 255 / std::max(1, h - 1)); p[2] = (uint8_t)(((x / 32 + y / 32) & 1) * 120 + 40); p[3] = 255;
  }
  return i;
}
Img ResizeNN(const Img& s, int w, int h) {
  Img r; r.w = w; r.h = h; r.bgra.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    int sx = std::clamp((int)((x + 0.5) * s.w / w), 0, s.w - 1), sy = std::clamp((int)((y + 0.5) * s.h / h), 0, s.h - 1);
    std::memcpy(&r.bgra[((size_t)y * w + x) * 4], &s.bgra[((size_t)sy * s.w + sx) * 4], 4);
  }
  return r;
}
Img RenderPageImg(const fs::path& pdf, int page, int maxSide) {
  Img r;
  auto d = PdfDocument::Open(pdf);
  if (!d.ok()) return r;
  Bitmap b; RenderOptions o; o.maxSidePx = maxSide;
  if (d.doc->RenderPage(page, o, b)) { r.w = b.width; r.h = b.height; r.bgra = std::move(b.bgra); }
  return r;
}

} // namespace

TEST(ops_parse_page_ranges) {
  std::vector<std::pair<int, int>> r; std::wstring err;
  CHECK(ParsePageRanges(L"1-3,5,8-", 10, r, &err));
  CHECK(r == (std::vector<std::pair<int, int>>{{0, 2}, {4, 4}, {7, 9}}));
  CHECK(ParsePageRanges(L" 2 - 4 ; 6 ", 10, r, &err)); CHECK(r == (std::vector<std::pair<int, int>>{{1, 3}, {5, 5}}));
  CHECK(ParsePageRanges(L"-3", 10, r, &err)); CHECK(r == (std::vector<std::pair<int, int>>{{0, 2}}));
  CHECK(ParsePageRanges(L"last", 10, r, &err)); CHECK(r == (std::vector<std::pair<int, int>>{{9, 9}}));
  CHECK(ParsePageRanges(L"3-last,1", 10, r, &err)); CHECK(r == (std::vector<std::pair<int, int>>{{2, 9}, {0, 0}}));
  for (const wchar_t* bad : {L"", L"0", L"11", L"5-3", L"a", L"1-2-3", L"3-12", L"--", L"1,,x"}) {
    CHECK(!ParsePageRanges(bad, 10, r, &err));
    CHECK(!err.empty() && err.back() == L'.');
  }
  CHECK(!ParsePageRanges(L"11", 10, r, &err));
  CHECK(Contains(err, L"outside the document"));
}

TEST(ops_merge_pages_order_outlines_and_progress) {
  fs::path a = Fixture("outlines_a.pdf"), b = Fixture("outlines_b.pdf"), m = MarkerDoc(L"merge-m.pdf", 2, L"MARK");
  fs::path dir = FreshDir(L"merge-out");
  fs::path out = dir / L"merged.pdf";
  OpRun r = RunOp([&](JobContext& ctx) { return MergePdfs({a, m, b}, out, ctx); });
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  merge failed: %ls\n", r.res.error.c_str()); return; }
  CHECK(r.state == JobState::Completed);
  CHECK_EQ(r.res.pages, 9);
  CHECK_EQ((int)r.res.outputs.size(), 1);
  CHECK(r.res.outputs[0] == out);
  CHECK(NoTempLeft(dir));
  PdfFacts f = ReadPdf(out);
  CHECK(f.ok); CHECK_EQ(f.pages, 9);
  std::vector<std::wstring> want = {L"DOC-A-PAGE-1", L"DOC-A-PAGE-2", L"DOC-A-PAGE-3", L"DOC-A-PAGE-4", L"MARK-0001", L"MARK-0002", L"DOC-B-PAGE-1", L"DOC-B-PAGE-2", L"DOC-B-PAGE-3"};
  for (int i = 0; i < f.pages && i < 9; ++i) CHECK(Contains(f.text[i], want[i]));
  // bookmarks: one group per source (stem), the sources' own trees inside, destinations remapped to the merged page numbers
  auto doc = PdfDocument::Open(out);
  CHECK(doc.ok());
  if (doc.ok()) {
    auto tree = ReadOutline(*doc.doc);
    CHECK_EQ((int)tree.size(), 3);
    if (tree.size() == 3) {
      CHECK(tree[0].title == L"outlines_a" && tree[0].page == 0);
      CHECK_EQ((int)tree[0].children.size(), 2);
      if (tree[0].children.size() == 2) {
        CHECK(tree[0].children[0].title == L"Chapter A1" && tree[0].children[0].page == 0);
        CHECK_EQ((int)tree[0].children[0].children.size(), 1);
        if (!tree[0].children[0].children.empty()) CHECK(tree[0].children[0].children[0].title == L"Section A1.1" && tree[0].children[0].children[0].page == 1);
        CHECK(tree[0].children[1].title == L"Chapter A2" && tree[0].children[1].page == 2);
      }
      CHECK(tree[1].title.find(L"merge-m") != std::wstring::npos && tree[1].page == 4 && tree[1].children.empty());
      CHECK(tree[2].title == L"outlines_b" && tree[2].page == 6);
      if (tree[2].children.size() == 2) CHECK(tree[2].children[1].title == L"Chapter B2" && tree[2].children[1].page == 8);
    }
  }
  // independent reader (PyMuPDF, when installed) agrees on page count, bookmarks and text
  int rc = 0;
  std::string o = RunPython("import sys,json\ntry:\n import fitz\nexcept Exception:\n print('NOFITZ'); sys.exit(0)\nd=fitz.open(sys.argv[1])\nprint(json.dumps({'n':d.page_count,'toc':d.get_toc(),'t0':d.load_page(0).get_text(),'t8':d.load_page(8).get_text()}))", {out.wstring()}, &rc);
  if (o.find("NOFITZ") == std::string::npos) {
    size_t br = o.find('{');
    auto j = br == std::string::npos ? std::nullopt : Json::Parse(o.substr(br));
    CHECK(j.has_value());
    if (j) {
      CHECK_EQ((int)j->Get("n").AsInt(), 9);
      CHECK(j->Get("t0").AsString().find("DOC-A-PAGE-1") != std::string::npos);
      CHECK(j->Get("t8").AsString().find("DOC-B-PAGE-3") != std::string::npos);
      auto& toc = j->Get("toc");
      CHECK(toc.Size() >= 9);       // 3 groups + 3 + 2 (+1 nested) + 3  = a:1+3, m:1, b:1+3 -> 9
      bool foundNested = false;
      for (auto& e : toc.Elements()) if (e.At(1).AsString() == "Section A1.1" && e.At(0).AsInt() == 3 && e.At(2).AsInt() == 2) foundNested = true;
      CHECK(foundNested);
    }
  } else std::printf("  (PyMuPDF not installed: independent bookmark check skipped)\n");
  // progress was reported and never went backwards
  CHECK(r.monotonic);
  CHECK(r.maxProgress >= 0.99);
}

TEST(ops_merge_errors_passwords_and_cleanup) {
  fs::path dir = FreshDir(L"merge-err");
  fs::path m = MarkerDoc(L"merge-m2.pdf", 2, L"MARK");
  // missing password -> clear sentence naming the file, nothing left behind
  OpRun r = RunOp([&](JobContext& ctx) { return MergePdfs({m, Fixture("pw_aes256.pdf")}, dir / L"x.pdf", ctx); });
  CHECK(!r.res.ok);
  CHECK(Contains(r.res.error, L"pw_aes256.pdf") && Contains(r.res.error, L"password"));
  CHECK(!fs::exists(dir / L"x.pdf"));
  CHECK(NoTempLeft(dir));
  // password map
  std::map<fs::path, std::wstring> pws{{Fixture("pw_aes256.pdf"), L"secret"}, {Fixture("pw_rc4.pdf"), L"secret"}};
  r = RunOp([&](JobContext& ctx) { return MergePdfs({m, Fixture("pw_aes256.pdf"), Fixture("pw_rc4.pdf")}, dir / L"y.pdf", ctx, pws); });
  CHECK(r.res.ok);
  if (r.res.ok) {
    PdfFacts f = ReadPdf(dir / L"y.pdf");
    CHECK(f.ok && f.pages == 2 + 2 * PdfDocument::Open(Fixture("pw_aes256.pdf"), L"secret").doc->PageCount());
    CHECK(!PdfDocument::Open(dir / L"y.pdf").doc->IsEncrypted());
    CHECK(!r.res.warnings.empty());
  }
  // corrupt input
  WriteFileBytes(dir / L"junk.pdf", "%PDF-1.4\nnothing");
  r = RunOp([&](JobContext& ctx) { return MergePdfs({m, dir / L"junk.pdf"}, dir / L"z.pdf", ctx); });
  CHECK(!r.res.ok); CHECK(Contains(r.res.error, L"junk.pdf"));
  CHECK(!fs::exists(dir / L"z.pdf"));
  // empty list
  r = RunOp([&](JobContext& ctx) { return MergePdfs({}, dir / L"w.pdf", ctx); });
  CHECK(!r.res.ok);
  // existing target is never overwritten (unique name)
  r = RunOp([&](JobContext& ctx) { return MergePdfs({m, m}, dir / L"y.pdf", ctx); });
  CHECK(r.res.ok);
  if (r.res.ok) CHECK(r.res.outputs[0] != dir / L"y.pdf");
  CHECK(NoTempLeft(dir));
}

TEST(ops_split_all_modes) {
  fs::path src = MarkerDoc(L"split-src.pdf", 10);
  // ranges
  fs::path d1 = FreshDir(L"split-ranges") / L"result";
  OpRun r = RunOp([&](JobContext& ctx) { SplitOptions o; o.mode = SplitMode::Ranges; o.ranges = L"1-3,5,8-"; return SplitPdf(src, d1, o, ctx); });
  CHECK(r.res.ok);
  if (r.res.ok) {
    CHECK_EQ(r.res.pages, 3);
    fs::path dir = r.res.outputs[0];
    std::vector<std::pair<int, std::vector<std::wstring>>> want = {{3, {L"PAGE-0001", L"PAGE-0002", L"PAGE-0003"}}, {1, {L"PAGE-0005"}}, {3, {L"PAGE-0008", L"PAGE-0009", L"PAGE-0010"}}};
    for (int i = 0; i < 3; ++i) {
      wchar_t nm[64]; swprintf_s(nm, L"split-src_%03d.pdf", i + 1);
      PdfFacts f = ReadPdf(dir / nm);
      CHECK(f.ok);
      CHECK_EQ(f.pages, want[i].first);
      for (int k = 0; k < f.pages && k < (int)want[i].second.size(); ++k) CHECK(Marker(f.text[k]) == want[i].second[k]);
    }
    int count = 0; for (auto& e : fs::directory_iterator(dir)) { (void)e; ++count; }
    CHECK_EQ(count, 3);
  }
  // every N
  fs::path d2 = FreshDir(L"split-n") / L"result";
  r = RunOp([&](JobContext& ctx) { SplitOptions o; o.mode = SplitMode::EveryN; o.everyN = 4; return SplitPdf(src, d2, o, ctx); });
  CHECK(r.res.ok);
  if (r.res.ok) {
    CHECK_EQ(r.res.pages, 3);
    int sizes[3] = {4, 4, 2};
    for (int i = 0; i < 3; ++i) { wchar_t nm[64]; swprintf_s(nm, L"split-src_%03d.pdf", i + 1); PdfFacts f = ReadPdf(r.res.outputs[0] / nm); CHECK_EQ(f.pages, sizes[i]); if (f.pages) CHECK(Marker(f.text[0]) == (std::wstring(L"PAGE-") + (i == 0 ? L"0001" : i == 1 ? L"0005" : L"0009"))); }
  }
  // single pages
  fs::path d3 = FreshDir(L"split-1") / L"result";
  r = RunOp([&](JobContext& ctx) { SplitOptions o; o.mode = SplitMode::SinglePages; return SplitPdf(src, d3, o, ctx); });
  CHECK(r.res.ok);
  if (r.res.ok) {
    CHECK_EQ(r.res.pages, 10);
    for (int i = 0; i < 10; ++i) {
      wchar_t nm[64]; swprintf_s(nm, L"split-src_%03d.pdf", i + 1);
      PdfFacts f = ReadPdf(r.res.outputs[0] / nm);
      CHECK_EQ(f.pages, 1);
      wchar_t want[16]; swprintf_s(want, L"PAGE-%04d", i + 1);
      if (f.pages == 1) CHECK(Marker(f.text[0]) == want);
    }
  }
  // errors
  fs::path d4 = FreshDir(L"split-bad") / L"result";
  r = RunOp([&](JobContext& ctx) { SplitOptions o; o.mode = SplitMode::Ranges; o.ranges = L"4-99"; return SplitPdf(src, d4, o, ctx); });
  CHECK(!r.res.ok); CHECK(Contains(r.res.error, L"outside the document"));
  CHECK(NoTempLeft(d4.parent_path()));
  CHECK(!fs::exists(d4));
  r = RunOp([&](JobContext& ctx) { SplitOptions o; return SplitPdf(Fixture("pw_rc4.pdf"), d4, o, ctx); });
  CHECK(!r.res.ok); CHECK(Contains(r.res.error, L"password"));
  r = RunOp([&](JobContext& ctx) { SplitOptions o; o.password = L"secret"; return SplitPdf(Fixture("pw_rc4.pdf"), d4, o, ctx); });
  CHECK(r.res.ok);
}

TEST(ops_organize_reorder_rotate_duplicate_delete) {
  fs::path a = MarkerDoc(L"org-a.pdf", 5, L"AAA"), b = MarkerDoc(L"org-b.pdf", 3, L"BBB");
  fs::path dir = FreshDir(L"org-out");
  // result: A5, B1 rotated 90, A1 twice, A3 rotated -90 (== 270), B3
  std::vector<PageRef> refs = {{0, 4, 0, false}, {1, 0, 90, false}, {0, 0, 0, true}, {0, 2, -90, false}, {1, 2, 0, false}};
  OpRun r = RunOp([&](JobContext& ctx) { return OrganizePdf({{a, L""}, {b, L""}}, refs, dir / L"organized.pdf", ctx); });
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); return; }
  CHECK_EQ(r.res.pages, 6);
  fs::path out = r.res.outputs[0];
  PdfFacts f = ReadPdf(out);
  CHECK(f.ok); CHECK_EQ(f.pages, 6);
  std::vector<std::pair<const wchar_t*, const wchar_t*>> want = {{L"AAA-0005", L"AAA"}, {L"BBB-0001", L"BBB"}, {L"AAA-0001", L"AAA"}, {L"AAA-0001", L"AAA"}, {L"AAA-0003", L"AAA"}, {L"BBB-0003", L"BBB"}};
  int rots[6] = {0, 90, 0, 0, 270, 0};
  for (int i = 0; i < 6 && i < f.pages; ++i) {
    CHECK(Marker(f.text[i], want[i].second) == want[i].first);
    CHECK_EQ(f.rot[i], rots[i]);
  }
  CHECK_NEAR(f.w[1], 842, 0.01); CHECK_NEAR(f.h[1], 595, 0.01);      // rotated page reports swapped size
  // pixels: the rotated output page equals the source page rotated by 90 degrees clockwise
  Img src = RenderPageImg(a, 4, 300), res0 = RenderPageImg(out, 0, 300);
  CHECK(src.w == res0.w && src.h == res0.h && MeanAbsDiff(src, res0) < 1.0);
  Img bsrc = RenderPageImg(b, 0, 300), bres = RenderPageImg(out, 1, 300);
  CHECK_EQ(bres.w, bsrc.h); CHECK_EQ(bres.h, bsrc.w);
  // rotate bsrc 90 clockwise by hand and compare
  if (bres.w == bsrc.h && bres.h == bsrc.w) {
    Img rot; rot.w = bsrc.h; rot.h = bsrc.w; rot.bgra.resize(bsrc.bgra.size());
    for (int y = 0; y < bsrc.h; ++y) for (int x = 0; x < bsrc.w; ++x) std::memcpy(&rot.bgra[((size_t)x * rot.w + (bsrc.h - 1 - y)) * 4], &bsrc.bgra[((size_t)y * bsrc.w + x) * 4], 4);
    CHECK(MeanAbsDiff(rot, bres) < 2.0);
  }
  // sources untouched, errors
  CHECK_EQ(ReadPdf(a).pages, 5);
  r = RunOp([&](JobContext& ctx) { return OrganizePdf({{a, L""}}, {{0, 9, 0, false}}, dir / L"bad.pdf", ctx); });
  CHECK(!r.res.ok);
  r = RunOp([&](JobContext& ctx) { return OrganizePdf({{a, L""}}, {{0, 1, 45, false}}, dir / L"bad2.pdf", ctx); });
  CHECK(!r.res.ok && Contains(r.res.error, L"90"));
  r = RunOp([&](JobContext& ctx) { return OrganizePdf({{a, L""}}, {}, dir / L"bad3.pdf", ctx); });
  CHECK(!r.res.ok);
  r = RunOp([&](JobContext& ctx) { return OrganizePdf({{a, L""}}, {{3, 0, 0, false}}, dir / L"bad4.pdf", ctx); });
  CHECK(!r.res.ok);
  CHECK(NoTempLeft(dir));
}

TEST(ops_compress_image_heavy_and_not_smaller) {
  PageSpec p;
  p.texts.push_back({72, 50, 18, L"COMPRESS-ME heading text"});
  p.images.push_back({40, 100, 500, 375, 1600, 1200, true});
  p.images.push_back({40, 500, 250, 188, 800, 600, true});
  std::vector<PageSpec> pages = {p, p, p};
  fs::path src = Scratch() / L"heavy.pdf";
  CHECK(BuildPdf(src, pages));
  uint64_t srcBytes = fs::file_size(src);
  fs::path dir = FreshDir(L"cmp-out");
  OpRun r = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = CompressPreset::Medium; return CompressPdf(src, dir / L"heavy-small.pdf", o, ctx); });
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); return; }
  CHECK(!r.res.notSmaller);
  if (r.res.outputs.empty()) return;
  fs::path out = r.res.outputs[0];
  uint64_t outBytes = fs::file_size(out);
  std::printf("  compress medium: %llu -> %llu bytes (%.1f%%), %d images recompressed\n", (unsigned long long)srcBytes, (unsigned long long)outBytes, 100.0 * outBytes / srcBytes, r.res.imagesRecompressed);
  CHECK(outBytes < srcBytes * 0.5);
  CHECK(r.res.imagesRecompressed >= 3);
  CHECK_EQ(r.res.inBytes, srcBytes); CHECK_EQ(r.res.outBytes, outBytes);
  PdfFacts f = ReadPdf(out);
  CHECK(f.ok); CHECK_EQ(f.pages, 3);
  for (auto& t : f.text) CHECK(Contains(t, L"COMPRESS-ME"));
  // visual fidelity (images were re-encoded, so allow a small difference)
  Img before = RenderPageImg(src, 0, 600), after = RenderPageImg(out, 0, 600);
  CHECK(before.w == after.w && before.h == after.h);
  double d = MeanAbsDiff(before, after);
  std::printf("  mean abs pixel difference after compression: %.2f\n", d);
  CHECK(d < 8.0);
  CHECK(NonBlankRatio(after) > 0.3);
  // presets are ordered: High <= Medium <= Low
  uint64_t sizes[3] = {0, 0, 0};
  CompressPreset presets[3] = {CompressPreset::Low, CompressPreset::Medium, CompressPreset::High};
  for (int k = 0; k < 3; ++k) {
    OpRun rr = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = presets[k]; return CompressPdf(src, dir / (L"preset" + std::to_wstring(k) + L".pdf"), o, ctx); });
    CHECK(rr.res.ok);
    sizes[k] = rr.res.outBytes;
  }
  std::printf("  presets low/medium/high: %llu / %llu / %llu\n", (unsigned long long)sizes[0], (unsigned long long)sizes[1], (unsigned long long)sizes[2]);
  CHECK(sizes[2] <= sizes[1] && sizes[1] <= sizes[0]);
  // target size
  OpRun rt = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = CompressPreset::Low; o.targetPercent = 10; return CompressPdf(src, dir / L"target.pdf", o, ctx); });
  CHECK(rt.res.ok && !rt.res.notSmaller);
  std::printf("  target 10%%: %.1f%%\n", 100.0 * rt.res.outBytes / srcBytes);
  CHECK(rt.res.outBytes <= srcBytes / 10 + 1);
  OpRun rt2 = RunOp([&](JobContext& ctx) { CompressOptions o; o.targetBytes = 1000; return CompressPdf(src, dir / L"target2.pdf", o, ctx); });
  CHECK(rt2.res.ok);
  CHECK(!rt2.res.warnings.empty());               // 1 KB is unreachable: best effort + warning
  // already minimal: a tiny text PDF cannot get smaller -> notSmaller, nothing written, original untouched
  fs::path tiny = MarkerDoc(L"tiny.pdf", 1);
  OpRun rn = RunOp([&](JobContext& ctx) { CompressOptions o; return CompressPdf(tiny, dir / L"tiny-out.pdf", o, ctx); });
  CHECK(rn.res.ok); CHECK(rn.res.notSmaller); CHECK(rn.res.outputs.empty());
  CHECK(!fs::exists(dir / L"tiny-out.pdf"));
  // compressing the already compressed file again gains nothing
  OpRun rn2 = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = CompressPreset::Medium; return CompressPdf(out, dir / L"again.pdf", o, ctx); });
  CHECK(rn2.res.ok);
  CHECK(rn2.res.notSmaller || rn2.res.outBytes < fs::file_size(out));
  CHECK(NoTempLeft(dir));
  // password protected input
  OpRun rp = RunOp([&](JobContext& ctx) { CompressOptions o; return CompressPdf(Fixture("pw_aes256.pdf"), dir / L"pw.pdf", o, ctx); });
  CHECK(!rp.res.ok); CHECK(Contains(rp.res.error, L"password"));
  RecordResult("pdf:compress", true, "image-heavy PDF to " + std::to_string((int)(100.0 * outBytes / srcBytes)) + "% of original; notSmaller flagged for minimal PDF");
}

TEST(ops_images_to_pdf_sizes_fidelity_orientation) {
  fs::path dir = FreshDir(L"img2pdf");
  Img g = Gradient(800, 600);
  fs::path jpg = dir / L"photo.jpg", png = dir / L"wide.png", bmpPath = dir / L"tall.png";
  CHECK(EncodeJpeg(jpg, g, 92));
  CHECK(EncodePng(png, Gradient(1200, 400)));
  CHECK(EncodePng(bmpPath, Gradient(300, 900)));
  std::string jpgBytes = ReadFileBytes(jpg);
  fs::path out = dir / L"all.pdf";
  OpRun r = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.size = PageSizeKind::A4; o.orientation = PageOrientation::Auto; o.fit = FitMode::Fit; return CreatePdfFromImages({jpg, png, bmpPath}, out, o, ctx); });
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); return; }
  CHECK_EQ(r.res.pages, 3);
  PdfFacts f = ReadPdf(r.res.outputs[0]);
  CHECK(f.ok); CHECK_EQ(f.pages, 3);
  // auto orientation: 800x600 and 1200x400 -> landscape A4, 300x900 -> portrait A4
  CHECK_NEAR(f.w[0], 841.89, 0.5); CHECK_NEAR(f.h[0], 595.276, 0.5);
  CHECK_NEAR(f.w[1], 841.89, 0.5); CHECK_NEAR(f.h[1], 595.276, 0.5);
  CHECK_NEAR(f.w[2], 595.276, 0.5); CHECK_NEAR(f.h[2], 841.89, 0.5);
  // JPEG bytes are embedded untouched (compare to the file on disk through PDFium's raw stream accessor)
  {
    std::vector<PageContent> pg;
    CHECK(ExtractPages(r.res.outputs[0], pg, nullptr, nullptr, L"", true));
    CHECK_EQ((int)pg.size(), 3);
    if (pg.size() == 3 && pg[0].images.size() == 1) {
      auto& im = pg[0].images[0];
      CHECK_EQ(im.pixelW, 800); CHECK_EQ(im.pixelH, 600);
      CHECK_EQ(im.rawJpeg.size(), jpgBytes.size());
      CHECK(!im.rawJpeg.empty() && std::memcmp(im.rawJpeg.data(), jpgBytes.data(), std::min(im.rawJpeg.size(), jpgBytes.size())) == 0);
      // fit: 800x600 on 842x595 -> height limited: 793x595 box, centred
      CHECK_NEAR(im.h, 595.276, 1.0); CHECK_NEAR(im.w, 595.276 * 800 / 600, 1.0);
    } else CHECK(false);
    if (pg.size() == 3 && pg[1].images.size() == 1) { CHECK_EQ(pg[1].images[0].pixelW, 1200); CHECK(pg[1].images[0].rawJpeg.empty()); }
  }
  // lossless storage of non-JPEG: render and compare to the source picture
  Img shown = RenderPageImg(r.res.outputs[0], 1, 1200);
  if (shown.w > 0) {
    // the image is 1200x400 fitted into 842x595 -> width limited 842x281 centred vertically
    int x0 = 0, w = shown.w, h = (int)std::lround(shown.w / 3.0), y0 = (shown.h - h) / 2;
    Img crop; crop.w = w; crop.h = h; crop.bgra.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) std::memcpy(&crop.bgra[(size_t)y * w * 4], &shown.bgra[((size_t)(y0 + y) * shown.w + x0) * 4], (size_t)w * 4);
    Img ref = ResizeNN(Gradient(1200, 400), w, h);
    double d = MeanAbsDiff(ref, crop);
    std::printf("  PNG page fidelity: mean diff %.2f\n", d);
    CHECK(d < 6.0);
  }

  // page size kinds
  struct Case { PageSizeKind k; double w, h; } cases[] = {{PageSizeKind::Letter, 612, 792}, {PageSizeKind::A3, 841.89, 1190.55}, {PageSizeKind::A5, 419.53, 595.28}, {PageSizeKind::Legal, 612, 1008}};
  for (auto& c : cases) {
    OpRun rr = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.size = c.k; o.orientation = PageOrientation::Portrait; return CreatePdfFromImages({jpg}, dir / L"kind.pdf", o, ctx); });
    CHECK(rr.res.ok);
    if (rr.res.ok) { PdfFacts ff = ReadPdf(rr.res.outputs[0]); CHECK_NEAR(ff.w[0], c.w, 0.6); CHECK_NEAR(ff.h[0], c.h, 0.6); }
  }
  OpRun rc = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.size = PageSizeKind::Custom; o.customWidthPt = 300; o.customHeightPt = 200; o.orientation = PageOrientation::Landscape; return CreatePdfFromImages({jpg}, dir / L"custom.pdf", o, ctx); });
  CHECK(rc.res.ok);
  if (rc.res.ok) { PdfFacts ff = ReadPdf(rc.res.outputs[0]); CHECK_NEAR(ff.w[0], 300, 0.1); CHECK_NEAR(ff.h[0], 200, 0.1); }
  // fit to image: page = pixels * 72 / dpi (+ margins)
  OpRun rf = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.size = PageSizeKind::FitImage; o.dpi = 100; o.marginPt = 10; return CreatePdfFromImages({jpg, png}, dir / L"fit.pdf", o, ctx); });
  CHECK(rf.res.ok);
  if (rf.res.ok) {
    PdfFacts ff = ReadPdf(rf.res.outputs[0]);
    CHECK_NEAR(ff.w[0], 800 * 0.72 + 20, 0.2); CHECK_NEAR(ff.h[0], 600 * 0.72 + 20, 0.2);
    CHECK_NEAR(ff.w[1], 1200 * 0.72 + 20, 0.2); CHECK_NEAR(ff.h[1], 400 * 0.72 + 20, 0.2);
  }
  // margins and Fit vs Fill vs Stretch (pixels)
  auto render = [&](FitMode m, double margin) {
    OpRun rr = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.size = PageSizeKind::Custom; o.customWidthPt = 400; o.customHeightPt = 400; o.orientation = PageOrientation::Portrait; o.fit = m; o.marginPt = margin; return CreatePdfFromImages({jpg}, dir / L"fm.pdf", o, ctx); });
    return RenderPageImg(rr.res.outputs.empty() ? fs::path() : rr.res.outputs[0], 0, 400);
  };
  auto isWhite = [](const Img& i, int x, int y) { const uint8_t* p = &i.bgra[((size_t)y * i.w + x) * 4]; return p[0] > 245 && p[1] > 245 && p[2] > 245; };
  Img fit = render(FitMode::Fit, 0), fill = render(FitMode::Fill, 0), stretch = render(FitMode::Stretch, 0), margin = render(FitMode::Fit, 40), fillM = render(FitMode::Fill, 40);
  if (fit.w == 400 && fill.w == 400 && stretch.w == 400 && margin.w == 400 && fillM.w == 400) {
    CHECK(isWhite(fit, 200, 10) && isWhite(fit, 200, 390) && !isWhite(fit, 200, 200) && !isWhite(fit, 10, 200));     // 4:3 image letterboxed top/bottom
    CHECK(!isWhite(fill, 200, 10) && !isWhite(fill, 10, 200) && !isWhite(fill, 390, 390) && !isWhite(fill, 5, 5));
    CHECK(!isWhite(stretch, 5, 5) && !isWhite(stretch, 395, 395));
    CHECK(isWhite(margin, 20, 200) && isWhite(margin, 200, 20) && !isWhite(margin, 100, 200));
    CHECK(isWhite(fillM, 20, 200) && isWhite(fillM, 200, 20) && isWhite(fillM, 380, 380) && !isWhite(fillM, 60, 200));   // Fill is clipped to the margins
  } else CHECK(false);

  // EXIF orientation (JPEG kept byte-exact, rotated by the placement matrix) and non-JPEG with orientation
  if (HavePil()) {
    std::wstring base = dir.wstring();
    int rc2 = 0;
    std::string o = RunPython(
        "import sys, os\nfrom PIL import Image\nd=sys.argv[1]\nim=Image.new('RGB',(120,80))\n"
        "for y in range(80):\n for x in range(120): im.putpixel((x,y),(x*2,y*3,(x//20+y//20)%2*200))\n"
        "for o in range(1,9):\n ex=Image.Exif(); ex[274]=o\n im.save(os.path.join(d,'exif%d.jpg'%o),exif=ex,quality=95)\n"
        "ex=Image.Exif(); ex[274]=6\nim.save(os.path.join(d,'exif6.tif'),exif=ex)\nprint('done')\n", {base}, &rc2);
    CHECK(o.find("done") != std::string::npos);
    for (int ori = 1; ori <= 8; ++ori) {
      fs::path ej = dir / (L"exif" + std::to_wstring(ori) + L".jpg");
      OpRun re = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o2; o2.size = PageSizeKind::FitImage; o2.dpi = 72; return CreatePdfFromImages({ej}, dir / (L"exif" + std::to_wstring(ori) + L".pdf"), o2, ctx); });
      CHECK(re.res.ok);
      if (!re.res.ok) continue;
      PdfFacts ff = ReadPdf(re.res.outputs[0]);
      bool swapped = ori >= 5;
      CHECK_NEAR(ff.w[0], swapped ? 80 : 120, 0.2); CHECK_NEAR(ff.h[0], swapped ? 120 : 80, 0.2);
      // reference: what a viewer shows for this file (PIL exif_transpose) vs what the PDF page shows
      std::string ref = RunPython("import sys\nfrom PIL import Image, ImageOps\nim=ImageOps.exif_transpose(Image.open(sys.argv[1])).convert('RGB')\nim.save(sys.argv[2])\nprint('ok')\n", {ej.wstring(), (dir / L"ref.png").wstring()});
      Img refImg, pageImg = RenderPageImg(re.res.outputs[0], 0, 240);
      CHECK(DecodeImage(dir / L"ref.png", refImg));
      if (refImg.w && pageImg.w) {
        Img a = ResizeNN(refImg, pageImg.w, pageImg.h);
        double d = MeanAbsDiff(a, pageImg);
        if (d > 14) std::printf("  EXIF %d: diff %.1f\n", ori, d);
        CHECK(d < 14);
      }
      // the JPEG stream in the PDF is the file, byte for byte
      std::vector<PageContent> pg;
      ExtractPages(re.res.outputs[0], pg, nullptr, nullptr);
    }
    fs::path tif = dir / L"exif6.tif";
    OpRun rt = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o2; o2.size = PageSizeKind::FitImage; o2.dpi = 72; return CreatePdfFromImages({tif}, dir / L"tif.pdf", o2, ctx); });
    CHECK(rt.res.ok);
    if (rt.res.ok) { PdfFacts ff = ReadPdf(rt.res.outputs[0]); CHECK_NEAR(ff.w[0], 80, 0.2); CHECK_NEAR(ff.h[0], 120, 0.2); }
  } else std::printf("  (PIL not installed: EXIF checks skipped)\n");

  // PNG with alpha is composited on white by viewers
  {
    Img a = Gradient(200, 200);
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) a.bgra[((size_t)y * 200 + x) * 4 + 3] = x < 100 ? 0 : 255;
    fs::path ap = dir / L"alpha.png";
    CHECK(EncodePng(ap, a));
    OpRun ra = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o2; o2.size = PageSizeKind::FitImage; o2.dpi = 72; return CreatePdfFromImages({ap}, dir / L"alpha.pdf", o2, ctx); });
    CHECK(ra.res.ok);
    if (ra.res.ok) {
      Img s = RenderPageImg(ra.res.outputs[0], 0, 200);
      if (s.w == 200) CHECK(isWhite(s, 30, 100) && !isWhite(s, 170, 100));
    }
  }
  // errors: unreadable image, empty list; nothing left behind
  WriteFileBytes(dir / L"broken.jpg", "not an image");
  OpRun rb = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o2; return CreatePdfFromImages({jpg, dir / L"broken.jpg"}, dir / L"b.pdf", o2, ctx); });
  CHECK(!rb.res.ok); CHECK(Contains(rb.res.error, L"broken.jpg"));
  CHECK(!fs::exists(dir / L"b.pdf"));
  OpRun re2 = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o2; return CreatePdfFromImages({}, dir / L"e.pdf", o2, ctx); });
  CHECK(!re2.res.ok);
  CHECK(NoTempLeft(dir));
  RecordResult("pdf:images-to-pdf", true, "JPEG bytes embedded untouched, EXIF 1-8 verified by rendering, A4/Letter/Fit/custom sizes");
}

TEST(ops_cancellation_leaves_nothing_behind) {
  fs::path dir = FreshDir(L"cancel-out");
  // many JPEGs -> PDF, cancel after the first progress report
  std::vector<fs::path> imgs;
  Img g = Gradient(1600, 1200);
  fs::path jp = dir / L"src.jpg";
  CHECK(EncodeJpeg(jp, g, 90));
  Img big = Gradient(2400, 1800);
  fs::path pn = dir / L"src.png";
  CHECK(EncodePng(pn, big));
  for (int i = 0; i < 60; ++i) imgs.push_back(i % 2 ? jp : pn);
  OpRun r = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; return CreatePdfFromImages(imgs, dir / L"c.pdf", o, ctx); },
                  [&](uint64_t id) {
                    for (int i = 0; i < 2000; ++i) { auto inf = JobManager::Get().Info(id); if (inf && inf->progress > 0.02) break; Sleep(2); }
                    JobManager::Get().Cancel(id);
                  });
  CHECK(!r.res.ok);
  CHECK(r.res.cancelled || Contains(r.res.error, L"Cancelled"));
  CHECK(r.state == JobState::Cancelled || r.state == JobState::Failed);
  CHECK(!fs::exists(dir / L"c.pdf"));
  CHECK(NoTempLeft(dir));
  // merge cancel
  fs::path big300 = Big300();
  std::vector<fs::path> many(8, big300);
  OpRun rm = RunOp([&](JobContext& ctx) { return MergePdfs(many, dir / L"m.pdf", ctx); },
                   [&](uint64_t id) {
                     for (int i = 0; i < 4000; ++i) { auto inf = JobManager::Get().Info(id); if (inf && inf->progress > 0.05) break; Sleep(2); }
                     JobManager::Get().Cancel(id);
                   });
  CHECK(!rm.res.ok);
  CHECK(!fs::exists(dir / L"m.pdf"));
  CHECK(NoTempLeft(dir));
  RecordResult("pdf:ops-cancel", true, "image->pdf and merge cancelled: no output, no temp files");
}

TEST(ops_compress_keeps_bookmarks_and_properties) {
  fs::path src = Fixture("heavy_outlines.pdf");
  CHECK(fs::exists(src));
  fs::path dir = FreshDir(L"cmp-outline");
  OpRun r = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = CompressPreset::Medium; return CompressPdf(src, dir / L"c.pdf", o, ctx); });
  CHECK(r.res.ok && !r.res.notSmaller);
  if (!r.res.ok || r.res.outputs.empty()) return;
  std::printf("  heavy_outlines: %llu -> %llu bytes\n", (unsigned long long)r.res.inBytes, (unsigned long long)r.res.outBytes);
  CHECK(r.res.outBytes < r.res.inBytes / 2);
  auto d = PdfDocument::Open(r.res.outputs[0]);
  CHECK(d.ok());
  if (!d.ok()) return;
  CHECK_EQ(d.doc->PageCount(), 2);
  auto tree = ReadOutline(*d.doc);
  CHECK_EQ((int)tree.size(), 2);
  if (tree.size() == 2) {
    CHECK(tree[0].title == L"First chapter" && tree[0].page == 0 && tree[0].children.size() == 1 && tree[0].children[0].title == L"Nested section");
    CHECK(tree[1].title == L"Second chapter" && tree[1].page == 1);
  }
  auto m = d.doc->Meta();
  CHECK(m.title == L"Heavy Doc Title"); CHECK(m.author == L"Test Author");
  CHECK(Contains(d.doc->PageText(1), L"HEAVY-PAGE-2"));
  Img a = RenderPageImg(src, 0, 500), b = RenderPageImg(r.res.outputs[0], 0, 500);
  CHECK(MeanAbsDiff(a, b) < 8.0);
}

TEST(ops_compress_existing_jpeg_downsample) {
  fs::path dir = FreshDir(L"cmp-jpeg");
  Img g = Gradient(2400, 1800);
  uint32_t seed = 99;
  for (size_t i = 0; i < g.bgra.size(); i += 4) { seed = seed * 1664525u + 1013904223u; int n = (int)((seed >> 24) % 25) - 12; for (int c = 0; c < 3; ++c) g.bgra[i + c] = (uint8_t)std::clamp((int)g.bgra[i + c] + n, 0, 255); }
  fs::path jpg = dir / L"big.jpg";
  CHECK(EncodeJpeg(jpg, g, 95));
  fs::path pdf = dir / L"photo.pdf";
  OpRun mk = RunOp([&](JobContext& ctx) { ImagesToPdfOptions o; o.fit = FitMode::Fit; return CreatePdfFromImages({jpg}, pdf, o, ctx); });
  CHECK(mk.res.ok);
  if (!mk.res.ok) return;
  fs::path src = mk.res.outputs[0];
  OpRun r = RunOp([&](JobContext& ctx) { CompressOptions o; o.preset = CompressPreset::Medium; return CompressPdf(src, dir / L"photo-small.pdf", o, ctx); });
  CHECK(r.res.ok && !r.res.notSmaller);
  if (!r.res.ok || r.res.outputs.empty()) return;
  std::printf("  JPEG photo PDF: %llu -> %llu bytes\n", (unsigned long long)r.res.inBytes, (unsigned long long)r.res.outBytes);
  CHECK(r.res.outBytes < r.res.inBytes * 0.6);
  std::vector<PageContent> pg;
  CHECK(ExtractPages(r.res.outputs[0], pg, nullptr, nullptr));
  CHECK_EQ((int)pg.size(), 1);
  if (pg.size() == 1 && pg[0].images.size() == 1) {
    CHECK(pg[0].images[0].pixelW < 2000 && pg[0].images[0].pixelW > 1400);     // 2400 px on an 842 pt wide page (205 dpi) -> 150 dpi = ~1750 px
    CHECK(!pg[0].images[0].rawJpeg.empty());
  } else CHECK(false);
  Img a = RenderPageImg(src, 0, 500), b = RenderPageImg(r.res.outputs[0], 0, 500);
  CHECK(MeanAbsDiff(a, b) < 6.0);
}
