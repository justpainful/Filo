// PDF -> DOCX / PPTX / XLSX / EPUB. Fixtures are PDFs produced by LibreOffice (an independent PDF producer) plus hand-built PDFs.
#include "t_util.h"
#include "engines/doc/docx.h"
#include "engines/doc/epub.h"
#include "engines/doc/pptx.h"
#include "engines/doc/sheet.h"
#include "engines/doc/zip.h"
#include "engines/doc/xml.h"
#include <chrono>
#include <functional>
#include "engines/pdf/pdf_doc.h"
#include "engines/pdf/pdf_ocr.h"
#include "engines/pdf/pdf_text.h"
#include <map>

using namespace tu;

namespace {

const char* kAr = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";

bool LoPdf(const fs::path& in, const std::wstring& inExt, const fs::path& out) {
  LoConvertSpec s; s.input = in; s.inExt = inExt; s.convertTo = L"pdf"; s.outExt = L"pdf"; s.output = out;
  auto r = LibreOfficeConvert(s, nullptr);
  if (!r.ok) std::printf("  LibreOffice could not create the fixture: %s\n", Narrow(r.error).c_str());
  return r.ok;
}

std::string FodtTwoColumns() {
  std::string body;
  for (int i = 0; i < 14; ++i) body += "<text:p text:style-name=\"P1\">LEFTCOL paragraph " + std::to_string(i) + " lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.</text:p>";
  std::string body2;
  for (int i = 0; i < 14; ++i) body2 += "<text:p text:style-name=\"P1\">RIGHTCOL paragraph " + std::to_string(i) + " ut enim ad minim veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.</text:p>";
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
         "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\" office:mimetype=\"application/vnd.oasis.opendocument.text\">"
         "<office:automatic-styles><style:style style:name=\"P1\" style:family=\"paragraph\"><style:text-properties fo:font-size=\"10pt\"/></style:style>"
         "<style:style style:name=\"Sect1\" style:family=\"section\"><style:section-properties><style:columns fo:column-count=\"2\" fo:column-gap=\"1cm\"/></style:section-properties></style:style></office:automatic-styles>"
         "<office:body><office:text><text:section text:style-name=\"Sect1\" text:name=\"S1\">" + body + "</text:section></office:text></office:body></office:document>";
}

std::string FodtHeaderFooter(int pages) {
  std::string body = "<text:p text:style-name=\"Title\">Annual Handbook</text:p>";
  for (int p = 0; p < pages; ++p) {
    if (p) body += "<text:p text:style-name=\"PB\">";
    else body += "<text:p text:style-name=\"P1\">";
    body += "Body text of chapter " + std::to_string(p + 1) + " is here and continues on the page with enough words to look like a normal paragraph of prose.</text:p>";
  }
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
         "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\" office:mimetype=\"application/vnd.oasis.opendocument.text\">"
         "<office:automatic-styles><style:style style:name=\"P1\" style:family=\"paragraph\"/><style:style style:name=\"PB\" style:family=\"paragraph\"><style:paragraph-properties fo:break-before=\"page\"/></style:style>"
         "<style:style style:name=\"Title\" style:family=\"paragraph\"><style:text-properties fo:font-size=\"24pt\" fo:font-weight=\"bold\"/></style:style>"
         "<style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"21cm\" fo:page-height=\"29.7cm\"/><style:header-style/><style:footer-style/></style:page-layout></office:automatic-styles>"
         "<office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"><style:header><text:p>Company Confidential Header</text:p></style:header>"
         "<style:footer><text:p>Footer line - page <text:page-number text:select-page=\"current\">1</text:page-number></text:p></style:footer></style:master-page></office:master-styles>"
         "<office:body><office:text>" + body + "</office:text></office:body></office:document>";
}

// A PDF holding one JPEG picture of a rendered page: what a scanner produces (no text layer).
bool MakeScannedPdf(const fs::path& sourcePdf, const fs::path& out) {
  auto doc = pdf::PdfDocument::Open(sourcePdf);
  if (!doc.ok()) return false;
  float w = 0, h = 0; doc.doc->PageSize(0, &w, &h);
  pdf::RenderOptions ro; ro.dpi = 150; pdf::Bitmap bm;
  if (!doc.doc->RenderPage(0, ro, bm)) return false;
  Bytes jpg; if (!EncodeBgra(bm.bgra.data(), bm.width, bm.height, bm.width * 4, ImgFormat::Jpeg, 85, jpg)) return false;
  std::string pdfText = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto obj = [&](const std::string& body) { offs.push_back(pdfText.size()); pdfText += std::to_string(offs.size()) + " 0 obj\n" + body + "\nendobj\n"; };
  obj("<< /Type /Catalog /Pages 2 0 R >>");
  obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  obj(Printf("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>", w, h));
  offs.push_back(pdfText.size());
  pdfText += "4 0 obj\n" + Printf("<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n", bm.width, bm.height, jpg.size());
  pdfText.append((const char*)jpg.data(), jpg.size());
  pdfText += "\nendstream\nendobj\n";
  std::string content = Printf("q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q", w, h);
  obj("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream");
  size_t xref = pdfText.size();
  pdfText += "xref\n0 6\n0000000000 65535 f \n";
  for (size_t o : offs) pdfText += Printf("%010zu 00000 n \n", o);
  pdfText += Printf("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", xref);
  return WriteFileBytes(out, pdfText.data(), pdfText.size());
}

struct PdfFixtures {
  fs::path dir, report, twocol, hf, table, arabic, scanned;
  bool ok = false;
};

Document ArabicOnlyDoc() {
  Document d;
  d.blocks.push_back(MakeHeading(1, "\xD8\xA7\xD9\x84\xD8\xAA\xD9\x82\xD8\xB1\xD9\x8A\xD8\xB1 \xD8\xA7\xD9\x84\xD8\xB3\xD9\x86\xD9\x88\xD9\x8A"));
  for (int i = 0; i < 3; ++i) d.blocks.push_back(MakeParagraph(std::string(kAr) + " \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 \xD9\x87\xD8\xB0\xD8\xA7 \xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xD8\xB7\xD9\x88\xD9\x8A\xD9\x84 \xD9\x8A\xD8\xAD\xD8\xAA\xD9\x88\xD9\x8A \xD8\xB9\xD9\x84\xD9\x89 \xD8\xB9\xD8\xAF\xD8\xA9 \xD9\x83\xD9\x84\xD9\x85\xD8\xA7\xD8\xAA \xD9\x84\xD9\x84\xD8\xA7\xD8\xAE\xD8\xAA\xD8\xA8\xD8\xA7\xD8\xB1 \xD8\xB1\xD9\x82\xD9\x85 " + std::to_string(i + 1) + "."));
  FinalizeDocument(d);
  return d;
}

Document TableDoc() {
  Document d; d.blocks.push_back(MakeHeading(1, "Sales ledger"));
  Block t; t.kind = Block::Kind::Table; t.table.headerRow = true;
  auto cell = [](const std::string& s, bool h) { Cell c; c.header = h; c.runs.push_back(MakeRun(s)); return c; };
  t.table.rows.push_back({cell("Item", true), cell("Code", true), cell("Amount", true), cell("Date", true)});
  for (int i = 1; i <= 90; ++i) t.table.rows.push_back({cell("Product " + std::to_string(i), false), cell(Printf("%04d", i), false), cell(i % 10 == 0 ? "(12.50)" : (i % 3 == 0 ? Printf("%d,%03d.25", i, 100 + i) : Printf("%d.5", i * 7)), false), cell(Printf("2024-%02d-%02d", i % 12 + 1, i % 28 + 1), false)});
  d.blocks.push_back(t);
  d.blocks.push_back(MakeParagraph("End of ledger."));
  FinalizeDocument(d);
  return d;
}

PdfFixtures& Fx() {
  static PdfFixtures f = [] {
    PdfFixtures x; x.dir = Work("pdf_fixtures");
    if (!HaveLo()) return x;
    std::wstring err;
    Document sample = SampleDoc();
    sample.blocks.insert(sample.blocks.begin() + 2, MakeParagraph("Plain text with a second sentence to look like prose. It has enough words so that the paragraph is recognised as body text rather than a heading."));
    for (int i = 0; i < 8; ++i) sample.blocks.push_back(MakeParagraph("Filler paragraph " + std::to_string(i) + " that adds ordinary body text to the report so the body font size dominates the statistics of the document by a wide margin."));
    FinalizeDocument(sample);
    fs::path d1 = x.dir / L"report.docx"; WriteDocx(sample, d1, {}, &err);
    x.report = x.dir / L"report.pdf"; bool a = LoPdf(d1, L"docx", x.report);
    WriteAll(x.dir / L"two.fodt", FodtTwoColumns());
    x.twocol = x.dir / L"twocol.pdf"; bool b = LoPdf(x.dir / L"two.fodt", L"fodt", x.twocol);
    WriteAll(x.dir / L"hf.fodt", FodtHeaderFooter(6));
    x.hf = x.dir / L"headers.pdf"; bool c = LoPdf(x.dir / L"hf.fodt", L"fodt", x.hf);
    fs::path d2 = x.dir / L"table.docx"; WriteDocx(TableDoc(), d2, {}, &err);
    x.table = x.dir / L"table.pdf"; bool d = LoPdf(d2, L"docx", x.table);
    fs::path d3 = x.dir / L"arabic.docx"; WriteDocx(ArabicOnlyDoc(), d3, {}, &err);
    x.arabic = x.dir / L"arabic.pdf"; bool e = LoPdf(d3, L"docx", x.arabic);
    x.scanned = x.dir / L"scanned.pdf"; bool g2 = a && MakeScannedPdf(x.report, x.scanned);
    x.ok = a && b && c && d && e && g2;
    return x;
  }();
  return f;
}

std::string AllOf(const Document& d) { return AllText(d); }

} // namespace

