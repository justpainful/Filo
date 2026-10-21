// PDF engine registration: pdf>jpg, pdf>png (page rasterisation), pdf>txt, pdf>md (reading-order text, OCR for scanned pages).
// pdf>docx/pptx/xlsx/epub belong to the document engine (it consumes pdf_text.h); pdf>pages is intentionally not registered.
#include "engines/engines.h"
#include "engines/pdf/pdf_doc.h"
#include "engines/pdf/pdf_internal.h"
#include "engines/pdf/pdf_markdown.h"
#include "engines/pdf/pdf_ocr.h"
#include "engines/pdf/pdf_ops.h"
#include "engines/pdf/pdf_text.h"
#include "engines/pdf/pdf_wic.h"
#include "registry/capability.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>

namespace filo {
namespace {
using namespace pdf;

// ---- options ----------------------------------------------------------------------------------------------------------
bool OptBool(const Json& o, const char* k, bool def) {
  if (!o.Has(k)) return def;
  const Json& v = o.Get(k);
  if (v.IsBool()) return v.AsBool();
  if (v.IsNumber()) return v.AsNumber() != 0;
  if (v.IsString()) return v.AsString() == "true" || v.AsString() == "1" || v.AsString() == "on";
  return def;
}
double OptNum(const Json& o, const char* k, double def) {
  if (!o.Has(k)) return def;
  const Json& v = o.Get(k);
  if (v.IsNumber()) return v.AsNumber(def);
  if (v.IsString()) { try { return std::stod(v.AsString()); } catch (...) {} }
  return def;
}
std::wstring OptStr(const Json& o, const char* k, const std::wstring& def = L"") {
  if (!o.Has(k)) return def;
  const Json& v = o.Get(k);
  return v.IsString() ? Widen(v.AsString()) : def;
}

struct Opened { std::unique_ptr<PdfDocument> doc; std::vector<int> pages; };

// Opens the task's PDF and resolves the optional "pages" selection.
bool OpenTask(const ConvertTask& t, Opened& o, std::wstring& err) {
  auto r = PdfDocument::Open(t.input, OptStr(t.options, "password"));
  if (!r.ok()) { err = r.message; return false; }
  o.doc = std::move(r.doc);
  int n = o.doc->PageCount();
  std::wstring sel = OptStr(t.options, "pages");
  if (Trim(sel).empty()) { for (int i = 0; i < n; ++i) o.pages.push_back(i); return true; }
  std::vector<std::pair<int, int>> ranges;
  if (!ParsePageRanges(sel, n, ranges, &err)) return false;
  for (auto& rg : ranges) for (int i = rg.first; i <= rg.second; ++i) o.pages.push_back(i);
  return true;
}

std::wstring PageName(int page1, int total, const wchar_t* ext) {
  int w = std::max<int>(3, (int)std::to_wstring(total).size());
  wchar_t b[64]; swprintf_s(b, L"page-%0*d.%s", w, page1, ext);
  return b;
}

// ---- pdf > jpg / png ---------------------------------------------------------------------------------------------------
EngineResult RunPageImages(const ConvertTask& t, JobContext& ctx, bool jpg) {
  wic::ComScope com;
  Opened o; std::wstring err;
  if (!OpenTask(t, o, err)) return EngineResult::Fail(err);
  const double dpi = std::clamp(OptNum(t.options, "dpi", 300), 10.0, 1200.0);
  const int quality = (int)std::clamp(OptNum(t.options, "quality", 90), 1.0, 100.0);
  RenderOptions ro;
  ro.dpi = dpi;
  ro.annotations = OptBool(t.options, "annotations", true);
  ro.forms = ro.annotations;
  ro.grayscale = OptBool(t.options, "grayscale", false);
  ro.transparent = !jpg && OptBool(t.options, "transparent", false);
  ro.maxPixels = 150e6;
  EngineResult res = EngineResult::Ok();
  const int total = o.doc->PageCount();
  std::future<std::wstring> pending;       // encode of the previous page runs while the next one renders
  auto waitPending = [&]() -> std::wstring { if (pending.valid()) return pending.get(); return {}; };
  std::vector<int> clamped;
  for (size_t k = 0; k < o.pages.size(); ++k) {
    if (ctx.Cancelled()) { waitPending(); return EngineResult::Fail(L"Cancelled"); }
    int idx = o.pages[k];
    auto bmp = std::make_shared<Bitmap>();
    bool wasClamped = false;
    std::wstring rerr;
    if (!o.doc->RenderPage(idx, ro, *bmp, &rerr, &wasClamped)) { waitPending(); return EngineResult::Fail(L"Page " + std::to_wstring(idx + 1) + L" could not be rendered: " + rerr); }
    if (wasClamped) clamped.push_back(idx + 1);
    std::wstring e = waitPending();
    if (!e.empty()) return EngineResult::Fail(e);
    fs::path file = t.output / PageName(idx + 1, total, jpg ? L"jpg" : L"png");
    res.extraOutputs.push_back(file);
    bool transparent = ro.transparent;
    pending = std::async(std::launch::async, [bmp, file, jpg, quality, dpi, transparent]() -> std::wstring {
      wic::ComScope c;
      std::wstring err;
      bool ok = jpg ? wic::EncodeJpeg(file, bmp->bgra.data(), bmp->width, bmp->height, quality, dpi, &err)
                    : wic::EncodePng(file, bmp->bgra.data(), bmp->width, bmp->height, transparent, dpi, &err);
      return ok ? std::wstring() : (err.empty() ? L"An image could not be written." : err);
    });
    ctx.SetProgress((k + 1.0) / o.pages.size() * 0.98);
  }
  std::wstring e = waitPending();
  if (!e.empty()) return EngineResult::Fail(e);
  if (!clamped.empty()) {
    std::wstring l = L"Very large page(s) were rendered at a lower resolution to stay within memory limits: ";
    for (size_t i = 0; i < clamped.size() && i < 10; ++i) { if (i) l += L", "; l += std::to_wstring(clamped[i]); }
    res.warnings.push_back(l);
  }
  if (jpg) res.warnings.push_back(L"JPG has no transparency; pages were flattened onto white.");
  return res;
}

// ---- OCR plumbing -----------------------------------------------------------------------------------------------------
struct OcrState {
  bool enabled = true;
  std::wstring lang = L"auto";
  bool unavailable = false;
  std::wstring unavailableMsg;
  std::wstring used;
  std::vector<int> okPages, emptyPages, failedPages, skippedPages;
  std::wstring lastError;
};

bool NeedsOcr(const PageContent& pc) { return pc.looksScanned || pc.textUnreliable; }

bool OcrPage(PdfDocument& doc, int index, OcrState& st, PageContent& pc) {
  RenderOptions ro; ro.dpi = 300; ro.annotations = true; ro.forms = false; ro.maxPixels = 60e6;
  Bitmap bmp; std::wstring err;
  if (!doc.RenderPage(index, ro, bmp, &err)) { st.failedPages.push_back(index + 1); st.lastError = err; return false; }
  OcrResult r;
  if (!OcrRecognize(bmp.bgra.data(), bmp.width, bmp.height, st.lang, r, &err)) {
    OcrInfo info = QueryOcr();
    if (!info.available) { st.unavailable = true; st.unavailableMsg = info.message; }
    else { st.failedPages.push_back(index + 1); st.lastError = err; }
    return false;
  }
  if (st.used.empty()) { st.used = r.language; if (st.lang == L"auto") st.lang = r.language; }   // decide once, then reuse
  double toPt = bmp.width > 0 ? pc.width / (double)bmp.width : 72.0 / 300.0;
  std::vector<float> hs;
  for (auto& l : r.lines) hs.push_back(l.h);
  float medH = 10;
  if (!hs.empty()) { std::sort(hs.begin(), hs.end()); medH = hs[hs.size() / 2]; }
  float fs = (float)(medH * toPt * 1.15);
  pc.runs.clear();
  for (auto& l : r.lines) {
    TextRun run;
    run.text = l.text;
    run.x = (float)(l.x * toPt); run.w = (float)(l.w * toPt);
    float cy = (float)((l.y + l.h * 0.5) * toPt);
    run.h = fs * 1.15f; run.y = cy - run.h * 0.5f;
    run.fontSize = fs; run.baseline = run.y + run.h * 0.8f;
    run.rtl = detail::IsRtlText(l.text);
    run.fontName = L"OCR";
    pc.runs.push_back(std::move(run));
  }
  detail::ArrangeForeignRuns(pc);
  pc.hasText = r.chars > 0;
  pc.looksScanned = false;
  pc.images.clear();
  (r.chars > 0 ? st.okPages : st.emptyPages).push_back(index + 1);
  return true;
}

std::wstring PageList(const std::vector<int>& v) {
  std::wstring s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i == 6) { s += L", ..."; break; }
    if (i) s += L", ";
    s += std::to_wstring(v[i]);
  }
  return s;
}

