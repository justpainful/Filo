#include "pdf_testutil.h"
#include "core/job.h"
#include "core/json.h"
#include "engines/engines.h"
#include "engines/pdf/pdf_ocr.h"
#include "engines/pdf/pdf_text.h"
#include "engines/pdf/pdf_ops.h"
#include "registry/capability.h"
#include <chrono>
#include <regex>

using namespace ptest;
using namespace filo;
using namespace filo::pdf;

namespace {

fs::path FreshDir(const wchar_t* name) {
  fs::path d = Scratch() / name;
  std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d, ec);
  return d;
}

struct ConvRun { JobResult res; JobState state = JobState::Pending; double seconds = 0; };

const Capability* Cap(const char* id) {
  RegisterPdfEngine();
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == id) return &c;
  return nullptr;
}

ConvRun Convert(const char* id, const fs::path& input, Json options, const fs::path& forcedOut, const std::function<void(uint64_t)>& whileRunning = {}) {
  ConvRun out;
  const Capability* cap = Cap(id);
  if (!cap) { out.res = JobResult::Fail(L"capability missing"); return out; }
  JobSpec s; s.operation = L"convert"; s.title = Widen(id); s.inputs = {input};
  s.work = [=, &out](JobContext& ctx) -> JobResult { out.res = RunConversion(*cap, input, options, ctx, &forcedOut); return out.res; };
  double t0 = NowSeconds();
  uint64_t jid = JobManager::Get().Submit(std::move(s));
  if (whileRunning) whileRunning(jid);
  JobManager::Get().WaitFor(jid, 1200000);
  out.seconds = NowSeconds() - t0;
  if (auto i = JobManager::Get().Info(jid)) { out.state = i->state; out.res = i->result; }
  return out;
}

Json Opts(std::initializer_list<std::pair<const char*, Json>> kv) {
  Json o = Json::MakeObject();
  for (auto& [k, v] : kv) o[k] = v;
  return o;
}

std::string ReadText(const fs::path& p) { return ReadFileBytes(p); }
std::wstring Utf8ToW(const std::string& s) { return Widen(s); }

struct PilInfo { bool ok = false; int w = 0, h = 0; std::string mode, format; double nonwhite = 0; };
PilInfo Pil(const fs::path& f) {
  PilInfo p;
  if (!HavePil()) return p;
  std::string o = RunPython("import sys,json\nfrom PIL import Image\nim=Image.open(sys.argv[1]); im.load()\ng=im.convert('L'); h=g.histogram()\n"
                            "print(json.dumps({'w':im.width,'h':im.height,'mode':im.mode,'format':im.format,'nonwhite':sum(h[:230])/float(im.width*im.height)}))", {f.wstring()});
  size_t b = o.find('{');
  if (b == std::string::npos) return p;
  auto j = Json::Parse(o.substr(b));
  if (!j) return p;
  p.ok = true; p.w = (int)j->Get("w").AsInt(); p.h = (int)j->Get("h").AsInt(); p.mode = j->Get("mode").AsString(); p.format = j->Get("format").AsString(); p.nonwhite = j->Get("nonwhite").AsNumber();
  return p;
}

fs::path MixedPdf() {
  fs::path f = Scratch() / L"mixed.pdf";
  PageSpec a; a.texts.push_back({72, 72, 30, L"Page one A4 portrait", "Helvetica-Bold", 0x000000});
  PageSpec b; b.w = 792; b.h = 612; b.texts.push_back({72, 72, 30, L"Page two letter landscape"}); b.lines.push_back({72, 140, 700, 140, 4});
  PageSpec c; c.w = 300; c.h = 500; c.texts.push_back({20, 20, 20, L"Page three small"});
  PageSpec d; d.rotate = 90; d.texts.push_back({100, 600, 24, L"Page four rotated", "Helvetica", 0, 90});
  BuildPdf(f, {a, b, c, d});
  return f;
}

} // namespace

