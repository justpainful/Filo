// PDF compression: full re-save + downsampling / JPEG re-encoding of large embedded raster images.
//
// PDFium's own image API cannot be used for this: a replaced image keeps its old stream in the file (and the page keeps a
// resource entry pointing to it), so nothing shrinks. Instead the document is re-saved by PDFium and the saved file is rewritten
// at byte level (pdf_outline.cpp): image XObjects are decoded here (Flate + PNG/TIFF predictors, or JPEG via WIC), scaled and
// re-encoded as JPEG in place (same object numbers), and every unreachable object is dropped.
#include "engines/pdf/pdf_ops.h"
#include "engines/pdf/pdf_rewrite.h"
#include "engines/pdf/pdf_wic.h"
#include "core/output.h"
#include "core/settings.h"
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace filo::pdf {
namespace {

struct Params { double maxDpi; int quality; bool recompressJpeg; double gain; };
// ladder from gentle to aggressive
const Params kLadder[] = {
    {300, 88, false, 0.60},
    {220, 82, false, 0.75},
    {150, 75, false, 0.85},
    {120, 65, true, 0.90},
    {96, 55, true, 0.92},
    {72, 42, true, 0.95},
};
int StartStep(CompressPreset p) { return p == CompressPreset::Low ? 0 : p == CompressPreset::Medium ? 2 : 4; }

using Key = std::tuple<unsigned, unsigned, unsigned long>;       // pixel width, height, raw stream length
using Usage = std::map<Key, double>;                              // -> highest effective dpi the image is shown at

void CollectUsageObj(FPDF_PAGE page, FPDF_PAGEOBJECT obj, Usage& u, int depth) {
  int type = FPDFPageObj_GetType(obj);
  if (type == FPDF_PAGEOBJ_IMAGE) {
    FPDF_IMAGEOBJ_METADATA md{};
    if (!FPDFImageObj_GetImageMetadata(obj, page, &md)) return;
    unsigned long len = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
    if (!len) return;
    double dpi = std::max(md.horizontal_dpi, md.vertical_dpi);
    double& cur = u[{md.width, md.height, len}];
    cur = std::max(cur, dpi);
  } else if (type == FPDF_PAGEOBJ_FORM && depth < 4) {
    int n = FPDFFormObj_CountObjects(obj);
    for (int i = 0; i < n; ++i) if (FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(obj, i)) CollectUsageObj(page, c, u, depth + 1);
  }
}

Usage CollectUsage(const PdfDocument& doc, JobContext& ctx) {
  Usage u;
  int n = doc.PageCount();
  for (int i = 0; i < n && !ctx.Cancelled(); ++i) {
    PdfLock lock;
    PageHandle ph(doc, i);
    if (!ph) continue;
    FPDF_PAGE pg = (FPDF_PAGE)ph.get();
    int cnt = FPDFPage_CountObjects(pg);
    for (int k = 0; k < cnt; ++k) if (FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pg, k)) CollectUsageObj(pg, obj, u, 0);
  }
  return u;
}

// Decodes an image stream to BGRA. Raw / Flate (+ PNG or TIFF predictors) / DCT.
bool DecodeStream(const RewriteImage& im, std::vector<uint8_t>& bgra) {
  const size_t w = (size_t)im.width, h = (size_t)im.height, comps = (size_t)im.comps;
  if ((double)w * h > 120e6) return false;
  std::vector<uint8_t> px;
  if (im.filter == "DCTDecode") {
    wic::Decoded d;
    if (!wic::DecodeMemory((const uint8_t*)im.data.data(), im.data.size(), d, nullptr)) return false;
    if (d.w != im.width || d.h != im.height) return false;
    bgra = std::move(d.bgra);
    return true;
  }
  const size_t rowBytes = w * comps;
  if (im.filter.empty()) {
    if (im.data.size() < rowBytes * h) return false;
    px.assign(im.data.begin(), im.data.begin() + rowBytes * h);
  } else {
    bool png = im.predictor >= 10, tiff = im.predictor == 2;
    if (im.predictor != 1 && !png && !tiff) return false;
    if ((png || tiff) && ((size_t)im.predColumns != w || (size_t)im.predColors != comps)) return false;
    size_t expect = (rowBytes + (png ? 1 : 0)) * h;
    std::vector<uint8_t> buf(expect + 64);
    mz_ulong outLen = (mz_ulong)buf.size();
    int rc = mz_uncompress(buf.data(), &outLen, (const unsigned char*)im.data.data(), (mz_ulong)im.data.size());
    if (rc != MZ_OK || outLen < expect) return false;
    if (png) {
      px.resize(rowBytes * h);
      for (size_t y = 0; y < h; ++y) {
        const uint8_t* src = &buf[y * (rowBytes + 1)];
        uint8_t ft = src[0]; ++src;
        uint8_t* dst = &px[y * rowBytes];
        const uint8_t* up = y ? &px[(y - 1) * rowBytes] : nullptr;
        for (size_t x = 0; x < rowBytes; ++x) {
          int a = x >= comps ? dst[x - comps] : 0, b = up ? up[x] : 0, c = (up && x >= comps) ? up[x - comps] : 0, v = src[x];
          switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: { int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
            default: return false;
          }
          dst[x] = (uint8_t)v;
        }
      }
    } else {
      px.assign(buf.begin(), buf.begin() + rowBytes * h);
      if (tiff)
        for (size_t y = 0; y < h; ++y) { uint8_t* r = &px[y * rowBytes]; for (size_t x = comps; x < rowBytes; ++x) r[x] = (uint8_t)(r[x] + r[x - comps]); }
    }
  }
  try { bgra.resize(w * h * 4); } catch (...) { return false; }
  for (size_t i = 0, n = w * h; i < n; ++i) {
    uint8_t* d = &bgra[i * 4];
    if (comps == 3) { d[0] = px[i * 3 + 2]; d[1] = px[i * 3 + 1]; d[2] = px[i * 3]; }
    else d[0] = d[1] = d[2] = px[i];
    d[3] = 255;
  }
  return true;
}

bool Recompress(const RewriteImage& im, const Usage& usage, const Params& p, RewriteReplacement& rep) {
  if ((uint64_t)im.width * im.height < 60000 || im.width < 16 || im.height < 16) return false;
  auto it = usage.find({(unsigned)im.width, (unsigned)im.height, (unsigned long)im.data.size()});
  if (it == usage.end()) return false;
  double dpi = it->second;
  bool dct = im.filter == "DCTDecode";
  double scale = (dpi > p.maxDpi * 1.05) ? p.maxDpi / dpi : 1.0;
  if (!(scale < 1.0 || !dct || p.recompressJpeg)) return false;
  std::vector<uint8_t> px;
  if (!DecodeStream(im, px)) return false;
  int w = im.width, h = im.height;
  if (scale < 1.0) {
    int nw = std::max(8, (int)std::lround(w * scale)), nh = std::max(8, (int)std::lround(h * scale));
    px = wic::Downscale(px.data(), w, h, nw, nh);
    w = nw; h = nh;
  }
  bool gray = im.comps == 1;
  std::vector<uint8_t> jpg;
  if (!wic::EncodeJpegMemory(jpg, px.data(), w, h, p.quality, gray)) return false;
  if (jpg.size() >= im.data.size() * p.gain) return false;
  rep.jpeg.assign((const char*)jpg.data(), jpg.size());
  rep.width = w; rep.height = h;
  return true;
}

struct PassOut { bool ok = false; bool cancelled = false; std::wstring error; uint64_t bytes = 0; int replaced = 0; bool encrypted = false; };

PassOut RunPass(const fs::path& input, const std::wstring& password, const Params& p, bool recompress, const fs::path& outFile,
                JobContext& ctx, double progBase, double progSpan) {
  PassOut r;
  wic::ComScope com;      // job threads have no COM apartment of their own
  auto o = PdfDocument::Open(input, password);
  if (!o.ok()) { r.error = o.message; return r; }
  r.encrypted = o.doc->IsEncrypted();
  std::wstring err;
  if (!o.doc->Save(outFile, &err, ctx.CancelFlag(), true)) { r.cancelled = ctx.Cancelled(); r.error = err; return r; }
  o.doc.reset();
  ctx.SetProgress(progBase + progSpan * 0.2);
  // image usage is read from the file just written: its stream lengths (PDFium re-compresses on save) are the ones the rewrite sees
  Usage usage;
  if (recompress) {
    auto saved = PdfDocument::Open(outFile);
    if (saved.ok()) usage = CollectUsage(*saved.doc, ctx);
    if (ctx.Cancelled()) { r.cancelled = true; r.error = L"Cancelled"; return r; }
  }
  ctx.SetProgress(progBase + progSpan * 0.4);
  if (recompress && !usage.empty()) {
    RewriteOptions ro;
    ro.cancel = ctx.CancelFlag();
    ro.imageHook = [&](const RewriteImage& im, RewriteReplacement& rep) { return Recompress(im, usage, p, rep); };
    RewriteStats st;
    std::wstring gerr;
    if (!RewritePdf(outFile, ro, &st, &gerr)) {
      if (st.cancelled || ctx.Cancelled()) { r.cancelled = true; r.error = L"Cancelled"; return r; }
      // layout not understood: keep PDFium's plain re-save
    } else r.replaced = st.replaced;
  }
  std::error_code ec;
  r.bytes = fs::file_size(outFile, ec);
  r.ok = !ec && r.bytes > 0;
  ctx.SetProgress(progBase + progSpan);
  return r;
}

} // namespace

