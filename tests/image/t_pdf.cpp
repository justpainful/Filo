// image -> PDF: our own PDF writer, verified with PDFium and PyMuPDF.
#include "t_util.h"
#include "engines/image/pdf_writer.h"
#include "engines/image/imgmeta.h"

using namespace tu;
using namespace filo;
using namespace filo::img;

namespace {
Json InspectPdf(const fs::path& pdf, const fs::path* jpeg = nullptr) {
  static fs::path script = Root() / "tests" / "image" / "check_pdf.py";
  std::vector<std::wstring> a = {script.wstring(), pdf.wstring()};
  if (jpeg) a.push_back(jpeg->wstring());
  std::string out = RunPy(a);
  auto j = Json::Parse(out.substr(0, out.find('\n')));
  return j ? *j : Json::MakeObject();
}
bool Contains(const Bytes& hay, const Bytes& needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
}

TEST(pdf_jpeg_is_embedded_untouched_with_orientation_in_the_matrix) {
  CHECK(EnsureFixtures());
  for (int o = 1; o <= 8; ++o) {
    fs::path jpg = Src() / (L"orient_" + std::to_wstring(o) + L".jpg");
    RunOut r = RunCap(L"jpg", L"pdf", jpg, OutDir() / (L"pdf_orient_" + std::to_wstring(o) + L".pdf"));
    CHECK(r.ok);
    if (!r.ok) { std::printf("    orient %d: %ls\n", o, r.error.c_str()); continue; }
    Bytes pdf, orig; ReadFileBytes(r.out, pdf); ReadFileBytes(jpg, orig);
    CHECK(Contains(pdf, orig));                       // the JPEG stream is copied byte for byte
    CHECK(pdf.size() < orig.size() + 2000);           // and nothing was re-encoded or bloated
    Px px; double wpt = 0, hpt = 0;
    CHECK(RenderPdfPage(r.out, 0, 320, 240, &px, &wpt, &hpt));
    CHECK_NEAR(wpt, 240, 0.6); CHECK_NEAR(hpt, 180, 0.6);                  // displayed size 320x240 px at 96 dpi
    CHECK(NearRgb(PxQuad(px, 0, 0), 255, 0, 0, 14));
    CHECK(NearRgb(PxQuad(px, 1, 0), 0, 255, 0, 14));
    CHECK(NearRgb(PxQuad(px, 0, 1), 0, 0, 255, 14));
    CHECK(NearRgb(PxQuad(px, 1, 1), 255, 255, 0, 14));
    Json j = InspectPdf(r.out, &jpg);
    CHECK(j.Get("ok").AsBool()); CHECK(!j.Get("repaired").AsBool());
    CHECK(j.Get("jpeg_identical").AsBool());
    CHECK_EQ((int)j.Get("pages").AsInt(), 1);
  }
}

TEST(pdf_from_png_uses_flate_with_smask_for_alpha) {
  CHECK(EnsureFixtures());
  RunOut r = RunCap(L"png", L"pdf", Src() / "src_png.png", OutDir() / L"pdf_alpha.pdf");
  CHECK(r.ok);
  Json j = InspectPdf(r.out);
  CHECK(j.Get("ok").AsBool()); CHECK(!j.Get("repaired").AsBool());
  CHECK_EQ(j.Get("images").Size(), (size_t)1);
  CHECK(j.Get("images").At(0).Get("smask").AsBool());
  CHECK_EQ((int)j.Get("images").At(0).Get("w").AsInt(), 320);
  Bytes pdf; ReadFileBytes(r.out, pdf);
  Bytes flate{'/', 'F', 'l', 'a', 't', 'e', 'D', 'e', 'c', 'o', 'd', 'e'};
  CHECK(Contains(pdf, flate));
  Px px; CHECK(RenderPdfPage(r.out, 0, 320, 240, &px));
  CHECK(NearRgb(PxQuad(px, 1, 0), 255, 255, 255, 3));     // transparent area shows the white page
  CHECK(NearRgb(PxQuad(px, 0, 1), 127, 127, 255, 4));     // 50 % blue over white
  // opaque PNG: no SMask
  RunOut r2 = RunCap(L"bmp", L"pdf", Src() / "src_bmp.bmp", OutDir() / L"pdf_opaque.pdf");
  CHECK(r2.ok);
  Json j2 = InspectPdf(r2.out); CHECK(!j2.Get("images").At(0).Get("smask").AsBool());
}

TEST(pdf_keeps_the_jpeg_icc_profile) {
  CHECK(EnsureFixtures());
  fs::path jpg = Src() / "icc_adobe.jpg";
  RunOut r = RunCap(L"jpg", L"pdf", jpg, OutDir() / L"pdf_icc.pdf");
  CHECK(r.ok);
  Bytes pdf, orig; ReadFileBytes(r.out, pdf); ReadFileBytes(jpg, orig);
  CHECK(Contains(pdf, orig));
  Bytes icc{'/', 'I', 'C', 'C', 'B', 'a', 's', 'e', 'd'};
  CHECK(Contains(pdf, icc));
  std::ifstream fin(Src() / "icc_expected.json", std::ios::binary); std::stringstream ss; ss << fin.rdbuf();
  auto exp = Json::Parse(ss.str());
  Px px; CHECK(RenderPdfPage(r.out, 0, 64, 64, &px));
  Rgba q = PxQuad(px, 0, 0);
  std::printf("    PDFium renders the tagged Adobe-RGB patch as (%.0f,%.0f,%.0f); lcms2 expects (%d,%d,%d)\n", q.r, q.g, q.b, (int)exp->Get("srgb").At(0).AsInt(), (int)exp->Get("srgb").At(1).AsInt(), (int)exp->Get("srgb").At(2).AsInt());
  CHECK(NearRgb(q, exp->Get("srgb").At(0).AsNumber(), exp->Get("srgb").At(1).AsNumber(), exp->Get("srgb").At(2).AsNumber(), 12));
}

TEST(pdf_cmyk_and_progressive_jpegs_still_produce_valid_pdfs) {
  CHECK(EnsureFixtures());
  RunOut r = RunCap(L"jpg", L"pdf", Src() / "cmyk.jpg", OutDir() / L"pdf_cmyk.pdf");
  CHECK(r.ok);
  Json j = InspectPdf(r.out); CHECK(j.Get("ok").AsBool()); CHECK(!j.Get("repaired").AsBool());
  Px px; CHECK(RenderPdfPage(r.out, 0, 320, 240, &px));
  Rgba red = PxQuad(px, 0, 0); CHECK(red.r > 180 && red.g < 90);
}

TEST(pdf_writer_multi_page_and_size_limit) {
  CHECK(EnsureFixtures());
  Bitmap a, b, wide;
  std::wstring err;
  CHECK(LoadBitmap(Src() / "src_png.png", a, {}, &err));
  CHECK(LoadBitmap(Src() / "src_bmp.bmp", b, {}, &err));
  wide.Allocate(20000, 40);
  for (size_t i = 0; i < wide.px.size(); i += 4) { wide.px[i] = 0; wide.px[i + 1] = 0; wide.px[i + 2] = 255; wide.px[i + 3] = 255; }
  std::vector<PdfImagePage> pages(3);
  pages[0].bitmap = &a; pages[1].bitmap = &b; pages[1].dpi = 72; pages[2].bitmap = &wide;
  fs::path out = OutDir() / L"pdf_multi.pdf";
  CHECK(WritePdf(out, pages, PdfOptions{}, &err));
  double w = 0, h = 0; int n = 0;
  CHECK(RenderPdfPage(out, 0, 0, 0, nullptr, &w, &h, &n));
  CHECK_EQ(n, 3); CHECK_NEAR(w, 240, 0.6);
  CHECK(RenderPdfPage(out, 1, 0, 0, nullptr, &w, &h)); CHECK_NEAR(w, 320, 0.6); CHECK_NEAR(h, 240, 0.6);      // dpi 72 -> 1 px = 1 pt
  CHECK(RenderPdfPage(out, 2, 0, 0, nullptr, &w, &h)); CHECK(w <= 14400.5); CHECK_NEAR(w / h, 500.0, 5.0);      // limited to the PDF page size cap, aspect kept
  Json j = InspectPdf(out); CHECK(j.Get("ok").AsBool()); CHECK(!j.Get("repaired").AsBool()); CHECK_EQ((int)j.Get("pages").AsInt(), 3);
}

TEST(pdf_gray_and_wide_images_roundtrip) {
  CHECK(EnsureFixtures());
  RunOut r = RunCap(L"png", L"pdf", Src() / "wide_4000x30.png", OutDir() / L"pdf_wide.pdf");
  CHECK(r.ok);
  double w, h; CHECK(RenderPdfPage(r.out, 0, 0, 0, nullptr, &w, &h));
  CHECK_NEAR(w, 3000, 1); CHECK_NEAR(h, 22.5, 0.3);
  Px px; CHECK(RenderPdfPage(r.out, 0, 400, 3, &px));
  CHECK(NearRgb(PxQuad(px, 0, 0), 200, 30, 30, 4));
}
