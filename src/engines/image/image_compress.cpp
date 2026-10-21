#include "engines/image/image_compress.h"
#include "engines/image/image_internal.h"
#include "core/log.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>

namespace filo::img {

namespace {

struct Trial {
  fs::path file;
  uint64_t bytes = 0;
  int quality = 0;
  int colors = 0;
  int w = 0, h = 0;
  bool valid = false;
};

struct Outcome {
  bool met = false;
  Trial best;        // chosen (met: the best quality that fits; otherwise the smallest produced)
  bool downscaled = false;
  std::vector<std::wstring> warnings;
};

bool IsLossy(const std::wstring& f) { return f == L"jpg" || f == L"webp" || f == L"avif" || f == L"heic"; }

struct Engine {
  const CompressRequest& req;
  JobContext& job;
  Scratch scratch;
  std::wstring fmt;
  int serial = 0;
  std::wstring error;
  std::vector<std::wstring> saveWarnings;
  bool cancelled = false;

  Engine(const CompressRequest& r, JobContext& j) : req(r), job(j) {}

  bool EncodeTrial(const Bitmap& bm, int quality, int colors, Trial& t) {
    if (job.Cancelled()) { cancelled = true; error = L"Cancelled"; return false; }
    SaveOptions so;
    so.quality = quality > 0 ? quality : 90;
    so.preserveMetadata = req.preserveMetadata;
    so.pngLevel = 9; so.pngPalette = colors > 0; so.pngMaxColors = colors > 0 ? colors : 256;
    so.tiffZip = true; so.avifSpeed = 7; so.ctx = &job;
    t.file = scratch.File((L"t" + std::to_wstring(serial++) + L"." + fmt).c_str());
    SaveResult sr = SaveBitmap(t.file, bm, fmt, so);
    if (!sr.ok) { error = sr.error; return false; }
    for (auto& w : sr.warnings) if (std::find(saveWarnings.begin(), saveWarnings.end(), w) == saveWarnings.end()) saveWarnings.push_back(w);
    std::error_code ec; t.bytes = fs::file_size(t.file, ec);
    t.quality = quality; t.colors = colors; t.w = bm.width; t.h = bm.height; t.valid = true;
    return true;
  }

  // Lossy: highest quality that fits `target` (binary search). Returns false on error.
  bool SearchQuality(const Bitmap& bm, uint64_t target, int floorQ, Outcome& out) {
    int lo = floorQ, hi = (fmt == L"jpg" || fmt == L"webp") ? 95 : 92;
    Trial best, smallest;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      Trial t;
      if (!EncodeTrial(bm, mid, 0, t)) return false;
      if (!smallest.valid || t.bytes < smallest.bytes) smallest = t;
      if (t.bytes <= target) {
        best = t; lo = mid + 1;
        if (t.bytes * 100 >= target * 92) break;           // within 8 % under the target: good enough
      } else hi = mid - 1;
      job.SetProgress(std::min(0.9, 0.15 + 0.12 * serial));
    }
    out.met = best.valid;
    out.best = best.valid ? best : smallest;
    return true;
  }

  // PNG: lossless first, then palettes with fewer and fewer colours.
  bool SearchPng(const Bitmap& bm, uint64_t target, Outcome& out) {
    Trial t, smallest;
    if (!EncodeTrial(bm, 0, 0, t)) return false;
    smallest = t;
    if (t.bytes <= target) { out.met = true; out.best = t; return true; }
    for (int colors : {256, 192, 128, 96, 64, 48, 32, 24, 16}) {
      Trial q;
      if (!EncodeTrial(bm, 0, colors, q)) return false;
      if (q.bytes < smallest.bytes) smallest = q;
      if (q.bytes <= target) { out.met = true; out.best = q; return true; }
    }
    out.met = false; out.best = smallest;
    return true;
  }

  bool SearchAt(const Bitmap& bm, uint64_t target, Outcome& out) {
    if (fmt == L"png") return SearchPng(bm, target, out);
    if (IsLossy(fmt)) return SearchQuality(bm, target, req.allowDownscale ? std::clamp(req.minQuality, 1, 90) : 1, out);
    Trial t; if (!EncodeTrial(bm, 0, 0, t)) return false;     // tiff: lossless only
    out.best = t; out.met = t.bytes <= target;
    return true;
  }

  bool TargetSearch(const Bitmap& base, uint64_t target, Outcome& out) {
    Bitmap cur = base;
    double scale = 1.0;
    for (int round = 0; round < 5; ++round) {
      Outcome o;
      if (!SearchAt(cur, target, o)) return false;
      out.met = o.met; out.best = o.best;
      if (o.met || !req.allowDownscale) return true;
      if (job.Cancelled()) { cancelled = true; error = L"Cancelled"; return false; }
      double f = std::sqrt((double)target / std::max<uint64_t>(1, o.best.bytes)) * 0.96;
      f = std::clamp(f, 0.3, 0.95);
      scale *= f;
      int nw = std::max(1, (int)std::lround(base.width * scale)), nh = std::max(1, (int)std::lround(base.height * scale));
      if (std::max(nw, nh) < 16) break;
      Bitmap smaller;
      if (!ResizeBitmap(base, nw, nh, smaller)) break;
      cur = std::move(smaller);
      out.downscaled = true;
    }
    return true;
  }

