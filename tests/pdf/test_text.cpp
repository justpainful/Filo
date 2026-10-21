#include "pdf_testutil.h"
#include "core/job.h"
#include "engines/pdf/pdf_text.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <psapi.h>
#include <regex>

using namespace ptest;
using namespace filo;
using namespace filo::pdf;

static bool Extract(const fs::path& f, std::vector<PageContent>& out, const std::wstring& pw = L"", bool images = true) {
  std::wstring err;
  bool ok = ExtractPages(f, out, nullptr, &err, pw, images);
  if (!ok) std::printf("  ExtractPages failed: %ls\n", err.c_str());
  return ok;
}
static std::wstring AllText(const std::vector<PageContent>& v) {
  std::wstring s;
  for (auto& p : v) s += PageToPlainText(p) + L"\n";
  return s;
}

TEST(text_geometry_and_styles) {
  PageSpec p;
  p.texts.push_back({100, 200, 14, L"Plain Helvetica line", "Helvetica", 0x000000});
  p.texts.push_back({100, 240, 20, L"Bold blue heading", "Helvetica-Bold", 0x336699});
  p.texts.push_back({100, 280, 11, L"Italic serif text", "Times-Italic", 0xAA0000});
  p.texts.push_back({100, 320, 12, L"Mono Courier", "Courier", 0x000000});
  fs::path f = Scratch() / L"styles.pdf";
  CHECK(BuildPdf(f, {p}));
  std::vector<PageContent> pg;
  CHECK(Extract(f, pg));
  CHECK_EQ((int)pg.size(), 1);
  if (pg.empty()) return;
  auto& pc = pg[0];
  CHECK_NEAR(pc.width, 595, 0.01); CHECK_NEAR(pc.height, 842, 0.01);
  CHECK(pc.hasText); CHECK(!pc.looksScanned); CHECK_EQ(pc.columns, 1);
  CHECK_EQ((int)pc.runs.size(), 4);
  if (pc.runs.size() != 4) return;
  auto& a = pc.runs[0]; auto& b = pc.runs[1]; auto& c = pc.runs[2];
  CHECK(a.text == L"Plain Helvetica line");
  CHECK_NEAR(a.x, 100, 1.5); CHECK_NEAR(a.baseline, 214, 1.5); CHECK_NEAR(a.fontSize, 14, 0.2);
  CHECK(a.w > 80 && a.w < 140); CHECK(a.h > 10 && a.h < 20);
  CHECK(!a.bold && !a.italic); CHECK_EQ(a.rgb, 0u);
  CHECK(b.bold); CHECK_NEAR(b.fontSize, 20, 0.2); CHECK_EQ(b.rgb, 0x336699u);
  CHECK(c.italic); CHECK_EQ(c.rgb, 0xAA0000u);
  CHECK(!a.fontName.empty());
  // reading order top to bottom, distinct lines and blocks
  CHECK(pc.runs[0].line < pc.runs[1].line && pc.runs[1].line < pc.runs[2].line);
  CHECK(a.y < b.y && b.y < c.y);
  CHECK(a.angle == 0.f);
}

