#include "t_util.h"
#include "engines/doc/opc.h"
#include "engines/doc/sheet.h"
#include "engines/doc/xml.h"
#include <chrono>
#include <set>

using namespace tu;

static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Sheet Parse(const std::string& s, CsvReadOptions o = {}, CsvInfo* info = nullptr) {
  Sheet sh; std::wstring e; if (!ParseCsv(B(s), o, sh, info, &e)) std::printf("  ParseCsv failed: %s\n", Narrow(e).c_str()); return sh;
}
static std::string CellStr(const Sheet& s, size_t r, size_t c) { return r < s.rows.size() && c < s.rows[r].size() ? IsoFromCell(s.rows[r][c]) : std::string("<none>"); }
static SCell::T Type(const Sheet& s, size_t r, size_t c) { return r < s.rows.size() && c < s.rows[r].size() ? s.rows[r][c].t : SCell::T::Empty; }

// ---------------------------------------------------------------------------------------------------------------- CSV parsing
TEST(csv_quotes_newlines_and_line_endings) {
  CsvReadOptions o; o.keepText = true;
  Sheet s = Parse("a,b,c\r\n\"x,1\",\"he said \"\"hi\"\"\",\"multi\r\nline\"\r\n\"\",,last\r\n", o);
  CHECK_EQ(s.rows.size(), (size_t)3);
  CHECK_EQ(CellStr(s, 1, 0), std::string("x,1"));
  CHECK_EQ(CellStr(s, 1, 1), std::string("he said \"hi\""));
  CHECK_EQ(CellStr(s, 1, 2), std::string("multi\r\nline"));
  CHECK_EQ(CellStr(s, 2, 0), std::string("")); CHECK(Type(s, 2, 0) == SCell::T::Empty);
  CHECK_EQ(CellStr(s, 2, 2), std::string("last"));
  // lone CR and LF line ends, no trailing newline, blank interior line kept as empty row, trailing blank lines dropped
  Sheet t = Parse("1,2\r3,4\n\n5,6\n\n\n", o);
  CHECK_EQ(t.rows.size(), (size_t)4);
  CHECK(t.rows[2].empty());
  CHECK_EQ(CellStr(t, 3, 1), std::string("6"));
  Sheet u = Parse("only one line without newline", o);
  CHECK_EQ(u.rows.size(), (size_t)1); CHECK_EQ(CellStr(u, 0, 0), std::string("only one line without newline"));
  // unterminated quote swallows the rest but never crashes
  Sheet v = Parse("a,\"unterminated,b\nc,d\n", o); CHECK_EQ(v.rows.size(), (size_t)1);
  // quote in the middle of an unquoted field is literal
  Sheet w = Parse("it's a \"test\",2\n", o); CHECK_EQ(CellStr(w, 0, 0), std::string("it's a \"test\""));
  // empty input
  Sheet e = Parse("", o); CHECK(e.rows.empty());
  // custom quote character
  CsvReadOptions q; q.quote = '\''; q.delimiter = ';'; q.keepText = true;
  Sheet x = Parse("'a;b';c\n", q); CHECK_EQ(CellStr(x, 0, 0), std::string("a;b")); CHECK_EQ(CellStr(x, 0, 1), std::string("c"));
}

TEST(csv_delimiter_and_encoding_detection) {
  CsvInfo info;
  Parse("a;b;c\n1,5;2,5;3,5\n4,5;5,5;6,5\n", {}, &info); CHECK_EQ(info.delimiter, ';');            // decimal commas must not win
  Parse("a\tb\tc\n1\t2\t3\n", {}, &info); CHECK_EQ(info.delimiter, '\t');
  Parse("a|b|c\n1|2|3\n", {}, &info); CHECK_EQ(info.delimiter, '|');
  Parse("a,b,c\n1,2,3\n", {}, &info); CHECK_EQ(info.delimiter, ',');
  Parse("sep=;\na;b\n1;2\n", {}, &info); CHECK_EQ(info.delimiter, ';'); CHECK_EQ(info.rows, (size_t)2);
  Parse("\"a;b\",\"c;d\"\n\"1;2\",\"3;4\"\n", {}, &info); CHECK_EQ(info.delimiter, ',');         // delimiters inside quotes do not count
  Parse("single column\nsecond\nthird\n", {}, &info); CHECK_EQ(info.cols, (size_t)1);
  CsvReadOptions fixed; fixed.delimiter = '|';
  Parse("a,b|c\n", fixed, &info); CHECK_EQ(info.cols, (size_t)2);
  // encodings
  const std::string ar = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
  { std::string csv = "\xEF\xBB\xBF" "name,city\n" + ar + ",Cairo\n"; Sheet s = Parse(csv, {}, &info); CHECK_EQ(CellStr(s, 0, 0), std::string("name")); CHECK_EQ(CellStr(s, 1, 0), ar); CHECK(info.encoding == TextEnc::Utf8Bom); }
  { Bytes b = EncodeText("name,city\n" + ar + ",Cairo\n", TextEnc::Utf16LEBom); Sheet s; std::wstring e; CHECK(ParseCsv(b, {}, s, &info, &e)); CHECK_EQ(CellStr(s, 1, 0), ar); CHECK(info.encoding == TextEnc::Utf16LEBom); }
  { Bytes b = EncodeText("name,city\n" + ar + "," + ar + "," + ar + "\n" + ar + " " + ar + ",x,y\n", TextEnc::Cp1256); Sheet s; std::wstring e; CHECK(ParseCsv(b, {}, s, &info, &e)); CHECK_EQ(CellStr(s, 1, 0), ar); CHECK(info.encoding == TextEnc::Cp1256); }
  { Bytes b = EncodeText("a,b\ncaf\xC3\xA9,x\n", TextEnc::Cp1252); CsvReadOptions o; o.encoding = TextEnc::Cp1252; Sheet s; std::wstring e; CHECK(ParseCsv(b, o, s, &info, &e)); CHECK_EQ(CellStr(s, 1, 0), std::string("caf\xC3\xA9")); }
  // binary garbage is refused
  { Bytes junk(4000); for (size_t i = 0; i < junk.size(); ++i) junk[i] = (uint8_t)(i * 131 + 7) % 32; Sheet s; std::wstring e; CHECK(!ParseCsv(junk, {}, s, &info, &e)); CHECK(!e.empty()); }
}

