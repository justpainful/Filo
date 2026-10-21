#include "engines/doc/convert_common.h"
#include "engines/doc/libreoffice.h"
#include "engines/doc/sheet.h"
#include "engines/doc/subs.h"
#include <algorithm>

namespace filo::doc {

namespace {

char DelimiterOpt(const Json& o, const char* key, char def) {
  std::string v = OptStr(o, key, "");
  if (v.empty() || v == "auto") return def;
  if (v == "tab" || v == "\\t" || v == "\t") return '\t';
  if (v == "comma") return ',';
  if (v == "semicolon") return ';';
  if (v == "pipe") return '|';
  return v[0];
}

bool LoadWorkbook(const ConvertTask& t, JobContext& ctx, Workbook& wb, std::wstring* err) {
  const std::wstring& f = t.inKind.format;
  if (t.inKind.size == 0) { if (err) *err = L"The file is empty, so there is nothing to convert."; return false; }
  if (f == L"csv" || f == L"tsv") {
    Bytes data;
    if (!ReadFileBytes(t.input, data, 1536ull << 20, err)) return false;
    CsvReadOptions co;
    co.delimiter = DelimiterOpt(t.options, "delimiter", f == L"tsv" ? '\t' : 0);
    std::string q = OptStr(t.options, "quote", "\"");
    co.quote = q.empty() ? '"' : q[0];
    co.encoding = ParseEncodingName(OptStr(t.options, "encoding", "auto"));
    co.inferTypes = OptBool(t.options, "inferTypes", false);
    co.dayFirst = OptStr(t.options, "dateOrder", "dmy") != "mdy";
    co.keepText = t.outExt == L"csv" || t.outExt == L"tsv";
    Sheet sh; CsvInfo info;
    if (!ParseCsv(data, co, sh, &info, err)) return false;
    if (info.cols == 0) { if (err) *err = L"The file contains no table data."; return false; }
    sh.name = SafeSheetName(Narrow(t.input.stem().wstring()), {});
    wb.sheets.push_back(std::move(sh));
    return true;
  }
  if (f == L"xlsx") return ReadXlsx(t.input, wb, err);
  if (f == L"ods") return ReadOds(t.input, wb, err);
  if (f == L"xls") {
    fs::path tmp = ctx.TempDir() / L"legacy.xlsx";
    if (!LoViaTemp(t.input, L"xls", L"xlsx:Calc MS Excel 2007 XML", L"xlsx", tmp, ctx, err)) return false;
    return ReadXlsx(tmp, wb, err);
  }
  if (err) *err = L"This input format is not supported by the spreadsheet converter.";
  return false;
}

const Sheet* PickSheet(const Workbook& wb, const std::string& which, std::wstring* err) {
  if (wb.sheets.empty()) { if (err) *err = L"The workbook has no sheets."; return nullptr; }
  if (which.empty() || which == "first") return &wb.sheets[0];
  bool num = true; for (char c : which) if (c < '0' || c > '9') num = false;
  if (num) { size_t i = (size_t)atoll(which.c_str()); if (i >= 1 && i <= wb.sheets.size()) return &wb.sheets[i - 1]; }
  for (auto& s : wb.sheets) if (EqualsNoCase(s.name, which)) return &s;
  if (err) *err = L"The requested sheet was not found in the workbook.";
  return nullptr;
}

CsvWriteOptions CsvOutOptions(const ConvertTask& t, bool tsv) {
  CsvWriteOptions o;
  o.delimiter = tsv ? '\t' : DelimiterOpt(t.options, "outputDelimiter", ',');
  if (o.delimiter == 0) o.delimiter = ',';
  TextEnc enc = ParseEncodingName(OptStr(t.options, "outputEncoding", tsv ? "utf-8" : "utf-8-bom"));
  o.encoding = enc == TextEnc::Auto ? (tsv ? TextEnc::Utf8 : TextEnc::Utf8Bom) : enc;
  o.crlf = tsv ? OptStr(t.options, "lineEnding", "lf") == "crlf" : OptStr(t.options, "lineEnding", "crlf") != "lf";
  return o;
}

} // namespace

EngineResult RunSheetConversion(const ConvertTask& t, JobContext& ctx) {
  const std::wstring& in = t.inKind.format;
  const std::wstring& out = t.outExt;
  std::wstring err;
  EngineResult er;
  ctx.SetProgress(0.05);

  // direct LibreOffice paths
  if (out == L"pdf" && (in == L"xlsx" || in == L"ods" || in == L"xls")) {
    if (t.inKind.size == 0) return Fail(L"The file is empty, so there is nothing to convert.");
    if (!LoViaTemp(t.input, in, L"pdf", L"pdf", t.output, ctx, &err)) return Fail(err);
    ctx.SetProgress(1.0);
    er.ok = true;
    return er;
  }
  Workbook wb;
  if (!LoadWorkbook(t, ctx, wb, &err)) return Fail(err);
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  ctx.SetProgress(0.4);
  AddWarnings(er, wb.warnings);
  std::vector<std::wstring> w;

  if (out == L"csv" || out == L"tsv") {
    const Sheet* sh = PickSheet(wb, OptStr(t.options, "sheet", ""), &err);
    if (!sh) return Fail(err);
    if (wb.sheets.size() > 1 && OptStr(t.options, "sheet", "").empty()) er.warnings.push_back(L"Only the first sheet was exported (the workbook has " + std::to_wstring(wb.sheets.size()) + L" sheets). Choose a sheet in the options or use the all-sheets export.");
    bool formulaNote = false; (void)formulaNote;
    Bytes b = WriteCsv(*sh, CsvOutOptions(t, out == L"tsv"));
    if (!WriteFileBytes(t.output, b.data(), b.size(), &err)) return Fail(err);
    if (in == L"xlsx" || in == L"ods" || in == L"xls") er.warnings.push_back(L"Cell values are exported as stored (no number formatting); dates are written as ISO dates.");
  }
  else if (out == L"xlsx") {
    if (!WriteXlsx(wb, t.output, &err, &w)) return Fail(err);
    AddWarnings(er, w);
  }
  else if (out == L"ods") {
    if (!WriteOds(wb, t.output, &err, &w)) return Fail(err);
    AddWarnings(er, w);
  }
  else if (out == L"pdf") {
    fs::path tmp = ctx.TempDir() / L"stage.xlsx";
    if (!WriteXlsx(wb, tmp, &err, &w)) return Fail(err);
    AddWarnings(er, w);
    ctx.SetProgress(0.5);
    if (!LoViaTemp(tmp, L"xlsx", L"pdf", L"pdf", t.output, ctx, &err)) return Fail(err);
  }
  else return Fail(L"This output format is not supported.");
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  ctx.SetProgress(1.0);
  er.ok = true;
  return er;
}

EngineResult RunSheetFolderConversion(const ConvertTask& t, JobContext& ctx) {
  std::wstring err;
  EngineResult er;
  Workbook wb;
  if (!LoadWorkbook(t, ctx, wb, &err)) return Fail(err);
  AddWarnings(er, wb.warnings);
  bool tsv = t.outExt == L"tsv";
  CsvWriteOptions co = CsvOutOptions(t, tsv);
  std::vector<std::string> used;
  std::error_code ec;
  fs::create_directories(t.output, ec);
  for (size_t i = 0; i < wb.sheets.size(); ++i) {
    if (ctx.Cancelled()) return Fail(L"Cancelled");
    std::string base = SafeName(wb.sheets[i].name, 60);
    std::string nm = base; int k = 2;
    auto taken = [&](const std::string& n) { for (auto& u : used) if (LowerAscii(u) == LowerAscii(n)) return true; return false; };
    while (taken(nm)) nm = base + " (" + std::to_string(k++) + ")";
    used.push_back(nm);
    Bytes b = WriteCsv(wb.sheets[i], co);
    fs::path p = t.output / (Widen(nm) + (tsv ? L".tsv" : L".csv"));
    if (!WriteFileBytes(p, b.data(), b.size(), &err)) return Fail(err);
    ctx.SetProgress(0.4 + 0.6 * (double)(i + 1) / (double)wb.sheets.size());
  }
  if (wb.sheets.empty()) return Fail(L"The workbook has no sheets.");
  er.ok = true;
  return er;
}

// ============================================================================================================================
EngineResult RunSubtitleConversion(const ConvertTask& t, JobContext& ctx) {
  const std::wstring& in = t.inKind.format;
  const std::wstring& out = t.outExt;
  std::wstring err;
  if (t.inKind.size == 0) return Fail(L"The file is empty, so there is nothing to convert.");
  Bytes data;
  if (!ReadFileBytes(t.input, data, 256ull << 20, &err)) return Fail(err);
  DecodedText dt = DecodeText(data, ParseEncodingName(OptStr(t.options, "encoding", "auto")));
  SubFile sf;
  if (in == L"srt") { if (!ParseSrt(dt.utf8, sf, &err)) return Fail(err); }
  else if (in == L"vtt") { if (!ParseVtt(dt.utf8, sf, &err)) return Fail(err); }
  else return Fail(L"This input format is not supported by the subtitle converter.");
  ctx.SetProgress(0.5);
  EngineResult er;
  AddWarnings(er, sf.warnings);
  if (dt.hadErrors) er.warnings.push_back(L"Some bytes were not valid in the detected text encoding and were replaced.");
  std::string text;
  TextEnc enc = TextEnc::Utf8;
  bool crlf = true;
  if (out == L"srt") { text = WriteSrt(sf); enc = ParseEncodingName(OptStr(t.options, "outputEncoding", "utf-8-bom")); }
  else if (out == L"vtt") { text = WriteVtt(sf); enc = ParseEncodingName(OptStr(t.options, "outputEncoding", "utf-8")); crlf = false; }
  else if (out == L"txt") { text = WriteSubText(sf, OptBool(t.options, "timestamps", false)); enc = ParseEncodingName(OptStr(t.options, "outputEncoding", "utf-8")); crlf = OptStr(t.options, "lineEnding", "crlf") != "lf"; }
  else return Fail(L"This output format is not supported.");
  if (enc == TextEnc::Auto) enc = TextEnc::Utf8;
  if (out == L"srt" || out == L"vtt") {
    // keep the writer's own line ends (srt: CRLF, vtt: LF)
    if (out == L"vtt") { text = NormalizeNewlines(text); }
  } else text = crlf ? ToCrLf(text) : NormalizeNewlines(text);
  Bytes b = EncodeText(text, enc);
  if (!WriteFileBytes(t.output, b.data(), b.size(), &err)) return Fail(err);
  ctx.SetProgress(1.0);
  er.ok = true;
  return er;
}

} // namespace filo::doc