TEST(text_rotated_page_and_rotated_text) {
  // /Rotate 90 page whose text was drawn rotated 90deg ccw so that it reads horizontally on screen
  PageSpec p; p.rotate = 90;
  p.texts.push_back({150, 600, 18, L"First line on rotated page", "Helvetica", 0, 90});
  p.texts.push_back({120, 600, 18, L"Second line on rotated page", "Helvetica", 0, 90});
  fs::path f = Scratch() / L"rotpage.pdf";
  CHECK(BuildPdf(f, {p}));
  std::vector<PageContent> pg;
  CHECK(Extract(f, pg));
  if (pg.empty()) return;
  auto& pc = pg[0];
  CHECK_NEAR(pc.width, 842, 0.01); CHECK_NEAR(pc.height, 595, 0.01);
  CHECK_EQ(pc.rotation, 90);
  CHECK(pc.hasText);
  CHECK_EQ((int)pc.runs.size(), 2);
  for (auto& r : pc.runs) {
    CHECK(r.angle == 0.f);
    CHECK(r.w > r.h);
    CHECK(r.x >= 0 && r.x + r.w <= pc.width && r.y >= 0 && r.y + r.h <= pc.height);
  }
  std::wstring t = PageToPlainText(pc);
  CHECK(Contains(t, L"First line on rotated page"));
  CHECK(Contains(t, L"Second line on rotated page"));
  // upright page with text rotated 90deg: flagged as rotated text, still extracted
  PageSpec q; q.texts.push_back({300, 100, 16, L"Vertical text", "Helvetica", 0, 90});
  fs::path g = Scratch() / L"rottext.pdf";
  CHECK(BuildPdf(g, {q}));
  CHECK(Extract(g, pg));
  CHECK_EQ((int)pg[0].runs.size(), 1);
  if (!pg[0].runs.empty()) { CHECK_NEAR(pg[0].runs[0].angle, 90, 0.1); CHECK(pg[0].runs[0].h > pg[0].runs[0].w); CHECK(pg[0].runs[0].text == L"Vertical text"); }
}

TEST(text_two_column_reading_order) {
  std::vector<PageContent> pg;
  CHECK(Extract(Fixture("lo_twocol.pdf"), pg));
  CHECK_EQ((int)pg.size(), 1);
  if (pg.empty()) return;
  CHECK_EQ(pg[0].columns, 2);
  std::wstring t = PageToPlainText(pg[0]);
  // sentence tokens must come out as S001..S040 in order, then the full width footer
  std::wregex re(L"S(\\d{3})");
  std::vector<int> seq;
  for (auto it = std::wsregex_iterator(t.begin(), t.end(), re); it != std::wsregex_iterator(); ++it) seq.push_back(std::stoi((*it)[1]));
  std::vector<int> want; for (int i = 1; i <= 40; ++i) want.push_back(i); want.push_back(999);
  CHECK(seq == want);
  if (seq != want) { for (int s : seq) std::printf("%d ", s); std::printf("\n"); }
  CHECK(t.find(L"Two Column Report") < t.find(L"S001"));
  CHECK(t.find(L"abstract") < t.find(L"S001"));
  // runs: title in column 0 first, then left column, then right column
  int leftMax = -1, rightMin = 1 << 30;
  for (auto& r : pg[0].runs) {
    if (r.text.find(L"S0") == std::wstring::npos) continue;
    (r.x < 200 ? leftMax : rightMin) = r.x < 200 ? std::max(leftMax, r.line) : std::min(rightMin, r.line);
  }
  CHECK(leftMax < rightMin);
  for (auto& r : pg[0].runs) if (r.x > 300 && r.text.find(L"Sentence") != std::wstring::npos) CHECK_EQ(r.column, 1);
  // single column documents stay single column
  CHECK(Extract(Fixture("lo_rich.pdf"), pg));
  CHECK_EQ(pg[0].columns, 1);
}

