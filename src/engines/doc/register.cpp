// Registers every document / presentation / spreadsheet / subtitle capability of the doc engine.
#include "engines/engines.h"
#include "engines/doc/convert_common.h"
#include "engines/doc/libreoffice.h"
#include "registry/capability.h"
#include "engines/image/bitmap.h"

namespace filo {

namespace {
using doc::LibreOfficeAvailable;

OptionSpec Choice(const char* id, const wchar_t* label, std::vector<std::pair<std::string, std::wstring>> c) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Choice; o.choices = std::move(c); o.def = 0; return o;
}
OptionSpec Toggle(const char* id, const wchar_t* label, bool def) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Toggle; o.min = 0; o.max = 1; o.step = 1; o.def = def ? 1 : 0; return o;
}
OptionSpec TextOpt(const char* id, const wchar_t* label) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Text; return o;
}
OptionSpec Slider(const char* id, const wchar_t* label, double mn, double mx, double st, double def, const wchar_t* unit) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Slider; o.min = mn; o.max = mx; o.step = st; o.def = def; o.unit = unit; return o;
}

std::vector<std::pair<std::string, std::wstring>> EncIn() {
  return {{"auto", L"Auto-detect"}, {"utf-8", L"UTF-8"}, {"utf-16le", L"UTF-16 LE"}, {"utf-16be", L"UTF-16 BE"}, {"windows-1252", L"Western (Windows-1252)"},
          {"windows-1256", L"Arabic (Windows-1256)"}, {"windows-1251", L"Cyrillic (Windows-1251)"}, {"windows-1250", L"Central European (Windows-1250)"}, {"windows-1255", L"Hebrew (Windows-1255)"}};
}
std::vector<std::pair<std::string, std::wstring>> EncOut(bool bomDefault) {
  std::vector<std::pair<std::string, std::wstring>> v = {{"utf-8", L"UTF-8"}, {"utf-8-bom", L"UTF-8 with BOM"}, {"utf-16le-bom", L"UTF-16 LE with BOM"}, {"windows-1252", L"Western (Windows-1252)"}, {"windows-1256", L"Arabic (Windows-1256)"}};
  if (bomDefault) std::swap(v[0], v[1]);
  return v;
}
std::vector<std::pair<std::string, std::wstring>> Delims() {
  return {{"auto", L"Auto-detect"}, {"comma", L"Comma (,)"}, {"semicolon", L"Semicolon (;)"}, {"tab", L"Tab"}, {"pipe", L"Pipe (|)"}};
}

using Opts = std::vector<OptionSpec>;

ConvertFn DocRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunDocConversion(t, c); }; }
ConvertFn SheetRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunSheetConversion(t, c); }; }
ConvertFn SheetFolderRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunSheetFolderConversion(t, c); }; }
ConvertFn SubRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunSubtitleConversion(t, c); }; }
ConvertFn PdfRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunPdfToOffice(t, c); }; }
ConvertFn PresRun() { return [](const ConvertTask& t, JobContext& c) { return doc::RunPresentationConversion(t, c); }; }
VerifyFn Ver() { return [](const fs::path& p, std::wstring* why) { return doc::VerifyProduced(p, why); }; }

struct Spec {
  const wchar_t* in; const wchar_t* out; Group group; const char* engine; bool lossy; const wchar_t* limitation;
  Opts options; bool needsLo; ConvertFn run; bool folder = false; const char* idSuffix = "";
};

void Add(Spec s) {
  Capability c;
  c.id = Narrow(s.in) + ">" + Narrow(s.out) + s.idSuffix;
  c.group = s.group; c.in = s.in; c.out = s.out; c.engine = s.engine; c.lossy = s.lossy; c.producesFolder = s.folder;
  c.limitation = s.limitation; c.options = std::move(s.options);
  if (s.needsLo) c.available = [] { return LibreOfficeAvailable(); };
  c.run = std::move(s.run);
  if (!s.folder) c.verify = Ver();
  CapabilityRegistry::Get().Add(std::move(c));
}

void Unsupported(const wchar_t* in, Group g, const wchar_t* why) {
  Capability c;
  c.id = Narrow(in) + std::string(">pages");
  c.group = g; c.in = in; c.out = L"pages"; c.engine = "none"; c.lossy = false;
  c.limitation = why;
  CapabilityRegistry::Get().Add(std::move(c));   // run == nullptr: documented, never offered
}

} // namespace

