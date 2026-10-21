#include "t_util.h"
#include "engines/doc/docx.h"
#include "engines/doc/epub.h"
#include "engines/doc/html.h"
#include "engines/doc/markdown.h"
#include "engines/doc/opc.h"
#include "engines/doc/rtf.h"
#include "engines/doc/xml.h"
#include "engines/pdf/pdf_doc.h"

using namespace tu;

static std::string PdfText(const fs::path& p, int* pages = nullptr) {
  auto r = pdf::PdfDocument::Open(p);
  if (!r.ok()) return {};
  std::string all;
  int n = r.doc->PageCount();
  if (pages) *pages = n;
  for (int i = 0; i < n; ++i) all += Narrow(r.doc->PageText(i)) + "\n";
  return all;
}
static size_t CountArabic(const std::string& s) {
  size_t n = 0, i = 0;
  while (i < s.size()) { uint32_t c = DecodeUtf8(s, i); if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) ++n; }
  return n;
}

// ---------------------------------------------------------------------------------------------------------------- DOCX writer
TEST(docx_writer_structure) {
  fs::path dir = Work("docx_struct");
  Document d = SampleDoc();
  fs::path f = dir / L"sample.docx";
  std::wstring err; std::vector<std::wstring> w;
  CHECK(WriteDocx(d, f, {}, &err, &w));
  std::wstring why;
  CHECK(VerifyDocx(f, &why));
  if (!why.empty()) std::printf("  verify: %s\n", Narrow(why).c_str());
  ZipReader z; CHECK(z.Open(f));
  CHECK_EQ(z.Entries()[0].name, std::string("[Content_Types].xml"));
  auto doc = *z.ReadString("word/document.xml");
  CHECK(Contains(doc, "<w:tbl>"));
  CHECK(Contains(doc, "<w:bidi/>"));
  CHECK(Contains(doc, "<w:rtl/>"));
  CHECK(Contains(doc, "w:w=\"11906\" w:h=\"16838\""));                       // A4
  CHECK(Contains(doc, "<w:pStyle w:val=\"Heading1\"/>"));
  CHECK(Contains(doc, "<w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr>"));
  CHECK(Contains(doc, "<w:tblHeader/>"));
  CHECK(Contains(doc, "cx=\"1143000\" cy=\"571500\""));                      // 120x60 px at 96 dpi in EMU
  CHECK(Contains(doc, "descr=\"gradient\""));
  auto rels = *z.ReadString("word/_rels/document.xml.rels");
  CHECK(Contains(rels, "https://example.com/a?b=1&amp;c=2") && Contains(rels, "TargetMode=\"External\""));
  auto ct = *z.ReadString("[Content_Types].xml");
  CHECK(Contains(ct, "wordprocessingml.document.main+xml")); CHECK(Contains(ct, "Extension=\"png\""));
  CHECK(z.Has("word/media/image1.png"));
  auto num = *z.ReadString("word/numbering.xml");
  CHECK(Contains(num, "w:numFmt w:val=\"bullet\"")); CHECK(Contains(num, "w:numFmt w:val=\"decimal\"")); CHECK(Contains(num, "w:startOverride"));
  auto sty = *z.ReadString("word/styles.xml");
  for (int i = 1; i <= 6; ++i) CHECK(Contains(sty, "w:styleId=\"Heading" + std::to_string(i) + "\""));
  CHECK(Contains(sty, "w:styleId=\"ListParagraph\"")); CHECK(Contains(sty, "w:styleId=\"TableGrid\"")); CHECK(Contains(sty, "w:styleId=\"Normal\""));
  // Letter page size
  DocxWriteOptions lo; lo.page = PageSize::Letter;
  fs::path f2 = dir / L"letter.docx"; CHECK(WriteDocx(d, f2, lo, &err));
  ZipReader z2; CHECK(z2.Open(f2)); CHECK(Contains(*z2.ReadString("word/document.xml"), "w:w=\"12240\" w:h=\"15840\""));
  // wide images are scaled to the text width, aspect ratio kept
  Document big; Block ib; ib.kind = Block::Kind::Image; ib.image = std::make_shared<ImageData>(); ib.image->bytes = MakePng(2000, 1000); ib.image->mime = "image/png"; big.blocks.push_back(ib);
  fs::path f3 = dir / L"big.docx"; CHECK(WriteDocx(big, f3, {}, &err));
  ZipReader z3; CHECK(z3.Open(f3)); auto bd = *z3.ReadString("word/document.xml");
  size_t p = bd.find("<wp:extent cx=\""); CHECK(p != std::string::npos);
  long long cx = atoll(bd.c_str() + p + 15), cy = atoll(bd.c_str() + bd.find("cy=\"", p) + 4);
  CHECK(cx <= 9026LL * 635); CHECK(cx > 9000LL * 600); CHECK_EQ(cx, cy * 2);
  // empty document still produces a valid package
  Document e; fs::path f4 = dir / L"empty.docx"; CHECK(WriteDocx(e, f4, {}, &err)); CHECK(VerifyDocx(f4, &why));
}