// Extracts one page including the OCR fallback. Returns false if cancelled.
void ExtractWithOcr(PdfDocument& doc, int idx, PageContent& pc, OcrState& st) {
  if (!ExtractPage(doc, idx, pc, false)) {
    pc = PageContent(); pc.index = idx;
    doc.PageSize(idx, &pc.width, &pc.height);
    return;
  }
  if (NeedsOcr(pc)) {
    if (!st.enabled) st.skippedPages.push_back(idx + 1);
    else if (st.unavailable) st.skippedPages.push_back(idx + 1);
    else if (!OcrPage(doc, idx, st, pc)) st.skippedPages.push_back(idx + 1);
  }
}

void OcrWarnings(const OcrState& st, EngineResult& res) {
  if (!st.okPages.empty()) res.warnings.push_back(L"Pages " + PageList(st.okPages) + L" are scans and were read with OCR (" + st.used + L"); recognition may contain errors.");
  if (!st.emptyPages.empty()) res.warnings.push_back(L"No text was recognised on page(s) " + PageList(st.emptyPages) + L".");
  if (!st.failedPages.empty()) res.warnings.push_back(L"Text recognition failed on page(s) " + PageList(st.failedPages) + L".");
  if (!st.skippedPages.empty()) {
    if (!st.enabled) res.warnings.push_back(L"Page(s) " + PageList(st.skippedPages) + L" look scanned and have no text layer; OCR is turned off, so they are empty in the result.");
    else if (st.unavailable) res.warnings.push_back(L"Page(s) " + PageList(st.skippedPages) + L" look scanned but " + st.unavailableMsg);
    else res.warnings.push_back(L"Page(s) " + PageList(st.skippedPages) + L" have no readable text.");
  }
}

