#include "engines/doc/convert_common.h"
#include "engines/doc/imgenc.h"
#include "engines/doc/libreoffice.h"
#include "engines/doc/zip.h"
#include "engines/pdf/pdf_doc.h"
#include <algorithm>

namespace filo::doc {

int RenderPdfToImages(const fs::path& pdfPath, const fs::path& dir, bool jpeg, int dpi, int jpegQuality, JobContext& ctx, std::wstring* err, double p0, double p1) {
  auto open = pdf::PdfDocument::Open(pdfPath);
  if (!open.ok()) { if (err) *err = open.message.empty() ? L"The intermediate PDF could not be opened." : open.message; return -1; }
  int pages = open.doc->PageCount();
  if (pages <= 0) { if (err) *err = L"The document has no pages."; return -1; }
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (int i = 0; i < pages; ++i) {
    if (ctx.Cancelled()) { if (err) *err = L"Cancelled"; return -1; }
    pdf::RenderOptions ro;
    ro.dpi = std::clamp(dpi, 36, 600);
    ro.annotations = true; ro.forms = false; ro.transparent = false;
    pdf::Bitmap bm;
    std::wstring e2;
    if (!open.doc->RenderPage(i, ro, bm, &e2) || bm.width <= 0 || bm.height <= 0) { if (err) *err = L"A slide could not be rendered."; return -1; }
    Bytes enc;
    if (!EncodeBgra(bm.bgra.data(), bm.width, bm.height, bm.width * 4, jpeg ? ImgFormat::Jpeg : ImgFormat::Png, std::clamp(jpegQuality, 1, 100), enc)) { if (err) *err = L"A slide image could not be encoded."; return -1; }
    char name[32]; snprintf(name, sizeof name, "slide-%03d.%s", i + 1, jpeg ? "jpg" : "png");
    if (!WriteFileBytes(dir / Widen(name), enc.data(), enc.size(), err)) return -1;
    ctx.SetProgress(p0 + (p1 - p0) * (double)(i + 1) / (double)pages);
  }
  return pages;
}

EngineResult RunPresentationConversion(const ConvertTask& t, JobContext& ctx) {
  const std::wstring& in = t.inKind.format;
  const std::wstring& out = t.outExt;
  std::wstring err;
  EngineResult er;
  if (t.inKind.size == 0) return Fail(L"The file is empty, so there is nothing to convert.");
  ctx.SetProgress(0.05);
  if (out == L"pdf") {
    if (!LoViaTemp(t.input, in, L"pdf", L"pdf", t.output, ctx, &err)) return Fail(err);
    ctx.SetProgress(1.0);
    er.ok = true;
    return er;
  }
  if (out == L"png" || out == L"jpg") {
    fs::path pdfTmp = ctx.TempDir() / L"slides.pdf";
    if (!LoViaTemp(t.input, in, L"pdf", L"pdf", pdfTmp, ctx, &err)) return Fail(err);
    ctx.SetProgress(0.5);
    int dpi = (int)OptNum(t.options, "dpi", 150);
    int q = (int)OptNum(t.options, "quality", 90);
    int n = RenderPdfToImages(pdfTmp, t.output, out == L"jpg", dpi, q, ctx, &err, 0.5, 1.0);
    if (n < 0) return Fail(err);
    if (out == L"jpg") er.warnings.push_back(L"JPG has no transparency; slide backgrounds are rendered on white.");
    er.ok = true;
    return er;
  }
  return Fail(L"This output format is not supported.");
}

EngineResult RunPagesToPdf(const ConvertTask& t, JobContext& ctx) {
  std::wstring err;
  ZipReader z;
  if (!z.Open(t.input, &err)) return Fail(L"This does not look like a Pages document: " + err);
  ctx.SetProgress(0.3);
  static const char* names[] = {"QuickLook/Preview.pdf", "preview.pdf", "Preview.pdf"};
  Bytes pdfBytes;
  for (auto* n : names) {
    if (z.Has(n) && z.Read(n, pdfBytes) && pdfBytes.size() > 8 && memcmp(pdfBytes.data(), "%PDF-", 5) == 0) break;
    pdfBytes.clear();
  }
  if (pdfBytes.empty()) {
    bool hasThumb = z.Has("QuickLook/Thumbnail.jpg") || z.Has("QuickLook/Preview.jpg") || z.Has("preview.jpg") || z.Has("QuickLook/Thumbnail.png");
    return Fail(hasThumb ? L"This Pages document contains only a small preview picture, not a PDF preview, so it cannot be converted. Export it as PDF or DOCX from Pages instead."
                         : L"This Pages document has no embedded PDF preview. Pages is an Apple format that cannot be read here; export it as PDF or DOCX from Pages.");
  }
  if (!WriteFileBytes(t.output, pdfBytes.data(), pdfBytes.size(), &err)) return Fail(err);
  EngineResult er;
  er.warnings.push_back(L"This PDF is the preview that Pages stored inside the file; it can differ from a fresh export (for example if the file was last saved with the preview option turned off).");
  ctx.SetProgress(1.0);
  er.ok = true;
  return er;
}

} // namespace filo::doc