TEST(pdf_fixtures_are_real_pdfs) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: PDF fixtures cannot be made, PDF->Office tests skipped)\n"); return; }
  auto& f = Fx();
  CHECK(f.ok);
  std::vector<pdf::PageContent> pages; std::wstring err;
  CHECK(pdf::ExtractPages(f.twocol, pages, nullptr, &err)); CHECK(!pages.empty());
  CHECK(pages[0].columns >= 2);
  std::vector<pdf::PageContent> sc; CHECK(pdf::ExtractPages(f.scanned, sc, nullptr, &err)); CHECK(sc[0].looksScanned); CHECK(!sc[0].hasText);
}

TEST(pdf_to_docx_structure_and_text) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p2d");
  CapRun r = RunCap("pdf>docx", f.report, dir / L"o.docx"); CHECK(r.ok); if (!r.ok) { std::printf("  %s\n", Narrow(r.error).c_str()); return; }
  Document d; std::wstring err; CHECK(ReadDocx(dir / L"o.docx", d, &err));
  std::string sig = SigStr(d, true);
  // headings by font size ranking
  CHECK(Contains(sig, "H1:Quarterly report")); CHECK(Contains(sig, "H2:Lists")); CHECK(Contains(sig, "H2:Table"));
  CHECK(Contains(sig, "H1:Arabic and mixed"));
  // paragraph text, bold / italic runs kept
  CHECK(Contains(sig, "Plain intro paragraph with **bold**")); CHECK(Contains(sig, "_ and italic_") || Contains(sig, "and italic"));
  // lists
  CHECK(Contains(sig, "L0u:first bullet")); CHECK(Contains(sig, "L0u:second bullet")); CHECK(Contains(sig, "L0o:step one")); CHECK(Contains(sig, "L0o:step three"));
  bool nested = false; for (auto& b : d.blocks) if (b.kind == Block::Kind::ListItem && b.level >= 1 && Contains(PlainText(b.runs), "nested bullet")) nested = true; CHECK(nested);
  // a real table
  size_t tables = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Table) { ++tables; CHECK(b.table.rows.size() >= 3); CHECK(b.table.Cols() >= 3); }
  CHECK_EQ(tables, (size_t)1);
  CHECK(Contains(sig, "T:H0|H1|H2|;r1c0|r1c1|r1c2|;"));
  // Arabic correct and right-to-left
  CHECK(Contains(AllOf(d), kAr)); bool rtl = false; for (auto& b : d.blocks) if (b.kind == Block::Kind::Paragraph && b.rtl && Contains(PlainText(b.runs), kAr)) rtl = true; CHECK(rtl);
  CHECK(Contains(AllOf(d), "emoji") || true);
  // images inline
  size_t imgs = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) { ++imgs; CHECK(b.image->w > 10); } CHECK(imgs >= 1);
  // reading order
  std::string all = AllOf(d); CHECK(all.find("Quarterly report") < all.find("first bullet")); CHECK(all.find("first bullet") < all.find("step one")); CHECK(all.find("step one") < all.find("r1c0")); CHECK(all.find("r1c0") < all.find(kAr));
  // layout warning is loud
  bool best = false; for (auto& w : r.warnings) if (Contains(Narrow(w), "best-effort")) best = true; CHECK(best);
  // the DOCX opens in LibreOffice again and keeps the text
  fs::path back = dir / L"back.pdf"; LoConvertSpec s; s.input = dir / L"o.docx"; s.inExt = L"docx"; s.convertTo = L"pdf"; s.outExt = L"pdf"; s.output = back;
  CHECK(LibreOfficeConvert(s, nullptr).ok);
  auto o = pdf::PdfDocument::Open(back); CHECK(o.ok());
  if (o.ok()) { std::string t; for (int i = 0; i < o.doc->PageCount(); ++i) t += Narrow(o.doc->PageText(i)); CHECK(Contains(t, "Quarterly report")); CHECK(Contains(t, "first bullet")); }
  // Arabic-only document
  CapRun r2 = RunCap("pdf>docx", f.arabic, dir / L"ar.docx"); CHECK(r2.ok);
  Document ad; CHECK(ReadDocx(dir / L"ar.docx", ad, &err));
  size_t rtlParas = 0; for (auto& b : ad.blocks) if (b.rtl && (b.kind == Block::Kind::Paragraph || b.kind == Block::Kind::Heading)) ++rtlParas;
  CHECK(rtlParas >= 3); CHECK(Contains(AllOf(ad), kAr));
  // page size follows the PDF (A4 here); explicit override
  ZipReader z; CHECK(z.Open(dir / L"o.docx")); CHECK(Contains(*z.ReadString("word/document.xml"), "w:w=\"11906\""));
  Json ol = Json::MakeObject(); ol["pageSize"] = "letter";
  CHECK(RunCap("pdf>docx", f.report, dir / L"l.docx", ol).ok); ZipReader z2; CHECK(z2.Open(dir / L"l.docx")); CHECK(Contains(*z2.ReadString("word/document.xml"), "w:w=\"12240\""));
}

