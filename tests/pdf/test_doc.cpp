#include "pdf_testutil.h"
#include "engines/pdf/pdf_text.h"
#include <random>

using namespace ptest;
using namespace filo;
using namespace filo::pdf;

static fs::path Markers(int n = 5) {
  fs::path p = Scratch() / (L"markers" + std::to_wstring(n) + L".pdf");
  if (!fs::exists(p)) BuildMarkerPdf(p, n);
  return p;
}

TEST(doc_open_status) {
  auto bad = [&](const fs::path& p, OpenStatus want, const wchar_t* label, const std::wstring& pw = L"") {
    auto r = PdfDocument::Open(p, pw);
    if (r.status != want) std::printf("  %ls: got status %d (%ls) want %d\n", label, (int)r.status, r.message.c_str(), (int)want);
    CHECK(r.status == want);
    CHECK(r.ok() == (want == OpenStatus::Ok));
    if (want != OpenStatus::Ok) CHECK(!r.message.empty() && r.message.back() == L'.');
  };
  bad(Markers(), OpenStatus::Ok, L"markers");
  bad(Scratch() / L"does-not-exist.pdf", OpenStatus::NotFound, L"missing");
  WriteFileBytes(Scratch() / L"empty.pdf", "");
  bad(Scratch() / L"empty.pdf", OpenStatus::NotPdf, L"empty");
  WriteFileBytes(Scratch() / L"text.pdf", "This is not a PDF at all, just a text file named .pdf\n");
  bad(Scratch() / L"text.pdf", OpenStatus::NotPdf, L"text");
  WriteFileBytes(Scratch() / L"hdronly.pdf", "%PDF-1.7\n%garbage garbage garbage\n");
  bad(Scratch() / L"hdronly.pdf", OpenStatus::Corrupt, L"header only");
  for (const char* pw : {"pw_aes256.pdf", "pw_rc4.pdf"}) {
    fs::path f = Fixture(pw);
    CHECK(fs::exists(f));
    bad(f, OpenStatus::NeedsPassword, L"no password");
    bad(f, OpenStatus::WrongPassword, L"wrong password", L"nope");
    bad(f, OpenStatus::Ok, L"right password", L"secret");
    auto r = PdfDocument::Open(f, L"secret");
    if (r.ok()) { CHECK(r.doc->IsEncrypted()); CHECK(r.doc->PageCount() >= 1); CHECK(Contains(r.doc->PageText(0), L"Rich Document Title")); }
  }
  bad(Scratch(), OpenStatus::IoError, L"directory");
}

TEST(doc_pages_size_rotation_labels) {
  PageSpec a; a.w = 300; a.h = 500; a.texts.push_back({20, 20, 12, L"small page"});
  PageSpec b; b.w = 595; b.h = 842; b.rotate = 90; b.texts.push_back({50, 50, 20, L"rotated"});
  PageSpec c; c.w = 612; c.h = 792; c.rotate = 270; c.texts.push_back({50, 50, 20, L"rot270"});
  fs::path f = Scratch() / L"sizes.pdf";
  CHECK(BuildPdf(f, {a, b, c}));
  auto r = PdfDocument::Open(f);
  CHECK(r.ok());
  if (!r.ok()) return;
  CHECK_EQ(r.doc->PageCount(), 3);
  float w, h;
  CHECK(r.doc->PageSize(0, &w, &h)); CHECK_NEAR(w, 300, 0.01); CHECK_NEAR(h, 500, 0.01);
  CHECK(r.doc->PageSize(1, &w, &h)); CHECK_NEAR(w, 842, 0.01); CHECK_NEAR(h, 595, 0.01);   // displayed size swaps
  CHECK(r.doc->PageSize(2, &w, &h)); CHECK_NEAR(w, 792, 0.01); CHECK_NEAR(h, 612, 0.01);
  CHECK_EQ(r.doc->PageRotation(0), 0); CHECK_EQ(r.doc->PageRotation(1), 90); CHECK_EQ(r.doc->PageRotation(2), 270);
  CHECK(!r.doc->PageSize(3, &w, &h)); CHECK(!r.doc->PageSize(-1, &w, &h));
  CHECK_EQ(r.doc->PageRotation(7), -1);
  CHECK(r.doc->PageLabel(0).empty());
  CHECK(r.doc->FileVersion() >= 14);
  auto lo = PdfDocument::Open(Fixture("lo_rich.pdf"));
  CHECK(lo.ok());
  if (lo.ok()) { auto m = lo.doc->Meta(); CHECK(!m.producer.empty()); }
}