TEST(conv_registered_capabilities) {
  const char* ids[] = {"pdf>jpg", "pdf>png", "pdf>txt", "pdf>md"};
  for (auto id : ids) {
    const Capability* c = Cap(id);
    CHECK(c != nullptr);
    if (!c) continue;
    CHECK(c->group == Group::Pdf); CHECK(c->in == L"pdf"); CHECK(c->engine == "pdfium");
    CHECK(c->Runnable()); CHECK(!c->limitation.empty()); CHECK(!c->options.empty()); CHECK(c->verify != nullptr);
  }
  CHECK(Cap("pdf>png")->producesFolder && Cap("pdf>jpg")->producesFolder && Cap("pdf>md")->producesFolder && !Cap("pdf>txt")->producesFolder);
  CHECK(Cap("pdf>docx") == nullptr && Cap("pdf>pages") == nullptr);          // document engine / not offered
  CHECK(CapabilityRegistry::Get().Find(L"pdf", L"png") == Cap("pdf>png"));
  bool hasDpi = false; for (auto& o : Cap("pdf>png")->options) if (o.id == "dpi") { hasDpi = true; CHECK_NEAR(o.def, 300, 0.1); }
  CHECK(hasDpi);
}

TEST(conv_pdf_to_png_all_pages_sizes_and_transparency) {
  fs::path src = MixedPdf();
  fs::path dir = FreshDir(L"conv-png");
  ConvRun r = Convert("pdf>png", src, Json::MakeObject(), dir / L"mixed.png");
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); RecordResult("pdf>png", false, "failed"); return; }
  CHECK(r.state == JobState::Completed);
  fs::path out = r.res.outputs[0];
  CHECK(out.filename() == L"mixed");
  struct E { const char* name; double w, h; } want[] = {{"page-001.png", 595, 842}, {"page-002.png", 792, 612}, {"page-003.png", 300, 500}, {"page-004.png", 842, 595}};
  int files = 0; for (auto& e : fs::directory_iterator(out)) { (void)e; ++files; }
  CHECK_EQ(files, 4);
  bool allOk = true;
  for (auto& e : want) {
    fs::path f = out / e.name;
    CHECK(fs::exists(f));
    Img im;
    bool dec = DecodeImage(f, im);
    CHECK(dec);
    if (!dec) { allOk = false; continue; }
    double ew = e.w * 300 / 72, eh = e.h * 300 / 72;
    CHECK(std::abs(im.w - ew) <= 1.0); CHECK(std::abs(im.h - eh) <= 1.0);
    CHECK(NonBlankRatio(im) > 0.0005);
    CHECK_EQ((int)im.bgra[0], 255);                                // white page, opaque
    PilInfo p = Pil(f);
    if (p.ok) { CHECK(std::abs(p.w - ew) <= 1.0 && std::abs(p.h - eh) <= 1.0); CHECK(p.format == "PNG"); CHECK(p.nonwhite > 0.0005); }
    if (std::abs(im.w - ew) > 1.0 || std::abs(im.h - eh) > 1.0) allOk = false;
  }
  // option dpi + page subset + transparency
  fs::path dir2 = FreshDir(L"conv-png2");
  ConvRun r2 = Convert("pdf>png", src, Opts({{"dpi", 100}, {"transparent", true}, {"pages", "2-3"}}), dir2 / L"t.png");
  CHECK(r2.res.ok);
  if (r2.res.ok) {
    fs::path o2 = r2.res.outputs[0];
    int n = 0; for (auto& e : fs::directory_iterator(o2)) { (void)e; ++n; }
    CHECK_EQ(n, 2);
    CHECK(fs::exists(o2 / L"page-002.png") && fs::exists(o2 / L"page-003.png"));
    Img im; CHECK(DecodeImage(o2 / L"page-002.png", im));
    CHECK(std::abs(im.w - 792 * 100 / 72.0) <= 1.0);
    CHECK_EQ((int)im.bgra[3], 0);                                   // transparent corner
    PilInfo p = Pil(o2 / L"page-002.png");
    if (p.ok) CHECK(p.mode == "RGBA");
  }
  // 'transparent' off -> RGB (no alpha channel written)
  PilInfo p0 = Pil(out / L"page-001.png");
  if (p0.ok) CHECK(p0.mode == "RGB");
  // bad inputs
  ConvRun bad = Convert("pdf>png", Fixture("pw_aes256.pdf"), Json::MakeObject(), dir / L"pw.png");
  CHECK(!bad.res.ok); CHECK(Contains(bad.res.error, L"password"));
  CHECK(!fs::exists(dir / L"pw"));
  ConvRun good = Convert("pdf>png", Fixture("pw_aes256.pdf"), Opts({{"password", "secret"}, {"dpi", 50}}), dir / L"pw2.png");
  CHECK(good.res.ok);
  ConvRun rng = Convert("pdf>png", src, Opts({{"pages", "9"}}), dir / L"rng.png");
  CHECK(!rng.res.ok); CHECK(Contains(rng.res.error, L"outside the document"));
  RecordResult("pdf>png", allOk && r.res.ok, "4 mixed-size/rotated pages at 300 dpi, sizes = pt*dpi/72 +-1, transparency option");
}