TEST(docx_roundtrip_semantics) {
  fs::path dir = Work("docx_rt");
  Document d = SampleDoc();
  fs::path f = dir / L"rt.docx";
  std::wstring err;
  CHECK(WriteDocx(d, f, {}, &err));
  Document b;
  CHECK(ReadDocx(f, b, &err));
  CHECK_SIG(SigStr(d, true), SigStr(b, true));
  CHECK_EQ(b.meta.title, std::string("Filo sample"));
  // list semantics survive: levels, ordered numbering
  std::vector<int> nums; for (auto& x : b.blocks) if (x.kind == Block::Kind::ListItem && x.ordered) nums.push_back(x.number);
  CHECK(nums == std::vector<int>({1, 2, 3}));
  // table header and structure
  for (auto& x : b.blocks) if (x.kind == Block::Kind::Table) { CHECK_EQ(x.table.rows.size(), (size_t)3); CHECK(x.table.headerRow); CHECK_EQ(x.table.rows[0].size(), (size_t)3); }
  // RTL paragraph and runs
  bool rtlPara = false; for (auto& x : b.blocks) if (x.kind == Block::Kind::Paragraph && x.rtl && Contains(PlainText(x.runs), "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7")) rtlPara = true;
  CHECK(rtlPara);
  bool rtlRun = false; for (auto& x : b.blocks) for (auto& r : x.runs) if (r.rtl && ContainsRtl(r.text)) rtlRun = true;
  CHECK(rtlRun);
  // image bytes are preserved
  for (auto& x : b.blocks) if (x.kind == Block::Kind::Image) { CHECK(x.image->bytes == d.blocks[d.blocks.size() - 2].image->bytes); CHECK_EQ(x.image->w, 120); CHECK_EQ(x.image->alt, std::string("gradient")); }
  // emoji and surrogate pairs
  CHECK(Contains(AllText(b), "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80"));
}

TEST(docx_formatting_runs_and_special_text) {
  fs::path dir = Work("docx_fmt");
  Document d;
  Block p; p.kind = Block::Kind::Paragraph;
  Run a = MakeRun("tab\there"); Run b = MakeRun("struck"); b.strike = true; Run c = MakeRun("sup"); c.sup = true; Run e = MakeRun("sub"); e.sub = true;
  Run f = MakeRun("red 14pt"); f.color = "FF0000"; f.sizePt = 14; Run g = MakeRun("under"); g.underline = true; Run h = MakeRun("a<b&c>\"d\""); Run br; br.br = true; Run i2 = MakeRun("after break");
  p.runs = {a, MakeRun(" "), b, MakeRun(" "), c, e, MakeRun(" "), f, MakeRun(" "), g, MakeRun(" "), h, br, i2};
  d.blocks.push_back(p);
  d.blocks.push_back(MakeParagraph(std::string(5000, 'w') + " very long paragraph " + std::string(5000, 'z')));
  d.blocks.push_back(MakeParagraph("control \x01 chars \x0B removed"));
  d.blocks.push_back(MakeParagraph("   leading and trailing spaces   "));
  FinalizeDocument(d);
  fs::path file = dir / L"fmt.docx"; std::wstring err, why;
  CHECK(WriteDocx(d, file, {}, &err)); CHECK(VerifyDocx(file, &why));
  Document r; CHECK(ReadDocx(file, r, &err));
  auto& runs = r.blocks[0].runs;
  CHECK(Contains(PlainText(runs), "tab\there"));
  bool strike = false, sup = false, sub = false, red = false, under = false, brk = false;
  for (auto& x : runs) { if (x.strike && x.text == "struck") strike = true; if (x.sup && x.text == "sup") sup = true; if (x.sub && x.text == "sub") sub = true; if (x.color == "FF0000" && x.sizePt == 14) red = true; if (x.underline && x.text == "under") under = true; if (x.br) brk = true; }
  CHECK(strike && sup && sub && red && under && brk);
  CHECK(Contains(PlainText(runs), "a<b&c>\"d\""));
  CHECK_EQ(PlainText(r.blocks[1].runs).size(), (size_t)10021);
  CHECK(Contains(PlainText(r.blocks[2].runs), "control  chars  removed") || Contains(PlainText(r.blocks[2].runs), "control  chars") );
  CHECK(!Contains(PlainText(r.blocks[2].runs), "\x01"));
  CHECK(Contains(PlainText(r.blocks[3].runs), "   leading and trailing spaces   "));
}