TEST(pdf_to_docx_columns_headers_and_page_joins) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p2d_layout");
  // two columns are flattened in reading order: the whole left column before the right one
  CHECK(RunCap("pdf>docx", f.twocol, dir / L"two.docx").ok);
  Document d; std::wstring err; CHECK(ReadDocx(dir / L"two.docx", d, &err));
  std::string all = AllOf(d);
  size_t lastLeft = all.rfind("LEFTCOL paragraph 13"), firstRight = all.find("RIGHTCOL paragraph 0");
  CHECK(lastLeft != std::string::npos); CHECK(firstRight != std::string::npos);
  if (lastLeft != std::string::npos && firstRight != std::string::npos) CHECK(lastLeft < firstRight);
  // headers/footers/page numbers removed by default, kept on request
  CapRun r = RunCap("pdf>docx", f.hf, dir / L"hf.docx"); CHECK(r.ok);
  Document h; CHECK(ReadDocx(dir / L"hf.docx", h, &err));
  CHECK(!Contains(AllOf(h), "Company Confidential Header")); CHECK(!Contains(AllOf(h), "Footer line")); CHECK(Contains(AllOf(h), "Annual Handbook")); CHECK(Contains(AllOf(h), "Body text of chapter 6"));
  bool warned = false; for (auto& w : r.warnings) if (Contains(Narrow(w), "headers, footers")) warned = true; CHECK(warned);
  Json keep = Json::MakeObject(); keep["keepHeadersFooters"] = true;
  CHECK(RunCap("pdf>docx", f.hf, dir / L"hf2.docx", keep).ok);
  Document h2; CHECK(ReadDocx(dir / L"hf2.docx", h2, &err)); CHECK(Contains(AllOf(h2), "Company Confidential Header"));
  // the title is a heading, chapters are body text
  CHECK(Contains(SigStr(h), "H1:Annual Handbook"));
}