TEST(csv_type_inference_guard) {
  Sheet s = Parse("007,0,3.14,-5,+15551234567,5551234567,123456789,12345678901234567890,2024-01-31,01/02/2024,1e5,TRUE,2024-01-31 10:20:30,1,234.50,.5,5.,00,-0.5,0.0\n");
  CHECK(Type(s, 0, 0) == SCell::T::Text); CHECK_EQ(CellStr(s, 0, 0), std::string("007"));
  CHECK(Type(s, 0, 1) == SCell::T::Number);
  CHECK(Type(s, 0, 2) == SCell::T::Number); CHECK_NEAR(s.rows[0][2].n, 3.14, 1e-12);
  CHECK(Type(s, 0, 3) == SCell::T::Number); CHECK_NEAR(s.rows[0][3].n, -5, 0);
  CHECK(Type(s, 0, 4) == SCell::T::Text);                       // phone number with plus sign
  CHECK(Type(s, 0, 5) == SCell::T::Text);                       // 10 digits: could be a phone number
  CHECK(Type(s, 0, 6) == SCell::T::Number);
  CHECK(Type(s, 0, 7) == SCell::T::Text);                       // would lose precision as a double
  CHECK(Type(s, 0, 8) == SCell::T::Date); CHECK_EQ(CellStr(s, 0, 8), std::string("2024-01-31"));
  CHECK(Type(s, 0, 9) == SCell::T::Text);                       // ambiguous date
  CHECK(Type(s, 0, 10) == SCell::T::Text);
  CHECK(Type(s, 0, 11) == SCell::T::Text);
  CHECK(Type(s, 0, 12) == SCell::T::DateTime); CHECK_EQ(CellStr(s, 0, 12), std::string("2024-01-31 10:20:30"));
  // "1,234.50" was split by the comma delimiter into 1 and 234.50
  CHECK(Type(s, 0, 13) == SCell::T::Number); CHECK(Type(s, 0, 14) == SCell::T::Number);
  CHECK(Type(s, 0, 15) == SCell::T::Text); CHECK(Type(s, 0, 16) == SCell::T::Text); CHECK(Type(s, 0, 17) == SCell::T::Text);
  CHECK(Type(s, 0, 18) == SCell::T::Number);
  // aggressive mode converts the guarded values
  CsvReadOptions inf; inf.inferTypes = true; inf.dayFirst = true;
  Sheet t = Parse("007;+15551234567;5551234567;01/02/2024;13/02/2024;02/13/2024;1e5;TRUE;\"1,234.50\";12:30;2024-01-31;.5;abc;1/2/24;31.12.2023\n", [&] { auto o = inf; o.delimiter = ';'; return o; }());
  CHECK(Type(t, 0, 0) == SCell::T::Number); CHECK_NEAR(t.rows[0][0].n, 7, 0);
  CHECK(Type(t, 0, 1) == SCell::T::Number);
  CHECK(Type(t, 0, 2) == SCell::T::Number); CHECK_NEAR(t.rows[0][2].n, 5551234567.0, 0);
  CHECK_EQ(CellStr(t, 0, 3), std::string("2024-02-01"));            // day first
  CHECK_EQ(CellStr(t, 0, 4), std::string("2024-02-13"));
  CHECK_EQ(CellStr(t, 0, 5), std::string("2024-02-13"));            // unambiguous month first
  CHECK(Type(t, 0, 6) == SCell::T::Number); CHECK(Type(t, 0, 7) == SCell::T::Bool);
  CHECK_NEAR(t.rows[0][8].n, 1234.5, 1e-9);
  CHECK(Type(t, 0, 9) == SCell::T::Time); CHECK_EQ(CellStr(t, 0, 9), std::string("12:30:00"));
  CHECK_EQ(CellStr(t, 0, 10), std::string("2024-01-31"));
  CHECK(Type(t, 0, 11) == SCell::T::Number); CHECK(Type(t, 0, 12) == SCell::T::Text);
  CHECK_EQ(CellStr(t, 0, 13), std::string("2024-02-01"));
  CHECK_EQ(CellStr(t, 0, 14), std::string("2023-12-31"));
  inf.dayFirst = false;
  Sheet m = Parse("01/02/2024\n", inf); CHECK_EQ(CellStr(m, 0, 0), std::string("2024-01-02"));
  // impossible dates stay text
  Sheet bad = Parse("2023-02-30,2024-13-01,1899-12-31\n"); for (size_t i = 0; i < 3; ++i) CHECK(Type(bad, 0, i) == SCell::T::Text);
}