OpResult CompressPdf(const fs::path& input, const fs::path& desiredOutput, const CompressOptions& opt, JobContext& ctx) {
  OpResult res;
  auto fail = [&](std::wstring m) { OpResult r; r.error = std::move(m); return r; };
  std::error_code ec;
  uint64_t inBytes = fs::file_size(input, ec);
  if (ec) return fail(L"The file is no longer available.");
  res.inBytes = inBytes;
  uint64_t target = 0;
  if (opt.targetBytes > 0) target = opt.targetBytes;
  if (opt.targetPercent > 0) {
    uint64_t t = inBytes * std::min(100, opt.targetPercent) / 100;
    target = target ? std::min(target, t) : t;
  }
  int step = StartStep(opt.preset);
  const int lastStep = (int)(sizeof(kLadder) / sizeof(kLadder[0])) - 1;
  OutputTransaction tx(desiredOutput, SettingsStore::Get().Snapshot().overwritePolicy);

  PassOut best;
  fs::path bestFile;
  if (!target) {
    best = RunPass(input, opt.password, kLadder[step], opt.recompressImages, tx.TempPath(), ctx, 0, 0.95);
    if (!best.ok) { OpResult r; r.cancelled = best.cancelled; r.error = best.error.empty() ? L"The PDF could not be compressed." : best.error; return r; }
    bestFile = tx.TempPath();
  } else {
    fs::path dir = ctx.TempDir();
    int passes = lastStep - step + 1, done = 0;
    for (int s = step; s <= lastStep; ++s, ++done) {
      fs::path f = dir / (L"compress-" + std::to_wstring(s) + L".pdf");
      PassOut r = RunPass(input, opt.password, kLadder[s], opt.recompressImages, f, ctx, 0.9 * done / passes, 0.9 / passes);
      if (!r.ok) { OpResult x; x.cancelled = r.cancelled; x.error = r.error.empty() ? L"The PDF could not be compressed." : r.error; return x; }
      if (!best.ok || r.bytes < best.bytes) { best = r; bestFile = f; }
      if (r.bytes <= target) { best = r; bestFile = f; break; }
      if (!opt.recompressImages) break;     // nothing stronger to try
    }
    if (best.bytes > target) res.warnings.push_back(L"The requested size could not be reached; this is the smallest version possible without removing content.");
  }
  res.outBytes = best.bytes;
  res.imagesRecompressed = best.replaced;
  if (best.encrypted) res.warnings.push_back(L"The source was password-protected; the compressed copy is not.");

  if (best.bytes >= inBytes * 99 / 100) {
    // no meaningful gain: keep the original, write nothing
    tx.Rollback();
    res.ok = true; res.notSmaller = true; res.outputs.clear();
    ctx.SetProgress(1.0);
    return res;
  }
  if (bestFile != tx.TempPath()) {
    std::error_code cec;
    fs::copy_file(bestFile, tx.TempPath(), fs::copy_options::overwrite_existing, cec);
    if (cec) return fail(L"The compressed file could not be saved.");
  }
  int expectedPages = 0;
  {
    auto src = PdfDocument::Open(input, opt.password);
    if (src.ok()) expectedPages = src.doc->PageCount();
  }
  std::wstring cerr;
  if (!tx.Commit(&cerr, [&](const fs::path& p, std::wstring* why) {
        auto r = PdfDocument::Open(p);
        if (!r.ok() || (expectedPages && r.doc->PageCount() != expectedPages)) { if (why) *why = L"The compressed file could not be verified."; return false; }
        return true;
      }))
    return fail(cerr);
  res.ok = true; res.pages = expectedPages; res.outputs = {tx.FinalPath()};
  ctx.SetProgress(1.0);
  return res;
}

} // namespace filo::pdf