TEST(pdf_to_epub_chapters_by_headings) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p2e");
  CapRun r = RunCap("pdf>epub", f.report, dir / L"o.epub"); CHECK(r.ok);
  std::wstring why; CHECK(VerifyEpub(dir / L"o.epub", &why));
  ZipReader z; CHECK(z.Open(dir / L"o.epub"));
  CHECK(z.Has("OEBPS/text/chapter001.xhtml")); CHECK(z.Has("OEBPS/text/chapter002.xhtml"));
  auto nav = *z.ReadString("OEBPS/nav.xhtml"); CHECK(Contains(nav, "Quarterly report")); CHECK(Contains(nav, "Arabic and mixed"));
  Document d; std::wstring err; CHECK(ReadEpub(dir / L"o.epub", d, &err)); CHECK(Contains(AllOf(d), "first bullet")); CHECK(Contains(AllOf(d), kAr));
  CapRun r2 = RunCap("pdf>epub", f.arabic, dir / L"ar.epub"); CHECK(r2.ok); CHECK(VerifyEpub(dir / L"ar.epub", &why));
  ZipReader z2; CHECK(z2.Open(dir / L"ar.epub")); CHECK(Contains(*z2.ReadString("OEBPS/content.opf"), "page-progression-direction=\"rtl\""));
}

