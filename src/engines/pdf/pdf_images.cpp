// Images -> PDF.
#include "engines/pdf/pdf_ops.h"
#include "engines/pdf/pdf_wic.h"
#include "core/output.h"
#include "core/settings.h"
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_transformpage.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace filo::pdf {
namespace {

struct MemAccess : FPDF_FILEACCESS {
  const std::string* data = nullptr;
};
int MemBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
  auto* d = static_cast<const std::string*>(param);
  if (pos > d->size() || size > d->size() - pos) return 0;
  std::memcpy(buf, d->data() + pos, size);
  return 1;
}

bool ReadAll(const fs::path& p, std::string& out) {
  std::ifstream f(fs::path(LongPath(p)), std::ios::binary | std::ios::ate);
  if (!f) return false;
  auto n = (size_t)f.tellg();
  if (n > (size_t)512 << 20) return false;
  out.resize(n);
  f.seekg(0);
  f.read(out.data(), (std::streamsize)n);
  return (bool)f;
}

void PageDims(PageSizeKind k, double& w, double& h) {
  switch (k) {
    case PageSizeKind::A3: w = 841.890; h = 1190.551; break;
    case PageSizeKind::A5: w = 419.528; h = 595.276; break;
    case PageSizeKind::Letter: w = 612; h = 792; break;
    case PageSizeKind::Legal: w = 612; h = 1008; break;
    default: w = 595.276; h = 841.890; break;
  }
}

// EXIF orientation -> direction of the stored x / y axes in the displayed (y-down) image
struct Orient { int dxx, dxy, dyx, dyy; };
Orient OrientOf(int o) {
  switch (o) {
    case 2: return {-1, 0, 0, 1};
    case 3: return {-1, 0, 0, -1};
    case 4: return {1, 0, 0, -1};
    case 5: return {0, 1, 1, 0};
    case 6: return {0, 1, -1, 0};
    case 7: return {0, -1, -1, 0};
    case 8: return {0, -1, 1, 0};
    default: return {1, 0, 0, 1};
  }
}

} // namespace

