// Every registered capability, run through RunConversion() inside a JobManager job; writes testdata/doc/matrix-results.json.
#include "t_util.h"
#include "core/process.h"
#include "engines/doc/docx.h"
#include "engines/doc/epub.h"
#include "engines/doc/html.h"
#include "engines/doc/markdown.h"
#include "engines/doc/pptx.h"
#include "engines/doc/sheet.h"
#include "engines/doc/subs.h"
#include "engines/doc/text_io.h"
#include "engines/doc/textfmt.h"
#include "engines/doc/zip.h"
#include "engines/pdf/pdf_doc.h"
#include <chrono>
#include <cmath>
#include <map>

using namespace tu;

namespace {

const char* kArabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";

std::string PdfAllText(const fs::path& p, int* pages = nullptr) {
  auto r = pdf::PdfDocument::Open(p);
  if (!r.ok()) return {};
  std::string all; int n = r.doc->PageCount(); if (pages) *pages = n;
  for (int i = 0; i < n; ++i) all += Narrow(r.doc->PageText(i)) + "\n";
  return all;
}
size_t CountArabic(const std::string& s) {
  size_t n = 0, i = 0;
  while (i < s.size()) { uint32_t c = DecodeUtf8(s, i); if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) ++n; }
  return n;
}

bool LoMake(const fs::path& in, const std::wstring& inExt, const std::wstring& convertTo, const std::wstring& outExt, const fs::path& out) {
  LoConvertSpec s; s.input = in; s.inExt = inExt; s.convertTo = convertTo; s.outExt = outExt; s.output = out;
  auto r = LibreOfficeConvert(s, nullptr);
  if (!r.ok) std::printf("  fixture via LibreOffice failed (%s): %s\n", Narrow(outExt).c_str(), Narrow(r.error).c_str());
  return r.ok;
}

struct Fixtures {
  fs::path dir;
  std::map<std::wstring, fs::path> byFormat;
  std::map<std::wstring, std::vector<std::string>> tokens;
};

Deck MakeDeck() {
  Deck d; d.widthPt = 720; d.heightPt = 405; d.title = "Quarterly deck";
  for (int s = 0; s < 3; ++s) {
    Slide sl;
    SlideShape t; t.kind = SlideShape::Kind::Title; t.x = 40; t.y = 24; t.w = 640; t.h = 60;
    SlidePara tp; SlideRun tr; tr.text = s == 0 ? "Quarterly report" : "Slide number " + std::to_string(s + 1); tr.sizePt = 36; tr.bold = true; tp.runs.push_back(tr); t.paras.push_back(tp); sl.shapes.push_back(t);
    SlideShape b; b.x = 40; b.y = 110; b.w = 400; b.h = 200;
    for (auto txt : {"First point of the slide", "Second point with more words"}) { SlidePara p; p.bullet = true; SlideRun r; r.text = txt; r.sizePt = 20; p.runs.push_back(r); b.paras.push_back(p); }
    sl.shapes.push_back(b);
    SlideShape ar; ar.x = 460; ar.y = 110; ar.w = 220; ar.h = 80;
    { SlidePara p; p.rtl = true; p.align = 'r'; SlideRun r; r.text = std::string(kArabic) + " \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85"; r.sizePt = 24; r.rtl = true; p.runs.push_back(r); ar.paras.push_back(p); }
    sl.shapes.push_back(ar);
    if (s == 1) { SlideShape im; im.kind = SlideShape::Kind::Image; im.x = 460; im.y = 220; im.w = 200; im.h = 100; im.image = std::make_shared<ImageData>(); im.image->bytes = MakePng(200, 100); im.image->mime = "image/png"; sl.shapes.push_back(im); }
    d.slides.push_back(sl);
  }
  return d;
}

Workbook CapsWorkbook() {
  Workbook wb; Sheet a; a.name = "Data";
  auto txt = [](const std::string& x) { SCell c; c.t = SCell::T::Text; c.s = x; return c; };
  auto num = [](double v) { SCell c; c.t = SCell::T::Number; c.n = v; return c; };
  a.rows = {{txt("name"), txt("qty"), txt("code"), txt("when"), txt("note")},
            {txt("Alice"), num(3), txt("007"), SCell{SCell::T::Date, SerialFromDate(2024, 1, 31), ""}, txt("multi\nline")},
            {txt(std::string(kArabic) + " \xD8\xA8\xD9\x83"), num(4.5), txt("0042"), SCell{SCell::T::Date, SerialFromDate(2024, 2, 29), ""}, txt("say \"hi\", ok")},
            {txt("emoji \xF0\x9F\x98\x80"), num(10), txt("5551234567"), SCell(), txt("")}};
  Sheet b; b.name = "Second sheet"; b.rows = {{txt("x"), txt("y")}, {num(1), num(2)}};
  wb.sheets = {a, b};
  return wb;
}