TEST(text_table_detection_ruled_and_aligned) {
  std::vector<PageContent> pg;
  CHECK(Extract(Fixture("lo_table.pdf"), pg));
  if (pg.empty()) return;
  auto& pc = pg[0];
  CHECK(pc.rules.size() >= 10);
  auto tabs = DetectTables(pc);
  CHECK_EQ((int)tabs.size(), 2);
  if (tabs.size() != 2) return;
  const auto& t0 = tabs[0];
  CHECK(t0.ruled); CHECK_EQ(t0.rows, 5); CHECK_EQ(t0.cols, 4);
  CHECK(t0.headerRow);
  CHECK(t0.cells[0][0].text == L"Region" && t0.cells[0][3].text == L"Q3");
  CHECK(t0.cells[1][0].text == L"North" && t0.cells[1][2].text == L"135");
  CHECK(t0.cells[2][1].text == L"98");
  CHECK_EQ(t0.cells[3][0].colSpan, 2); CHECK(t0.cells[3][1].covered);
  CHECK(t0.cells[3][0].text == L"East + West merged");
  CHECK(t0.cells[3][3].text == L"88");
  CHECK(t0.cells[4][3].text == L"337");
  CHECK(t0.w > 400 && t0.h > 100);
  const auto& t1 = tabs[1];
  CHECK(!t1.ruled); CHECK_EQ(t1.rows, 5); CHECK_EQ(t1.cols, 3);
  CHECK(t1.cells[0][0].text == L"Item" && t1.cells[0][2].text == L"Price");
  CHECK(t1.cells[2][0].text == L"Gadget" && t1.cells[2][1].text == L"7" && t1.cells[2][2].text == L"12.00");
  CHECK(t1.cells[4][2].text == L"99.99");
  // runs know their table; table text is contiguous in reading order
  int in0 = 0, in1 = 0, firstOf0 = -1, lastOf0 = -1;
  for (size_t i = 0; i < pc.runs.size(); ++i) {
    if (pc.runs[i].table == 0) { ++in0; if (firstOf0 < 0) firstOf0 = (int)i; lastOf0 = (int)i; }
    if (pc.runs[i].table == 1) ++in1;
  }
  CHECK(in0 >= 19 && in1 >= 15);
  CHECK_EQ(lastOf0 - firstOf0 + 1, in0);
  std::wstring plain = PageToPlainText(pc);
  CHECK(Contains(plain, L"Region\tQ1\tQ2\tQ3"));
  CHECK(Contains(plain, L"East + West merged\t\t77\t88"));
  // negative checks: prose, lists and two columns are not tables
  for (const char* n : {"lo_twocol.pdf", "lo_rich.pdf", "lo_arabic.pdf"}) {
    std::vector<PageContent> o;
    CHECK(Extract(Fixture(n), o));
    for (auto& p : o) { CHECK(DetectTables(p).empty()); }
  }
  RecordResult("pdf:tables", true, "ruled grid with merged cell + aligned grid; no false positives on prose/lists/columns");
}

TEST(text_arabic_rtl) {
  std::vector<PageContent> pg;
  CHECK(Extract(Fixture("lo_arabic.pdf"), pg));
  if (pg.empty()) return;
  std::wstring t = PageToPlainText(pg[0]);
  // logical order exactly as typed (not visually reversed)
  CHECK(Contains(t, L"مرحبا بالعالم هذا"));          // مرحبا بالعالم هذا
  CHECK(Contains(t, L"لاستخراج"));                                                      // لاستخراج (lam-alef ligature restored)
  CHECK(Contains(t, L"الإنترنت"));                                                      // الإنترنت
  CHECK(Contains(t, L"12345"));
  CHECK(Contains(t, L"example"));
  CHECK(Contains(t, L"اللغة العربية"));                              // اللغة العربية
  int rtlRuns = 0;
  for (auto& r : pg[0].runs) if (r.rtl) ++rtlRuns;
  CHECK(rtlRuns >= 5);
  // the digits/latin run is embedded in the logical sequence of its line: Arabic, 12345, Arabic, example, Arabic
  size_t a = t.find(L"رقم الهاتف"), b = t.find(L"12345"), c = t.find(L"example");
  CHECK(a != std::wstring::npos && a < b && b < c);
  // lines are in top-to-bottom order
  CHECK(t.find(L"مرحبا") < t.find(L"اللغة العربية"));
  CHECK(t.find(L"اللغة العربية") < t.find(L"الصفحة"));
  RecordResult("pdf:arabic-extraction", true, "logical order, lam-alef ligature fixed, mixed digits/latin");
}