void RegisterDocEngine() {
  const Group D = Group::Document, P = Group::Presentation, S = Group::Spreadsheet, B = Group::Subtitle;
  const wchar_t* kPdfViaLo = L"PDF output is produced by LibreOffice (must be installed); layout follows LibreOffice's rendering, which can differ slightly from Word.";
  Opts pageOpt = {Choice("pageSize", L"Page size", {{"a4", L"A4"}, {"letter", L"US Letter"}})};
  Opts txtIn = {Choice("encoding", L"Text encoding", EncIn()), Toggle("reflow", L"Join wrapped lines into paragraphs", false)};
  Opts txtOut = {Choice("outputEncoding", L"Output encoding", EncOut(false)), Choice("lineEnding", L"Line endings", {{"crlf", L"Windows (CRLF)"}, {"lf", L"Unix (LF)"}})};
  Opts mdOut = {Choice("images", L"Images", {{"embed", L"Embed as data: links"}, {"omit", L"Leave out"}})};
  Opts epubOpt = {Toggle("splitChapters", L"One chapter per top-level heading", true)};

  // ---------------------------------------------------------------- DOCX
  Add({L"docx", L"md", D, "native", true, L"Headings, paragraphs, lists, tables, links, bold/italic/strike, code and images are kept; fonts, colours, page layout, headers/footers, text boxes and comments are not.", mdOut, false, DocRun()});
  Add({L"docx", L"txt", D, "native", true, L"Text only: all formatting and images are dropped, tables become tab-separated lines.", txtOut, false, DocRun()});
  Add({L"docx", L"html", D, "native", true, L"Single HTML file with embedded images; page layout, headers/footers and text boxes are not kept.", {}, false, DocRun()});
  Add({L"docx", L"epub", D, "native", true, L"Reflowable EPUB 3 split on top-level headings; page layout, headers/footers and text boxes are not kept.", epubOpt, false, DocRun()});
  Add({L"docx", L"pdf", D, "libreoffice", false, kPdfViaLo, {}, true, DocRun()});
  Unsupported(L"docx", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- Markdown
  Add({L"md", L"docx", D, "native", false, L"CommonMark + GitHub tables; raw HTML is converted when simple. Remote images are not downloaded.", pageOpt, false, DocRun()});
  Add({L"md", L"txt", D, "native", true, L"Markup is removed; tables become tab-separated lines.", txtOut, false, DocRun()});
  Add({L"md", L"html", D, "native", false, L"Single HTML file with a default style sheet; local images are embedded.", {}, false, DocRun()});
  Add({L"md", L"epub", D, "native", false, L"Reflowable EPUB 3 split on top-level headings.", epubOpt, false, DocRun()});
  Add({L"md", L"pdf", D, "libreoffice", false, L"Rendered through a temporary DOCX and LibreOffice (must be installed).", pageOpt, true, DocRun()});
  Unsupported(L"md", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- plain text
  Add({L"txt", L"docx", D, "native", false, L"Paragraphs are split on blank lines; lines starting with -, * or 1. become lists. Encoding is detected (UTF-8/16, BOM, Windows-1256/1252).", Opts{txtIn[0], txtIn[1], pageOpt[0]}, false, DocRun()});
  Add({L"txt", L"md", D, "native", false, L"Special Markdown characters are escaped; underlined titles (=== / ---) become headings.", txtIn, false, DocRun()});
  Add({L"txt", L"html", D, "native", false, L"Paragraphs and lists become HTML; encoding is detected.", txtIn, false, DocRun()});
  Add({L"txt", L"epub", D, "native", false, L"Single-chapter EPUB 3 unless the text has underlined top-level titles.", Opts{txtIn[0], txtIn[1]}, false, DocRun()});
  Add({L"txt", L"pdf", D, "libreoffice", false, L"Rendered through a temporary DOCX and LibreOffice (must be installed).", Opts{txtIn[0], txtIn[1], pageOpt[0]}, true, DocRun()});
  Unsupported(L"txt", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- HTML
  Add({L"html", L"docx", D, "native", true, L"Static HTML only: scripts and CSS layout are ignored, remote images are not downloaded.", pageOpt, false, DocRun()});
  Add({L"html", L"md", D, "native", true, L"Static HTML only: scripts and CSS layout are ignored, remote images are not downloaded.", mdOut, false, DocRun()});
  Add({L"html", L"txt", D, "native", true, L"Text only.", txtOut, false, DocRun()});
  Add({L"html", L"epub", D, "native", true, L"Static HTML only: scripts and CSS layout are ignored.", epubOpt, false, DocRun()});
  Add({L"html", L"pdf", D, "libreoffice", true, L"Static HTML rendered through a temporary DOCX and LibreOffice (must be installed); CSS layout is not reproduced.", pageOpt, true, DocRun()});
  Unsupported(L"html", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- EPUB
  Add({L"epub", L"docx", D, "native", true, L"Chapters in spine order; CSS styling, covers and fixed-layout books are not kept. DRM-protected books cannot be read.", pageOpt, false, DocRun()});
  Add({L"epub", L"txt", D, "native", true, L"Text only, chapters in spine order.", txtOut, false, DocRun()});
  Add({L"epub", L"md", D, "native", true, L"Chapters in spine order; CSS styling and covers are not kept.", mdOut, false, DocRun()});
  Add({L"epub", L"html", D, "native", true, L"All chapters merged into one HTML file.", {}, false, DocRun()});
  Add({L"epub", L"pdf", D, "libreoffice", true, L"Rendered through a temporary DOCX and LibreOffice (must be installed); CSS styling is not kept.", pageOpt, true, DocRun()});
  Unsupported(L"epub", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- RTF / RTFD
  Add({L"rtf", L"docx", D, "native", true, L"Own RTF parser: text, styles, lists, simple tables, links and PNG/JPEG pictures are kept; EMF/WMF pictures, text frames, headers/footers and fields other than hyperlinks are not.", pageOpt, false, DocRun()});
  Add({L"rtf", L"txt", D, "native", true, L"Text only.", txtOut, false, DocRun()});
  Add({L"rtf", L"md", D, "native", true, L"Headings, lists, tables, links and emphasis are kept; page layout and fonts are not.", mdOut, false, DocRun()});
  Add({L"rtf", L"html", D, "native", true, L"Headings, lists, tables, links and emphasis are kept; page layout is not.", {}, false, DocRun()});
  Add({L"rtf", L"epub", D, "native", true, L"Headings, lists, tables, links and emphasis are kept; page layout is not.", epubOpt, false, DocRun()});
  Add({L"rtf", L"pdf", D, "libreoffice", false, kPdfViaLo, {}, true, DocRun()});
  Unsupported(L"rtf", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");
  Add({L"rtfd", L"docx", D, "native", true, L"RTFD bundle (a folder with TXT.rtf and attachments): text, styles, lists and image attachments are kept.", pageOpt, false, DocRun()});
  Add({L"rtfd", L"txt", D, "native", true, L"Text only.", txtOut, false, DocRun()});
  Add({L"rtfd", L"md", D, "native", true, L"Headings, lists, tables, links and emphasis are kept; images are embedded as data: links.", mdOut, false, DocRun()});

  // ---------------------------------------------------------------- legacy .doc (LibreOffice)
  Add({L"doc", L"docx", D, "libreoffice", false, L"Legacy Word 97-2003 files are read by LibreOffice (must be installed); complex layouts can shift slightly.", {}, true, DocRun()});
  Add({L"doc", L"pdf", D, "libreoffice", false, kPdfViaLo, {}, true, DocRun()});
  Add({L"doc", L"txt", D, "libreoffice", true, L"Read by LibreOffice (must be installed); text only.", txtOut, true, DocRun()});
  Unsupported(L"doc", D, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");

  // ---------------------------------------------------------------- macOS text clipping / Pages
  Add({L"textclipping", L"txt", D, "native", false, L"XML and binary property-list clippings are read. Old clippings that keep their text only in a macOS resource fork cannot exist on Windows file systems and are reported as unreadable.", txtOut, false, DocRun()});
  Add({L"textclipping", L"md", D, "native", false, L"Same limitation as .textClipping > TXT.", mdOut, false, DocRun()});
  Add({L"textclipping", L"docx", D, "native", false, L"Same limitation as .textClipping > TXT.", pageOpt, false, DocRun()});
  {
    Capability c;
    c.id = "pages>pdf"; c.group = D; c.in = L"pages"; c.out = L"pdf"; c.engine = "native"; c.lossy = true;
    c.limitation = L"Pages files are Apple's proprietary IWA container and are NOT parsed. Only the PDF preview that Pages embeds (QuickLook/Preview.pdf) is extracted; it is a snapshot made when the file was saved and some files carry only a small JPEG instead (those fail).";
    c.run = [](const ConvertTask& t, JobContext& ctx) { return doc::RunPagesToPdf(t, ctx); };
    c.verify = Ver();
    CapabilityRegistry::Get().Add(std::move(c));
  }

  // ---------------------------------------------------------------- presentations
  Opts slideOpt = {Slider("dpi", L"Resolution", 72, 300, 6, 150, L"dpi")};
  Opts slideJpg = {Slider("dpi", L"Resolution", 72, 300, 6, 150, L"dpi"), Slider("quality", L"JPEG quality", 40, 100, 1, 90, L"")};
  Add({L"pptx", L"pdf", P, "libreoffice", false, L"Rendered by LibreOffice (must be installed); fonts that are missing on this PC are substituted and some animations/transitions are dropped.", {}, true, PresRun()});
  Add({L"ppt", L"pdf", P, "libreoffice", false, L"Legacy PowerPoint rendered by LibreOffice (must be installed); fonts that are missing on this PC are substituted.", {}, true, PresRun()});
  Add({L"pptx", L"png", P, "libreoffice", true, L"One PNG per slide (slide-001.png...) rendered from the LibreOffice PDF at the chosen resolution; aspect ratio is preserved. LibreOffice must be installed.", slideOpt, true, PresRun(), true});
  Add({L"pptx", L"jpg", P, "libreoffice", true, L"One JPG per slide (slide-001.jpg...) rendered from the LibreOffice PDF; slides are flattened on white. LibreOffice must be installed.", slideJpg, true, PresRun(), true});
  Add({L"ppt", L"png", P, "libreoffice", true, L"One PNG per slide rendered through LibreOffice (must be installed).", slideOpt, true, PresRun(), true});
  Add({L"ppt", L"jpg", P, "libreoffice", true, L"One JPG per slide rendered through LibreOffice (must be installed).", slideJpg, true, PresRun(), true});

  // ---------------------------------------------------------------- spreadsheets
  Opts csvIn = {Choice("delimiter", L"Delimiter", Delims()), Choice("encoding", L"Text encoding", EncIn())};
  Opts csvToXl = {Choice("delimiter", L"Delimiter", Delims()), Choice("encoding", L"Text encoding", EncIn()),
                  Toggle("inferTypes", L"Convert numbers with leading zeros, long digit strings and ambiguous dates", false),
                  Choice("dateOrder", L"Ambiguous dates are", {{"dmy", L"day/month/year"}, {"mdy", L"month/day/year"}})};
  Opts toCsv = {TextOpt("sheet", L"Sheet (name or number; empty = first)"), Choice("outputDelimiter", L"Delimiter", {{"comma", L"Comma (,)"}, {"semicolon", L"Semicolon (;)"}, {"tab", L"Tab"}}),
                Choice("outputEncoding", L"Output encoding", EncOut(true))};
  Opts toTsv = {TextOpt("sheet", L"Sheet (name or number; empty = first)"), Choice("outputEncoding", L"Output encoding", EncOut(false))};
  Opts csvToTsv = {csvIn[0], csvIn[1], Choice("outputEncoding", L"Output encoding", EncOut(false))};
  Opts tsvToCsv = {csvIn[0], csvIn[1], Choice("outputDelimiter", L"Delimiter", {{"comma", L"Comma (,)"}, {"semicolon", L"Semicolon (;)"}}), Choice("outputEncoding", L"Output encoding", EncOut(true))};
  const wchar_t* kGuard = L"Numbers are only recognised when they are safe (no leading zeros, fewer than 10 integer digits); ISO dates become dates; everything else stays text so phone numbers, ids and ambiguous dates are never altered. Enable the option to convert those too.";
  Add({L"csv", L"tsv", S, "native", false, L"Pure delimiter conversion: values are kept as text exactly as written; quoted fields with line breaks are preserved. Delimiter and encoding are detected.", csvToTsv, false, SheetRun()});
  Add({L"tsv", L"csv", S, "native", false, L"Pure delimiter conversion: values are kept as text exactly as written; fields are quoted when needed.", tsvToCsv, false, SheetRun()});
  Add({L"csv", L"xlsx", S, "native", false, kGuard, csvToXl, false, SheetRun()});
  Add({L"tsv", L"xlsx", S, "native", false, kGuard, csvToXl, false, SheetRun()});
  Add({L"csv", L"ods", S, "native", false, kGuard, csvToXl, false, SheetRun()});
  Add({L"tsv", L"ods", S, "native", false, kGuard, csvToXl, false, SheetRun()});
  Add({L"csv", L"pdf", S, "libreoffice", false, L"Table is placed in a workbook and printed by LibreOffice (must be installed); wide tables are split across pages.", csvToXl, true, SheetRun()});
  Add({L"tsv", L"pdf", S, "libreoffice", false, L"Table is placed in a workbook and printed by LibreOffice (must be installed); wide tables are split across pages.", csvToXl, true, SheetRun()});

  const wchar_t* kXlsxOut = L"Cell values only: formulas are replaced by their last calculated value, number formats are not applied (dates are written as ISO dates), formatting and other sheets are not exported.";
  Add({L"xlsx", L"csv", S, "native", true, kXlsxOut, toCsv, false, SheetRun()});
  Add({L"xlsx", L"tsv", S, "native", true, kXlsxOut, toTsv, false, SheetRun()});
  Add({L"ods", L"csv", S, "native", true, kXlsxOut, toCsv, false, SheetRun()});
  Add({L"ods", L"tsv", S, "native", true, kXlsxOut, toTsv, false, SheetRun()});
  Add({L"xlsx", L"ods", S, "native", true, L"Values, text, dates and sheet names are kept; formulas become values and cell formatting, charts and images are not carried over.", {}, false, SheetRun()});
  Add({L"ods", L"xlsx", S, "native", true, L"Values, text, dates and sheet names are kept; formulas become values and cell formatting, charts and images are not carried over.", {}, false, SheetRun()});
  Add({L"xlsx", L"pdf", S, "libreoffice", false, L"Printed by LibreOffice (must be installed) using the workbook's own page setup.", {}, true, SheetRun()});
  Add({L"ods", L"pdf", S, "libreoffice", false, L"Printed by LibreOffice (must be installed) using the document's own page setup.", {}, true, SheetRun()});
  Add({L"xls", L"xlsx", S, "libreoffice", false, L"Legacy Excel 97-2003 read by LibreOffice (must be installed); macros and some formatting are not kept.", {}, true, SheetRun()});
  Add({L"xls", L"ods", S, "libreoffice", false, L"Legacy Excel read by LibreOffice, then written as ODS (must be installed); formulas become values.", {}, true, SheetRun()});
  Add({L"xls", L"csv", S, "libreoffice", true, kXlsxOut, toCsv, true, SheetRun()});
  Add({L"xls", L"pdf", S, "libreoffice", false, L"Printed by LibreOffice (must be installed).", {}, true, SheetRun()});
  // all-sheets variants: one CSV/TSV per sheet in a folder (a distinct capability id because the output is a folder)
  Opts allCsv = {Choice("outputDelimiter", L"Delimiter", {{"comma", L"Comma (,)"}, {"semicolon", L"Semicolon (;)"}, {"tab", L"Tab"}}), Choice("outputEncoding", L"Output encoding", EncOut(true))};
  Opts allTsv = {Choice("outputEncoding", L"Output encoding", EncOut(false))};
  for (auto* in : {L"xlsx", L"ods"}) {
    Add({in, L"csv", S, "native", true, L"All sheets: writes one CSV per sheet into a folder. Values only (see the single-sheet export for details).", allCsv, false, SheetFolderRun(), true, "#sheets"});
    Add({in, L"tsv", S, "native", true, L"All sheets: writes one TSV per sheet into a folder. Values only.", allTsv, false, SheetFolderRun(), true, "#sheets"});
  }
  Add({L"xls", L"csv", S, "libreoffice", true, L"All sheets: writes one CSV per sheet into a folder (read by LibreOffice, must be installed). Values only.", allCsv, true, SheetFolderRun(), true, "#sheets"});
  Add({L"xls", L"tsv", S, "libreoffice", true, L"All sheets: writes one TSV per sheet into a folder (read by LibreOffice, must be installed). Values only.", allTsv, true, SheetFolderRun(), true, "#sheets"});

  // ---------------------------------------------------------------- subtitles
  Opts subIn = {Choice("encoding", L"Text encoding", EncIn())};
  Opts subToSrt = {subIn[0], Choice("outputEncoding", L"Output encoding", EncOut(true))};
  Opts subToVtt = {subIn[0], Choice("outputEncoding", L"Output encoding", EncOut(false))};
  Opts subToTxt = {subIn[0], Toggle("timestamps", L"Keep timestamps", false), Choice("outputEncoding", L"Output encoding", EncOut(false)), Choice("lineEnding", L"Line endings", {{"crlf", L"Windows (CRLF)"}, {"lf", L"Unix (LF)"}})};
  Add({L"srt", L"vtt", B, "native", false, L"Timing is kept to the millisecond. Only <i>, <b> and <u> styling is kept; font colours/positions are stripped with a warning.", subToVtt, false, SubRun()});
  Add({L"vtt", L"srt", B, "native", true, L"Timing is kept to the millisecond. Cue settings (position, alignment), voices, classes, STYLE/REGION blocks are not representable in SRT and are removed with a warning.", subToSrt, false, SubRun()});
  Add({L"srt", L"txt", B, "native", true, L"Text only (optionally with timestamps).", subToTxt, false, SubRun()});
  Add({L"vtt", L"txt", B, "native", true, L"Text only (optionally with timestamps).", subToTxt, false, SubRun()});

  // ---------------------------------------------------------------- image -> DOCX (the reference image wheel offers DOCX)
  for (const wchar_t* in : {L"jpg", L"png", L"webp", L"heic", L"tiff", L"bmp", L"gif", L"avif"}) {
    Capability c;
    c.id = Narrow(in) + ">docx"; c.group = Group::Image; c.in = in; c.out = L"docx"; c.engine = "native"; c.lossy = false;
    c.limitation = L"One page with the picture scaled to fit; text in the picture is not recognised. Animated pictures use their first frame.";
    const std::wstring fmt = in;
    c.available = [fmt] { return img::CanDecode(fmt); };
    c.run = [](const ConvertTask& t, JobContext& ctx) { return doc::RunImageToDocx(t, ctx); };
    c.verify = Ver();
    CapabilityRegistry::Get().Add(std::move(c));
  }

  // ---------------------------------------------------------------- PDF -> Office (best-effort layout reconstruction)
  {
    const wchar_t* kBest = L"BEST-EFFORT layout: a PDF stores glyphs at coordinates, so paragraphs, lists, tables and columns are reconstructed heuristically and WILL differ from the original on complex pages. Repeating headers/footers are removed. Scanned pages use Windows OCR when a language pack is installed (otherwise they stay pictures).";
    Opts ocrOpt = {Choice("ocr", L"Scanned pages", {{"auto", L"Recognise text (OCR) when available"}, {"off", L"Keep as pictures"}})};
    Opts docxOpts = {Choice("pageSize", L"Page size", {{"auto", L"Same as the PDF"}, {"a4", L"A4"}, {"letter", L"US Letter"}}), ocrOpt[0], Toggle("keepHeadersFooters", L"Keep repeating headers and footers", false)};
    Opts epubOpts = {ocrOpt[0], Toggle("keepHeadersFooters", L"Keep repeating headers and footers", false)};
    Add({L"pdf", L"docx", Group::Pdf, "native", true, kBest, docxOpts, false, PdfRun()});
    Add({L"pdf", L"epub", Group::Pdf, "native", true, kBest, epubOpts, false, PdfRun()});
    Add({L"pdf", L"xlsx", Group::Pdf, "native", true, L"BEST-EFFORT: tables are detected from ruling lines and aligned text and written one sheet per table (tables continuing over pages are merged); without tables the text lines go to one sheet. Text outside tables is not exported; numbers stay numbers, leading zeros stay text.", ocrOpt, false, PdfRun()});
    Add({L"pdf", L"pptx", Group::Pdf, "native", true, L"BEST-EFFORT: one slide per page with the largest text as title, text boxes and pictures at their PDF coordinates (not a screenshot). Table borders, vector graphics and charts are not reproduced; text boxes may wrap differently in PowerPoint.", ocrOpt, false, PdfRun()});
  }

  Unsupported(L"pdf", Group::Pdf, L"Pages is an Apple proprietary format; DOCX opens directly in Pages.");
}

} // namespace filo