std::wstring NoTextMessage(const OcrState& st) {
  if (!st.skippedPages.empty() && !st.enabled) return L"This PDF has no text layer (it looks scanned). Turn OCR on to read it.";
  if (!st.skippedPages.empty() && st.unavailable) return L"This PDF is scanned and no text recognition (OCR) language is installed. Add one in Windows Settings > Time & language > Language & region, then try again.";
  if (!st.failedPages.empty()) return st.lastError.empty() ? std::wstring(L"Text recognition failed for this scanned PDF.") : L"Text recognition failed: " + st.lastError;
  return L"No text could be found in this PDF (it may contain only drawings or pictures).";
}

OcrState OcrFromOptions(const ConvertTask& t) {
  OcrState st;
  st.enabled = OptStr(t.options, "ocr", L"auto") != L"off";
  std::wstring l = OptStr(t.options, "ocrLanguage", L"auto");
  st.lang = l.empty() ? L"auto" : l;
  return st;
}

bool WriteUtf8(const fs::path& file, const std::wstring& text, bool bom, std::wstring& err) {
  HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { err = L"The output file could not be created: " + LastErrorMessage(); return false; }
  std::string data;
  if (bom) data = "\xEF\xBB\xBF";
  data += Narrow(text);
  size_t off = 0; bool ok = true;
  while (off < data.size()) {
    DWORD wrote = 0;
    if (!WriteFile(h, data.data() + off, (DWORD)std::min<size_t>(data.size() - off, 1u << 24), &wrote, nullptr) || !wrote) { ok = false; break; }
    off += wrote;
  }
  DWORD le = GetLastError();
  CloseHandle(h);
  if (!ok) err = L"The output file could not be written: " + LastErrorMessage(le);
  return ok;
}