TEST(text_rich_document_styles_and_images) {
  std::vector<PageContent> pg;
  CHECK(Extract(Fixture("lo_rich.pdf"), pg));
  if (pg.empty()) return;
  auto& pc = pg[0];
  const TextRun *title = nullptr, *boldRun = nullptr, *italicRun = nullptr;
  for (auto& r : pc.runs) {
    if (r.text.find(L"Rich Document Title") != std::wstring::npos) title = &r;
    if (r.text.find(L"bold words") != std::wstring::npos) boldRun = &r;
    if (r.text.find(L"italic words") != std::wstring::npos) italicRun = &r;
  }
  CHECK(title && title->bold && title->fontSize > 20);
  CHECK(boldRun && boldRun->bold && !boldRun->italic);
  CHECK(italicRun && italicRun->italic && !italicRun->bold);
  CHECK_EQ((int)pc.images.size(), 1);
  if (pc.images.size() == 1) {
    auto& im = pc.images[0];
    CHECK_EQ(im.pixelW, 160); CHECK_EQ(im.pixelH, 100);
    CHECK_EQ(im.bgra.size(), (size_t)160 * 100 * 4);
    CHECK(im.w > 100 && im.h > 60);
    // gradient: left-top is dark blue-ish (x=0,y=0 -> r=0,g=0,b=128), right-bottom is bright
    auto px = [&](int x, int y, int c) { return (int)im.bgra[((size_t)y * 160 + x) * 4 + c]; };
    CHECK(px(2, 2, 2) < 40 && px(2, 2, 1) < 40);                    // R,G low in the top-left
    CHECK(px(157, 97, 2) > 200 && px(157, 97, 1) > 200);            // R,G high in the bottom-right (RGB: B=idx0, G=1, R=2)
    if (!im.rawJpeg.empty()) {
      CHECK(im.rawJpeg.size() > 100 && (uint8_t)im.rawJpeg[0] == 0xFF && (uint8_t)im.rawJpeg[1] == 0xD8);
      fs::path jf = Scratch() / L"extracted.jpg";
      WriteFileBytes(jf, std::string((const char*)im.rawJpeg.data(), im.rawJpeg.size()));
      Img dec;
      CHECK(DecodeImage(jf, dec));
      CHECK_EQ(dec.w, 160); CHECK_EQ(dec.h, 100);
      Img mine{160, 100, im.bgra};
      CHECK(MeanAbsDiff(dec, mine) < 3.0);                          // raw JPEG bytes decode to the same picture
    }
  }
  // images=false keeps geometry only
  CHECK(Extract(Fixture("lo_rich.pdf"), pg, L"", false));
  CHECK_EQ((int)pg[0].images.size(), 1);
  CHECK(pg[0].images[0].bgra.empty());
  CHECK(pg[0].images[0].w > 100);
}

static Img CropResize(const Bitmap& b, float x, float y, float w, float h, float scale, int outW, int outH) {
  // crop the rendering (at `scale` px per pt) to the box and box-filter it to outW x outH
  int x0 = (int)std::lround(x * scale), y0 = (int)std::lround(y * scale), cw = std::max(1, (int)std::lround(w * scale)), ch = std::max(1, (int)std::lround(h * scale));
  Img r; r.w = outW; r.h = outH; r.bgra.assign((size_t)outW * outH * 4, 255);
  for (int oy = 0; oy < outH; ++oy)
    for (int ox = 0; ox < outW; ++ox) {
      int sx = std::clamp(x0 + (int)((ox + 0.5) * cw / outW), 0, b.width - 1), sy = std::clamp(y0 + (int)((oy + 0.5) * ch / outH), 0, b.height - 1);
      std::memcpy(&r.bgra[((size_t)oy * outW + ox) * 4], &b.bgra[((size_t)sy * b.width + sx) * 4], 4);
    }
  return r;
}
static Img Resize(const Img& s, int outW, int outH) {
  Img r; r.w = outW; r.h = outH; r.bgra.resize((size_t)outW * outH * 4);
  for (int oy = 0; oy < outH; ++oy)
    for (int ox = 0; ox < outW; ++ox) {
      int sx = std::clamp((int)((ox + 0.5) * s.w / outW), 0, s.w - 1), sy = std::clamp((int)((oy + 0.5) * s.h / outH), 0, s.h - 1);
      std::memcpy(&r.bgra[((size_t)oy * outW + ox) * 4], &s.bgra[((size_t)sy * s.w + sx) * 4], 4);
    }
  return r;
}