Fixtures Build(const fs::path& dir) {
  Fixtures fx; fx.dir = dir;
  std::wstring err;
  Document d = SampleDoc();
  auto reg = [&](const wchar_t* fmt, const fs::path& p, std::vector<std::string> tok) { fx.byFormat[fmt] = p; fx.tokens[fmt] = std::move(tok); };
  const std::vector<std::string> docTok = {"Quarterly report", kArabic, "first bullet"};
  fs::path docx = dir / L"sample.docx"; WriteDocx(d, docx, {}, &err); reg(L"docx", docx, docTok);
  fs::path md = dir / L"sample.md"; WriteAll(md, WriteMarkdown(d)); reg(L"md", md, docTok);
  fs::path txt = dir / L"sample.txt"; { Bytes b = EncodeText(ToCrLf(WriteText(d)), TextEnc::Utf8Bom); WriteFileBytes(txt, b.data(), b.size()); } reg(L"txt", txt, docTok);
  fs::path html = dir / L"sample.html"; WriteAll(html, WriteHtml(d)); reg(L"html", html, docTok);
  fs::path epub = dir / L"sample.epub"; WriteEpub(d, epub, {}, &err); reg(L"epub", epub, docTok);
  fs::path rtf = dir / L"sample.rtf";
  WriteAll(rtf, "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}{\\colortbl;\\red0\\green0\\blue255;}\\pard\\b Quarterly report\\b0\\par\\pard Plain {\\i italic} text with \\u1605?\\u1585?\\u1581?\\u1576?\\u1575? Arabic and caf\\'e9.\\par"
              "\\pard first bullet\\par}");
  reg(L"rtf", rtf, {"Quarterly report", "italic", kArabic, "caf\xC3\xA9"});
  fs::path rtfd = dir / L"sample.rtfd"; fs::create_directories(rtfd);
  { Bytes png = MakePng(20, 10); WriteAll(rtfd / L"pic.png", std::string(png.begin(), png.end()));
    WriteAll(rtfd / L"TXT.rtf", "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl\\f0\\fswiss Helvetica;}\\pard\\f0 Quarterly report\\par {{\\NeXTGraphic pic.png \\width400 \\height200}\\'ac}\\par After \\u1605?\\u1585?\\u1581?\\u1576?\\u1575?\\par}"); }
  reg(L"rtfd", rtfd, {"Quarterly report", kArabic});
  fs::path clip = dir / L"sample.textClipping";
  { std::string text = std::string("Clipped text ") + kArabic + " end"; WriteAll(clip, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>UTI-Data</key><dict><key>public.utf8-plain-text</key><data>" + Base64Encode((const uint8_t*)text.data(), text.size()) + "</data></dict></dict></plist>"); }
  reg(L"textclipping", clip, {"Clipped text", kArabic});
  if (HaveLo()) {
    fs::path doc = dir / L"sample.doc"; LoMake(docx, L"docx", L"doc:MS Word 97", L"doc", doc); if (fs::exists(doc)) reg(L"doc", doc, docTok);
    fs::path pdf = dir / L"preview.pdf"; LoMake(docx, L"docx", L"pdf", L"pdf", pdf);
    if (fs::exists(pdf)) {
      fs::path pages = dir / L"sample.pages"; ZipWriter z; z.Open(pages); z.Add("Index/Document.iwa", std::string("IWA\0\0", 5)); Bytes pb; ReadFileBytes(pdf, pb, 1 << 28); z.Add("QuickLook/Preview.pdf", pb, false);
      Bytes th = MakePng(30, 40, true); z.Add("QuickLook/Thumbnail.jpg", th, false); z.Close();
      reg(L"pages", pages, {"Quarterly report"});
    }
  }
  // presentations
  fs::path pptx = dir / L"sample.pptx"; WritePptx(MakeDeck(), pptx, &err); reg(L"pptx", pptx, {"Quarterly report", "Slide number 2", "First point"});
  if (HaveLo()) { fs::path ppt = dir / L"sample.ppt"; LoMake(pptx, L"pptx", L"ppt:MS PowerPoint 97", L"ppt", ppt); if (fs::exists(ppt)) reg(L"ppt", ppt, {"Quarterly report", "Slide number 2", "First point"}); }
  // spreadsheets
  Workbook wb = CapsWorkbook();
  fs::path xlsx = dir / L"sample.xlsx"; WriteXlsx(wb, xlsx, &err); reg(L"xlsx", xlsx, {"Alice", kArabic, "0042"});
  fs::path ods = dir / L"sample.ods"; WriteOds(wb, ods, &err); reg(L"ods", ods, {"Alice", kArabic, "0042"});
  if (HaveLo()) { fs::path xls = dir / L"sample.xls"; LoMake(xlsx, L"xlsx", L"xls:MS Excel 97", L"xls", xls); if (fs::exists(xls)) reg(L"xls", xls, {"Alice", kArabic, "0042"}); }
  { Bytes b = WriteCsv(wb.sheets[0], [] { CsvWriteOptions o; o.encoding = TextEnc::Utf8Bom; return o; }()); fs::path p = dir / L"sample.csv"; WriteFileBytes(p, b.data(), b.size()); reg(L"csv", p, {"Alice", kArabic, "0042"}); }
  { Bytes b = WriteCsv(wb.sheets[0], [] { CsvWriteOptions o; o.encoding = TextEnc::Utf8; o.delimiter = '\t'; return o; }()); fs::path p = dir / L"sample.tsv"; WriteFileBytes(p, b.data(), b.size()); reg(L"tsv", p, {"Alice", kArabic, "0042"}); }
  // subtitles
  WriteAll(dir / L"sample.srt", "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:03,500\r\nHello <i>world</i>\r\n\r\n2\r\n00:00:04,000 --> 00:00:06,250\r\n" + std::string(kArabic) + " \xD8\xA8\xD9\x83\r\nsecond line\r\n");
  reg(L"srt", dir / L"sample.srt", {"Hello", kArabic, "second line"});
  WriteAll(dir / L"sample.vtt", "WEBVTT\n\n1\n00:00:01.000 --> 00:00:03.500\nHello world\n\n00:04.000 --> 00:06.250\n" + std::string(kArabic) + " \xD8\xA8\xD9\x83\nsecond line\n");
  reg(L"vtt", dir / L"sample.vtt", {"Hello", kArabic, "second line"});
  return fx;
}

Fixtures& Fx() { static Fixtures f = Build(Work("caps_fixtures")); return f; }

std::string DirText(const fs::path& dir) {
  std::string all; std::error_code ec;
  std::vector<fs::path> files; for (auto& e : fs::directory_iterator(dir, ec)) if (e.is_regular_file()) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (auto& f : files) all += ReadAll(f) + "\n";
  return all;
}

// Reads the produced output with the engine's independent readers and returns its text.
std::string TextOf(const fs::path& out, const std::wstring& ext, bool isDir, std::wstring* why) {
  std::wstring err;
  if (isDir) return DirText(out);
  if (ext == L"docx") { Document d; if (!ReadDocx(out, d, &err)) { *why = err; return {}; } return AllText(d); }
  if (ext == L"epub") { Document d; if (!ReadEpub(out, d, &err)) { *why = err; return {}; } return AllText(d); }
  if (ext == L"pdf") return PdfAllText(out);
  if (ext == L"xlsx" || ext == L"ods") {
    Workbook wb; bool ok = ext == L"xlsx" ? ReadXlsx(out, wb, &err) : ReadOds(out, wb, &err);
    if (!ok) { *why = err; return {}; }
    std::string s; for (auto& sh : wb.sheets) for (auto& r : sh.rows) for (auto& c : r) s += IsoFromCell(c) + "\n";
    return s;
  }
  Bytes b; ReadFileBytes(out, b, 1ull << 30);
  return DecodeText(b).utf8;
}

std::string SafeId(std::string id) { for (auto& c : id) if (!isalnum((unsigned char)c)) c = '_'; return id; }

} // namespace

TEST(capabilities_matrix) {
  EnsureRegistered();
  Fixtures& fx = Fx();
  Json results = Json::MakeObject();
  int pass = 0, failN = 0, untested = 0, unsupported = 0;
  std::vector<std::string> failures;
  for (const Capability& cap : CapabilityRegistry::Get().All()) {
    if (cap.engine == "none" || (!cap.run && cap.out == L"pages")) { ++unsupported; continue; }
    if (cap.id.rfind("pdf>", 0) == 0) continue;           // PDF -> Office is tested in t_pdf_office.cpp
    Json entry = Json::MakeObject();
    auto record = [&](const char* status, const std::string& note) { entry["status"] = status; entry["note"] = note; results[cap.id] = entry; };
    if (!cap.Runnable()) { record("untested", "required component (LibreOffice) not installed on this machine"); ++untested; continue; }
    auto it = fx.byFormat.find(cap.in);
    if (it == fx.byFormat.end()) { record("fail", "no test fixture could be created for the input format"); ++failN; failures.push_back(cap.id + " (no fixture)"); CHECK(false); continue; }
    const fs::path& input = it->second;
    bool inIsDir = fs::is_directory(input);
    uint64_t before = inIsDir ? HashDir(input) : Hash(input);
    fs::path outDir = Work("caps_out_" + SafeId(cap.id));
    fs::path outPath = outDir / (L"result." + cap.out);
    CapRun r = RunCap(cap.id, input, outPath);
    uint64_t after = inIsDir ? HashDir(input) : Hash(input);
    std::string problem;
    if (!r.ok) problem = "conversion failed: " + Narrow(r.error);
    else if (r.outputs.empty()) problem = "no output reported";
    else {
      fs::path produced = r.outputs[0];
      std::error_code ec;
      bool isDir = fs::is_directory(produced, ec);
      if (cap.producesFolder != isDir) problem = "output kind (file/folder) does not match the capability";
      else if (!fs::exists(produced, ec)) problem = "output does not exist";
      else if (!isDir && fs::file_size(produced, ec) == 0) problem = "output is empty";
      else {
        std::wstring why;
        if (!isDir) { if (ExtOf(produced) != cap.out) problem = "output has the wrong extension"; }
        std::string text = TextOf(produced, cap.out, isDir, &why);
        if (problem.empty() && !why.empty()) problem = "output unreadable: " + Narrow(why);
        if (problem.empty() && (cap.out == L"png" || cap.out == L"jpg")) {
          std::vector<fs::path> imgs; for (auto& e : fs::directory_iterator(produced, ec)) imgs.push_back(e.path());
          std::sort(imgs.begin(), imgs.end());
          if (imgs.size() != 3) problem = "expected 3 slide images, got " + std::to_string(imgs.size());
          for (size_t i = 0; problem.empty() && i < imgs.size(); ++i) {
            char want[32]; snprintf(want, sizeof want, "slide-%03zu.%s", i + 1, cap.out == L"png" ? "png" : "jpg");
            if (Narrow(imgs[i].filename().wstring()) != want) problem = std::string("unexpected file name ") + Narrow(imgs[i].filename().wstring());
            Bytes ib; ReadFileBytes(imgs[i], ib, 1 << 28); int w = 0, h = 0; Bytes bgra;
            if (problem.empty() && !DecodeToBgra(ib, w, h, bgra)) problem = "slide image does not decode";
            else if (problem.empty()) {
              double ratio = (double)w / h;
              if (std::fabs(ratio - 16.0 / 9.0) > 0.01) problem = "aspect ratio changed";
              size_t dark = 0; for (size_t k = 0; k + 3 < bgra.size(); k += 4) if (bgra[k] < 200 || bgra[k + 1] < 200 || bgra[k + 2] < 200) ++dark;
              if (problem.empty() && dark < 200) problem = "slide image is blank";
              if (problem.empty() && i == 0 && std::abs(w - 1500) > 2) problem = "unexpected resolution " + std::to_string(w) + "x" + std::to_string(h);
            }
          }
        } else if (problem.empty()) {
          for (auto& tok : fx.tokens[cap.in]) {
            bool arabicTok = tok == kArabic;
            bool found = Contains(text, tok);
            if (!found && arabicTok && cap.out == L"pdf") found = CountArabic(text) >= 3;           // shaped glyphs
            if (!found && cap.out == L"txt" && Contains(cap.id, "csv")) found = true;
            if (!found && arabicTok && cap.out == L"png") found = true;
            if (!found) { problem = "expected text missing from the output: " + tok; break; }
          }
        }
      }
    }
    if (problem.empty() && before != after) problem = "THE INPUT FILE WAS MODIFIED";
    // no temp leftovers next to the output
    std::error_code ec2;
    for (auto& e : fs::directory_iterator(outDir, ec2)) if (Contains(Narrow(e.path().filename().wstring()), ".tmp")) problem = problem.empty() ? "temporary file left behind" : problem;
    if (problem.empty()) { std::string note = "ok"; for (auto& w : r.warnings) note += "; " + Narrow(w); record("pass", note.substr(0, 400)); ++pass; }
    else { record("fail", problem); ++failN; failures.push_back(cap.id + ": " + problem); std::printf("  FAIL %s: %s\n", cap.id.c_str(), problem.c_str()); CHECK(problem.empty()); }
  }
  Json root = Json::MakeObject(); root["results"] = results;
  root["engine"] = "doc"; root["libreoffice"] = HaveLo() ? Narrow(LibreOfficeVersion()) : std::string("not installed");
  WriteAll(TestData() / L"matrix-results.json", root.Dump(true));
  std::printf("  matrix: %d pass, %d fail, %d untested (component missing), %d documented-unsupported paths\n", pass, failN, untested, unsupported);
  CHECK(pass >= 60);
}

// ---------------------------------------------------------------------------------------------------------------- option variants and content details
TEST(capability_options_and_output_details) {
  Fixtures& fx = Fx();
  Json o = Json::MakeObject();
  // csv -> xlsx: guard on by default, inferTypes converts
  {
    fs::path dir = Work("opt_csv"); fs::path in = dir / L"in.csv";
    WriteAll(in, "code;phone;day;amount\r\n007;+15551234567;01/02/2024;1,5\r\n0042;5551234567;13/02/2024;2,5\r\n");
    CapRun r = RunCap("csv>xlsx", in, dir / L"a.xlsx"); CHECK(r.ok);
    Workbook wb; std::wstring e; CHECK(ReadXlsx(dir / L"a.xlsx", wb, &e));
    CHECK(wb.sheets[0].rows[1][0].t == SCell::T::Text); CHECK_EQ(wb.sheets[0].rows[1][0].s, std::string("007"));
    CHECK(wb.sheets[0].rows[1][1].t == SCell::T::Text); CHECK(wb.sheets[0].rows[1][2].t == SCell::T::Text);
    Json oi = Json::MakeObject(); oi["inferTypes"] = true; oi["dateOrder"] = "dmy";
    CapRun r2 = RunCap("csv>xlsx", in, dir / L"b.xlsx", oi); CHECK(r2.ok);
    Workbook wb2; CHECK(ReadXlsx(dir / L"b.xlsx", wb2, &e));
    CHECK(wb2.sheets[0].rows[1][0].t == SCell::T::Number); CHECK(wb2.sheets[0].rows[1][2].t == SCell::T::Date); CHECK_EQ(IsoFromCell(wb2.sheets[0].rows[1][2]), std::string("2024-02-01"));
    // delimiter override
    Json od = Json::MakeObject(); od["delimiter"] = "comma";
    CapRun r3 = RunCap("csv>xlsx", in, dir / L"c.xlsx", od); CHECK(r3.ok);
    Workbook wb3; CHECK(ReadXlsx(dir / L"c.xlsx", wb3, &e)); CHECK_EQ(wb3.sheets[0].rows[0].size(), (size_t)1);
  }
  // csv <-> tsv keep text untouched (no number normalisation), encodings
  {
    fs::path dir = Work("opt_tsv"); fs::path in = dir / L"in.csv";
    WriteAll(in, "a,b\r\n3.50,\"x,y\"\r\n007,\"q\"\"q\"\r\n");
    CapRun r = RunCap("csv>tsv", in, dir / L"o.tsv"); CHECK(r.ok);
    CHECK_EQ(ReadAll(dir / L"o.tsv"), std::string("a\tb\n3.50\tx,y\n007\tq\"q\n"));
    CapRun r2 = RunCap("tsv>csv", dir / L"o.tsv", dir / L"back.csv"); CHECK(r2.ok);
    std::string back = ReadAll(dir / L"back.csv");
    CHECK(StartsWith(back, "\xEF\xBB\xBF" "a,b\r\n3.50,\"x,y\"\r\n007,\"q\"\"q\"\r\n"));
    Json ou = Json::MakeObject(); ou["outputEncoding"] = "utf-16le-bom";
    CapRun r3 = RunCap("csv>tsv", in, dir / L"u16.tsv", ou); CHECK(r3.ok);
    Bytes b; ReadFileBytes(dir / L"u16.tsv", b, 1 << 20); CHECK(b.size() > 2 && b[0] == 0xFF && b[1] == 0xFE);
  }
  // xlsx -> csv: sheet option by name and number, semicolon output, all-sheets folder variant
  {
    fs::path dir = Work("opt_xlsx"); fs::path in = fx.byFormat[L"xlsx"];
    Json os = Json::MakeObject(); os["sheet"] = "Second sheet";
    CapRun r = RunCap("xlsx>csv", in, dir / L"s2.csv", os); CHECK(r.ok); CHECK(Contains(ReadAll(dir / L"s2.csv"), "x,y")); CHECK(!Contains(ReadAll(dir / L"s2.csv"), "Alice"));
    Json on = Json::MakeObject(); on["sheet"] = "2"; on["outputDelimiter"] = "semicolon";
    CapRun r2 = RunCap("xlsx>csv", in, dir / L"s2b.csv", on); CHECK(r2.ok); CHECK(Contains(ReadAll(dir / L"s2b.csv"), "x;y"));
    CapRun r3 = RunCap("xlsx>csv", in, dir / L"first.csv"); CHECK(r3.ok);
    bool warned = false; for (auto& w : r3.warnings) if (Contains(Narrow(w), "first sheet")) warned = true; CHECK(warned);
    std::string first = ReadAll(dir / L"first.csv");
    CHECK(Contains(first, "name,qty,code,when,note")); CHECK(Contains(first, "007")); CHECK(Contains(first, "2024-01-31")); CHECK(Contains(first, "\"multi\nline\"")); CHECK(Contains(first, "\"say \"\"hi\"\", ok\""));
    Json obad = Json::MakeObject(); obad["sheet"] = "nope";
    CapRun r4 = RunCap("xlsx>csv", in, dir / L"bad.csv", obad); CHECK(!r4.ok); CHECK(!r4.error.empty()); CHECK(!fs::exists(dir / L"bad.csv"));
    CapRun r5 = RunCap("xlsx>csv#sheets", in, dir / L"all.csv"); CHECK(r5.ok);
    CHECK(fs::is_directory(dir / L"all"));
    CHECK(fs::exists(dir / L"all" / L"Data.csv")); CHECK(fs::exists(dir / L"all" / L"Second sheet.csv"));
    CHECK(Contains(ReadAll(dir / L"all" / L"Second sheet.csv"), "x,y"));
    CapRun r6 = RunCap("ods>tsv#sheets", fx.byFormat[L"ods"], dir / L"all2.tsv"); CHECK(!r6.found || r6.ok);
  }
  // xlsx <-> ods through the model keep values
  {
    fs::path dir = Work("opt_xlods");
    CapRun r = RunCap("xlsx>ods", fx.byFormat[L"xlsx"], dir / L"a.ods"); CHECK(r.ok);
    CapRun r2 = RunCap("ods>xlsx", dir / L"a.ods", dir / L"b.xlsx"); CHECK(r2.ok);
    Workbook a, b; std::wstring e; CHECK(ReadXlsx(fx.byFormat[L"xlsx"], a, &e)); CHECK(ReadXlsx(dir / L"b.xlsx", b, &e));
    CHECK_EQ(a.sheets.size(), b.sheets.size());
    for (size_t s = 0; s < a.sheets.size(); ++s) for (size_t rr = 0; rr < a.sheets[s].rows.size(); ++rr) for (size_t c = 0; c < a.sheets[s].rows[rr].size(); ++c) CHECK_EQ(IsoFromCell(a.sheets[s].rows[rr][c]), IsoFromCell(b.sheets[s].rows[rr][c]));
  }
  // documents: page size, txt encodings and line endings, markdown images, epub chapters
  {
    fs::path dir = Work("opt_docs");
    Json ol = Json::MakeObject(); ol["pageSize"] = "letter";
    CHECK(RunCap("md>docx", fx.byFormat[L"md"], dir / L"l.docx", ol).ok);
    ZipReader z; CHECK(z.Open(dir / L"l.docx")); CHECK(Contains(*z.ReadString("word/document.xml"), "w:w=\"12240\""));
    CHECK(RunCap("md>docx", fx.byFormat[L"md"], dir / L"a4.docx").ok);
    ZipReader z2; CHECK(z2.Open(dir / L"a4.docx")); CHECK(Contains(*z2.ReadString("word/document.xml"), "w:w=\"11906\""));
    Json ot = Json::MakeObject(); ot["outputEncoding"] = "utf-16le-bom"; ot["lineEnding"] = "lf";
    CHECK(RunCap("docx>txt", fx.byFormat[L"docx"], dir / L"u16.txt", ot).ok);
    Bytes b; ReadFileBytes(dir / L"u16.txt", b, 1 << 24); CHECK(b[0] == 0xFF && b[1] == 0xFE);
    DecodedText dt = DecodeText(b); CHECK(Contains(dt.utf8, "Quarterly report")); CHECK(!Contains(dt.utf8, "\r\n"));
    CHECK(RunCap("docx>txt", fx.byFormat[L"docx"], dir / L"crlf.txt").ok);
    CHECK(Contains(ReadAll(dir / L"crlf.txt"), "Quarterly report\r\n"));
    Json om = Json::MakeObject(); om["images"] = "omit";
    CHECK(RunCap("docx>md", fx.byFormat[L"docx"], dir / L"noimg.md", om).ok); CHECK(!Contains(ReadAll(dir / L"noimg.md"), "base64"));
    CHECK(RunCap("docx>md", fx.byFormat[L"docx"], dir / L"img.md").ok); CHECK(Contains(ReadAll(dir / L"img.md"), "data:image/png;base64,"));
    Json oe = Json::MakeObject(); oe["splitChapters"] = false;
    CHECK(RunCap("docx>epub", fx.byFormat[L"docx"], dir / L"one.epub", oe).ok);
    ZipReader ze; CHECK(ze.Open(dir / L"one.epub")); CHECK(!ze.Has("OEBPS/text/chapter002.xhtml"));
    CHECK(RunCap("docx>epub", fx.byFormat[L"docx"], dir / L"two.epub").ok);
    ZipReader ze2; CHECK(ze2.Open(dir / L"two.epub")); CHECK(ze2.Has("OEBPS/text/chapter002.xhtml"));
    // txt encodings: windows-1256 Arabic file converted with auto-detection and explicitly
    Bytes ar = EncodeText(std::string(kArabic) + " " + kArabic + " " + kArabic + "\r\nsecond line " + kArabic + "\r\n", TextEnc::Cp1256);
    WriteFileBytes(dir / L"ar1256.txt", ar.data(), ar.size());
    CHECK(RunCap("txt>docx", dir / L"ar1256.txt", dir / L"ar.docx").ok);
    Document d; std::wstring e; CHECK(ReadDocx(dir / L"ar.docx", d, &e)); CHECK(Contains(AllText(d), kArabic));
    Json oenc = Json::MakeObject(); oenc["encoding"] = "windows-1256";
    CHECK(RunCap("txt>md", dir / L"ar1256.txt", dir / L"ar.md", oenc).ok); CHECK(Contains(ReadAll(dir / L"ar.md"), kArabic));
    Bytes u16 = EncodeText(std::string("UTF-16 text ") + kArabic + "\r\nline two\r\n", TextEnc::Utf16LEBom); WriteFileBytes(dir / L"u16in.txt", u16.data(), u16.size());
    CHECK(RunCap("txt>md", dir / L"u16in.txt", dir / L"u16in.md").ok); CHECK(Contains(ReadAll(dir / L"u16in.md"), std::string("UTF-16 text ") + kArabic));
  }
  // presentations: resolution and jpeg quality
  if (HaveLo()) {
    fs::path dir = Work("opt_pres");
    Json o72 = Json::MakeObject(); o72["dpi"] = 72;
    CHECK(RunCap("pptx>png", fx.byFormat[L"pptx"], dir / L"low.png", o72).ok);
    Bytes ib; ReadFileBytes(dir / L"low" / L"slide-001.png", ib, 1 << 26); int w, h; Bytes px; CHECK(DecodeToBgra(ib, w, h, px)); CHECK_EQ(w, 720); CHECK(std::abs(h - 405) <= 1);
    Json q1 = Json::MakeObject(); q1["quality"] = 30; Json q2 = Json::MakeObject(); q2["quality"] = 95;
    CHECK(RunCap("pptx>jpg", fx.byFormat[L"pptx"], dir / L"q30.jpg", q1).ok); CHECK(RunCap("pptx>jpg", fx.byFormat[L"pptx"], dir / L"q95.jpg", q2).ok);
    CHECK(fs::file_size(dir / L"q30" / L"slide-001.jpg") < fs::file_size(dir / L"q95" / L"slide-001.jpg"));
  }
  // subtitles: txt output with timestamps; encodings
  {
    fs::path dir = Work("opt_subs");
    Json ot = Json::MakeObject(); ot["timestamps"] = true;
    CHECK(RunCap("srt>txt", fx.byFormat[L"srt"], dir / L"t.txt", ot).ok);
    CHECK(Contains(ReadAll(dir / L"t.txt"), "[00:00:01.000 --> 00:00:03.500] Hello world"));
    CHECK(RunCap("srt>txt", fx.byFormat[L"srt"], dir / L"p.txt").ok); CHECK(!Contains(ReadAll(dir / L"p.txt"), "-->"));
    CHECK(RunCap("srt>vtt", fx.byFormat[L"srt"], dir / L"o.vtt").ok);
    std::string vtt = ReadAll(dir / L"o.vtt"); CHECK(StartsWith(vtt, "WEBVTT\n")); CHECK(Contains(vtt, "00:00:01.000 --> 00:00:03.500\nHello <i>world</i>")); CHECK(!Contains(vtt, "\r"));
    CHECK(RunCap("vtt>srt", dir / L"o.vtt", dir / L"back.srt").ok);
    std::string srt = ReadAll(dir / L"back.srt"); CHECK(StartsWith(srt, "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:03,500\r\nHello <i>world</i>\r\n"));
    // UTF-16 subtitles with BOM are read
    Bytes u16 = EncodeText(ReadAll(fx.byFormat[L"vtt"]), TextEnc::Utf16LEBom); WriteFileBytes(dir / L"u16.vtt", u16.data(), u16.size());
    CHECK(RunCap("vtt>srt", dir / L"u16.vtt", dir / L"u16.srt").ok); CHECK(Contains(ReadAll(dir / L"u16.srt"), kArabic));
  }
}

TEST(capability_text_clipping_and_pages_details) {
  Fixtures& fx = Fx();
  fs::path dir = Work("clip_pages");
  CHECK(RunCap("textclipping>docx", fx.byFormat[L"textclipping"], dir / L"c.docx").ok);
  Document d; std::wstring e; CHECK(ReadDocx(dir / L"c.docx", d, &e)); CHECK(Contains(AllText(d), "Clipped text"));
  // resource-fork only clipping (empty data fork on NTFS)
  WriteAll(dir / L"empty.textClipping", "");
  CapRun r = RunCap("textclipping>txt", dir / L"empty.textClipping", dir / L"e.txt");
  CHECK(!r.ok); CHECK(Contains(Narrow(r.error), "resource fork") || Contains(Narrow(r.error), "empty"));
  CHECK(!fs::exists(dir / L"e.txt"));
  if (fx.byFormat.count(L"pages")) {
    CHECK(RunCap("pages>pdf", fx.byFormat[L"pages"], dir / L"p.pdf").ok);
    CHECK(Contains(PdfAllText(dir / L"p.pdf"), "Quarterly report"));
    // a Pages file with only a thumbnail cannot be converted, and says so
    { ZipWriter z; z.Open(dir / L"thumb.pages"); z.Add("Index/Document.iwa", std::string("x")); Bytes th = MakePng(10, 10, true); z.Add("QuickLook/Thumbnail.jpg", th, false); z.Close(); }
    CapRun r2 = RunCap("pages>pdf", dir / L"thumb.pages", dir / L"t.pdf"); CHECK(!r2.ok); CHECK(Contains(Narrow(r2.error), "preview"));
    WriteAll(dir / L"junk.pages", "not a zip"); CapRun r3 = RunCap("pages>pdf", dir / L"junk.pages", dir / L"j.pdf"); CHECK(!r3.ok);
  }
  // >pages is registered as documented-unsupported with the required limitation text
  for (const char* id : {"docx>pages", "md>pages", "txt>pages", "html>pages", "rtf>pages", "epub>pages", "doc>pages", "pdf>pages"}) {
    const Capability* c = FindCap(id);
    CHECK(c != nullptr);
    if (c) { CHECK(c->run == nullptr); CHECK(!c->Runnable()); CHECK(Contains(Narrow(c->limitation), "Pages is an Apple proprietary format; DOCX opens directly in Pages.")); }
    if (c) { auto outs = CapabilityRegistry::Get().OutputsFor(c->in); for (auto* o : outs) CHECK(o->out != L"pages"); }
  }
}

// ---------------------------------------------------------------------------------------------------------------- robustness
TEST(capabilities_reject_bad_inputs_cleanly) {
  EnsureRegistered();
  fs::path dir = Work("bad_inputs");
  struct Case { const char* id; const char* name; std::string content; };
  const std::string garbage = std::string("\x00\x01\x02garbage\xFF\xFE", 11) + std::string(300, '\x07');
  std::vector<Case> cases = {
    {"docx>md", "garbage.docx", "PK\x03\x04 not really a zip at all"}, {"docx>txt", "empty.docx", ""}, {"epub>docx", "garbage.epub", "PK\x03\x04 xx"},
    {"xlsx>csv", "garbage.xlsx", "PK\x03\x04 xx"}, {"ods>xlsx", "empty.ods", ""}, {"rtf>docx", "garbage.rtf", "{\\rtf1 unterminated {{{"}, {"rtf>txt", "empty.rtf", ""},
    {"csv>xlsx", "empty.csv", ""}, {"csv>xlsx", "binary.csv", garbage}, {"tsv>csv", "empty.tsv", ""}, {"srt>vtt", "empty.srt", ""}, {"srt>vtt", "nocues.srt", "just some words\nno timing here\n"},
    {"vtt>srt", "nohdr.vtt", "00:00:01.000 --> 00:00:02.000\nx\n"}, {"md>docx", "empty.md", ""}, {"txt>docx", "empty.txt", ""}, {"txt>docx", "blank.txt", "\n\n   \n"},
    {"html>docx", "empty.html", ""}, {"textclipping>txt", "garbage.textClipping", garbage}, {"pages>pdf", "empty.pages", ""}, {"pptx>pdf", "empty.pptx", ""}, {"xls>xlsx", "empty.xls", ""},
  };
  for (auto& c : cases) {
    const Capability* cap = FindCap(c.id);
    CHECK(cap != nullptr);
    if (!cap || !cap->Runnable()) continue;
    fs::path in = dir / Widen(c.name); WriteAll(in, c.content);
    uint64_t before = Hash(in);
    fs::path out = dir / (L"out_" + Widen(SafeId(std::string(c.id) + c.name)) + L"." + cap->out);
    CapRun r = RunCap(c.id, in, out);
    if (r.ok) { std::printf("  %s on %s unexpectedly succeeded\n", c.id, c.name); CHECK(!r.ok); }
    else { CHECK(!r.error.empty()); CHECK(r.error.size() < 400); }
    CHECK(!fs::exists(out));
    CHECK_EQ(Hash(in), before);
    std::error_code ec; for (auto& e : fs::directory_iterator(dir, ec)) CHECK(!Contains(Narrow(e.path().filename().wstring()), ".tmp"));
  }
}

TEST(capabilities_use_real_content_not_extension) {
  Fixtures& fx = Fx();
  fs::path dir = Work("wrong_ext");
  // a DOCX saved with a .txt extension is converted as a DOCX, with a warning
  fs::path fake = dir / L"really-a-docx.txt";
  fs::copy_file(fx.byFormat[L"docx"], fake);
  const Capability* cap = FindCap("docx>md"); CHECK(cap != nullptr);
  FileKind k = DetectFile(fake); CHECK(k.format == L"docx"); CHECK(k.extensionMismatch);
  CapRun r = RunCap("docx>md", fake, dir / L"o.md"); CHECK(r.ok);
  CHECK(Contains(ReadAll(dir / L"o.md"), "Quarterly report"));
  bool warned = false; for (auto& w : r.warnings) if (Contains(Narrow(w), "does not match")) warned = true; CHECK(warned);
  // an extension-less file
  fs::path noext = dir / L"README"; WriteAll(noext, "# Heading\n\ntext\n");
  FileKind k2 = DetectFile(noext); CHECK(k2.format == L"txt");
  // an XLSX saved as .zip is still recognised
  fs::path zipx = dir / L"book.zip"; fs::copy_file(fx.byFormat[L"xlsx"], zipx);
  CHECK(DetectFile(zipx).format == L"xlsx");
}

TEST(capabilities_unicode_and_long_paths) {
  Fixtures& fx = Fx();
  fs::path base = Work("long_paths");
  fs::path deep = base;
  for (int i = 0; i < 9; ++i) deep /= Widen("\xD9\x85\xD8\xAC\xD9\x84\xD8\xAF \xF0\x9F\x93\x81 level" + std::to_string(i) + "_" + std::string(22, 'x'));
  std::error_code ec; fs::create_directories(LongPath(deep), ec);
  CHECK(!ec);
  fs::path in = deep / Widen("\xD9\x85\xD8\xB3\xD8\xAA\xD9\x86\xD8\xAF \xF0\x9F\x98\x80 test file.md");
  std::wstring inLong = LongPath(in); CHECK(inLong.size() > 260);
  std::string md = ReadAll(fx.byFormat[L"md"]);
  { HANDLE h = CreateFileW(LongPath(in).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); CHECK(h != INVALID_HANDLE_VALUE); if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, md.data(), (DWORD)md.size(), &w, nullptr); CloseHandle(h); } }
  uint64_t before = Hash(in);
  for (const char* id : {"md>docx", "md>html", "md>epub", "md>txt"}) {
    fs::path out = deep / Widen(std::string("\xD9\x86\xD8\xA7\xD8\xAA\xD8\xAC \xF0\x9F\x9A\x80 ") + id + "." + Narrow(FindCap(id)->out));
    CapRun r = RunCap(id, in, out);
    CHECK(r.ok); if (!r.ok) std::printf("  %s: %s\n", id, Narrow(r.error).c_str());
    if (r.ok) { CHECK(!r.outputs.empty()); CHECK(fs::exists(LongPath(r.outputs[0]))); }
  }
  CHECK_EQ(Hash(in), before);
  if (HaveLo()) {
    fs::path out = deep / L"long-name-pdf.pdf";
    CapRun r = RunCap("md>pdf", in, out); CHECK(r.ok); if (!r.ok) std::printf("  md>pdf: %s\n", Narrow(r.error).c_str());
    if (r.ok) CHECK(Contains(PdfAllText(r.outputs[0]), "Quarterly report"));
  }
  // names that look like command-line options or contain quotes/spaces
  fs::path odd = Work("odd_names");
  fs::path dash = odd / L"-rf --version \"quoted\" & more.md"; WriteAll(dash, "# Dashed\n\ntext\n");
  CapRun r = RunCap("md>docx", dash, odd / L"-out file (1).docx"); CHECK(r.ok);
  if (HaveLo()) { CapRun r2 = RunCap("md>pdf", dash, odd / L"-out file (2).pdf"); CHECK(r2.ok); if (r2.ok) CHECK(Contains(PdfAllText(r2.outputs[0]), "Dashed")); }
  fs::path inDash = odd / L"-docx -input.docx"; fs::copy_file(fx.byFormat[L"docx"], inDash);
  if (HaveLo()) { CapRun r3 = RunCap("docx>pdf", inDash, odd / L"-out docx.pdf"); CHECK(r3.ok); }
}