std::wstring ToCrLf(const std::wstring& s) {
  std::wstring o; o.reserve(s.size() + s.size() / 40);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) o += L'\r';
    o += s[i];
  }
  return o;
}

// ---- pdf > txt ---------------------------------------------------------------------------------------------------------
// Pages are separated by a line holding a single form feed (U+000C) - the convention of pdftotext - or, with
// pageBreak=blank, by two blank lines. Lines end with CRLF; the file is UTF-8 with a BOM.
EngineResult RunText(const ConvertTask& t, JobContext& ctx) {
  Opened o; std::wstring err;
  if (!OpenTask(t, o, err)) return EngineResult::Fail(err);
  OcrState ocr = OcrFromOptions(t);
  bool blank = OptStr(t.options, "pageBreak", L"formfeed") == L"blank";
  std::wstring out;
  bool anyText = false;
  for (size_t k = 0; k < o.pages.size(); ++k) {
    if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
    PageContent pc;
    ExtractWithOcr(*o.doc, o.pages[k], pc, ocr);
    std::wstring tx = PageToPlainText(pc);
    if (pc.hasText) anyText = true;
    if (k) out += blank ? L"\n\n\n" : L"\n\f\n";
    out += tx;
    ctx.SetProgress((k + 1.0) / o.pages.size() * 0.95);
  }
  if (!anyText) return EngineResult::Fail(NoTextMessage(ocr));
  EngineResult res = EngineResult::Ok();
  OcrWarnings(ocr, res);
  if (!WriteUtf8(t.output, ToCrLf(out), true, err)) return EngineResult::Fail(err);
  return res;
}