OpResult CreatePdfFromImages(const std::vector<fs::path>& images, const fs::path& desiredOutput, const ImagesToPdfOptions& opt,
                             JobContext& ctx) {
  OpResult res;
  auto fail = [&](std::wstring m) { OpResult r; r.error = std::move(m); return r; };
  if (images.empty()) return fail(L"Add at least one image.");
  wic::ComScope com;
  OutputTransaction tx(desiredOutput, SettingsStore::Get().Snapshot().overwritePolicy);
  auto docp = PdfDocument::CreateNew();
  if (!docp) return fail(L"The PDF engine could not be started.");
  const double dpi = std::clamp(opt.dpi, 10.0, 2400.0);
  const double margin = std::max(0.0, opt.marginPt);

  for (size_t i = 0; i < images.size(); ++i) {
    if (ctx.Cancelled()) { OpResult r; r.cancelled = true; r.error = L"Cancelled"; return r; }
    const fs::path& file = images[i];
    std::wstring name = file.filename().wstring();
    wic::Decoded info;
    std::wstring err;
    if (!wic::Probe(file, info, &err)) return fail(L"\"" + name + L"\": " + err);
    if (info.frames > 1) res.warnings.push_back(L"\"" + name + L"\" has several frames; only the first one was used.");
    Orient o = OrientOf(info.orientation);
    bool swapWH = info.orientation >= 5;
    int dwPx = swapWH ? info.h : info.w, dhPx = swapWH ? info.w : info.h;

    // ---- page size
    double pageW, pageH;
    if (opt.size == PageSizeKind::FitImage) {
      pageW = dwPx * 72.0 / dpi + 2 * margin; pageH = dhPx * 72.0 / dpi + 2 * margin;
    } else {
      if (opt.size == PageSizeKind::Custom) { pageW = std::max(10.0, opt.customWidthPt); pageH = std::max(10.0, opt.customHeightPt); }
      else PageDims(opt.size, pageW, pageH);
      bool landscape = opt.orientation == PageOrientation::Landscape || (opt.orientation == PageOrientation::Auto && dwPx > dhPx);
      if (landscape != (pageW > pageH) && pageW != pageH) std::swap(pageW, pageH);
    }
    const double maxPage = 14400.0;
    if (pageW > maxPage || pageH > maxPage) {
      double f = maxPage / std::max(pageW, pageH);
      pageW *= f; pageH *= f;
      res.warnings.push_back(L"\"" + name + L"\" is very large; its page was reduced to the PDF size limit.");
    }
    double cw = std::max(1.0, pageW - 2 * margin), ch = std::max(1.0, pageH - 2 * margin);
    double Wd, Hd;
    if (opt.size == PageSizeKind::FitImage || opt.fit == FitMode::Stretch) { Wd = cw; Hd = ch; }
    else {
      double s = opt.fit == FitMode::Fill ? std::max(cw / dwPx, ch / dhPx) : std::min(cw / dwPx, ch / dhPx);
      Wd = dwPx * s; Hd = dhPx * s;
    }
    double ox = margin + (cw - Wd) / 2, oy = margin + (ch - Hd) / 2;   // top-left of the displayed box

    // ---- matrix (stored unit square -> page)
    double Lx = o.dxx != 0 ? Wd : Hd, Ly = o.dyx != 0 ? Wd : Hd;
    double Sx = (o.dxx < 0 || o.dyx < 0) ? Wd : 0, Sy = (o.dxy < 0 || o.dyy < 0) ? Hd : 0;
    double a = Lx * o.dxx, b = -Lx * o.dxy, c = -Ly * o.dyx, d = Ly * o.dyy;
    double e = ox + Sx + Ly * o.dyx, f = pageH - oy - (Sy + Ly * o.dyy);

    {
      PdfLock lock;
      FPDF_DOCUMENT doc = (FPDF_DOCUMENT)docp->Handle();
      FPDF_PAGE pg = FPDFPage_New(doc, (int)i, pageW, pageH);
      if (!pg) return fail(L"A PDF page could not be created.");
      FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc);
      bool loaded = false;
      std::string bytes;
      if (info.isJpeg && !info.cmyk && ReadAll(file, bytes) && bytes.size() > 4) {
        MemAccess acc;
        acc.m_FileLen = (unsigned long)bytes.size(); acc.m_GetBlock = &MemBlock; acc.m_Param = &bytes; acc.data = &bytes;
        if (FPDFImageObj_LoadJpegFileInline(&pg, 1, obj, &acc)) {
          unsigned int pw = 0, ph = 0;
          if (FPDFImageObj_GetImagePixelSize(obj, &pw, &ph) && (int)pw == info.w && (int)ph == info.h) loaded = true;
        }
        if (!loaded) { FPDFPageObj_Destroy(obj); obj = FPDFPageObj_NewImageObj(doc); }
      }
      if (!loaded) {
        wic::Decoded px;
        std::wstring derr;
        if (!wic::Decode(file, px, &derr)) { FPDFPageObj_Destroy(obj); FPDF_ClosePage(pg); return fail(L"\"" + name + L"\": " + derr); }
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(px.w, px.h, FPDFBitmap_BGRA, px.bgra.data(), px.w * 4);
        if (!bmp) { FPDFPageObj_Destroy(obj); FPDF_ClosePage(pg); return fail(L"Not enough memory to place \"" + name + L"\"."); }
        // straight alpha -> PDFium wants premultiplied-free BGRA with an alpha channel; opaque images are stored as plain RGB
        bool anyAlpha = false;
        for (size_t k = 3; k < px.bgra.size() && !anyAlpha; k += 4) if (px.bgra[k] != 255) anyAlpha = true;
        if (!anyAlpha) { FPDFBitmap_Destroy(bmp); bmp = FPDFBitmap_CreateEx(px.w, px.h, FPDFBitmap_BGRx, px.bgra.data(), px.w * 4); }
        bool ok = bmp && FPDFImageObj_SetBitmap(&pg, 1, obj, bmp);
        if (bmp) FPDFBitmap_Destroy(bmp);
        if (!ok) { FPDFPageObj_Destroy(obj); FPDF_ClosePage(pg); return fail(L"\"" + name + L"\" could not be placed on a page."); }
      }
      FPDFImageObj_SetMatrix(obj, a, b, c, d, e, f);
      FPDFPage_InsertObject(pg, obj);
      FPDFPage_GenerateContent(pg);
      if (opt.fit == FitMode::Fill && opt.size != PageSizeKind::FitImage) {
        FPDF_CLIPPATH clip = FPDF_CreateClipPath((float)margin, (float)margin, (float)(pageW - margin), (float)(pageH - margin));
        if (clip) { FPDFPage_InsertClipPath(pg, clip); FPDF_DestroyClipPath(clip); }
      }
      FPDF_ClosePage(pg);
    }
    ctx.SetProgress(0.9 * (i + 1) / images.size());
  }
  std::wstring serr;
  if (!docp->Save(tx.TempPath(), &serr, ctx.CancelFlag())) {
    if (ctx.Cancelled()) { OpResult r; r.cancelled = true; r.error = L"Cancelled"; return r; }
    return fail(serr);
  }
  docp.reset();
  int expected = (int)images.size();
  std::wstring cerr;
  if (!tx.Commit(&cerr, [&](const fs::path& p, std::wstring* why) {
        auto r = PdfDocument::Open(p);
        if (!r.ok() || r.doc->PageCount() != expected) { if (why) *why = L"The new PDF could not be verified."; return false; }
        return true;
      }))
    return fail(cerr);
  res.ok = true; res.pages = expected; res.outputs = {tx.FinalPath()};
  ctx.SetProgress(1.0);
  return res;
}

} // namespace filo::pdf