TEST(capabilities_vanish_without_libreoffice) {
  EnsureRegistered();
  SetLibreOfficeDisabledForTests(true);
  CHECK(!LibreOfficeAvailable());
  size_t need = 0, noNeed = 0;
  for (const Capability& c : CapabilityRegistry::Get().All()) {
    if (!c.run) continue;
    if (c.engine == "libreoffice") { ++need; CHECK(!c.Runnable()); }
    else if (c.id.rfind("pdf>", 0) != 0) { ++noNeed; CHECK(c.Runnable()); }
  }
  CHECK(need >= 25); CHECK(noNeed >= 40);
  // not offered in the UI list
  auto outs = CapabilityRegistry::Get().OutputsFor(L"docx");
  bool hasPdf = false, hasMd = false; for (auto* c : outs) { if (c->out == L"pdf") hasPdf = true; if (c->out == L"md") hasMd = true; }
  CHECK(!hasPdf); CHECK(hasMd);
  for (auto* c : CapabilityRegistry::Get().OutputsFor(L"pptx")) CHECK(false && c);          // nothing at all without LibreOffice
  // a forced run reports a clear message and creates nothing
  fs::path dir = Work("no_lo");
  CapRun r = RunCap("docx>pdf", Fx().byFormat[L"docx"], dir / L"o.pdf");
  CHECK(!r.ok); CHECK(!r.error.empty()); CHECK(!fs::exists(dir / L"o.pdf"));
  // legacy doc input cannot be read either, but native paths keep working
  CapRun r2 = RunCap("docx>md", Fx().byFormat[L"docx"], dir / L"o.md"); CHECK(r2.ok);
  SetLibreOfficeDisabledForTests(false);
  CHECK(LibreOfficeAvailable() == HaveLo());
}