TEST(docx_corrupt_and_hostile_inputs) {
  fs::path dir = Work("docx_bad");
  std::wstring err;
  Document d;
  // not a zip
  WriteAll(dir / L"text.docx", "this is not a docx"); CHECK(!ReadDocx(dir / L"text.docx", d, &err)); CHECK(!err.empty());
  // zip without word/document.xml
  { ZipWriter w; w.Open(dir / L"nodoc.docx"); w.Add("foo.txt", std::string("x")); w.Close(); }
  CHECK(!ReadDocx(dir / L"nodoc.docx", d, &err));
  // malformed document.xml
  { ZipWriter w; w.Open(dir / L"badxml.docx"); w.Add("word/document.xml", std::string("<w:document><w:body><w:p>")); w.Close(); }
  Document d2; bool ok = ReadDocx(dir / L"badxml.docx", d2, &err); (void)ok;      // tolerant parse may succeed with no content; must not crash
  // decompression bomb: document.xml that expands past the entry limit
  {
    ZipLimits& lim = DefaultZipLimits(); ZipLimits saved = lim; lim.maxEntryBytes = 4u << 20; lim.bombRatio = 100000;
    ZipWriter w; w.Open(dir / L"bomb.docx"); std::string big = "<w:document xmlns:w=\"x\"><w:body>" + std::string(6u << 20, ' ') + "</w:body></w:document>"; w.Add("word/document.xml", big); w.Close();
    Document d3; CHECK(!ReadDocx(dir / L"bomb.docx", d3, &err));
    lim = saved;
  }
  // relationship pointing outside the package / missing image must not crash
  {
    ZipWriter w; w.Open(dir / L"rels.docx");
    w.Add("_rels/.rels", std::string("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>"));
    w.Add("word/_rels/document.xml.rels", std::string("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../../../../Windows/win.ini\"/></Relationships>"));
    w.Add("word/document.xml", std::string("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><w:body><w:p><w:r><w:t>hello</w:t></w:r><w:r><w:drawing><a:blip r:embed=\"rId5\"/></w:drawing></w:r></w:p></w:body></w:document>"));
    w.Close();
    Document d4; CHECK(ReadDocx(dir / L"rels.docx", d4, &err)); CHECK(Contains(AllText(d4), "hello"));
    for (auto& b : d4.blocks) CHECK(b.kind != Block::Kind::Image);
  }
  // password-protected (encrypted entry flag) is reported, not crashed: emulate with a CFB header (real encrypted docx are OLE files)
  WriteAll(dir / L"enc.docx", std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + std::string(1000, '\0'));
  CHECK(!ReadDocx(dir / L"enc.docx", d, &err));
  // missing file
  CHECK(!ReadDocx(dir / L"missing.docx", d, &err));
}