  bool PresetEncode(const Bitmap& bm, Outcome& out) {
    Trial t;
    int q = 0, colors = 0;
    switch (req.preset) {
      case CompressPreset::Low: q = 90; colors = 0; break;
      case CompressPreset::Medium: q = 75; colors = 256; break;
      case CompressPreset::High: q = 55; colors = 64; break;
    }
    if (fmt == L"avif") q = req.preset == CompressPreset::Low ? 85 : req.preset == CompressPreset::Medium ? 65 : 45;
    if (fmt == L"png") { if (!EncodeTrial(bm, 0, colors, t)) return false; }
    else if (IsLossy(fmt)) { if (!EncodeTrial(bm, q, 0, t)) return false; }
    else { if (!EncodeTrial(bm, 0, 0, t)) return false; }
    out.best = t; out.met = true;
    return true;
  }
};

// Loads and prepares the bitmap (orientation baked, pixels kept in their own colour space, resized on request).
bool Prepare(const CompressRequest& req, JobContext& job, Bitmap& bm, std::wstring& fmt, std::wstring* err, std::vector<std::wstring>* warnings) {
  LoadOptions lo; lo.convertToSRGB = false; lo.applyExifOrientation = true; lo.ctx = &job; lo.svgLongSide = 2048;
  if (!LoadBitmap(req.input, bm, lo, err)) return false;
  if (bm.info.isAnimated) { if (err) *err = L"Animated images cannot be compressed yet. Convert the animation to WebP or GIF instead."; return false; }
  std::wstring in = bm.info.format;
  fmt = req.outFormat.empty() ? in : CanonicalExt(req.outFormat);
  if (req.outFormat.empty()) {
    if (in == L"svg") fmt = L"png";
    else if (in == L"bmp") { if (err) *err = L"BMP files cannot be compressed in place. Choose PNG, JPG or WebP as the output format."; return false; }
    else if (in == L"gif") { if (err) *err = L"GIF files cannot be compressed in place. Choose WebP or PNG as the output format."; return false; }
  }
  if (!(fmt == L"jpg" || fmt == L"png" || fmt == L"webp" || fmt == L"avif" || fmt == L"heic" || fmt == L"tiff")) {
    if (err) *err = L"Compression to " + fmt + L" is not supported.";
    return false;
  }
  if (!CanEncode(fmt)) { if (err) *err = L"Saving as " + fmt + L" is not available on this computer."; return false; }
  // optional resize
  int nw = bm.width, nh = bm.height;
  if (req.maxSide > 0 && std::max(bm.width, bm.height) > req.maxSide) { double k = (double)req.maxSide / std::max(bm.width, bm.height); nw = std::max(1, (int)std::lround(bm.width * k)); nh = std::max(1, (int)std::lround(bm.height * k)); }
  else if (req.maxSide <= 0 && req.resizePercent > 0 && req.resizePercent < 100) { nw = std::max(1, (int)std::lround(bm.width * req.resizePercent / 100.0)); nh = std::max(1, (int)std::lround(bm.height * req.resizePercent / 100.0)); }
  if (nw != bm.width || nh != bm.height) {
    Bitmap r; if (!ResizeBitmap(bm, nw, nh, r)) { if (err) *err = L"The image could not be resized."; return false; }
    r.info = bm.info; bm = std::move(r);
    if (warnings) warnings->push_back(L"The image was resized to " + std::to_wstring(nw) + L"x" + std::to_wstring(nh));
  }
  return true;
}

uint64_t TargetFor(const CompressRequest& req, uint64_t inputBytes) {
  if (req.mode == CompressMode::TargetBytes) return req.targetBytes;
  if (req.mode == CompressMode::PercentReduction) { int p = std::clamp(req.percentReduction, 1, 99); return (uint64_t)((long double)inputBytes * (100 - p) / 100.0L); }
  return UINT64_MAX;
}

bool Run(Engine& e, const Bitmap& bm, uint64_t target, Outcome& out) {
  if (e.req.mode == CompressMode::Preset) return e.PresetEncode(bm, out);
  if (target == 0) { e.error = L"The target size must be larger than zero."; return false; }
  return e.TargetSearch(bm, target, out);
}

} // namespace