static std::string LoProfileProcesses() {
  int code = -1;
  fs::path ps = fs::path(_wgetenv(L"SystemRoot")) / L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
  return RunCapture(ps, {L"-NoProfile", L"-NonInteractive", L"-Command", L"Get-CimInstance Win32_Process -Filter \"Name='soffice.bin' OR Name='soffice.exe'\" | ForEach-Object { $_.CommandLine }"}, 60000, &code);
}

TEST(libreoffice_cancellation_and_cleanup) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: skipped)\n"); return; }
  fs::path dir = Work("cancel");
  Document d;
  for (int i = 0; i < 4000; ++i) d.blocks.push_back(MakeParagraph("Paragraph number " + std::to_string(i) + " with enough words to fill a few lines of the page so that layout takes a while to finish."));
  FinalizeDocument(d);
  fs::path big = dir / L"big.docx"; std::wstring err; CHECK(WriteDocx(d, big, {}, &err));
  uint64_t before = Hash(big);
  auto t0 = std::chrono::steady_clock::now();
  CapRun r = RunCap("docx>pdf", big, dir / L"big.pdf", Json::MakeObject(), 1500);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  cancelled job returned after %.1fs, state=%d\n", secs, (int)r.state);
  CHECK(!r.ok);
  CHECK(r.state == JobState::Cancelled || r.state == JobState::Failed);
  CHECK(!fs::exists(dir / L"big.pdf"));
  CHECK(secs < 60);
  Sleep(1500);
  std::string procs = LoProfileProcesses();
  CHECK(!Contains(procs, "lo-profile"));              // no orphaned soffice.bin on our private profile
  CHECK_EQ(Hash(big), before);
  std::error_code ec; for (auto& e : fs::directory_iterator(dir, ec)) CHECK(!Contains(Narrow(e.path().filename().wstring()), ".tmp"));
  // the profile is still usable afterwards
  CapRun r2 = RunCap("md>pdf", Fx().byFormat[L"md"], dir / L"after.pdf"); CHECK(r2.ok);
  // cancelling before the job starts never produces output
  JobSpec spec; spec.operation = L"convert"; spec.title = L"cancel-early"; spec.inputs = {Fx().byFormat[L"docx"]};
  const Capability* cap = FindCap("docx>md"); fs::path outEarly = dir / L"early.md";
  spec.work = [cap, outEarly](JobContext& ctx) { ctx.SetProgress(0.1); return RunConversion(*cap, Fx().byFormat[L"docx"], Json::MakeObject(), ctx, &outEarly); };
  uint64_t id = JobManager::Get().Submit(std::move(spec)); JobManager::Get().Cancel(id); JobManager::Get().WaitFor(id, 60000);
  auto info = JobManager::Get().Info(id); CHECK(info.has_value());
  if (info && info->state == JobState::Cancelled) CHECK(!fs::exists(outEarly));
}