TEST(conv_pdf_to_jpg_background_quality_dpi) {
  fs::path src = MixedPdf();
  fs::path dir = FreshDir(L"conv-jpg");
  ConvRun r = Convert("pdf>jpg", src, Opts({{"dpi", 150}, {"quality", 85}}), dir / L"mixed.jpg");
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); RecordResult("pdf>jpg", false, "failed"); return; }
  fs::path out = r.res.outputs[0];
  bool allOk = true;
  double sizes[4][2] = {{595, 842}, {792, 612}, {300, 500}, {842, 595}};
  for (int i = 0; i < 4; ++i) {
    wchar_t nm[32]; swprintf_s(nm, L"page-%03d.jpg", i + 1);
    fs::path f = out / nm;
    Img im;
    CHECK(DecodeImage(f, im));
    CHECK(std::abs(im.w - sizes[i][0] * 150 / 72) <= 1.0); CHECK(std::abs(im.h - sizes[i][1] * 150 / 72) <= 1.0);
    CHECK_EQ((int)im.bgra[3], 255);
    CHECK(im.bgra[0] > 250 && im.bgra[1] > 250 && im.bgra[2] > 250);        // white background (no black/transparent flatten)
    CHECK(NonBlankRatio(im) > 0.0005);
    PilInfo p = Pil(f);
    if (p.ok) { CHECK(p.format == "JPEG"); CHECK(p.mode == "RGB"); CHECK(std::abs(p.w - sizes[i][0] * 150 / 72) <= 1.0); }
    else if (i == 0) std::printf("  (PIL missing: independent JPEG check skipped)\n");
    if (!(std::abs(im.w - sizes[i][0] * 150 / 72) <= 1.0)) allOk = false;
  }
  bool hasWhiteNote = false;
  for (auto& w : r.res.warnings) if (Contains(w, L"white")) hasWhiteNote = true;
  CHECK(hasWhiteNote);
  // quality affects file size
  fs::path dir2 = FreshDir(L"conv-jpg2");
  ConvRun lo = Convert("pdf>jpg", Fixture("lo_rich.pdf"), Opts({{"dpi", 150}, {"quality", 20}}), dir2 / L"lo.jpg");
  ConvRun hi = Convert("pdf>jpg", Fixture("lo_rich.pdf"), Opts({{"dpi", 150}, {"quality", 98}}), dir2 / L"hi.jpg");
  CHECK(lo.res.ok && hi.res.ok);
  if (lo.res.ok && hi.res.ok) CHECK(fs::file_size(lo.res.outputs[0] / L"page-001.jpg") * 2 < fs::file_size(hi.res.outputs[0] / L"page-001.jpg"));
  RecordResult("pdf>jpg", allOk, "4 pages at 150 dpi, white background, quality option, PIL-verified");
}

TEST(conv_pdf_to_images_300_pages_speed) {
  fs::path big = Big300();
  fs::path dir = FreshDir(L"conv-300");
  ConvRun r = Convert("pdf>png", big, Opts({{"dpi", 60}}), dir / L"big.png");
  CHECK(r.res.ok);
  if (r.res.ok) {
    int n = 0; for (auto& e : fs::directory_iterator(r.res.outputs[0])) { (void)e; ++n; }
    CHECK_EQ(n, 300);
    CHECK(fs::exists(r.res.outputs[0] / L"page-300.png"));
    std::printf("  300 pages -> PNG at 60 dpi in %.1f s\n", r.seconds);
  }
}

TEST(conv_cancel_pdf_to_png_removes_temp_folder) {
  fs::path big = Big300();
  fs::path dir = FreshDir(L"conv-cancel");
  ConvRun r = Convert("pdf>png", big, Opts({{"dpi", 150}}), dir / L"c.png", [&](uint64_t id) {
    for (int i = 0; i < 4000; ++i) { auto inf = JobManager::Get().Info(id); if (inf && inf->progress > 0.05) break; Sleep(2); }
    JobManager::Get().Cancel(id);
  });
  CHECK(!r.res.ok);
  CHECK(r.state == JobState::Cancelled || r.state == JobState::Failed);
  CHECK(!fs::exists(dir / L"c"));
  int left = 0; for (auto& e : fs::directory_iterator(dir)) { (void)e; ++left; }
  CHECK_EQ(left, 0);
}