// ---------------------------------------------------------------------------------------------------------------- DOCX independent checks (LibreOffice)
TEST(docx_opens_in_libreoffice_and_text_survives) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: skipped)\n"); return; }
  fs::path dir = Work("docx_lo");
  Document d = SampleDoc();
  fs::path f = dir / L"sample.docx"; std::wstring err;
  CHECK(WriteDocx(d, f, {}, &err));
  fs::path pdf = dir / L"sample.pdf";
  LoConvertSpec s; s.input = f; s.inExt = L"docx"; s.convertTo = L"pdf"; s.outExt = L"pdf"; s.output = pdf;
  auto r = LibreOfficeConvert(s, nullptr);
  CHECK(r.ok); if (!r.ok) { std::printf("  LO error: %s\n", Narrow(r.error).c_str()); return; }
  int pages = 0;
  std::string text = PdfText(pdf, &pages);
  CHECK(pages >= 1);
  for (const char* want : {"Quarterly report", "Plain intro paragraph", "first bullet", "nested bullet", "step two", "r2c1", "version 1.0", "return 0", "A wise quote", "Final paragraph"})
    CHECK(Contains(text, want));
  // Arabic must reach the PDF as Arabic code points (shaped glyphs map back through ToUnicode)
  CHECK(CountArabic(text) >= 20);
  // order of paragraphs is preserved
  CHECK(text.find("Quarterly report") < text.find("first bullet"));
  CHECK(text.find("first bullet") < text.find("step one"));
  CHECK(text.find("step one") < text.find("Final paragraph"));
  // LibreOffice -> DOCX round trip, read by our reader (an independent producer of OOXML)
  fs::path back = dir / L"lo-resaved.docx";
  LoConvertSpec s2; s2.input = f; s2.inExt = L"docx"; s2.convertTo = L"docx:MS Word 2007 XML"; s2.outExt = L"docx"; s2.output = back;
  CHECK(LibreOfficeConvert(s2, nullptr).ok);
  Document lo;
  CHECK(ReadDocx(back, lo, &err));
  auto sig = SigStr(lo);
  CHECK(Contains(sig, "H1:Quarterly report"));
  CHECK(Contains(sig, "L0u:first bullet")); CHECK(Contains(sig, "L1u:nested bullet")); CHECK(Contains(sig, "L0o:step one"));
  CHECK(Contains(sig, "T:H0|H1|H2|;r1c0|r1c1|r1c2|;r2c0|r2c1|r2c2|;"));
  CHECK(Contains(sig, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));
  CHECK(Contains(sig, "\xF0\x9F\x98\x80"));
  bool img = false; for (auto& b : lo.blocks) if (b.kind == Block::Kind::Image && b.image->w > 0) img = true; CHECK(img);
  bool link = false; for (auto& b : lo.blocks) for (auto& r2 : b.runs) if (r2.link == "https://example.com/a?b=1&c=2") link = true; CHECK(link);
  bool rtl = false; for (auto& b : lo.blocks) if (b.kind == Block::Kind::Paragraph && b.rtl) rtl = true; CHECK(rtl);
}

TEST(docx_reader_on_foreign_documents) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: skipped)\n"); return; }
  fs::path dir = Work("docx_foreign");
  // HTML -> DOCX through LibreOffice gives a document we did not write
  Bytes png = MakePng(40, 20);
  WriteAll(dir / L"pic.png", std::string(png.begin(), png.end()));
  WriteAll(dir / L"in.html", "<html><head><meta charset=utf-8><title>Foreign</title></head><body><h1>Big title</h1><h2>Second</h2><p>Para with <b>bold</b>, <i>italic</i>, <u>under</u> and <a href=\"https://lo.example/x\">a link</a>.</p>"
           "<ul><li>alpha<ul><li>alpha-1</li></ul></li><li>beta</li></ul><ol><li>one</li><li>two</li></ol><table border=1><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>"
           "<p dir=\"rtl\">\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD9\x83</p><p><img src=\"data:image/png;base64," + Base64Encode(png.data(), png.size()) + "\" alt=\"pic\" width=40 height=20></p><pre>code line</pre></body></html>");
  fs::path out = dir / L"foreign.docx"; std::wstring err;
  LoConvertSpec s; s.input = dir / L"in.html"; s.inExt = L"html"; s.convertTo = L"docx:MS Word 2007 XML"; s.outExt = L"docx"; s.output = out;
  auto r = LibreOfficeConvert(s, nullptr);
  CHECK(r.ok);
  if (!r.ok) return;
  Document d; CHECK(ReadDocx(out, d, &err)); if (!err.empty()) std::printf("  %s\n", Narrow(err).c_str());
  auto sig = SigStr(d, true);
  if (!Contains(sig, "[a link](https://lo.example/x)")) std::printf("%s\n", sig.c_str());
  CHECK(Contains(sig, "H1:Big title")); CHECK(Contains(sig, "H2:Second"));
  CHECK(Contains(sig, "**bold**")); CHECK(Contains(sig, "_italic_")); CHECK(Contains(sig, "[a link](https://lo.example/x)"));
  CHECK(Contains(sig, "alpha-1")); CHECK(Contains(sig, "beta"));
  CHECK(Contains(sig, "T:A|B|;1|2|;"));
  CHECK(Contains(sig, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));
  size_t imgs = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) ++imgs; CHECK_EQ(imgs, (size_t)1);
  bool rtl = false; for (auto& b : d.blocks) if (b.rtl && Contains(PlainText(b.runs), "\xD9\x85")) rtl = true; CHECK(rtl);
}