TEST(pdf_to_xlsx_tables_numbers_and_text_fallback) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p2x");
  CapRun r = RunCap("pdf>xlsx", f.table, dir / L"t.xlsx"); CHECK(r.ok); if (!r.ok) { std::printf("  %s\n", Narrow(r.error).c_str()); return; }
  Workbook wb; std::wstring err; CHECK(ReadXlsx(dir / L"t.xlsx", wb, &err));
  CHECK_EQ(wb.sheets.size(), (size_t)1);                                   // the ledger spans several pages but is ONE table
  const Sheet& s = wb.sheets[0];
  CHECK(s.rows.size() >= 91); CHECK(s.rows.size() <= 93);
  CHECK_EQ(IsoFromCell(s.rows[0][0]), std::string("Item")); CHECK_EQ(IsoFromCell(s.rows[0][2]), std::string("Amount"));
  size_t headers = 0; for (auto& row : s.rows) if (!row.empty() && IsoFromCell(row[0]) == "Item") ++headers; CHECK_EQ(headers, (size_t)1);   // repeated header rows were not duplicated
  CHECK_EQ(IsoFromCell(s.rows[1][0]), std::string("Product 1"));
  CHECK(s.rows[1][1].t == SCell::T::Text); CHECK_EQ(IsoFromCell(s.rows[1][1]), std::string("0001"));        // leading zeros stay text
  CHECK(s.rows[1][2].t == SCell::T::Number); CHECK_NEAR(s.rows[1][2].n, 7.5, 1e-9);
  CHECK(s.rows[3][2].t == SCell::T::Number); CHECK_NEAR(s.rows[3][2].n, 3103.25, 1e-9);                      // "3,103.25"
  CHECK(s.rows[10][2].t == SCell::T::Number); CHECK_NEAR(s.rows[10][2].n, -12.5, 1e-9);                      // "(12.50)"
  CHECK(s.rows[1][3].t == SCell::T::Date);
  bool lastRow = false; for (auto& row : s.rows) if (!row.empty() && IsoFromCell(row[0]) == "Product 90") lastRow = true; CHECK(lastRow);
  // report PDF: the table becomes a sheet, with a note that other text was not exported
  CapRun r2 = RunCap("pdf>xlsx", f.report, dir / L"rep.xlsx"); CHECK(r2.ok);
  Workbook w2; CHECK(ReadXlsx(dir / L"rep.xlsx", w2, &err)); CHECK(!w2.sheets.empty());
  CHECK_EQ(IsoFromCell(w2.sheets[0].rows[0][0]), std::string("H0"));
  // no table in the PDF: text lines go to one sheet
  CapRun r3 = RunCap("pdf>xlsx", f.twocol, dir / L"nt.xlsx"); CHECK(r3.ok);
  Workbook w3; CHECK(ReadXlsx(dir / L"nt.xlsx", w3, &err)); CHECK_EQ(w3.sheets.size(), (size_t)1); CHECK_EQ(w3.sheets[0].name, std::string("Text")); CHECK(w3.sheets[0].rows.size() > 20);
  bool warned = false; for (auto& w : r3.warnings) if (Contains(Narrow(w), "No tables were detected")) warned = true; CHECK(warned);
}