TEST(csv_writer_quoting_and_encodings) {
  Sheet s; s.rows.resize(3);
  auto txt = [](const std::string& x) { SCell c; c.t = SCell::T::Text; c.s = x; return c; };
  s.rows[0] = {txt("plain"), txt("with,comma"), txt("with \"quote\""), txt("multi\nline")};
  s.rows[1] = {txt(""), txt("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"), txt(" lead"), txt("tab\there")};
  s.rows[2] = {SCell{SCell::T::Number, 1.5, ""}, SCell{SCell::T::Bool, 1, ""}, SCell{SCell::T::Date, 45000, ""}, SCell{SCell::T::DateTime, 45000.5, ""}};
  CsvWriteOptions o; o.encoding = TextEnc::Utf8;
  std::string out = [&] { Bytes b = WriteCsv(s, o); return std::string(b.begin(), b.end()); }();
  CHECK_EQ(out, std::string("plain,\"with,comma\",\"with \"\"quote\"\"\",\"multi\nline\"\r\n,\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7, lead,tab\there\r\n1.5,TRUE,2023-03-15,2023-03-15 12:00:00\r\n"));
  // round trip through the parser (text mode) gives the same strings
  CsvReadOptions ro; ro.keepText = true;
  Sheet back = Parse(out, ro);
  CHECK_EQ(CellStr(back, 0, 3), std::string("multi\nline")); CHECK_EQ(CellStr(back, 1, 3), std::string("tab\there")); CHECK_EQ(CellStr(back, 2, 3), std::string("2023-03-15 12:00:00"));
  // semicolon + TSV quoting + BOM
  CsvWriteOptions sc; sc.delimiter = ';'; sc.encoding = TextEnc::Utf8Bom; sc.crlf = false;
  Bytes bb = WriteCsv(s, sc); CHECK(bb.size() > 3 && bb[0] == 0xEF && bb[1] == 0xBB && bb[2] == 0xBF);
  CsvWriteOptions ts; ts.delimiter = '\t'; ts.encoding = TextEnc::Utf8; ts.crlf = false;
  std::string t = [&] { Bytes b = WriteCsv(s, ts); return std::string(b.begin(), b.end()); }();
  CHECK(Contains(t, "\"multi\nline\"\n")); CHECK(Contains(t, "\"tab\there\""));
  // a row holding only an empty string is quoted so it does not become a blank line
  Sheet one; one.rows.resize(2); one.rows[0] = {txt("")}; one.rows[0][0].t = SCell::T::Text; one.rows[1] = {txt("x")};
  Bytes ob = WriteCsv(one, o); CHECK_EQ(std::string(ob.begin(), ob.end()), std::string("\"\"\r\nx\r\n"));
  // legacy encodings
  CsvWriteOptions u16; u16.encoding = TextEnc::Utf16LEBom; Bytes u = WriteCsv(s, u16); CHECK(u[0] == 0xFF && u[1] == 0xFE);
  CsvWriteOptions a1256; a1256.encoding = TextEnc::Cp1256; Bytes ab = WriteCsv(s, a1256); CHECK(DecodeText(ab, TextEnc::Cp1256).utf8.find("\xD9\x85\xD8\xB1") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------- XLSX
static Workbook MakeWb() {
  Workbook wb; Sheet a; a.name = "Data";
  auto txt = [](const std::string& x) { SCell c; c.t = SCell::T::Text; c.s = x; return c; };
  auto num = [](double v) { SCell c; c.t = SCell::T::Number; c.n = v; return c; };
  a.rows = {{txt("Name"), txt("Qty"), txt("When"), txt("Flag"), txt("Note")},
            {txt("Alice"), num(3), SCell{SCell::T::Date, SerialFromDate(2024, 1, 31), ""}, SCell{SCell::T::Bool, 1, ""}, txt("line1\nline2")},
            {txt("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD9\x83"), num(-0.125), SCell{SCell::T::DateTime, SerialFromDate(2023, 12, 31) + 0.75, ""}, SCell{SCell::T::Bool, 0, ""}, txt("007")},
            {txt("emoji \xF0\x9F\x98\x80"), num(1234567890123.0), SCell{SCell::T::Time, 0.5, ""}, SCell(), txt("  spaces  ")},
            {txt("<&>\"'"), num(1e-9), SCell(), SCell(), txt("Alice")}};
  Sheet b; b.name = "Second/Sheet?"; b.rows = {{txt("x")}, {}, {txt("after empty row")}};
  wb.sheets = {a, b};
  return wb;
}

TEST(xlsx_write_read_roundtrip_and_structure) {
  fs::path dir = Work("xlsx_rt"); fs::path f = dir / L"book.xlsx";
  Workbook wb = MakeWb(); std::wstring err, why; std::vector<std::wstring> w;
  CHECK(WriteXlsx(wb, f, &err, &w));
  CHECK(VerifyXlsx(f, &why)); if (!why.empty()) std::printf("  verify: %s\n", Narrow(why).c_str());
  Workbook back; CHECK(ReadXlsx(f, back, &err));
  CHECK_EQ(back.sheets.size(), (size_t)2);
  CHECK_EQ(back.sheets[0].name, std::string("Data")); CHECK_EQ(back.sheets[1].name, std::string("Second_Sheet_"));
  const Sheet& a = back.sheets[0];
  for (size_t r = 0; r < wb.sheets[0].rows.size(); ++r) for (size_t c = 0; c < wb.sheets[0].rows[r].size(); ++c) {
    auto& x = wb.sheets[0].rows[r][c];
    if (x.t == SCell::T::Empty) { CHECK(Type(a, r, c) == SCell::T::Empty); continue; }
    CHECK(Type(a, r, c) == x.t);
    CHECK_EQ(CellStr(a, r, c), IsoFromCell(x));
  }
  CHECK_EQ(a.rows[1][4].s, std::string("line1\nline2"));
  CHECK_EQ(a.rows[3][4].s, std::string("  spaces  "));
  CHECK_NEAR(a.rows[4][1].n, 1e-9, 1e-18);
  CHECK_EQ(CellStr(back.sheets[1], 2, 0), std::string("after empty row"));
  // package details
  ZipReader z; CHECK(z.Open(f));
  auto sheet1 = *z.ReadString("xl/worksheets/sheet1.xml");
  CHECK(Contains(sheet1, "rightToLeft") == false);              // mixed content: first column is mostly LTR
  CHECK(Contains(sheet1, "<dimension ref=\"A1:E5\"/>"));
  CHECK(Contains(*z.ReadString("xl/styles.xml"), "yyyy\\-mm\\-dd"));
  auto sst = *z.ReadString("xl/sharedStrings.xml");
  size_t first = sst.find("Alice"); CHECK(first != std::string::npos); CHECK(sst.find("Alice", first + 5) == std::string::npos);   // shared strings are de-duplicated
  // an Arabic-only sheet is written right-to-left
  Workbook ar; Sheet s; s.name = "ar"; SCell c; c.t = SCell::T::Text; c.s = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"; s.rows = {{c, c}}; ar.sheets = {s};
  fs::path f2 = dir / L"ar.xlsx"; CHECK(WriteXlsx(ar, f2, &err)); ZipReader z2; CHECK(z2.Open(f2)); CHECK(Contains(*z2.ReadString("xl/worksheets/sheet1.xml"), "rightToLeft=\"1\""));
  // empty workbook, empty sheet
  Workbook e0; CHECK(WriteXlsx(e0, dir / L"e0.xlsx", &err)); CHECK(VerifyXlsx(dir / L"e0.xlsx", &why));
  Workbook e1; e1.sheets.emplace_back(); CHECK(WriteXlsx(e1, dir / L"e1.xlsx", &err)); CHECK(VerifyXlsx(dir / L"e1.xlsx", &why));
  // duplicate / invalid / long sheet names are repaired
  Workbook nm; for (const char* n : {"Same", "same", "a:b", "0123456789012345678901234567890123456789", ""}) { Sheet x; x.name = n; x.rows = {{c}}; nm.sheets.push_back(x); }
  fs::path f3 = dir / L"names.xlsx"; CHECK(WriteXlsx(nm, f3, &err)); Workbook nb; CHECK(ReadXlsx(f3, nb, &err));
  std::set<std::string> seen; for (auto& sh : nb.sheets) { CHECK(seen.insert(LowerAscii(sh.name)).second); CHECK(Utf8Length(sh.name) <= 31); CHECK(!sh.name.empty()); }
  // oversized cell text is shortened with a warning
  Workbook big; Sheet bs; SCell bc; bc.t = SCell::T::Text; bc.s = std::string(40000, 'q'); bs.rows = {{bc}}; big.sheets = {bs};
  std::vector<std::wstring> w2; CHECK(WriteXlsx(big, dir / L"big.xlsx", &err, &w2)); CHECK(!w2.empty());
  Workbook bb; CHECK(ReadXlsx(dir / L"big.xlsx", bb, &err)); CHECK_EQ(bb.sheets[0].rows[0][0].s.size(), (size_t)32767);
}

static fs::path HandXlsx(const fs::path& dir, const std::string& sheetXml, const std::string& styles, const std::string& wbExtra = "", const std::string& sst = "") {
  fs::path f = dir / L"hand.xlsx";
  ZipWriter w; w.Open(f);
  w.Add("[Content_Types].xml", std::string("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/></Types>"));
  w.Add("_rels/.rels", std::string("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>"));
  w.Add("xl/workbook.xml", "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">" + wbExtra + "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/><sheet name=\"Hidden\" sheetId=\"2\" state=\"hidden\" r:id=\"rId2\"/></sheets></workbook>");
  w.Add("xl/_rels/workbook.xml.rels", std::string("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"/xl/worksheets/sheet2.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/><Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/></Relationships>"));
  w.Add("xl/styles.xml", styles);
  w.Add("xl/sharedStrings.xml", sst.empty() ? std::string("<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>") : sst);
  w.Add("xl/worksheets/sheet1.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>" + sheetXml + "</sheetData></worksheet>");
  w.Add("xl/worksheets/sheet2.xml", std::string("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>hidden one</t></is></c></row></sheetData></worksheet>"));
  w.Close();
  return f;
}

TEST(xlsx_reader_handles_foreign_features) {
  fs::path dir = Work("xlsx_hand");
  std::string styles = "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><numFmts count=\"4\"><numFmt numFmtId=\"164\" formatCode=\"dd/mm/yyyy\\ hh:mm\"/><numFmt numFmtId=\"165\" formatCode=\"[h]:mm:ss\"/>"
                       "<numFmt numFmtId=\"166\" formatCode=\"0.00&quot;d&quot;\"/><numFmt numFmtId=\"167\" formatCode=\"[$-409]mmmm\\ d\\,\\ yyyy\"/></numFmts>"
                       "<cellXfs count=\"7\"><xf numFmtId=\"0\"/><xf numFmtId=\"14\"/><xf numFmtId=\"164\"/><xf numFmtId=\"165\"/><xf numFmtId=\"166\"/><xf numFmtId=\"167\"/><xf numFmtId=\"22\"/></cellXfs></styleSheet>";
  std::string sst = "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"3\" uniqueCount=\"3\"><si><t>plain</t></si><si><r><t xml:space=\"preserve\">rich </t></r><r><rPr><b/></rPr><t>text</t></r><rPh><t>PHONETIC</t></rPh></si><si><t></t></si></sst>";
  std::string rows =
    "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c><c r=\"C1\" t=\"inlineStr\"><is><r><t>in</t></r><r><t>line</t></r></is></c><c r=\"D1\" t=\"str\"><f>A1&amp;\"x\"</f><v>plainx</v></c></row>"
    "<row r=\"3\"><c r=\"A3\" s=\"1\"><v>45000</v></c><c r=\"B3\" s=\"2\"><v>45000.75</v></c><c r=\"C3\" s=\"3\"><v>1.5</v></c><c r=\"D3\" s=\"4\"><v>3.25</v></c><c r=\"E3\" s=\"5\"><v>45000</v></c><c r=\"F3\" s=\"6\"><v>45000.25</v></c></row>"
    "<row r=\"4\"><c r=\"A4\" t=\"b\"><v>1</v></c><c r=\"B4\" t=\"e\"><v>#DIV/0!</v></c><c r=\"C4\"><f>SUM(A3:B3)</f></c><c r=\"E4\"><f>1+1</f><v>2</v></c><c t=\"n\"><v>7</v></c><c r=\"H4\" t=\"d\"><v>2021-05-06</v></c></row>"
    "<row r=\"5\"><c r=\"A5\" t=\"s\"><v>2</v></c><c r=\"B5\"/><c r=\"C5\"><v>  12  </v></c><c r=\"D5\"><v>abc</v></c></row>";
  fs::path f = HandXlsx(dir, rows, styles, "", sst);
  Workbook wb; std::wstring err; CHECK(ReadXlsx(f, wb, &err));
  CHECK_EQ(wb.sheets.size(), (size_t)2);
  const Sheet& s = wb.sheets[0];
  CHECK_EQ(CellStr(s, 0, 0), std::string("plain"));
  CHECK_EQ(CellStr(s, 0, 1), std::string("rich text"));                 // rich text runs joined, phonetic hint dropped
  CHECK_EQ(CellStr(s, 0, 2), std::string("inline"));
  CHECK_EQ(CellStr(s, 0, 3), std::string("plainx"));                    // formula string: cached value
  CHECK(Type(s, 2, 0) == SCell::T::Date); CHECK_EQ(CellStr(s, 2, 0), std::string("2023-03-15"));
  CHECK(Type(s, 2, 1) == SCell::T::DateTime); CHECK_EQ(CellStr(s, 2, 1), std::string("2023-03-15 18:00:00"));
  CHECK(Type(s, 2, 2) == SCell::T::Time); CHECK_EQ(CellStr(s, 2, 2), std::string("36:00:00"));
  CHECK(Type(s, 2, 3) == SCell::T::Number); CHECK_NEAR(s.rows[2][3].n, 3.25, 0);                // "0.00"d"" : the quoted d must not make it a date
  CHECK(Type(s, 2, 4) == SCell::T::Date);                           // mmmm d, yyyy
  CHECK(Type(s, 2, 5) == SCell::T::DateTime); CHECK_EQ(CellStr(s, 2, 5), std::string("2023-03-15 06:00:00"));
  CHECK(Type(s, 3, 0) == SCell::T::Bool); CHECK_EQ(CellStr(s, 3, 0), std::string("TRUE"));
  CHECK_EQ(CellStr(s, 3, 1), std::string("#DIV/0!"));
  CHECK(Type(s, 3, 2) == SCell::T::Empty);                          // formula without a cached value
  CHECK(Type(s, 3, 4) == SCell::T::Number); CHECK_NEAR(s.rows[3][4].n, 2, 0);
  CHECK_EQ(CellStr(s, 3, 5), std::string("7"));                         // cell without a reference follows the previous one
  CHECK_EQ(CellStr(s, 3, 7), std::string("2021-05-06"));
  CHECK(Type(s, 4, 0) == SCell::T::Empty); CHECK_NEAR(s.rows[4][2].n, 12, 0); CHECK_EQ(CellStr(s, 4, 3), std::string("abc"));
  CHECK(Type(s, 1, 0) == SCell::T::Empty && s.rows[1].empty());      // skipped row 2
  CHECK_EQ(CellStr(wb.sheets[1], 0, 0), std::string("hidden one"));
  bool fw = false, nc = false; for (auto& x : wb.warnings) { if (Contains(Narrow(x), "Formulas were replaced")) fw = true; if (Contains(Narrow(x), "no stored result")) nc = true; } CHECK(fw); CHECK(nc);
  // 1904 date system
  fs::path f2 = HandXlsx(dir, "<row r=\"1\"><c r=\"A1\" s=\"1\"><v>43538</v></c></row>", styles, "<workbookPr date1904=\"1\"/>");
  Workbook w2; CHECK(ReadXlsx(f2, w2, &err)); CHECK_EQ(CellStr(w2.sheets[0], 0, 0), std::string("2023-03-15"));
}

TEST(xlsx_hostile_inputs) {
  fs::path dir = Work("xlsx_bad");
  std::wstring err;
  // sparse sheet that would need billions of cells if expanded naively
  std::string rows;
  for (int r = 1; r <= 3000; ++r) rows += "<row r=\"" + std::to_string(r) + "\"><c r=\"XFD" + std::to_string(r) + "\" t=\"inlineStr\"><is><t>x</t></is></c></row>";
  fs::path f = HandXlsx(dir, rows, "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>");
  Workbook wb; auto t0 = std::chrono::steady_clock::now();
  bool ok = ReadXlsx(f, wb, &err);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  sparse hostile sheet: %s in %.2fs\n", ok ? "accepted" : "refused", secs);
  CHECK(!ok);                    // 3000 rows x 16384 columns of allocation is refused with a message
  CHECK(!err.empty());
  CHECK(secs < 60.0);
  // row index beyond the sheet limit is ignored
  fs::path f2 = HandXlsx(dir, "<row r=\"99999999\"><c r=\"A99999999\" t=\"inlineStr\"><is><t>x</t></is></c></row><row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>ok</t></is></c></row>", "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>");
  Workbook w2; CHECK(ReadXlsx(f2, w2, &err)); CHECK_EQ(CellStr(w2.sheets[0], 1, 0), std::string("ok"));
  // not a workbook
  WriteAll(dir / L"junk.xlsx", "PK nonsense"); Workbook w3; CHECK(!ReadXlsx(dir / L"junk.xlsx", w3, &err));
  { ZipWriter w; w.Open(dir / L"empty.xlsx"); w.Add("a.txt", std::string("x")); w.Close(); }
  Workbook w4; CHECK(!ReadXlsx(dir / L"empty.xlsx", w4, &err));
  // truncated sheet XML is tolerated
  fs::path f5 = HandXlsx(dir, "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>half", "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>");
  Workbook w5; ReadXlsx(f5, w5, &err);
}

TEST(csv_to_xlsx_to_csv_is_lossless_for_text_and_fast_at_scale) {
  fs::path dir = Work("csv_scale");
  // tricky text content must survive csv -> xlsx -> csv exactly
  std::string csv = "id,name,note\r\n001,\"Smith, John\",\"line1\r\nline2\"\r\n002,\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7,\"say \"\"hi\"\"\"\r\n003,emoji \xF0\x9F\x98\x80,+966 55 123 4567\r\n004,,5551234567\r\n";
  Sheet s = Parse(csv);
  CHECK(Type(s, 1, 0) == SCell::T::Text);                           // 001 keeps its zeros
  Workbook wb; wb.sheets = {s}; std::wstring err;
  CHECK(WriteXlsx(wb, dir / L"t.xlsx", &err));
  Workbook back; CHECK(ReadXlsx(dir / L"t.xlsx", back, &err));
  CsvWriteOptions o; o.encoding = TextEnc::Utf8;
  Bytes out = WriteCsv(back.sheets[0], o);
  CHECK_SIG(std::string(out.begin(), out.end()), csv);
  // 100k rows x 8 columns
  std::string big; big.reserve(8u << 20);
  big += "id,name,amount,date,flag,text,code,note\r\n";
  for (int i = 0; i < 100000; ++i) big += std::to_string(i) + ",name " + std::to_string(i % 977) + "," + std::to_string(i * 0.25) + ",2024-" + (i % 12 + 1 < 10 ? "0" : "") + std::to_string(i % 12 + 1) + "-15,TRUE,\"a,b " + std::to_string(i) + "\",00" + std::to_string(i) + ",\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\r\n";
  WriteAll(dir / L"big.csv", big);
  auto t0 = std::chrono::steady_clock::now();
  Bytes data; ReadFileBytes(dir / L"big.csv", data, 1ull << 30);
  Sheet bs; CsvInfo info; CHECK(ParseCsv(data, {}, bs, &info, &err));
  double tParse = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(bs.rows.size(), (size_t)100001); CHECK_EQ(info.cols, (size_t)8);
  Workbook bw; bw.sheets = {bs};
  auto t1 = std::chrono::steady_clock::now();
  CHECK(WriteXlsx(bw, dir / L"big.xlsx", &err));
  double tWrite = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  auto t2 = std::chrono::steady_clock::now();
  Workbook br; CHECK(ReadXlsx(dir / L"big.xlsx", br, &err));
  double tRead = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
  CHECK_EQ(br.sheets[0].rows.size(), (size_t)100001);
  CHECK_EQ(CellStr(br.sheets[0], 100000, 6), std::string("0099999"));
  CHECK_EQ(CellStr(br.sheets[0], 99999, 3), std::string("2024-03-15"));
  std::printf("  100k rows: parse %.2fs, write xlsx %.2fs, read xlsx %.2fs (Debug build)\n", tParse, tWrite, tRead);
  CHECK(tParse < 30 && tWrite < 60 && tRead < 60);
}

TEST(xlsx_row_limit_splits_sheets) {
  fs::path dir = Work("xlsx_limit");
  Workbook wb; Sheet s; s.name = "Huge"; SCell c; c.t = SCell::T::Number; c.n = 1;
  s.rows.assign(1048576 + 5, std::vector<SCell>{c});
  wb.sheets = {s}; std::wstring err; std::vector<std::wstring> w;
  CHECK(WriteXlsx(wb, dir / L"huge.xlsx", &err, &w));
  CHECK(!w.empty());
  Workbook back; CHECK(ReadXlsx(dir / L"huge.xlsx", back, &err));
  CHECK_EQ(back.sheets.size(), (size_t)2);
  CHECK_EQ(back.sheets[0].rows.size(), (size_t)1048576); CHECK_EQ(back.sheets[1].rows.size(), (size_t)5);
}

// ---------------------------------------------------------------------------------------------------------------- ODS
TEST(ods_write_read_roundtrip) {
  fs::path dir = Work("ods_rt"); fs::path f = dir / L"book.ods";
  Workbook wb = MakeWb(); std::wstring err, why;
  CHECK(WriteOds(wb, f, &err)); CHECK(VerifyOds(f, &why)); if (!why.empty()) std::printf("  verify: %s\n", Narrow(why).c_str());
  Workbook back; CHECK(ReadOds(f, back, &err));
  CHECK_EQ(back.sheets.size(), (size_t)2);
  const Sheet& a = back.sheets[0];
  for (size_t r = 0; r < wb.sheets[0].rows.size(); ++r) for (size_t c = 0; c < wb.sheets[0].rows[r].size(); ++c) {
    auto& x = wb.sheets[0].rows[r][c];
    if (x.t == SCell::T::Empty) { CHECK(Type(a, r, c) == SCell::T::Empty); continue; }
    CHECK(Type(a, r, c) == x.t);
    CHECK_EQ(CellStr(a, r, c), IsoFromCell(x));
  }
  CHECK_EQ(a.rows[1][4].s, std::string("line1\nline2"));
  CHECK_EQ(a.rows[3][4].s, std::string("  spaces  "));
  CHECK_EQ(CellStr(back.sheets[1], 2, 0), std::string("after empty row"));
  ZipReader z; CHECK(z.Open(f)); CHECK_EQ(z.Entries()[0].name, std::string("mimetype")); CHECK_EQ(z.Entries()[0].csize, z.Entries()[0].size);
  // empty and hostile
  Workbook e; CHECK(WriteOds(e, dir / L"e.ods", &err)); CHECK(VerifyOds(dir / L"e.ods", &why));
  WriteAll(dir / L"junk.ods", "not a zip"); Workbook j; CHECK(!ReadOds(dir / L"junk.ods", j, &err));
  { ZipWriter w; w.Open(dir / L"rep.ods"); w.Add("mimetype", std::string("application/vnd.oasis.opendocument.spreadsheet"), false);
    w.Add("content.xml", std::string("<office:document-content xmlns:office=\"o\" xmlns:table=\"t\" xmlns:text=\"x\"><office:body><office:spreadsheet><table:table table:name=\"S\"><table:table-row table:number-rows-repeated=\"1000000\"><table:table-cell table:number-columns-repeated=\"16000\"/></table:table-row>"
      "<table:table-row><table:table-cell office:value-type=\"float\" office:value=\"5\"/></table:table-row><table:table-row table:number-rows-repeated=\"1048000\"><table:table-cell/></table:table-row></table:table></office:spreadsheet></office:body></office:document-content>")); w.Close(); }
  auto t0 = std::chrono::steady_clock::now();
  Workbook rb; CHECK(ReadOds(dir / L"rep.ods", rb, &err));
  CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0);
  CHECK_EQ(rb.sheets[0].rows.size(), (size_t)1000001);                 // trailing empty repeats are dropped, leading ones kept as empty rows
  CHECK_EQ(CellStr(rb.sheets[0], 1000000, 0), std::string("5"));
}

// ---------------------------------------------------------------------------------------------------------------- LibreOffice as independent referee
static std::string LoCsv(const fs::path& in, const std::wstring& inExt, const fs::path& out) {
  LoConvertSpec s; s.input = in; s.inExt = inExt; s.convertTo = L"csv:Text - txt - csv (StarCalc):44,34,76,1"; s.outExt = L"csv"; s.output = out;
  auto r = LibreOfficeConvert(s, nullptr);
  if (!r.ok) { std::printf("  LO failed: %s\n", Narrow(r.error).c_str()); return {}; }
  return ReadAll(out);
}

TEST(xlsx_and_ods_agree_with_libreoffice) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: skipped)\n"); return; }
  fs::path dir = Work("lo_sheets");
  Workbook wb = MakeWb(); std::wstring err;
  CHECK(WriteXlsx(wb, dir / L"a.xlsx", &err)); CHECK(WriteOds(wb, dir / L"a.ods", &err));
  std::string fromXlsx = LoCsv(dir / L"a.xlsx", L"xlsx", dir / L"x.csv");
  std::string fromOds = LoCsv(dir / L"a.ods", L"ods", dir / L"o.csv");
  CHECK(!fromXlsx.empty()); CHECK(!fromOds.empty());
  // LibreOffice applies display formats, so compare the stable cells
  std::printf("--- LO csv from xlsx ---\n%s\n", fromXlsx.substr(0, 500).c_str());
  std::printf("--- LO csv from ods ---\n%s\n", fromOds.substr(0, 500).c_str());
  for (std::string csv : {fromXlsx, fromOds}) {
    csv.erase(std::remove(csv.begin(), csv.end(), 0x22), csv.end());          // LibreOffice quotes every text cell
    CHECK(Contains(csv, "Name,Qty,When,Flag,Note"));
    CHECK(Contains(csv, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD9\x83"));
    CHECK(Contains(csv, "emoji \xF0\x9F\x98\x80"));
    CHECK(Contains(csv, "line1\nline2"));
    CHECK(Contains(csv, "007"));
    CHECK(Contains(csv, "2024-01-31"));
    CHECK(Contains(csv, "<&>'"));
    CHECK(Contains(csv, "TRUE"));
  }
  // LibreOffice-written xlsx / ods read by us
  fs::path csvIn = dir / L"in.csv";
  WriteAll(csvIn, "n,v,d\n\xD8\xA3\xD8\xAD\xD9\x85\xD8\xAF,1.5,2024-03-01\nplain,2,2024-03-02\n");
  LoConvertSpec s; s.input = csvIn; s.inExt = L"csv"; s.convertTo = L"xlsx:Calc MS Excel 2007 XML"; s.outExt = L"xlsx"; s.output = dir / L"lo.xlsx";
  CHECK(LibreOfficeConvert(s, nullptr).ok);
  Workbook lo; CHECK(ReadXlsx(dir / L"lo.xlsx", lo, &err));
  CHECK_EQ(CellStr(lo.sheets[0], 1, 0), std::string("\xD8\xA3\xD8\xAD\xD9\x85\xD8\xAF"));
  CHECK(Type(lo.sheets[0], 1, 1) == SCell::T::Number); CHECK_NEAR(lo.sheets[0].rows[1][1].n, 1.5, 0);
  CHECK(Type(lo.sheets[0], 1, 2) == SCell::T::Date); CHECK_EQ(CellStr(lo.sheets[0], 1, 2), std::string("2024-03-01"));
  std::wstring why; CHECK(VerifyXlsx(dir / L"lo.xlsx", &why));
  s.convertTo = L"ods"; s.outExt = L"ods"; s.output = dir / L"lo.ods";
  CHECK(LibreOfficeConvert(s, nullptr).ok);
  Workbook lo2; CHECK(ReadOds(dir / L"lo.ods", lo2, &err));
  CHECK_EQ(CellStr(lo2.sheets[0], 2, 0), std::string("plain"));
  CHECK(Type(lo2.sheets[0], 2, 2) == SCell::T::Date); CHECK_EQ(CellStr(lo2.sheets[0], 2, 2), std::string("2024-03-02"));
  CHECK(Type(lo2.sheets[0], 2, 1) == SCell::T::Number);
  // legacy .xls produced by LibreOffice, converted back through the capability layer is covered in the capability tests
}