TEST(conv_pdf_to_txt_content_order_bom_pages) {
  fs::path dir = FreshDir(L"conv-txt");
  ConvRun r = Convert("pdf>txt", Fixture("lo_twocol.pdf"), Json::MakeObject(), dir / L"twocol.txt");
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); RecordResult("pdf>txt", false, "failed"); return; }
  std::string bytes = ReadText(r.res.outputs[0]);
  CHECK(bytes.size() > 100 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB && (uint8_t)bytes[2] == 0xBF);
  std::wstring t = Utf8ToW(bytes.substr(3));
  CHECK(Contains(t, L"\r\n")); CHECK(!Contains(t, L"\n\n\n\n"));
  std::wregex re(L"S(\\d{3})");
  std::vector<int> seq;
  for (auto it = std::wsregex_iterator(t.begin(), t.end(), re); it != std::wsregex_iterator(); ++it) seq.push_back(std::stoi((*it)[1]));
  std::vector<int> want; for (int i = 1; i <= 40; ++i) want.push_back(i); want.push_back(999);
  CHECK(seq == want);
  CHECK(t.find(L"Two Column Report") == 0);
  // verify() agrees
  std::wstring why;
  CHECK(Cap("pdf>txt")->verify(r.res.outputs[0], &why));
  // multi page: form feed lines between pages (default) or blank lines
  fs::path five = Scratch() / L"markers5.pdf";
  if (!fs::exists(five)) BuildMarkerPdf(five, 5);
  ConvRun m = Convert("pdf>txt", five, Json::MakeObject(), dir / L"five.txt");
  CHECK(m.res.ok);
  if (m.res.ok) {
    std::wstring mt = Utf8ToW(ReadText(m.res.outputs[0]).substr(3));
    int ff = 0; for (wchar_t c : mt) if (c == L'\f') ++ff;
    CHECK_EQ(ff, 4);
    size_t p1 = mt.find(L"PAGE-0001"), p2 = mt.find(L"PAGE-0002"), f1 = mt.find(L'\f');
    CHECK(p1 < f1 && f1 < p2);
  }
  ConvRun mb = Convert("pdf>txt", five, Opts({{"pageBreak", "blank"}}), dir / L"five-blank.txt");
  CHECK(mb.res.ok);
  if (mb.res.ok) { std::wstring mt = Utf8ToW(ReadText(mb.res.outputs[0]).substr(3)); CHECK(mt.find(L'\f') == std::wstring::npos); CHECK(Contains(mt, L"PAGE-0005")); }
  // arabic + table fixtures
  ConvRun a = Convert("pdf>txt", Fixture("lo_arabic.pdf"), Json::MakeObject(), dir / L"ar.txt");
  CHECK(a.res.ok);
  if (a.res.ok) {
    std::wstring at = Utf8ToW(ReadText(a.res.outputs[0]).substr(3));
    CHECK(Contains(at, L"مرحبا بالعالم"));
    CHECK(Contains(at, L"الإنترنت"));
  }
  ConvRun tb = Convert("pdf>txt", Fixture("lo_table.pdf"), Json::MakeObject(), dir / L"tab.txt");
  CHECK(tb.res.ok);
  if (tb.res.ok) CHECK(Contains(Utf8ToW(ReadText(tb.res.outputs[0]).substr(3)), L"North\t120\t135\t150"));
  // protected / unreadable
  ConvRun pw = Convert("pdf>txt", Fixture("pw_rc4.pdf"), Json::MakeObject(), dir / L"pw.txt");
  CHECK(!pw.res.ok && Contains(pw.res.error, L"password")); CHECK(!fs::exists(dir / L"pw.txt"));
  ConvRun pw2 = Convert("pdf>txt", Fixture("pw_rc4.pdf"), Opts({{"password", "secret"}}), dir / L"pw2.txt");
  CHECK(pw2.res.ok);
  if (pw2.res.ok) CHECK(Contains(Utf8ToW(ReadText(pw2.res.outputs[0])), L"Rich Document Title"));
  WriteFileBytes(dir / L"broken.pdf", "%PDF-1.4\n1 0 obj<<>>endobj\n");
  ConvRun br = Convert("pdf>txt", dir / L"broken.pdf", Json::MakeObject(), dir / L"broken.txt");
  CHECK(!br.res.ok); CHECK(!br.res.error.empty()); CHECK(!fs::exists(dir / L"broken.txt"));
  // a PDF without any text and without scans never yields an empty file
  fs::path blank = Scratch() / L"blank.pdf";
  PageSpec bp; BuildPdf(blank, {bp, bp});
  ConvRun bl = Convert("pdf>txt", blank, Json::MakeObject(), dir / L"blank.txt");
  CHECK(!bl.res.ok); CHECK(Contains(bl.res.error, L"No text"));
  CHECK(!fs::exists(dir / L"blank.txt"));
  RecordResult("pdf>txt", r.res.ok && seq == want, "UTF-8 BOM, CRLF, reading order (2 columns), form-feed/blank page separators, Arabic, tables as TSV, password option");
}