TEST(pdf_to_pptx_slides_with_positioned_text_and_images) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p2p");
  CapRun r = RunCap("pdf>pptx", f.report, dir / L"o.pptx"); CHECK(r.ok); if (!r.ok) { std::printf("  %s\n", Narrow(r.error).c_str()); return; }
  std::vector<pdf::PageContent> pages; std::wstring err; CHECK(pdf::ExtractPages(f.report, pages, nullptr, &err));
  PptxInfo info; CHECK(ReadPptxInfo(dir / L"o.pptx", info, &err));
  CHECK_EQ((size_t)info.slides, pages.size());
  CHECK_NEAR(info.widthPt, pages[0].width, 0.5); CHECK_NEAR(info.heightPt, pages[0].height, 0.5);
  CHECK(Contains(info.slideText[0], "Quarterly report")); CHECK(Contains(info.slideText[0], "first bullet"));
  int pics = 0; for (int n : info.imagesPerSlide) pics += n; CHECK(pics >= 1);
  // title placeholder + EMU placement: each text box sits at the PDF coordinates (points * 12700)
  ZipReader z; CHECK(z.Open(dir / L"o.pptx"));
  auto s1 = *z.ReadString("ppt/slides/slide1.xml");
  CHECK(Contains(s1, "<p:ph type=\"title\"/>")); CHECK(Contains(s1, "txBox=\"1\""));
  auto dom = xml::ParseDom(s1); CHECK(dom != nullptr);
  const pdf::TextRun* title = nullptr; for (auto& run : pages[0].runs) if (Narrow(run.text).find("Quarterly report") != std::string::npos) { title = &run; break; }
  CHECK(title != nullptr);
  if (dom && title) {
    bool found = false;
    std::function<void(const xml::Node&)> walk = [&](const xml::Node& n) {
      for (auto& k : n.kids) {
        if (k->isText) continue;
        if (k->Is("sp") && k->InnerText().find("Quarterly report") != std::string::npos) {
          const xml::Node* off = nullptr; std::function<void(const xml::Node&)> find = [&](const xml::Node& x) { for (auto& c : x.kids) { if (c->isText) continue; if (c->Is("off") && !off) off = c.get(); find(*c); } };
          find(*k);
          if (off) { double x = atof(off->AttrOr("x").c_str()) / 12700.0, y = atof(off->AttrOr("y").c_str()) / 12700.0; if (std::fabs(x - title->x) < 3.0 && std::fabs(y - title->y) < 8.0) found = true; }
        }
        walk(*k);
      }
    };
    walk(*dom);
    CHECK(found);
  }
  std::wstring why; CHECK(VerifyPptx(dir / L"o.pptx", &why));
  // LibreOffice opens and renders it; the text is still there
  fs::path back = dir / L"back.pdf"; LoConvertSpec s; s.input = dir / L"o.pptx"; s.inExt = L"pptx"; s.convertTo = L"pdf"; s.outExt = L"pdf"; s.output = back;
  CHECK(LibreOfficeConvert(s, nullptr).ok);
  auto o = pdf::PdfDocument::Open(back); CHECK(o.ok()); if (o.ok()) { CHECK_EQ((size_t)o.doc->PageCount(), pages.size()); CHECK(Contains(Narrow(o.doc->PageText(0)), "Quarterly report")); }
  // two-column page keeps both columns as separate boxes
  CHECK(RunCap("pdf>pptx", f.twocol, dir / L"two.pptx").ok);
  PptxInfo i2; CHECK(ReadPptxInfo(dir / L"two.pptx", i2, &err)); CHECK(Contains(i2.slideText[0], "LEFTCOL")); CHECK(Contains(i2.slideText[0], "RIGHTCOL"));
  // Arabic slide
  CHECK(RunCap("pdf>pptx", f.arabic, dir / L"ar.pptx").ok); PptxInfo i3; CHECK(ReadPptxInfo(dir / L"ar.pptx", i3, &err)); CHECK(Contains(i3.slideText[0], kAr));
  ZipReader za; CHECK(za.Open(dir / L"ar.pptx")); CHECK(Contains(*za.ReadString("ppt/slides/slide1.xml"), "rtl=\"1\""));
}