TEST(text_image_orientation_matches_rendering) {
  // images drawn rotated / mirrored: the extracted pixels must look like what the page shows
  for (int mode = 0; mode < 6; ++mode) {
    PageSpec p; p.w = 400; p.h = 400;
    ImageSpec im{100, 120, 160, 100, 64, 40};   // stored image is always the wide 64x40 one
    im.rot90 = mode % 4; im.flipX = mode >= 4;
    if (im.rot90 & 1) { im.w = 100; im.h = 160; }
    p.images.push_back(im);
    fs::path f = Scratch() / (L"imgrot" + std::to_wstring(mode) + L".pdf");
    CHECK(BuildPdf(f, {p}));
    std::vector<PageContent> pg;
    CHECK(Extract(f, pg));
    if (pg.empty() || pg[0].images.size() != 1) { CHECK(false); continue; }
    auto& ib = pg[0].images[0];
    CHECK_NEAR(ib.x, im.x, 1.0); CHECK_NEAR(ib.y, im.y, 1.0); CHECK_NEAR(ib.w, im.w, 1.0); CHECK_NEAR(ib.h, im.h, 1.0);
    auto doc = PdfDocument::Open(f);
    Bitmap bm; RenderOptions ro; ro.dpi = 144;
    CHECK(doc.doc->RenderPage(0, ro, bm));
    Img shown = CropResize(bm, ib.x, ib.y, ib.w, ib.h, 2.f, 32, 32);
    Img mine = Resize(Img{ib.pixelW, ib.pixelH, ib.bgra}, 32, 32);
    double d = MeanAbsDiff(shown, mine);
    if (d > 14) std::printf("  mode %d (rot %d flip %d): diff %.1f\n", mode, im.rot90, (int)im.flipX, d);
    CHECK(d < 14);
    // the aspect of the extracted pixels equals the displayed box aspect
    CHECK_NEAR((double)ib.pixelW / ib.pixelH, (double)ib.w / ib.h, 0.05);
  }
}

TEST(text_scanned_detection_and_jpeg_images) {
  PageSpec scan; scan.images.push_back({10, 10, 575, 822, 100, 140, false, true, 85});
  PageSpec mixed; mixed.images.push_back({50, 400, 200, 100, 64, 32}); mixed.texts.push_back({72, 72, 12, L"Some real text on this page."});
  PageSpec blank;
  fs::path f = Scratch() / L"scan.pdf";
  CHECK(BuildPdf(f, {scan, mixed, blank}));
  std::vector<PageContent> pg;
  CHECK(Extract(f, pg));
  CHECK_EQ((int)pg.size(), 3);
  if (pg.size() != 3) return;
  CHECK(!pg[0].hasText); CHECK(pg[0].looksScanned);
  CHECK_EQ((int)pg[0].images.size(), 1);
  CHECK(!pg[0].images[0].rawJpeg.empty());            // JPEG-compressed scan: original bytes are exposed
  CHECK(pg[1].hasText); CHECK(!pg[1].looksScanned);
  CHECK(!pg[2].hasText); CHECK(!pg[2].looksScanned);
}