TEST(conv_pdf_to_md_structure_tables_images) {
  fs::path dir = FreshDir(L"conv-md");
  ConvRun r = Convert("pdf>md", Fixture("lo_rich.pdf"), Json::MakeObject(), dir / L"rich.md");
  CHECK(r.res.ok);
  if (!r.res.ok) { std::printf("  %ls\n", r.res.error.c_str()); RecordResult("pdf>md", false, "failed"); return; }
  fs::path folder = r.res.outputs[0];
  CHECK(folder.filename() == L"rich");
  fs::path md = folder / L"lo_rich.md";
  CHECK(fs::exists(md));
  std::wstring t = Utf8ToW(ReadText(md));
  bool structure = Contains(t, L"# Rich Document Title") && Contains(t, L"\n## Section One") && Contains(t, L"\n## Section Two") && Contains(t, L"\n### Sub section");
  CHECK(structure);
  CHECK(Contains(t, L"**bold words**")); CHECK(Contains(t, L"*italic words*"));
  CHECK(Contains(t, L"- First bullet item\r\n- Second bullet item\r\n- Third bullet item"));
  CHECK(Contains(t, L"1. First numbered step\r\n2. Second numbered step"));
  CHECK(Contains(t, L"Plain paragraph under section one. It is long enough to wrap onto a second line when rendered in the page width given."));
  std::wregex img(L"!\\[[^\\]]*\\]\\((images/[^)]+)\\)");
  std::wsmatch m;
  bool imgOk = false;
  if (std::regex_search(t, m, img)) {
    fs::path ip = folder / m[1].str();
    imgOk = fs::exists(ip);
    CHECK(imgOk);
    Img im; CHECK(DecodeImage(ip, im)); CHECK_EQ(im.w, 160); CHECK_EQ(im.h, 100);
  } else CHECK(false);
  if (!structure) std::printf("--- md ---\n%s\n", Narrow(t).c_str());
  // table fixture
  ConvRun tb = Convert("pdf>md", Fixture("lo_table.pdf"), Json::MakeObject(), dir / L"table.md");
  CHECK(tb.res.ok);
  if (tb.res.ok) {
    std::wstring tt = Utf8ToW(ReadText(tb.res.outputs[0] / L"lo_table.md"));
    CHECK(Contains(tt, L"| Region | Q1 | Q2 | Q3 |"));
    CHECK(Contains(tt, L"| --- | --- | --- | --- |"));
    CHECK(Contains(tt, L"| North | 120 | 135 | 150 |"));
    CHECK(Contains(tt, L"| East + West merged |  | 77 | 88 |"));
    CHECK(Contains(tt, L"| Item | Qty | Price |"));
    CHECK(Contains(tt, L"| Widget | 12 | 3.50 |"));
    CHECK(!fs::exists(tb.res.outputs[0] / L"images"));
  }
  // two columns + arabic
  ConvRun tc = Convert("pdf>md", Fixture("lo_twocol.pdf"), Opts({{"images", false}}), dir / L"twocol.md");
  CHECK(tc.res.ok);
  if (tc.res.ok) {
    std::wstring tt = Utf8ToW(ReadText(tc.res.outputs[0] / L"lo_twocol.md"));
    std::wregex re(L"S(\\d{3})");
    std::vector<int> seq;
    for (auto it = std::wsregex_iterator(tt.begin(), tt.end(), re); it != std::wsregex_iterator(); ++it) seq.push_back(std::stoi((*it)[1]));
    std::vector<int> want; for (int i = 1; i <= 40; ++i) want.push_back(i); want.push_back(999);
    CHECK(seq == want);
    CHECK(Contains(tt, L"# Two Column Report"));
  }
  ConvRun ta = Convert("pdf>md", Fixture("lo_arabic.pdf"), Json::MakeObject(), dir / L"arabic.md");
  CHECK(ta.res.ok);
  if (ta.res.ok) {
    std::wstring tt = Utf8ToW(ReadText(ta.res.outputs[0] / L"lo_arabic.md"));
    CHECK(Contains(tt, L"مرحبا بالعالم"));
    CHECK(Contains(tt, L"# Arabic sample"));
  }
  // multi page: page rule between pages
  fs::path five = Scratch() / L"markers5.pdf";
  if (!fs::exists(five)) BuildMarkerPdf(five, 5);
  ConvRun m5 = Convert("pdf>md", five, Json::MakeObject(), dir / L"five.md");
  CHECK(m5.res.ok);
  if (m5.res.ok) { std::wstring tt = Utf8ToW(ReadText(m5.res.outputs[0] / L"markers5.md")); int rules = 0; size_t p = 0; while ((p = tt.find(L"\r\n---\r\n", p)) != std::wstring::npos) { ++rules; ++p; } CHECK_EQ(rules, 4); }
  ConvRun bl = Convert("pdf>md", Scratch() / L"blank.pdf", Json::MakeObject(), dir / L"blank.md");
  CHECK(!bl.res.ok);
  RecordResult("pdf>md", r.res.ok && structure && imgOk, "headings, bold/italic, lists, GFM tables (merged cells), images/ folder, 2-column order, Arabic");
}

