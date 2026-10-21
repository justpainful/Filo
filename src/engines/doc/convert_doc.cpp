#include "engines/doc/convert_common.h"
#include "engines/doc/docx.h"
#include "engines/doc/epub.h"
#include "engines/doc/html.h"
#include "engines/doc/libreoffice.h"
#include "engines/doc/markdown.h"
#include "engines/doc/pptx.h"
#include "engines/doc/rtf.h"
#include "engines/doc/sheet.h"
#include "engines/doc/subs.h"
#include "engines/doc/textfmt.h"
#include "engines/pdf/pdf_doc.h"
#include "core/log.h"
#include <algorithm>

namespace filo::doc {

bool OptBool(const Json& o, const char* key, bool def) {
  if (!o.Has(key)) return def;
  const Json& v = o.Get(key);
  if (v.IsBool()) return v.AsBool();
  if (v.IsNumber()) return v.AsNumber() != 0;
  if (v.IsString()) { std::string s = LowerAscii(v.AsString()); return s == "1" || s == "true" || s == "yes" || s == "on"; }
  return def;
}
std::string OptStr(const Json& o, const char* key, const std::string& def) {
  if (!o.Has(key)) return def;
  const Json& v = o.Get(key);
  if (v.IsString()) return v.AsString();
  if (v.IsNumber()) return FormatNumber(v.AsNumber());
  if (v.IsBool()) return v.AsBool() ? "true" : "false";
  return def;
}
double OptNum(const Json& o, const char* key, double def) {
  if (!o.Has(key)) return def;
  const Json& v = o.Get(key);
  if (v.IsNumber()) return v.AsNumber();
  if (v.IsString()) return atof(v.AsString().c_str());
  return def;
}

EngineResult Fail(const std::wstring& why) { return EngineResult::Fail(why); }
void AddWarnings(EngineResult& er, const std::vector<std::wstring>& w) {
  for (auto& x : w) { bool dup = false; for (auto& y : er.warnings) if (y == x) dup = true; if (!dup) er.warnings.push_back(x); }
}

bool LoViaTemp(const fs::path& input, const std::wstring& inExt, const std::wstring& convertTo, const std::wstring& outExt, const fs::path& outFile, JobContext& ctx, std::wstring* err) {
  LoConvertSpec s;
  s.input = input; s.inExt = inExt; s.convertTo = convertTo; s.outExt = outExt; s.output = outFile;
  LoResult r = LibreOfficeConvert(s, &ctx);
  if (!r.ok && err) *err = r.error;
  return r.ok;
}

static std::wstring EmptyFileMsg() { return L"The file is empty, so there is nothing to convert."; }

bool LoadDocument(const ConvertTask& t, JobContext& ctx, Document& d, std::wstring* err) {
  const std::wstring& f = t.inKind.format;
  const fs::path& in = t.input;
  if (f == L"rtfd") return ParseRtfd(in, d, err);
  if (t.inKind.isDir) { if (err) *err = L"A folder cannot be converted as a document."; return false; }
  if (t.inKind.size == 0) { if (err) *err = EmptyFileMsg(); return false; }
  if (f == L"docx") return ReadDocx(in, d, err);
  if (f == L"epub") return ReadEpub(in, d, err);
  if (f == L"doc") {
    fs::path tmp = ctx.TempDir() / L"legacy.docx";
    if (!LoViaTemp(in, L"doc", L"docx:MS Word 2007 XML", L"docx", tmp, ctx, err)) return false;
    return ReadDocx(tmp, d, err);
  }
  Bytes data;
  if (!ReadFileBytes(in, data, 512ull << 20, err)) return false;
  if (f == L"rtf") return ParseRtf(data.data(), data.size(), d, {}, err);
  if (f == L"textclipping") {
    std::string text;
    if (!ParseTextClipping(data, text, err)) return false;
    TextReadOptions to; to.reflow = OptBool(t.options, "reflow", false);
    d = ParseText(text, to);
    return true;
  }
  if (f == L"html") {
    HtmlReadOptions ho; ho.baseDir = in.parent_path();
    d = ParseHtml(DecodeHtmlBytes(data.data(), data.size()), ho);
    return true;
  }
  if (f == L"md") {
    DecodedText dt = DecodeText(data, ParseEncodingName(OptStr(t.options, "encoding", "auto")));
    MdReadOptions mo; mo.baseDir = in.parent_path();
    d = ParseMarkdown(dt.utf8, mo);
    if (dt.hadErrors) d.Warn(L"Some bytes were not valid in the detected text encoding and were replaced.");
    return true;
  }
  if (f == L"txt") {
    DecodedText dt = DecodeText(data, ParseEncodingName(OptStr(t.options, "encoding", "auto")));
    TextReadOptions to; to.reflow = OptBool(t.options, "reflow", false);
    d = ParseText(dt.utf8, to);
    if (dt.hadErrors) d.Warn(L"Some bytes were not valid in the detected text encoding and were replaced.");
    return true;
  }
  if (err) *err = L"This input format is not supported by the document converter.";
  return false;
}

static bool WriteTextFile(const fs::path& out, const std::string& utf8, TextEnc enc, bool crlf, std::wstring* err) {
  std::string s = crlf ? ToCrLf(utf8) : NormalizeNewlines(utf8);
  Bytes b = EncodeText(s, enc);
  return WriteFileBytes(out, b.data(), b.size(), err);
}

EngineResult RunDocConversion(const ConvertTask& t, JobContext& ctx) {
  const std::wstring& in = t.inKind.format;
  const std::wstring& out = t.outExt;
  std::wstring err;
  EngineResult er;

  // direct LibreOffice paths (best fidelity, no intermediate model)
  if (out == L"pdf" && (in == L"docx" || in == L"doc" || in == L"rtf")) {
    if (t.inKind.size == 0) return Fail(EmptyFileMsg());
    ctx.SetProgress(0.1);
    if (!LoViaTemp(t.input, in, L"pdf", L"pdf", t.output, ctx, &err)) return Fail(err);
    ctx.SetProgress(1.0);
    er.ok = true;
    return er;
  }

  Document d;
  ctx.SetProgress(0.05);
  if (!LoadDocument(t, ctx, d, &err)) return Fail(err);
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  if (!DocumentHasContent(d)) return Fail(L"The document is empty, so there is nothing to convert.");
  ctx.SetProgress(0.4);
  AddWarnings(er, d.warnings);

  if (out == L"docx") {
    DocxWriteOptions o; o.page = OptStr(t.options, "pageSize", "a4") == "letter" ? PageSize::Letter : PageSize::A4;
    std::vector<std::wstring> w;
    if (!WriteDocx(d, t.output, o, &err, &w)) return Fail(err);
    AddWarnings(er, w);
  }
  else if (out == L"md") {
    MdWriteOptions o; o.embedImages = OptStr(t.options, "images", "embed") != "omit";
    std::string md = WriteMarkdown(d, o);
    bool hasImg = false; for (auto& b : d.blocks) if (b.kind == Block::Kind::Image) hasImg = true;
    if (hasImg && !o.embedImages) er.warnings.push_back(L"Images were left out of the Markdown file.");
    if (hasImg && o.embedImages) er.warnings.push_back(L"Images are embedded in the Markdown file as data: links.");
    if (!WriteTextFile(t.output, md, TextEnc::Utf8, false, &err)) return Fail(err);
  }
  else if (out == L"txt") {
    TextWriteOptions o;
    std::string txt = WriteText(d, o);
    TextEnc enc = ParseEncodingName(OptStr(t.options, "outputEncoding", "utf-8"));
    if (enc == TextEnc::Auto) enc = TextEnc::Utf8;
    bool crlf = OptStr(t.options, "lineEnding", "crlf") != "lf";
    bool hasImg = false, hasTable = false; for (auto& b : d.blocks) { if (b.kind == Block::Kind::Image) hasImg = true; if (b.kind == Block::Kind::Table) hasTable = true; }
    if (hasImg) er.warnings.push_back(L"Images cannot be stored in a plain text file and were left out.");
    if (hasTable) er.warnings.push_back(L"Tables were converted to tab-separated lines.");
    if (!WriteTextFile(t.output, txt, enc, crlf, &err)) return Fail(err);
  }
  else if (out == L"html") {
    HtmlWriteOptions o; o.embedImages = true;
    std::string html = WriteHtml(d, o);
    if (!WriteTextFile(t.output, html, TextEnc::Utf8, false, &err)) return Fail(err);
  }
  else if (out == L"epub") {
    EpubWriteOptions o; o.splitOnH1 = OptBool(t.options, "splitChapters", true);
    std::vector<std::wstring> w;
    if (!WriteEpub(d, t.output, o, &err, &w)) return Fail(err);
    AddWarnings(er, w);
  }
  else if (out == L"pdf") {
    // model -> temporary DOCX -> LibreOffice -> PDF
    fs::path tmp = ctx.TempDir() / L"stage.docx";
    DocxWriteOptions o; o.page = OptStr(t.options, "pageSize", "a4") == "letter" ? PageSize::Letter : PageSize::A4;
    std::vector<std::wstring> w;
    if (!WriteDocx(d, tmp, o, &err, &w)) return Fail(err);
    AddWarnings(er, w);
    ctx.SetProgress(0.5);
    if (!LoViaTemp(tmp, L"docx", L"pdf", L"pdf", t.output, ctx, &err)) return Fail(err);
  }
  else return Fail(L"This output format is not supported.");
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  ctx.SetProgress(1.0);
  er.ok = true;
  return er;
}

// ============================================================================================================================
// Verification
// ============================================================================================================================
bool VerifyProduced(const fs::path& p, std::wstring* why) {
  std::wstring ext = ExtOf(p);
  auto fail = [&](const std::wstring& m) { if (why) *why = m; return false; };
  if (ext == L"docx") return VerifyDocx(p, why);
  if (ext == L"xlsx") return VerifyXlsx(p, why);
  if (ext == L"ods") return VerifyOds(p, why);
  if (ext == L"epub") return VerifyEpub(p, why);
  if (ext == L"pptx") return VerifyPptx(p, why);
  if (ext == L"pdf") {
    auto r = pdf::PdfDocument::Open(p);
    if (!r.ok()) return fail(L"the PDF does not open: " + r.message);
    if (r.doc->PageCount() < 1) return fail(L"the PDF has no pages");
    return true;
  }
  Bytes b;
  if (!ReadFileBytes(p, b, 1ull << 31, why)) return false;
  if (b.empty()) return fail(L"the output file is empty");
  if (ext == L"md" || ext == L"txt" || ext == L"html" || ext == L"srt" || ext == L"vtt" || ext == L"csv" || ext == L"tsv") {
    DecodedText dt = DecodeText(b);
    if (dt.hadErrors) return fail(L"the output is not valid text");
    if (ext == L"html" && dt.utf8.find("<html") == std::string::npos) return fail(L"the HTML output has no <html> element");
    if (ext == L"srt") { SubFile s; std::wstring e; if (!ParseSrt(dt.utf8, s, &e) || s.cues.empty()) return fail(L"the SRT output has no readable cues"); }
    if (ext == L"vtt") { SubFile s; std::wstring e; if (!ParseVtt(dt.utf8, s, &e) || s.cues.empty()) return fail(L"the WebVTT output has no readable cues"); }
    if (ext == L"csv" || ext == L"tsv") {
      Sheet sh; CsvReadOptions co; co.delimiter = ext == L"tsv" ? '\t' : 0; co.keepText = true; std::wstring e;
      if (!ParseCsv(b, co, sh, nullptr, &e)) return fail(e);
    }
  }
  return true;
}

} // namespace filo::doc