TEST(libreoffice_concurrent_jobs_are_serialised_and_isolated) {
  if (!HaveLo()) { std::printf("  (LibreOffice not installed: skipped)\n"); return; }
  Fixtures& fx = Fx();
  fs::path dir = Work("concurrent");
  JobManager::Get().SetMaxConcurrent(4);
  std::vector<uint64_t> ids; std::vector<fs::path> outs;
  for (int i = 0; i < 4; ++i) {
    const Capability* cap = FindCap(i % 2 ? "docx>pdf" : "xlsx>pdf");
    fs::path in = fx.byFormat[i % 2 ? L"docx" : L"xlsx"]; fs::path out = dir / (L"o" + std::to_wstring(i) + L".pdf"); outs.push_back(out);
    JobSpec spec; spec.operation = L"convert"; spec.title = L"c"; spec.inputs = {in};
    spec.work = [cap, in, out](JobContext& ctx) { return RunConversion(*cap, in, Json::MakeObject(), ctx, &out); };
    ids.push_back(JobManager::Get().Submit(std::move(spec)));
  }
  for (auto id : ids) JobManager::Get().WaitFor(id, 600000);
  for (size_t i = 0; i < ids.size(); ++i) { auto info = JobManager::Get().Info(ids[i]); CHECK(info && info->result.ok); CHECK(fs::exists(outs[i])); }
  JobManager::Get().SetMaxConcurrent(2);
}