// ---- OCR ------------------------------------------------------------------------------------------------------------------

static fs::path MakeScanPdf(const fs::path& textPdf, const wchar_t* name, int dpi) {
  // render the vector page, store it as an image-only PDF (no text layer)
  auto d = PdfDocument::Open(textPdf);
  Bitmap b; RenderOptions ro; ro.dpi = dpi;
  d.doc->RenderPage(0, ro, b);
  Img im{b.width, b.height, b.bgra};
  fs::path png = Scratch() / (std::wstring(name) + L".png");
  EncodePng(png, im);
  fs::path pdf = Scratch() / (std::wstring(name) + L".pdf");
  { std::error_code rec; fs::remove(pdf, rec); }
  JobSpec s; s.operation = L"t"; s.title = L"t";
  OpResult res;
  s.work = [&](JobContext& ctx) -> JobResult { ImagesToPdfOptions o; o.size = PageSizeKind::A4; o.fit = FitMode::Stretch; res = CreatePdfFromImages({png}, pdf, o, ctx); return res.ok ? JobResult::Ok() : JobResult::Fail(res.error); };
  uint64_t id = JobManager::Get().Submit(std::move(s));
  JobManager::Get().WaitFor(id, 120000);
  return res.ok ? res.outputs[0] : fs::path();
}