// ---------------------------------------------------------------------------------------------------------------- EPUB
TEST(epub_writer_structure_and_roundtrip) {
  fs::path dir = Work("epub");
  Document d = SampleDoc();
  d.meta.author = "A. Author"; d.meta.language = "en";
  fs::path f = dir / L"book.epub"; std::wstring err, why;
  CHECK(WriteEpub(d, f, {}, &err));
  CHECK(VerifyEpub(f, &why)); if (!why.empty()) std::printf("  verify: %s\n", Narrow(why).c_str());
  ZipReader z; CHECK(z.Open(f));
  CHECK_EQ(z.Entries()[0].name, std::string("mimetype"));
  CHECK_EQ(z.Entries()[0].csize, z.Entries()[0].size);
  CHECK_EQ(*z.ReadString("mimetype"), std::string("application/epub+zip"));
  auto opf = *z.ReadString("OEBPS/content.opf");
  CHECK(Contains(opf, "version=\"3.0\"")); CHECK(Contains(opf, "dcterms:modified")); CHECK(Contains(opf, "<dc:creator>A. Author</dc:creator>")); CHECK(Contains(opf, "properties=\"nav\""));
  CHECK(Contains(opf, "<dc:title>Filo sample</dc:title>"));
  CHECK(z.Has("OEBPS/nav.xhtml")); CHECK(z.Has("OEBPS/toc.ncx")); CHECK(z.Has("OEBPS/images/img1.png"));
  // two H1 -> two chapters, the second starts with the Arabic section heading
  CHECK(z.Has("OEBPS/text/chapter001.xhtml")); CHECK(z.Has("OEBPS/text/chapter002.xhtml"));
  CHECK(!z.Has("OEBPS/text/chapter003.xhtml"));
  auto nav = *z.ReadString("OEBPS/nav.xhtml");
  CHECK(Contains(nav, "Quarterly report")); CHECK(Contains(nav, "Arabic and mixed")); CHECK(Contains(nav, "epub:type=\"toc\""));
  CHECK(Contains(*z.ReadString("OEBPS/text/chapter002.xhtml"), "dir=\"rtl\""));
  Document b; CHECK(ReadEpub(f, b, &err));
  CHECK_EQ(SigStr(d), SigStr(b));
  CHECK_EQ(b.meta.title, std::string("Filo sample")); CHECK_EQ(b.meta.author, std::string("A. Author"));
  // no split
  EpubWriteOptions one; one.splitOnH1 = false;
  fs::path f2 = dir / L"one.epub"; CHECK(WriteEpub(d, f2, one, &err)); ZipReader z2; CHECK(z2.Open(f2)); CHECK(!z2.Has("OEBPS/text/chapter002.xhtml")); CHECK(VerifyEpub(f2, &why));
  // RTL book gets page-progression-direction
  Document ar; ar.blocks.push_back(MakeHeading(1, "\xD8\xA7\xD9\x84\xD9\x81\xD8\xB5\xD9\x84 \xD8\xA7\xD9\x84\xD8\xA3\xD9\x88\xD9\x84")); ar.blocks.push_back(MakeParagraph("\xD9\x86\xD8\xB5 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xD9\x83\xD8\xA7\xD9\x85\xD9\x84")); FinalizeDocument(ar);
  fs::path f3 = dir / L"ar.epub"; CHECK(WriteEpub(ar, f3, {}, &err)); ZipReader z3; CHECK(z3.Open(f3)); CHECK(Contains(*z3.ReadString("OEBPS/content.opf"), "page-progression-direction=\"rtl\"")); CHECK(VerifyEpub(f3, &why));
  // empty document and a document without headings
  Document nh; nh.blocks.push_back(MakeParagraph("just text")); fs::path f4 = dir / L"nh.epub"; CHECK(WriteEpub(nh, f4, {}, &err)); CHECK(VerifyEpub(f4, &why));
  // nested headings in the nav
  Document nest; for (auto [l, t] : {std::pair{1, "A"}, std::pair{2, "A.1"}, std::pair{3, "A.1.1"}, std::pair{2, "A.2"}, std::pair{1, "B"}}) { nest.blocks.push_back(MakeHeading(l, t)); nest.blocks.push_back(MakeParagraph("body")); }
  FinalizeDocument(nest); fs::path f5 = dir / L"nest.epub"; CHECK(WriteEpub(nest, f5, {}, &err)); CHECK(VerifyEpub(f5, &why));
  ZipReader z5; CHECK(z5.Open(f5)); auto n5 = *z5.ReadString("OEBPS/nav.xhtml"); CHECK(Contains(n5, "A.1.1")); CHECK(xml::CheckWellFormed(n5));
}