TEST(text_password_documents) {
  std::vector<PageContent> pg; std::wstring err;
  CHECK(!ExtractPages(Fixture("pw_aes256.pdf"), pg, nullptr, &err));
  CHECK(Contains(err, L"password"));
  CHECK(!ExtractPages(Fixture("pw_aes256.pdf"), pg, nullptr, &err, L"wrong"));
  CHECK(Contains(err, L"incorrect"));
  CHECK(ExtractPages(Fixture("pw_aes256.pdf"), pg, nullptr, &err, L"secret"));
  CHECK(Contains(AllText(pg), L"Rich Document Title"));
  CHECK(ExtractPages(Fixture("pw_rc4.pdf"), pg, nullptr, &err, L"secret"));
  CHECK(Contains(AllText(pg), L"Section One"));
}

static size_t CurMem() { PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc); return pmc.WorkingSetSize; }
// samples the working set while alive; Peak() = highest value seen
struct MemSampler {
  std::atomic<bool> stop{false};
  std::atomic<size_t> peak{0};
  std::thread th;
  MemSampler() { peak = CurMem(); th = std::thread([this] { while (!stop) { size_t c = CurMem(); size_t p = peak; while (c > p && !peak.compare_exchange_weak(p, c)) {} Sleep(2); } }); }
  ~MemSampler() { stop = true; th.join(); }
};

TEST(text_300_pages_speed_and_memory) {
  fs::path f = Big300();
  CHECK(fs::exists(f));
  Sleep(50);
  size_t before = CurMem();
  MemSampler mem;
  auto t0 = std::chrono::steady_clock::now();
  int pages = 0; size_t chars = 0; bool ordered = true; int lastPage = 0;
  std::wstring err;
  bool ok = ExtractPagesStreaming(f, [&](PageContent&& pc) {
    ++pages;
    chars += PageToPlainText(pc).size();
    if (pc.runs.empty() || pc.runs[0].text.find(L"Chapter page") == std::wstring::npos) ordered = false;
    lastPage = pc.index;
    return true;
  }, nullptr, &err);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  size_t after = mem.peak;
  CHECK(ok); CHECK_EQ(pages, 300); CHECK(ordered); CHECK_EQ(lastPage, 299);
  CHECK(chars > 300 * 44 * 80);
  double mb = (after > before ? after - before : 0) / 1048576.0;
  std::printf("  300 pages / %zu chars extracted in %.2f s, peak working-set growth %.0f MB\n", chars, secs, mb);
  CHECK(secs < 120);
  CHECK(mb < 500);
  RecordResult("pdf:300-page-extract", ok && pages == 300, "300 pages " + std::to_string((int)secs) + " s, +" + std::to_string((int)mb) + " MB (this build)");
}

TEST(text_cancellation_during_extraction) {
  fs::path f = Big300();
  JobSpec spec;
  spec.operation = L"test"; spec.title = L"cancel";
  std::wstring err;
  spec.work = [&](JobContext& ctx) -> JobResult {
    std::vector<PageContent> pg; std::wstring e;
    bool ok = ExtractPages(f, pg, &ctx, &e);
    if (!ok) return JobResult::Fail(e);
    return JobResult::Ok();
  };
  uint64_t id = JobManager::Get().Submit(std::move(spec));
  for (int i = 0; i < 400; ++i) {
    auto info = JobManager::Get().Info(id);
    if (info && info->progress > 0.05) break;
    Sleep(5);
  }
  double t0 = NowSeconds();
  JobManager::Get().Cancel(id);
  CHECK(JobManager::Get().WaitFor(id, 20000));
  double reaction = NowSeconds() - t0;
  auto info = JobManager::Get().Info(id);
  CHECK(info.has_value());
  if (info) { CHECK(info->state == JobState::Cancelled || info->state == JobState::Failed); CHECK(Contains(info->result.error, L"Cancelled") || info->state == JobState::Cancelled); }
  CHECK(reaction < 5.0);
  std::printf("  cancel reaction %.2f s\n", reaction);
  RecordResult("pdf:cancellation", reaction < 5.0, "extraction cancelled in " + std::to_string(reaction) + " s");
}