TEST(conv_ocr_scanned_pdf_to_txt_and_md) {
  OcrInfo info = QueryOcr();
  std::string langs;
  for (auto& l : info.languages) langs += Narrow(l) + " ";
  std::printf("  OCR languages installed: %s(profile: %s)\n", langs.c_str(), Narrow(info.profileLanguage).c_str());
  if (!info.available) {
    std::printf("  SKIPPED: no Windows OCR language installed on this machine\n");
    RecordResult("pdf:ocr", true, "SKIPPED: no OCR language installed");
    return;
  }
  // English scan
  PageSpec p;
  p.texts.push_back({50, 80, 20, L"The quick brown fox jumps over the lazy dog", "Helvetica"});
  p.texts.push_back({50, 130, 20, L"Invoice number 48151623 total 99.50", "Helvetica"});
  p.texts.push_back({50, 180, 18, L"Scanned documents need optical character recognition.", "Times-Roman"});
  fs::path textPdf = Scratch() / L"ocr-src.pdf";
  CHECK(BuildPdf(textPdf, {p}));
  fs::path scan = MakeScanPdf(textPdf, L"ocr-scan", 300);
  CHECK(!scan.empty());
  if (scan.empty()) return;
  std::vector<PageContent> pg;
  std::wstring err;
  CHECK(ExtractPages(scan, pg, nullptr, &err));
  CHECK(!pg[0].hasText && pg[0].looksScanned);
  fs::path dir = FreshDir(L"conv-ocr");
  ConvRun r = Convert("pdf>txt", scan, Json::MakeObject(), dir / L"scan.txt");
  CHECK(r.res.ok);
  bool english = false;
  if (r.res.ok) {
    std::wstring t = ToLower(Utf8ToW(ReadText(r.res.outputs[0]).substr(3)));
    std::printf("  OCR english result: %s\n", Narrow(t).substr(0, 220).c_str());
    english = Contains(t, L"quick brown fox") && Contains(t, L"lazy dog") && Contains(t, L"48151623") && Contains(t, L"optical character recognition");
    CHECK(english);
    bool noted = false; for (auto& w : r.res.warnings) if (Contains(w, L"OCR")) noted = true;
    CHECK(noted);
  }
  ConvRun md = Convert("pdf>md", scan, Json::MakeObject(), dir / L"scan.md");
  CHECK(md.res.ok);
  if (md.res.ok) {
    std::wstring t = ToLower(Utf8ToW(ReadText(md.res.outputs[0] / L"ocr-scan.md")));
    CHECK(Contains(t, L"quick brown fox"));
    CHECK(!fs::exists(md.res.outputs[0] / L"images"));           // the scan itself is not exported as an "image"
  }
  // OCR off: a clear error, never an empty file
  ConvRun off = Convert("pdf>txt", scan, Opts({{"ocr", "off"}}), dir / L"off.txt");
  CHECK(!off.res.ok); CHECK(Contains(off.res.error, L"OCR"));
  CHECK(!fs::exists(dir / L"off.txt"));
  // Arabic scan (only when an Arabic recognizer is installed)
  bool haveAr = false; for (auto& l : info.languages) if (l.rfind(L"ar", 0) == 0) haveAr = true;
  bool arabic = true;
  if (haveAr) {
    PageSpec ap;   // render a LibreOffice Arabic page as the source image
    fs::path arScan = MakeScanPdf(Fixture("lo_arabic.pdf"), L"ocr-ar-scan", 300);
    CHECK(!arScan.empty());
    ConvRun ar = Convert("pdf>txt", arScan, Opts({{"ocrLanguage", "ar-SA"}}), dir / L"ar.txt");
    CHECK(ar.res.ok);
    if (ar.res.ok) {
      std::wstring t = Utf8ToW(ReadText(ar.res.outputs[0]).substr(3));
      std::printf("  OCR arabic result: %s\n", Narrow(t).substr(0, 260).c_str());
      // count how many of the source words were recognised
      std::vector<std::wstring> words = {L"مرحبا", L"بالعالم", L"اللغة", L"العربية", L"الصفحة", L"الثانية", L"12345"};
      int hit = 0; for (auto& w : words) if (Contains(t, w)) ++hit;
      std::printf("  Arabic words recognised: %d/%zu\n", hit, words.size());
      arabic = hit >= 4 && Contains(t, L"مرحبا بالعالم");   // words in logical order
      CHECK(arabic);
    }
  }
  RecordResult("pdf:ocr", english && arabic, "Windows.Media.Ocr: english scan exact phrases" + std::string(haveAr ? " + arabic scan" : ""));
}

TEST(conv_ocr_unavailable_message_when_no_language) {
  // The no-language path cannot be forced here without uninstalling languages; verify the sentence the engine would use.
  OcrInfo info = QueryOcr();
  if (info.available) { CHECK(info.message.empty()); return; }
  CHECK(Contains(info.message, L"OCR"));
}

TEST(conv_ocr_bad_language_reports_clear_error) {
  OcrInfo info = QueryOcr();
  if (!info.available) return;
  fs::path scan = Scratch() / L"ocr-scan.pdf";
  if (!fs::exists(scan)) return;                 // created by conv_ocr_scanned_pdf_to_txt_and_md
  fs::path dir = FreshDir(L"conv-ocr-bad");
  ConvRun r = Convert("pdf>txt", scan, Opts({{"ocrLanguage", "xx-YY"}}), dir / L"x.txt");
  CHECK(!r.res.ok);
  CHECK(Contains(r.res.error, L"xx-YY") || Contains(r.res.error, L"recognition"));
  CHECK(!fs::exists(dir / L"x.txt"));
}