TEST(doc_render_sizes_and_content) {
  PageSpec p; p.w = 595; p.h = 842;
  p.texts.push_back({72, 72, 40, L"HELLO RENDER", "Helvetica-Bold", 0xCC0000});
  p.lines.push_back({72, 150, 500, 150, 3});
  fs::path f = Scratch() / L"render.pdf";
  CHECK(BuildPdf(f, {p}));
  auto r = PdfDocument::Open(f);
  CHECK(r.ok());
  if (!r.ok()) return;
  for (double dpi : {72.0, 96.0, 150.0, 300.0}) {
    RenderOptions o; o.dpi = dpi;
    Bitmap b; std::wstring err;
    CHECK(r.doc->RenderPage(0, o, b, &err));
    CHECK(std::abs(b.width - 595 * dpi / 72) <= 1.0);
    CHECK(std::abs(b.height - 842 * dpi / 72) <= 1.0);
    CHECK_EQ(b.bgra.size(), (size_t)b.width * b.height * 4);
    Img im{b.width, b.height, b.bgra};
    CHECK(NonBlankRatio(im) > 0.001);
    CHECK_EQ((int)b.bgra[3], 255);                                // opaque white page
    CHECK_EQ((int)b.bgra[0], 255);
  }
  RenderOptions o; o.dpi = 150; o.transparent = true;
  Bitmap b;
  CHECK(r.doc->RenderPage(0, o, b));
  CHECK_EQ((int)b.bgra[3], 0);
  int solid = 0, partial = 0, partialRed = 0, darkened = 0;
  for (size_t i = 0; i + 3 < b.bgra.size(); i += 4) {
    uint8_t B = b.bgra[i], G = b.bgra[i + 1], R = b.bgra[i + 2], A = b.bgra[i + 3];
    if (A == 255 && R > 180 && G < 40 && B < 40) ++solid;
    if (A > 40 && A < 220) { ++partial; if (R > 150 && G < 60 && B < 60) ++partialRed; if (R > 8 && R < 150 && G < 60 && B < 60) ++darkened; }
  }
  CHECK(solid > 200);
  CHECK(partial > 50);
  CHECK(partialRed > 50);
  CHECK_EQ(darkened, 0);                           // anti-aliased edge pixels are straight (not premultiplied)
  o = RenderOptions(); o.dpi = 100; o.grayscale = true;
  CHECK(r.doc->RenderPage(0, o, b));
  bool allGray = true;
  for (size_t i = 0; i + 3 < b.bgra.size(); i += 4) if (b.bgra[i] != b.bgra[i + 1] || b.bgra[i] != b.bgra[i + 2]) { allGray = false; break; }
  CHECK(allGray);
  o = RenderOptions(); o.maxSidePx = 200;
  CHECK(r.doc->RenderPage(0, o, b));
  CHECK_EQ(std::max(b.width, b.height), 200);
  o = RenderOptions(); o.dpi = 600; o.maxPixels = 2e6;
  bool clamped = false;
  CHECK(r.doc->RenderPage(0, o, b, nullptr, &clamped));
  CHECK(clamped);
  CHECK((double)b.width * b.height <= 2.05e6);
  CHECK(!r.doc->RenderPage(9, RenderOptions(), b));
}

TEST(doc_thumbnail_and_lister) {
  auto t = PageThumbnailer::Open(Markers(5), L"");
  CHECK(t != nullptr);
  if (!t) return;
  CHECK_EQ(t->PageCount(), 5);
  for (int i = 0; i < 5; ++i) {
    Bitmap b; CHECK(t->Render(i, 160, b));
    CHECK_EQ(std::max(b.width, b.height), 160);
    CHECK(NonBlankRatio({b.width, b.height, b.bgra}) > 0.0005);
  }
  OpenStatus st = OpenStatus::Ok; std::wstring msg;
  CHECK(PageThumbnailer::Open(Scratch() / L"nope.pdf", L"", &st, &msg) == nullptr);
  CHECK(st == OpenStatus::NotFound);
  auto info = GetPdfFileInfo(Markers(5));
  CHECK(info.status == OpenStatus::Ok); CHECK_EQ(info.pageCount, 5); CHECK(info.sizeBytes > 500);
  CHECK_NEAR(info.firstPageWidthPt, 595, 0.01);
  auto pw = GetPdfFileInfo(Fixture("pw_aes256.pdf"));
  CHECK(pw.status == OpenStatus::NeedsPassword);
}