// ---- pdf > md ----------------------------------------------------------------------------------------------------------
EngineResult RunMarkdown(const ConvertTask& t, JobContext& ctx) {
  wic::ComScope com;
  Opened o; std::wstring err;
  if (!OpenTask(t, o, err)) return EngineResult::Fail(err);
  OcrState ocr = OcrFromOptions(t);
  bool exportImages = OptBool(t.options, "images", true);
  MarkdownWriter::Options mo; mo.pageBreaks = OptBool(t.options, "pageBreaks", true);
  MarkdownWriter writer(mo);
  std::vector<PageContent> pcs;
  pcs.reserve(o.pages.size());
  bool anyText = false;
  for (size_t k = 0; k < o.pages.size(); ++k) {
    if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
    PageContent pc;
    ExtractWithOcr(*o.doc, o.pages[k], pc, ocr);
    if (pc.hasText) anyText = true;
    writer.Analyze(pc);
    pcs.push_back(std::move(pc));
    ctx.SetProgress((k + 1.0) / o.pages.size() * 0.5);
  }
  writer.Prepare();
  fs::path imagesDir = t.output / L"images";
  int imagesWritten = 0;
  std::wstring md;
  EngineResult res = EngineResult::Ok();
  for (size_t k = 0; k < pcs.size(); ++k) {
    if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
    PageContent& pc = pcs[k];
    if (exportImages && !pc.images.empty() && !pc.looksScanned) {
      PageContent full;
      // the lightweight first pass only kept image geometry: decode pixels now, and only for pages that have images
      if (ExtractPage(*o.doc, pc.index, full, true) && !full.images.empty()) pc.images = std::move(full.images);
    }
    auto saver = [&](int idx, const ImageBlock& im) -> std::wstring {
      if (im.bgra.empty() && im.rawJpeg.empty()) return L"";
      std::error_code ec; fs::create_directories(imagesDir, ec);
      bool jpeg = !im.rawJpeg.empty();
      wchar_t nm[96]; swprintf_s(nm, L"page-%03d-%d.%s", pc.index + 1, idx + 1, jpeg ? L"jpg" : L"png");
      fs::path f = imagesDir / nm;
      std::wstring e;
      if (jpeg) {
        std::ofstream of(f, std::ios::binary);
        of.write((const char*)im.rawJpeg.data(), (std::streamsize)im.rawJpeg.size());
        if (!of) return L"";
      } else if (!wic::EncodePng(f, im.bgra.data(), im.pixelW, im.pixelH, im.hasAlpha, 0, &e)) return L"";
      ++imagesWritten;
      return L"images/" + std::wstring(nm);
    };
    md += writer.RenderPage(pc, saver);
    pc.images.clear(); pc.images.shrink_to_fit();
    ctx.SetProgress(0.5 + (k + 1.0) / pcs.size() * 0.48);
  }
  if (!anyText && imagesWritten == 0) return EngineResult::Fail(NoTextMessage(ocr));
  OcrWarnings(ocr, res);
  if (md.empty() || md.back() != L'\n') md += L'\n';
  fs::path mdFile = t.output / (t.input.stem().wstring() + L".md");
  if (!WriteUtf8(mdFile, ToCrLf(md), false, err)) return EngineResult::Fail(err);
  res.extraOutputs.push_back(mdFile);
  return res;
}

// ---- verification ------------------------------------------------------------------------------------------------------
bool VerifyTextFile(const fs::path& p, std::wstring* why) {
  std::ifstream f(p, std::ios::binary);
  std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 4 || (uint8_t)d[0] != 0xEF || (uint8_t)d[1] != 0xBB || (uint8_t)d[2] != 0xBF) { if (why) *why = L"The text file is not UTF-8."; return false; }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d.data() + 3, (int)d.size() - 3, nullptr, 0);
  if (n <= 0) { if (why) *why = L"The text file is not valid UTF-8."; return false; }
  return true;
}

bool VerifyImageFolder(const fs::path& dir, const wchar_t* ext, std::wstring* why) {
  std::error_code ec;
  fs::path first;
  for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ext && (first.empty() || e.path() < first)) first = e.path();
  if (first.empty()) { if (why) *why = L"No page images were produced."; return false; }
  wic::ComScope com;
  wic::Decoded d; std::wstring err;
  if (!wic::Probe(first, d, &err)) { if (why) *why = L"A page image could not be opened."; return false; }
  return true;
}

} // namespace