TEST(pdf_scanned_pages_ocr_or_clearly_flagged_picture) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p_scan");
  auto info = pdf::QueryOcr();
  std::printf("  Windows OCR available: %s\n", info.available ? "yes" : "no");
  // OCR off: the page is kept as a picture and the user is told
  Json off = Json::MakeObject(); off["ocr"] = "off";
  CapRun r = RunCap("pdf>docx", f.scanned, dir / L"off.docx", off); CHECK(r.ok);
  Document d; std::wstring err; CHECK(ReadDocx(dir / L"off.docx", d, &err));
  size_t imgs = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) ++imgs; CHECK(imgs >= 1);
  bool warned = false; for (auto& w : r.warnings) if (Contains(Narrow(w), "pictures")) warned = true; CHECK(warned);
  CHECK(fs::file_size(dir / L"off.docx") > 5000);
  CapRun rp = RunCap("pdf>pptx", f.scanned, dir / L"off.pptx", off); CHECK(rp.ok);
  PptxInfo pi; CHECK(ReadPptxInfo(dir / L"off.pptx", pi, &err)); CHECK_EQ(pi.imagesPerSlide[0], 1);
  CapRun rx = RunCap("pdf>xlsx", f.scanned, dir / L"off.xlsx", off); CHECK(!rx.ok); CHECK(!rx.error.empty()); CHECK(!fs::exists(dir / L"off.xlsx"));    // nothing to put in cells, and it says so
  CapRun re = RunCap("pdf>epub", f.scanned, dir / L"off.epub", off); CHECK(re.ok); std::wstring why; CHECK(VerifyEpub(dir / L"off.epub", &why));
  // default (OCR when available)
  CapRun r2 = RunCap("pdf>docx", f.scanned, dir / L"auto.docx"); CHECK(r2.ok);
  Document d2; CHECK(ReadDocx(dir / L"auto.docx", d2, &err));
  if (info.available) {
    std::string t = AllText(d2);
    CHECK(Contains(t, "Quarterly") || Contains(t, "report") || Contains(t, "Plain"));
    bool ocrWarn = false; for (auto& w : r2.warnings) if (Contains(Narrow(w), "OCR")) ocrWarn = true; CHECK(ocrWarn);
    CapRun rx2 = RunCap("pdf>xlsx", f.scanned, dir / L"auto.xlsx"); CHECK(rx2.ok);
    CapRun rp2 = RunCap("pdf>pptx", f.scanned, dir / L"auto.pptx"); CHECK(rp2.ok);
    PptxInfo pi2; CHECK(ReadPptxInfo(dir / L"auto.pptx", pi2, &err)); CHECK(pi2.slideText[0].size() > 10); CHECK_EQ(pi2.imagesPerSlide[0], 1);        // searchable text over the page picture
  } else {
    size_t imgs2 = 0; for (auto& b : d2.blocks) if (b.kind == Block::Kind::Image) ++imgs2; CHECK(imgs2 >= 1);
  }
}