static std::vector<std::wstring> MarkersOf(const PdfFacts& f) {
  std::vector<std::wstring> v;
  for (auto& t : f.text) { size_t p = t.find(L"PAGE-"); v.push_back(p == std::wstring::npos ? L"?" : t.substr(p, 9)); }
  return v;
}

TEST(doc_page_operations) {
  auto src = PdfDocument::Open(Markers(5));
  CHECK(src.ok());
  if (!src.ok()) return;
  auto d = PdfDocument::CreateNew();
  std::wstring err;
  CHECK(d->ImportPages(*src.doc, {2, 0, 1, 1}, -1, &err));
  CHECK_EQ(d->PageCount(), 4);
  CHECK(d->RotatePage(0, 90));
  CHECK(d->RotatePage(0, 90));
  CHECK_EQ(d->PageRotation(0), 180);
  CHECK(d->RotatePage(1, -90));
  CHECK_EQ(d->PageRotation(1), 270);
  CHECK(!d->RotatePage(1, 45));
  CHECK(d->DuplicatePage(3));                       // [3,1,2,2,2] in 1-based marker numbering = pages 3,1,2,2,2
  CHECK_EQ(d->PageCount(), 5);
  CHECK(d->MovePage(4, 0));
  CHECK(d->DeletePage(2));
  CHECK(!d->ImportPages(*src.doc, {9}, -1, &err));
  CHECK(!err.empty());
  fs::path out = Scratch() / L"ops.pdf";
  CHECK(d->Save(out, &err));
  PdfFacts f = ReadPdf(out);
  CHECK(f.ok);
  CHECK_EQ(f.pages, 4);
  auto m = MarkersOf(f);
  // order after the steps: import [3,1,2,2]; duplicate idx3 -> [3,1,2,2,2]; move last to front -> [2,3,1,2,2]; delete idx 2 -> [2,3,2,2]
  std::vector<std::wstring> want = {L"PAGE-0002", L"PAGE-0003", L"PAGE-0002", L"PAGE-0002"};
  CHECK(m == want);
  if (m != want) for (auto& s : m) std::printf("  got %ls\n", s.c_str());
  CHECK_EQ(f.rot[1], 180);       // the page that was rotated twice (originally first) is now second
  CHECK(d->ImportPages(*src.doc, {4}, 1, &err));
  CHECK(d->Save(out, &err));
  f = ReadPdf(out);
  CHECK_EQ(f.pages, 5);
  CHECK(MarkersOf(f)[1] == L"PAGE-0005");
  CHECK(!d->Save(Scratch() / L"no-such-dir" / L"x.pdf", &err));
  CHECK(!err.empty());
  CHECK_EQ(src.doc->PageCount(), 5);
}

static void TryEverything(const fs::path& p, int* openedOk) {
  auto r = PdfDocument::Open(p);
  if (!r.ok()) { CHECK(!r.message.empty()); return; }
  ++*openedOk;
  int n = r.doc->PageCount();
  for (int i = 0; i < std::min(n, 3); ++i) {
    Bitmap b; RenderOptions o; o.dpi = 40;
    r.doc->RenderPage(i, o, b);
    PageContent pc; ExtractPage(*r.doc, i, pc, true);
    r.doc->PageText(i);
  }
  r.doc->Meta();
}