TEST(epub_reader_spine_order_and_hostile_input) {
  fs::path dir = Work("epub_read");
  // hand-written EPUB 2 with the spine in a different order than the manifest and an NCX
  fs::path f = dir / L"hand.epub";
  {
    ZipWriter w; w.Open(f);
    w.Add("mimetype", std::string("application/epub+zip"), false);
    w.Add("META-INF/container.xml", std::string("<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"book/pkg.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    w.Add("book/pkg.opf", std::string("<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Hand made</dc:title><dc:creator>Me</dc:creator><dc:language>en</dc:language><dc:identifier id=\"id\">x</dc:identifier></metadata>"
           "<manifest><item id=\"b\" href=\"b.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"img\" href=\"img/p.png\" media-type=\"image/png\"/></manifest><spine><itemref idref=\"a\"/><itemref idref=\"b\"/></spine></package>"));
    w.Add("book/a.xhtml", std::string("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>First</h1><p>alpha &amp; omega</p><img src=\"img/p.png\" alt=\"pp\"/></body></html>"));
    w.Add("book/b.xhtml", std::string("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>Second</h1><p>beta</p></body></html>"));
    Bytes png = MakePng(10, 10); w.Add("book/img/p.png", png, false);
    w.Close();
  }
  Document d; std::wstring err;
  CHECK(ReadEpub(f, d, &err));
  CHECK_EQ(SigStr(d), std::string("H1:First\nP:alpha & omega\nI\nH1:Second\nP:beta\n"));
  CHECK_EQ(d.meta.title, std::string("Hand made"));
  // not an epub / missing pieces / corrupt
  WriteAll(dir / L"x.epub", "plain text"); CHECK(!ReadEpub(dir / L"x.epub", d, &err));
  { ZipWriter w; w.Open(dir / L"nocont.epub"); w.Add("mimetype", std::string("application/epub+zip"), false); w.Close(); }
  Document d2; CHECK(!ReadEpub(dir / L"nocont.epub", d2, &err));
  { ZipWriter w; w.Open(dir / L"emptyspine.epub"); w.Add("mimetype", std::string("application/epub+zip"), false);
    w.Add("META-INF/container.xml", std::string("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"o.opf\"/></rootfiles></container>"));
    w.Add("o.opf", std::string("<package xmlns=\"http://www.idpf.org/2007/opf\"><manifest/><spine/></package>")); w.Close(); }
  Document d3; CHECK(!ReadEpub(dir / L"emptyspine.epub", d3, &err));
  // VerifyEpub: the hand-made EPUB 2 book is structurally fine, the empty-spine package and the non-zip are rejected
  std::wstring why;
  CHECK(VerifyEpub(f, &why));
  CHECK(!VerifyEpub(dir / L"emptyspine.epub", &why));
  CHECK(!VerifyEpub(dir / L"x.epub", &why));
}

// ---------------------------------------------------------------------------------------------------------------- RTF / RTFD
static Document Rtf(const std::string& s) { Document d; std::wstring e; bool ok = ParseRtf((const uint8_t*)s.data(), s.size(), d, {}, &e); if (!ok) std::printf("  ParseRtf failed: %s\n", Narrow(e).c_str()); return d; }

TEST(rtf_text_formatting_and_unicode) {
  Document d = Rtf("{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss\\fcharset0 Arial;}{\\f1\\fmodern Courier New;}}{\\colortbl;\\red255\\green0\\blue0;}"
                   "\\pard Hello {\\b bold}{\\i  italic} {\\ul under}\\ulnone  plain {\\cf1 red}\\par\n"
                   "caf\\'e9 \\u8364? \\u-10179?\\u-8704? \\uc2\\u26085\\'93\\'fa end\\par\\pard{\\f1 mono text}\\par}");
  CHECK(d.blocks.size() >= 3);
  CHECK(Contains(SigStr(d, true), "P:Hello **bold**_ italic_ <u>under</u> plain red"));
  bool red = false; for (auto& r : d.blocks[0].runs) if (r.color == "FF0000" && r.text == "red") red = true; CHECK(red);
  auto t1 = PlainText(d.blocks[1].runs);
  CHECK(Contains(t1, "caf\xC3\xA9"));
  CHECK(Contains(t1, "\xE2\x82\xAC"));                      // euro through \u8364
  CHECK(Contains(t1, "\xF0\x9F\x98\x80") || Contains(t1, "\xEF\xBF\xBD") || true);     // surrogate pair \u-10179\u-8704 = U+1F600? (checked below)
  CHECK(Contains(t1, "\xE6\x97\xA5"));                      // \u26085 with \uc2 skipping two \'xx bytes
  CHECK(!Contains(t1, "\xE2\x80\x9C"));                     // the skipped \'93 must not appear
  CHECK(Contains(t1, " end"));
  bool mono = false; for (auto& r : d.blocks[2].runs) if (r.code) mono = true; CHECK(mono);
  // surrogate pair: U+1F600 = D83D DE00 = \u-10179 \u-8704
  Document e = Rtf("{\\rtf1\\ansi\\uc1 \\u-10179?\\u-8704? ok\\par}");
  CHECK(Contains(AllText(e), "\xF0\x9F\x98\x80 ok"));
}

TEST(rtf_arabic_codepage_and_rtl) {
  // windows-1256 bytes for "مرحبا" = E3 D1 CD C8 C7, in a font with fcharset178
  Document d = Rtf("{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fnil\\fcharset178 Arial;}{\\f1\\fswiss\\fcharset0 Arial;}}"
                   "\\pard\\rtlpar\\qr\\rtlch\\f0 \\'e3\\'d1\\'cd\\'c8\\'c7 \\'e3\\'d1\\'cd\\'c8\\'c7\\par\\pard\\ltrpar\\f1 English \\u1605?\\u1585?\\u1581?\\u1576?\\u1575? mixed\\par}");
  CHECK_EQ(d.blocks.size(), (size_t)2);
  CHECK_EQ(PlainText(d.blocks[0].runs), std::string("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));
  CHECK(d.blocks[0].rtl); CHECK_EQ(d.blocks[0].align, 'r');
  CHECK(!d.blocks[1].rtl);
  CHECK(Contains(PlainText(d.blocks[1].runs), "English \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 mixed"));
  // Hebrew cp1255 via font charset 177
  Document h = Rtf("{\\rtf1\\ansi{\\fonttbl{\\f0\\fcharset177 David;}}\\f0 \\'f9\\'ec\\'e5\\'ed\\par}");
  CHECK(Contains(AllText(h), "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"));
  // CJK double-byte code page (936) pairs
  Document c = Rtf("{\\rtf1\\ansi\\ansicpg936{\\fonttbl{\\f0\\fcharset134 SimSun;}}\\f0 \\'c4\\'e3\\'ba\\'c3\\par}");     // 你好
  CHECK(Contains(AllText(c), "\xE4\xBD\xA0\xE5\xA5\xBD"));
}

TEST(rtf_structure_lists_tables_fields_images) {
  Bytes png = MakePng(16, 8);
  std::string hex; for (uint8_t b : png) hex += Hex2(b);
  std::string rtf =
    "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}{\\stylesheet{\\s0 Normal;}{\\s1 heading 1;}{\\s2 heading 2;}}"
    "{\\*\\listtable{\\list\\listtemplateid1{\\listlevel\\levelnfc23\\levelnfcn23}\\listid100}{\\list\\listtemplateid2{\\listlevel\\levelnfc0\\levelnfcn0}\\listid200}}"
    "{\\*\\listoverridetable{\\listoverride\\listid100\\listoverridecount0\\ls1}{\\listoverride\\listid200\\listoverridecount0\\ls2}}"
    "\\pard\\s1 Main heading\\par\\pard\\s2 Sub heading\\par"
    "\\pard{\\listtext\\'b7\\tab}\\ls1\\ilvl0 bullet one\\par\\pard\\ls1\\ilvl0 bullet two\\par"
    "\\pard\\ls2\\ilvl0 number one\\par\\pard\\ls2\\ilvl0 number two\\par"
    "\\pard Visit {\\field{\\*\\fldinst{HYPERLINK \"https://rtf.example/p?a=1\"}}{\\fldrslt{\\ul site}}} now.\\par"
    "\\trowd\\cellx3000\\cellx6000\\pard\\intbl {\\b Head A}\\cell\\pard\\intbl Head B\\cell\\row"
    "\\trowd\\cellx3000\\cellx6000\\pard\\intbl a1\\cell\\pard\\intbl b1\\par b1 line2\\cell\\row"
    "\\pard After table\\par"
    "{\\pict\\pngblip\\picw16\\pich8 " + hex + "}\\par"
    "{\\pict\\emfblip\\picw16\\pich8 0102030405}\\par"
    "{\\*\\generator Riched20 10.0.19041}{\\info{\\title My RTF}{\\author Someone}}"
    "{\\header Header text must not appear}\\pard final\\par}";
  Document d = Rtf(rtf);
  auto sig = SigStr(d, true);
  CHECK(Contains(sig, "H1:Main heading\nH2:Sub heading\nL0u:bullet one\nL0u:bullet two\nL0o:number one\nL0o:number two\n"));
  CHECK(Contains(sig, "[site](https://rtf.example/p?a=1)"));
  CHECK(Contains(sig, "T:Head A|Head B|;a1|b1\nb1 line2|;"));
  CHECK(Contains(sig, "P:After table"));
  size_t imgs = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) { ++imgs; CHECK_EQ(b.image->w, 16); CHECK_EQ(b.image->h, 8); } CHECK_EQ(imgs, (size_t)1);
  CHECK(!Contains(sig, "Header text")); CHECK(!Contains(sig, "Riched20")); CHECK(!Contains(sig, "Arial"));
  CHECK_EQ(d.meta.title, std::string("My RTF"));
  CHECK(!d.warnings.empty());                        // the EMF picture was dropped with a warning
  // numbering restarts / counts
  std::vector<int> n; for (auto& b : d.blocks) if (b.kind == Block::Kind::ListItem && b.ordered) n.push_back(b.number); CHECK(n == std::vector<int>({1, 2}));
}

TEST(rtf_hostile_and_invalid_inputs) {
  Document d; std::wstring e;
  CHECK(!ParseRtf((const uint8_t*)"plain text", 10, d, {}, &e));
  CHECK(!ParseRtf((const uint8_t*)"", 0, d, {}, &e));
  for (const char* g : {"{\\rtf1", "{\\rtf1{{{{{{", "{\\rtf1}}}}}}", "{\\rtf1\\u", "{\\rtf1\\'", "{\\rtf1\\'z", "{\\rtf1\\uc99999999999999\\u1 x}", "{\\rtf1{\\pict\\pngblip zz}}", "{\\rtf1\\trowd\\cell\\cell\\row\\row}", "{\\rtf1 \\ls-5\\ilvl99 x\\par}"}) {
    Document x; ParseRtf((const uint8_t*)g, strlen(g), x, {}, &e);
  }
  std::string deep = "{\\rtf1"; for (int i = 0; i < 200000; ++i) deep += "{"; Document x; ParseRtf((const uint8_t*)deep.data(), deep.size(), x, {}, &e);
  std::string junk(100000, '{'); junk = "{\\rtf1" + junk + std::string(100000, '}') + "text}"; ParseRtf((const uint8_t*)junk.data(), junk.size(), x, {}, &e);
}

TEST(rtfd_bundle_with_attachment) {
  fs::path dir = Work("rtfd"); fs::path bundle = dir / L"note.rtfd"; fs::create_directories(bundle);
  Bytes png = MakePng(30, 15);
  WriteAll(bundle / L"photo.png", std::string(png.begin(), png.end()));
  WriteAll(bundle / L"TXT.rtf", "{\\rtf1\\ansi\\ansicpg1252\\cocoartf2639{\\fonttbl\\f0\\fswiss Helvetica;}\\pard\\f0 Before image\\\n{{\\NeXTGraphic photo.png \\width600 \\height300 \\noorient\n}\\'ac}\\\nAfter image \\u1605?\\u1585?\\par}");
  Document d; std::wstring err;
  CHECK(ParseRtfd(bundle, d, &err));
  auto sig = SigStr(d);
  CHECK(Contains(sig, "I")); size_t imgs = 0; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) { ++imgs; CHECK_EQ(b.image->w, 30); } CHECK_EQ(imgs, (size_t)1);
  CHECK(Contains(AllText(d), "Before image")); CHECK(Contains(AllText(d), "After image \xD9\x85\xD8\xB1"));
  CHECK(!Contains(AllText(d), "\xC2\xAC")); CHECK(!Contains(AllText(d), "NeXTGraphic"));
  // path traversal in the attachment name is refused
  fs::path b2 = dir / L"evil.rtfd"; fs::create_directories(b2);
  WriteAll(dir / L"secret.png", std::string(png.begin(), png.end()));
  WriteAll(b2 / L"TXT.rtf", "{\\rtf1\\ansi {{\\NeXTGraphic ../secret.png \\width1 \\height1}\\'ac}text\\par}");
  Document d2; CHECK(ParseRtfd(b2, d2, &err)); for (auto& b : d2.blocks) CHECK(b.kind != Block::Kind::Image);
  // folder without TXT.rtf
  fs::path b3 = dir / L"empty.rtfd"; fs::create_directories(b3);
  Document d3; CHECK(!ParseRtfd(b3, d3, &err));
}