void RegisterPdfEngine() {
  using K = OptionSpec::Kind;
  auto opt = [](const char* id, const wchar_t* label, K kind, double mn, double mx, double step, double def, const wchar_t* unit = L"") {
    OptionSpec o; o.id = id; o.label = label; o.kind = kind; o.min = mn; o.max = mx; o.step = step; o.def = def; o.unit = unit; return o;
  };
  auto choice = [](const char* id, const wchar_t* label, std::vector<std::pair<std::string, std::wstring>> ch) {
    OptionSpec o; o.id = id; o.label = label; o.kind = K::Choice; o.choices = std::move(ch); return o;
  };
  auto text = [](const char* id, const wchar_t* label) { OptionSpec o; o.id = id; o.label = label; o.kind = K::Text; return o; };

  std::vector<std::pair<std::string, std::wstring>> ocrLangs{{"auto", L"Automatic"}};
  try {
    OcrInfo info = QueryOcr();
    for (auto& l : info.languages) ocrLangs.push_back({Narrow(l), l});
  } catch (...) {}

  std::vector<OptionSpec> common = {text("pages", L"Pages (e.g. 1-3,5)"), text("password", L"Password")};

  auto imageCap = [&](bool jpg) {
    Capability c;
    c.id = jpg ? "pdf>jpg" : "pdf>png";
    c.group = Group::Pdf; c.in = L"pdf"; c.out = jpg ? L"jpg" : L"png";
    c.engine = "pdfium"; c.lossy = true; c.producesFolder = true;
    c.limitation = jpg ? L"Pages are rasterised (text is no longer selectable) and flattened onto white."
                       : L"Pages are rasterised: text and vectors become pixels.";
    c.options = {opt("dpi", L"Resolution", K::Slider, 72, 600, 1, 300, L"dpi")};
    if (jpg) c.options.push_back(opt("quality", L"Quality", K::Slider, 1, 100, 1, 90));
    else c.options.push_back(opt("transparent", L"Transparent background", K::Toggle, 0, 1, 1, 0));
    c.options.push_back(opt("annotations", L"Include annotations and form fields", K::Toggle, 0, 1, 1, 1));
    c.options.push_back(opt("grayscale", L"Grayscale", K::Toggle, 0, 1, 1, 0));
    for (auto& o : common) c.options.push_back(o);
    c.run = [jpg](const ConvertTask& t, JobContext& ctx) { return RunPageImages(t, ctx, jpg); };
    const wchar_t* ext = jpg ? L".jpg" : L".png";
    c.verify = [ext](const fs::path& p, std::wstring* why) { return VerifyImageFolder(p, ext, why); };
    return c;
  };
  CapabilityRegistry::Get().Add(imageCap(true));
  CapabilityRegistry::Get().Add(imageCap(false));

  {
    Capability c;
    c.id = "pdf>txt"; c.group = Group::Pdf; c.in = L"pdf"; c.out = L"txt"; c.engine = "pdfium"; c.lossy = true;
    c.limitation = L"Plain text only (layout, images and fonts are dropped). Scanned pages are read with Windows OCR when a recognition language is installed.";
    c.options = {choice("ocr", L"OCR for scanned pages", {{"auto", L"Automatic"}, {"off", L"Off"}}),
                 choice("ocrLanguage", L"OCR language", ocrLangs),
                 choice("pageBreak", L"Page separator", {{"formfeed", L"Form feed line"}, {"blank", L"Blank lines"}})};
    for (auto& o : common) c.options.push_back(o);
    c.run = RunText;
    c.verify = VerifyTextFile;
    CapabilityRegistry::Get().Add(std::move(c));
  }
  {
    Capability c;
    c.id = "pdf>md"; c.group = Group::Pdf; c.in = L"pdf"; c.out = L"md"; c.engine = "pdfium"; c.lossy = true; c.producesFolder = true;
    c.limitation = L"Headings, lists and simple tables are inferred from the layout and can be imperfect. Images are saved in an images folder next to the Markdown file.";
    c.options = {choice("ocr", L"OCR for scanned pages", {{"auto", L"Automatic"}, {"off", L"Off"}}),
                 choice("ocrLanguage", L"OCR language", ocrLangs),
                 opt("images", L"Export images", K::Toggle, 0, 1, 1, 1),
                 opt("pageBreaks", L"Separate pages with a rule", K::Toggle, 0, 1, 1, 1)};
    for (auto& o : common) c.options.push_back(o);
    c.run = RunMarkdown;
    c.verify = [](const fs::path& dir, std::wstring* why) {
      std::error_code ec;
      for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == L".md") return true;
      if (why) *why = L"The Markdown file was not produced.";
      return false;
    };
    CapabilityRegistry::Get().Add(std::move(c));
  }
}

} // namespace filo