TEST(doc_corrupt_and_truncated_inputs_never_crash) {
  std::string good = ReadFileBytes(Fixture("lo_rich.pdf"));
  CHECK(good.size() > 1000);
  fs::path t = Scratch() / L"fuzz.pdf";
  int opened = 0, tries = 0;
  for (int k = 1; k <= 40; ++k) {
    size_t cut = good.size() * k / 41;
    WriteFileBytes(t, good.substr(0, cut));
    TryEverything(t, &opened); ++tries;
  }
  size_t sx = good.rfind("startxref");
  CHECK(sx != std::string::npos);
  WriteFileBytes(t, good.substr(0, sx)); TryEverything(t, &opened);
  std::mt19937 rng(12345);
  for (int k = 0; k < 150; ++k) {
    std::string m = good;
    int flips = 1 + (int)(rng() % 20);
    for (int i = 0; i < flips; ++i) m[rng() % m.size()] = (char)(rng() & 255);
    WriteFileBytes(t, m); TryEverything(t, &opened); ++tries;
  }
  { std::string m = good; for (size_t i = m.size() / 3; i < m.size() / 3 + 2000 && i < m.size(); ++i) m[i] = 0; WriteFileBytes(t, m); TryEverything(t, &opened); }
  { std::string m = good + std::string(5000, 'x'); WriteFileBytes(t, m); TryEverything(t, &opened); }
  std::printf("  fuzz: %d inputs, %d opened (repaired or intact), rest rejected cleanly\n", tries, opened);
  CHECK(opened > 0);
  RecordResult("pdf:corrupt-input", true, "190 truncated/mutated inputs: no crash");
}

TEST(doc_unicode_and_long_paths) {
  fs::path dir = Scratch() / L"ملف 日本語 émoji \U0001F600";
  std::error_code ec; fs::create_directories(dir, ec);
  fs::path f = dir / L"مستند 日本語 - résumé \U0001F600.pdf";
  fs::copy_file(Markers(3), f, fs::copy_options::overwrite_existing, ec);
  CHECK(!ec);
  auto r = PdfDocument::Open(f);
  CHECK(r.ok());
  if (r.ok()) CHECK_EQ(r.doc->PageCount(), 3);
  fs::path cur = Scratch();
  bool made = true;
  for (int i = 0; i < 6; ++i) { cur /= std::wstring(60, L'a' + i); if (!CreateDirectoryW(LongPath(cur).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) made = false; }
  if (made) {
    fs::path lf = cur / L"long-name-document.pdf";
    CHECK(CopyFileW(LongPath(Markers(3)).c_str(), LongPath(lf).c_str(), FALSE));
    CHECK(lf.wstring().size() > 260);
    auto r2 = PdfDocument::Open(lf);
    CHECK(r2.ok());
    if (r2.ok()) CHECK_EQ(r2.doc->PageCount(), 3);
    std::vector<PageContent> pages; std::wstring err;
    CHECK(ExtractPages(lf, pages, nullptr, &err));
    CHECK_EQ((int)pages.size(), 3);
    RecordResult("pdf:long-and-unicode-paths", r2.ok(), "260+ char path and Arabic/CJK/emoji names");
  } else std::printf("  (could not create >260 char directories)\n");
}

TEST(doc_page_labels) {
  auto r = PdfDocument::Open(Fixture("labels.pdf"));
  CHECK(r.ok());
  if (!r.ok()) return;
  CHECK_EQ(r.doc->PageCount(), 5);
  CHECK(r.doc->PageLabel(0) == L"i");
  CHECK(r.doc->PageLabel(1) == L"ii");
  CHECK(r.doc->PageLabel(2) == L"A-1");
  CHECK(r.doc->PageLabel(4) == L"A-3");
  PageContent pc;
  CHECK(ExtractPage(*r.doc, 3, pc));
  CHECK(pc.label == L"A-2");
  CHECK(Contains(PageToPlainText(pc), L"LABELPAGE-4"));
}

TEST(doc_form_fields_render_with_forms_flag) {
  auto r = PdfDocument::Open(Fixture("form.pdf"));
  CHECK(r.ok());
  if (!r.ok()) return;
  RenderOptions with; with.dpi = 100; with.annotations = true; with.forms = true;
  RenderOptions without = with; without.annotations = false; without.forms = false;
  Bitmap a, b, c;
  CHECK(r.doc->RenderPage(0, with, a));
  CHECK(r.doc->RenderPage(0, without, b));
  with.forms = false;
  CHECK(r.doc->RenderPage(0, with, c));
  Img ia{a.width, a.height, a.bgra}, ib{b.width, b.height, b.bgra}, ic{c.width, c.height, c.bgra};
  double withForms = NonBlankRatio(ia), without_ = NonBlankRatio(ib);
  std::printf("  form field pixels: with %.4f, annotations+forms off %.4f, annotations only %.4f\n", withForms, without_, NonBlankRatio(ic));
  CHECK(withForms > without_ + 0.01);          // the yellow field with its value is drawn only when forms/annotations are on
  // the field value is also visible as text
  CHECK(Contains(r.doc->PageText(0), L"FORMVALUE") || withForms > 0.02);
}
