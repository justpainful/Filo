// Image engine: registers one Capability per runnable "input -> output" path and implements the conversion pipeline
// (decode -> orientation/ICC -> encode) on top of bitmap.h, png_writer.h, pdf_writer.h and ffcodec.cpp.
#include "engines/engines.h"
#include "engines/image/image_convert.h"
#include "engines/image/image_internal.h"
#include "engines/image/pdf_writer.h"
#include "engines/common/ffmpeg.h"
#include "core/log.h"
#include "registry/capability.h"
#include "fpdfview.h"
#include <mutex>

namespace filo::img {

ConvertOptions ConvertOptionsFromJson(const Json& o) {
  ConvertOptions c;
  c.quality = (int)std::clamp<int64_t>(o.Get("quality").AsInt(90), 1, 100);
  c.lossless = o.Get("lossless").AsBool(false);
  c.preserveMetadata = o.Get("preserveMetadata").AsBool(true);
  c.frame = (int)o.Get("frame").AsInt(0);
  c.keepAnimation = o.Get("keepAnimation").AsBool(true);
  c.svgLongSide = (int)o.Get("svgSize").AsInt(0);
  return c;
}

static bool MoveInto(const fs::path& from, const fs::path& to) {
  return MoveFileExW(from.c_str(), LongPath(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

EngineResult ConvertImageFile(const fs::path& in, const fs::path& out, const std::wstring& outFormat0, const ConvertOptions& opt, JobContext& ctx) {
  ComInit com(COINIT_MULTITHREADED);
  std::wstring outFmt = CanonicalExt(outFormat0);
  EngineResult res;
  if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");

  LoadOptions lo; lo.applyExifOrientation = true; lo.convertToSRGB = true; lo.frame = opt.frame; lo.ctx = &ctx;
  lo.svgLongSide = opt.svgLongSide;
  if (outFmt == L"pdf" && lo.svgLongSide == 0) lo.svgLongSide = 3072;
  Bitmap bm; std::wstring err;
  ctx.SetProgress(0.02);
  if (!LoadBitmap(in, bm, lo, &err)) return EngineResult::Fail(err);
  if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
  ctx.SetProgress(0.35);
  const std::wstring inFmt = bm.info.format;
  for (auto& w : bm.info.warnings) res.warnings.push_back(w);

  // ---- animation ------------------------------------------------------------------------------------------------------
  bool animatedSrc = bm.info.isAnimated;
  if (animatedSrc && opt.keepAnimation && ((inFmt == L"gif" && outFmt == L"webp") || (inFmt == L"webp" && outFmt == L"gif"))) {
    Scratch sc;
    fs::path staged = StageForFfmpeg(in, sc.dir, (std::wstring(L"in.") + inFmt).c_str());
    fs::path tmp = sc.File((std::wstring(L"anim.") + outFmt).c_str());
    if (!FfAnimatedConvert(staged, tmp, outFmt == L"webp", opt.quality, opt.lossless, &ctx, &err)) return EngineResult::Fail(err);
    if (!MoveInto(tmp, out)) return EngineResult::Fail(L"The converted file could not be saved.");
    if (opt.preserveMetadata && bm.info.meta.HasExifOrXmp()) res.warnings.push_back(L"Metadata (EXIF/XMP) could not be kept in the animated image");
    ctx.SetProgress(1.0);
    res.ok = true;
    return res;
  }
  if (animatedSrc) res.warnings.push_back(L"Animation was reduced to the first frame (" + (outFmt == L"gif" || outFmt == L"webp" ? std::wstring(L"animation is only kept for GIF to WebP and WebP to GIF") : outFmt) + L")");
  else if (bm.info.format == L"tiff" && bm.info.frameCount > 1) res.warnings.push_back(L"Only the first page of the multi-page TIFF was converted");

  // ---- PDF --------------------------------------------------------------------------------------------------------------
  if (outFmt == L"pdf") {
    PdfImagePage page;
    bool passthrough = false;
    if (inFmt == L"jpg" && !animatedSrc) {
      Bytes raw; Metadata jm; JpegInfo ji;
      if (ReadFileBytes(in, raw, nullptr) && ParseJpeg(raw, ji, jm) && JpegPassthroughOk(ji)) {
        page.jpeg = std::move(raw); page.jpegInfo = ji; page.orientation = std::max(1, ExifOrientation(jm.exif)); page.icc = jm.icc;
        passthrough = true;
      }
    }
    if (!passthrough) {
      page.bitmap = &bm;
      if (bm.info.isVector && bm.info.storedWidth > 0) page.dpi = 96.0 * bm.width / bm.info.storedWidth;
    }
    if (opt.preserveMetadata && bm.info.meta.HasExifOrXmp()) res.warnings.push_back(L"Metadata (EXIF/XMP) could not be kept: PDF pages made from images do not carry it");
    PdfOptions po;
    if (!WritePdf(out, {page}, po, &err)) return EngineResult::Fail(err);
    ctx.SetProgress(1.0);
    res.ok = true;
    return res;
  }

  // ---- raster output -------------------------------------------------------------------------------------------------------
  SaveOptions so; so.quality = opt.quality; so.lossless = opt.lossless; so.preserveMetadata = opt.preserveMetadata; so.ctx = &ctx;
  SaveResult sr = SaveBitmap(out, bm, outFmt, so);
  if (!sr.ok) return EngineResult::Fail(sr.error);
  for (auto& w : sr.warnings) res.warnings.push_back(w);
  if (outFmt != L"png" && inFmt == outFmt) {}
  ctx.SetProgress(1.0);
  res.ok = true;
  return res;
}

// ---------------------------------------------------------------------------------------------------------------------
// verification (opens the produced file with an independent reader)
// ---------------------------------------------------------------------------------------------------------------------
static std::mutex g_pdfiumMu;
static bool VerifyPdf(const fs::path& p, std::wstring* why) {
  std::lock_guard lk(g_pdfiumMu);
  static std::once_flag once; std::call_once(once, [] { FPDF_InitLibrary(); });
  Bytes data; if (!ReadFileBytes(p, data, why)) return false;
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument(data.data(), (int)data.size(), nullptr);
  if (!doc) { if (why) *why = L"The PDF could not be opened."; return false; }
  int n = FPDF_GetPageCount(doc);
  bool ok = n >= 1;
  if (ok) { FPDF_PAGE pg = FPDF_LoadPage(doc, 0); ok = pg && FPDF_GetPageWidth(pg) > 0 && FPDF_GetPageHeight(pg) > 0; if (pg) FPDF_ClosePage(pg); }
  FPDF_CloseDocument(doc);
  if (!ok && why) *why = L"The PDF has no readable page.";
  return ok;
}

static bool VerifyImage(const fs::path& p, const std::wstring& fmt, std::wstring* why) {
  if (fmt == L"pdf") return VerifyPdf(p, why);
  Bitmap bm; LoadOptions lo; lo.maxSide = 256; lo.convertToSRGB = false;
  std::wstring e;
  if (!LoadBitmap(p, bm, lo, &e) || bm.Empty()) { if (why) *why = L"The converted image could not be opened: " + e; return false; }
  if (bm.info.format != fmt) { if (why) *why = L"The converted file is not a " + fmt + L" image."; return false; }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------------------------------------------------
static std::wstring DecoderEngine(const std::wstring& f) { return f == L"svg" ? L"direct2d" : (f == L"avif" ? L"ffmpeg" : L"wic"); }
static std::wstring EncoderEngine(const std::wstring& f) { return (f == L"webp" || f == L"avif") ? L"ffmpeg" : (f == L"pdf" ? L"native" : L"wic"); }

static std::wstring Limit(const std::wstring& in, const std::wstring& out) {
  std::wstring l;
  auto add = [&](const std::wstring& s) { if (!l.empty()) l += L" "; l += s; };
  if (out == L"jpg" || out == L"bmp") add(L"No transparency: transparent areas are flattened onto white.");
  if (out == L"gif") add(L"256 colours per image; transparency is on/off only.");
  if (out == L"heic") add(L"Needs the Windows HEVC/HEIF encoder; no transparency; metadata limited to EXIF tags.");
  if (out == L"avif") add(L"EXIF/XMP are not stored in AVIF files written by Filo.");
  if (out == L"pdf") add(in == L"svg" ? L"The SVG is rasterised at high resolution (not kept as vectors)." : L"One page, sized to the image at 96 dpi.");
  if (in == L"svg" && out != L"pdf") add(L"The SVG is rasterised at its intrinsic size (at least 1024 px on the long side).");
  if ((in == L"gif" && out != L"webp") || (in == L"webp" && out != L"gif")) add(L"Animated sources keep only the first frame.");
  if ((in == L"gif" && out == L"webp") || (in == L"webp" && out == L"gif")) add(L"Animation is kept.");
  return l;
}

static std::vector<OptionSpec> OptionsFor(const std::wstring& out) {
  std::vector<OptionSpec> o;
  auto slider = [&] { OptionSpec s; s.id = "quality"; s.label = L"Quality"; s.kind = OptionSpec::Kind::Slider; s.min = 1; s.max = 100; s.step = 1; s.def = 90; s.unit = L"%"; o.push_back(s); };
  auto toggle = [&](const char* id, const wchar_t* label, bool def) { OptionSpec s; s.id = id; s.label = label; s.kind = OptionSpec::Kind::Toggle; s.def = def ? 1 : 0; o.push_back(s); };
  if (out == L"jpg" || out == L"webp" || out == L"avif" || out == L"heic") slider();
  if (out == L"webp" || out == L"avif" || out == L"heic") toggle("lossless", L"Lossless", false);
  if (out == L"jpg" || out == L"png" || out == L"webp" || out == L"tiff" || out == L"heic") toggle("preserveMetadata", L"Keep metadata (EXIF)", true);
  return o;
}

static void AddCap(const std::wstring& in, const std::wstring& out) {
  Capability c;
  c.id = Narrow(in) + ">" + Narrow(out);
  c.group = Group::Image;
  c.in = in; c.out = out;
  std::wstring de = DecoderEngine(in), ee = EncoderEngine(out);
  c.engine = Narrow(de == ee ? de : de + L"+" + ee);
  c.lossy = (out == L"jpg" || out == L"webp" || out == L"avif" || out == L"heic" || out == L"gif" || in == L"svg");
  c.limitation = Limit(in, out);
  c.options = OptionsFor(out);
  c.available = [in, out] { return CanDecode(in) && (out == L"pdf" || CanEncode(out)); };
  c.verify = [out](const fs::path& p, std::wstring* why) { return VerifyImage(p, out, why); };
  c.run = [out](const ConvertTask& t, JobContext& ctx) { return ConvertImageFile(t.input, t.output, out, ConvertOptionsFromJson(t.options), ctx); };
  CapabilityRegistry::Get().Add(std::move(c));
}

} // namespace filo::img

namespace filo {

void RegisterImageEngine() {
  using namespace img;
  const std::vector<std::wstring> raster = {L"jpg", L"png", L"webp", L"heic", L"tiff", L"avif", L"bmp", L"gif"};
  for (auto& in : raster) {
    for (auto& out : raster) if (in != out) AddCap(in, out);
    AddCap(in, L"pdf");
  }
  for (auto& out : raster) AddCap(L"svg", out);
  AddCap(L"svg", L"pdf");
}

} // namespace filo