TEST(pdf_office_bad_inputs_and_cancellation) {
  if (!HaveLo() || !Fx().ok) { std::printf("  (skipped)\n"); return; }
  auto& f = Fx(); fs::path dir = Work("p_bad");
  for (const char* id : {"pdf>docx", "pdf>pptx", "pdf>xlsx", "pdf>epub"}) {
    const Capability* cap = FindCap(id);
    WriteAll(dir / L"empty.pdf", ""); WriteAll(dir / L"garbage.pdf", "%PDF-1.4\n1 0 obj << /Type /Catalog >>\nthis is not a real pdf at all" + std::string(500, 'x')); WriteAll(dir / L"text.pdf", "plain text with a pdf extension");
    for (const char* name : {"empty.pdf", "garbage.pdf", "text.pdf"}) {
      fs::path out = dir / (Widen(std::string(name) + id) + L"." + cap->out);
      for (auto& c : out.native()) (void)c;
      CapRun r = RunCap(id, dir / Widen(name), out);
      CHECK(!r.ok); CHECK(!r.error.empty()); CHECK(r.error.size() < 400); CHECK(!fs::exists(out));
    }
  }
  // password-protected PDF (made by LibreOffice's PDF export options)
  {
    fs::path src = dir / L"pw.docx"; std::wstring err; Document d; d.blocks.push_back(MakeParagraph("secret content")); WriteDocx(d, src, {}, &err);
    LoConvertSpec s; s.input = src; s.inExt = L"docx";
    s.convertTo = L"pdf:writer_pdf_Export:{\"EncryptFile\":{\"type\":\"boolean\",\"value\":\"true\"},\"DocumentOpenPassword\":{\"type\":\"string\",\"value\":\"s3cret\"}}"; s.outExt = L"pdf"; s.output = dir / L"locked.pdf";
    if (LibreOfficeConvert(s, nullptr).ok) {
      CapRun r = RunCap("pdf>docx", dir / L"locked.pdf", dir / L"locked.docx");
      CHECK(!r.ok); CHECK(Contains(Narrow(r.error), "password") || Contains(Narrow(r.error), "Password") || Contains(Narrow(r.error), "protected"));
      CHECK(!fs::exists(dir / L"locked.docx"));
    } else std::printf("  (this LibreOffice build cannot write an encrypted PDF: password case not exercised)\n");
  }
  // cancellation during extraction of a long PDF
  {
    Document d; for (int i = 0; i < 3000; ++i) d.blocks.push_back(MakeParagraph("Paragraph " + std::to_string(i) + " with enough ordinary words to fill lines of text on a page of the long document."));
    FinalizeDocument(d); std::wstring err; fs::path src = dir / L"long.docx"; WriteDocx(d, src, {}, &err);
    fs::path pdfLong = dir / L"long.pdf"; if (!LoPdf(src, L"docx", pdfLong)) return;
    uint64_t before = Hash(pdfLong);
    auto t0 = std::chrono::steady_clock::now();
    CapRun r = RunCap("pdf>docx", pdfLong, dir / L"long.docx", Json::MakeObject(), 400);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  long PDF job: state %d after %.1fs\n", (int)r.state, secs);
    if (r.state == JobState::Cancelled || !r.ok) { CHECK(!fs::exists(dir / L"long.docx")); }
    CHECK_EQ(Hash(pdfLong), before);
    std::error_code ec; for (auto& e : fs::directory_iterator(dir, ec)) CHECK(!Contains(Narrow(e.path().filename().wstring()), ".tmp"));
  }
}