CompressResult CompressImage(const CompressRequest& req, JobContext& job) {
  ComInit com(COINIT_MULTITHREADED);
  CompressResult res;
  std::error_code ec;
  res.inputBytes = fs::exists(req.input, ec) ? fs::file_size(req.input, ec) : 0;
  {
    // fs::file_size fails on very long paths without long-path support; fall back to the Win32 call
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (GetFileAttributesExW(LongPath(req.input).c_str(), GetFileExInfoStandard, &ad)) res.inputBytes = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
  }
  if (req.mode == CompressMode::TargetBytes && req.targetBytes == 0) { res.error = L"Enter a target size larger than zero."; return res; }
  if (req.mode == CompressMode::PercentReduction && (req.percentReduction < 1 || req.percentReduction > 99)) { res.error = L"The reduction must be between 1% and 99%."; return res; }
  Bitmap bm; std::wstring fmt, err;
  job.SetProgress(0.02);
  Engine e(req, job);
  if (!Prepare(req, job, bm, fmt, &err, &res.warnings)) { res.error = err; return res; }
  e.fmt = fmt;
  res.format = fmt;
  res.widthIn = bm.info.fullWidth ? bm.info.fullWidth : bm.width; res.heightIn = bm.info.fullHeight ? bm.info.fullHeight : bm.height;
  uint64_t target = TargetFor(req, res.inputBytes);
  res.targetBytesUsed = target == UINT64_MAX ? 0 : target;
  job.SetProgress(0.1);
  Outcome out;
  if (!Run(e, bm, target, out)) { res.error = e.error.empty() ? L"The image could not be compressed." : e.error; return res; }
  for (auto& w : e.saveWarnings) res.warnings.push_back(w);
  res.outputBytes = out.best.bytes;
  res.widthOut = out.best.w; res.heightOut = out.best.h;
  res.qualityUsed = out.best.quality; res.colorsUsed = out.best.colors;
  res.downscaled = out.downscaled || (out.best.w != bm.width || out.best.h != bm.height);
  res.targetMet = req.mode == CompressMode::Preset ? true : out.met;
  if (!res.targetMet) res.warnings.push_back(L"The target size could not be reached; this is the smallest result that keeps the image usable");
  if (res.downscaled && req.mode != CompressMode::Preset) res.warnings.push_back(L"The image was reduced to " + std::to_wstring(res.widthOut) + L"x" + std::to_wstring(res.heightOut) + L" pixels to reach the target size");
  if (res.outputBytes >= res.inputBytes) {
    res.ok = true; res.notSmaller = true;
    res.warnings.push_back(L"The compressed file would not be smaller than the original, so the original was kept");
    job.SetProgress(1.0);
    return res;
  }
  if (!MoveFileExW(out.best.file.c_str(), LongPath(req.output).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    res.error = L"The compressed file could not be saved."; return res;
  }
  res.ok = true;
  job.SetProgress(1.0);
  return res;
}

SizeEstimate EstimateCompressedSize(const CompressRequest& req, JobContext* jobPtr) {
  ComInit com(COINIT_MULTITHREADED);
  JobContext local;
  JobContext& job = jobPtr ? *jobPtr : local;
  SizeEstimate est;
  uint64_t inputBytes = 0;
  { WIN32_FILE_ATTRIBUTE_DATA ad{}; if (GetFileAttributesExW(LongPath(req.input).c_str(), GetFileExInfoStandard, &ad)) inputBytes = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow; }
  Bitmap bm; std::wstring fmt, err;
  if (!Prepare(req, job, bm, fmt, &err, nullptr)) { est.error = err; return est; }
  uint64_t target = TargetFor(req, inputBytes);
  const uint64_t pixels = (uint64_t)bm.width * bm.height;
  Engine e(req, job); e.fmt = fmt;
  CompressRequest sub = req;
  if (pixels <= 1300000) {
    Outcome out;
    if (!Run(e, bm, target, out)) { est.error = e.error.empty() ? L"The size could not be estimated." : e.error; return est; }
    est.ok = true; est.exact = true; est.bytes = out.best.bytes;
    return est;
  }
  // Large image: encode a full-resolution central crop (same per-pixel statistics as the whole) and scale up.
  int cw = std::min(bm.width, 1024), ch = std::min(bm.height, 1024);
  int cx = (bm.width - cw) / 2, cy = (bm.height - ch) / 2;
  Bitmap crop; crop.Allocate(cw, ch);
  for (int y = 0; y < ch; ++y) memcpy(crop.Row(y), bm.Row(cy + y) + (size_t)cx * 4, (size_t)cw * 4);
  size_t metaBytes = bm.info.meta.exif.size() + bm.info.meta.xmp.size() + bm.info.meta.icc.size();
  crop.info = bm.info; crop.info.meta = Metadata{};      // metadata is added back once, not scaled with the pixels
  double ratio = (double)pixels / ((double)cw * ch);
  uint64_t cropTarget = target == UINT64_MAX ? target : std::max<uint64_t>(1, (uint64_t)((double)target / ratio));
  Outcome out;
  if (!Run(e, crop, cropTarget, out)) { est.error = e.error.empty() ? L"The size could not be estimated." : e.error; return est; }
  double bytes = (double)out.best.bytes * ratio;
  if (req.mode != CompressMode::Preset && !out.met && req.allowDownscale) bytes = (double)target;          // the real run would shrink the dimensions to land on the target
  if (req.mode != CompressMode::Preset && out.met) bytes = std::min(bytes, (double)target);
  est.ok = true; est.exact = false; est.bytes = (uint64_t)bytes + (req.preserveMetadata ? metaBytes : bm.info.meta.icc.size());
  return est;
}

} // namespace filo::img
