// Bitmap layer of the image engine: decode any supported image into one simple in-memory format, and encode that
// format back to the supported file types. Reusable by the editing tools (crop, resize, background removal...).
//
// PIXEL FORMAT: 8-bit BGRA, STRAIGHT (non-premultiplied) alpha, row-major, stride = width * 4, top-down.
// Use Premultiply()/Unpremultiply() when a consumer (Direct2D, compositing) wants premultiplied data.
#pragma once
#include "core/job.h"
#include "engines/image/imgmeta.h"
#include <atomic>
#include <string>
#include <vector>

namespace filo::img {

struct ImageInfo {
  std::wstring format;                  // detected content format: jpg png gif bmp tiff webp avif heic svg
  int storedWidth = 0, storedHeight = 0;// raster size as stored in the file (before orientation / maxSide); SVG: intrinsic size
  int fullWidth = 0, fullHeight = 0;    // size as displayed (orientation applied), before maxSide downscale
  int orientation = 1;                  // EXIF orientation found in the file (1 = normal / not applicable)
  bool orientationApplied = false;      // the returned pixels were rotated/flipped (orientation is now effectively 1)
  bool hasAlpha = false;                // at least one pixel is not fully opaque
  bool formatHasAlpha = false;          // the file's pixel format carries an alpha channel
  bool isAnimated = false;              // GIF / animated WebP with more than one frame
  int frameCount = 1;                   // frames (animation) or pages (TIFF)
  bool hasIcc = false;                  // the file embeds an ICC profile
  bool iccConverted = false;            // pixels were converted from that profile to sRGB
  std::wstring iccName;                 // profile description when known
  bool isVector = false;                // SVG
  bool decodedWithFfmpeg = false;
  Metadata meta;                        // metadata normalised for re-writing: orientation reset when baked in; icc = sRGB when converted
  std::vector<std::wstring> warnings;   // non-fatal issues found while decoding
};

struct Bitmap {
  int width = 0, height = 0;
  std::vector<uint8_t> px;              // BGRA straight, width*height*4 bytes
  ImageInfo info;
  bool Empty() const { return width <= 0 || height <= 0 || px.size() < (size_t)width * height * 4; }
  size_t Stride() const { return (size_t)width * 4; }
  uint8_t* Row(int y) { return px.data() + (size_t)y * width * 4; }
  const uint8_t* Row(int y) const { return px.data() + (size_t)y * width * 4; }
  void Allocate(int w, int h) { width = w; height = h; px.assign((size_t)w * h * 4, 0); }
};

struct LoadOptions {
  bool applyExifOrientation = true;     // rotate/flip pixels so they match what viewers show (JPEG/PNG/WebP/TIFF; HEIC/AVIF decoders do it)
  bool convertToSRGB = true;            // convert from the embedded ICC profile to sRGB (info.meta.icc is then the sRGB profile)
  int  maxSide = 0;                     // >0: downscale (never upscale) so the longer side is <= maxSide (previews)
  int  frame = 0;                       // animation frame / TIFF page
  int  svgLongSide = 0;                 // SVG: pixel size of the longer side (0 = intrinsic size, at least 1024, at most 4096)
  JobContext* ctx = nullptr;            // for cancellation / ffmpeg fallbacks (optional)
};

// Decodes `path` (format detected from content, never from the extension). `out.info` describes the source.
bool LoadBitmap(const fs::path& path, Bitmap& out, const LoadOptions& opt = {}, std::wstring* err = nullptr);

// Same for bytes that are already in memory (`declaredExt` only breaks ties for text formats like SVG).
bool LoadBitmapFromMemory(const Bytes& data, Bitmap& out, const LoadOptions& opt = {}, std::wstring* err = nullptr, const std::wstring& declaredExt = L"");

// ---- pixel utilities ----------------------------------------------------------------------------------------------
bool UsesAlpha(const Bitmap& bm);                    // any alpha < 255
bool HasPartialAlpha(const Bitmap& bm);              // any alpha strictly between 0 and 255
void FlattenOnWhite(Bitmap& bm);                     // composite over white, alpha := 255
void Premultiply(Bitmap& bm);
void Unpremultiply(Bitmap& bm);
void ApplyOrientation(Bitmap& bm, int exifOrientation);   // 1..8 (EXIF numbering)
bool ResizeBitmap(const Bitmap& in, int newW, int newH, Bitmap& out);   // Fant (high quality), alpha-correct
bool ConvertToSRGB(Bitmap& bm, const Bytes& iccProfile);                // profile -> sRGB in place (RGB profiles only)

// ---- saving ---------------------------------------------------------------------------------------------------------
struct SaveOptions {
  int quality = 90;                     // 1..100 for lossy encoders
  bool lossless = false;                // WebP / AVIF / HEIC lossless mode
  bool preserveMetadata = true;         // write bm.info.meta (EXIF/XMP/ICC) where the target format can carry it
  int pngLevel = 0;                     // 1..9 deflate effort; 0 = automatic (6 for small images, 2-4 for big ones: miniz is slow on photos)
  bool pngPalette = false;              // PNG: quantise to a palette (lossy) when more than 256 colours
  int pngMaxColors = 256;
  bool tiffZip = false;                 // TIFF: Deflate instead of LZW
  JobContext* ctx = nullptr;            // cancellation / ffmpeg runs
  int avifSpeed = 6;                    // libaom cpu-used (0 slow/best .. 8 fast)
};

struct SaveResult {
  bool ok = false;
  std::wstring error;
  std::vector<std::wstring> warnings;   // "Transparency was flattened", metadata drops...
  static SaveResult Fail(std::wstring e) { SaveResult r; r.error = std::move(e); return r; }
};

// Writes `bm` to `out` in `format` (jpg png tiff bmp gif webp avif heic). Alpha is preserved when the format can carry
// it, otherwise flattened on white with the warning "Transparency was flattened". Writes directly to `out`.
SaveResult SaveBitmap(const fs::path& out, const Bitmap& bm, const std::wstring& format, const SaveOptions& opt = {});

// Capability probes (cached). Encoders: jpg png tiff bmp gif always; webp/avif need ffmpeg codecs; heic needs the WIC HEIF encoder + HEVC extension.
bool CanEncode(const std::wstring& format);
bool CanDecode(const std::wstring& format);
bool FormatSupportsAlpha(const std::wstring& format);
bool FormatSupportsMetadata(const std::wstring& format);   // can carry EXIF/XMP
// Reads a whole file (long-path safe). Limit 1.5 GB.
bool ReadFileBytes(const fs::path& p, Bytes& out, std::wstring* err = nullptr);
bool WriteFileBytes(const fs::path& p, const Bytes& data, std::wstring* err = nullptr);

// Scratch directory that is deleted when the object goes out of scope (ASCII path: safe for ffmpeg).
struct Scratch {
  Scratch();
  ~Scratch();
  Scratch(const Scratch&) = delete;
  fs::path dir;
  fs::path File(const wchar_t* name) const { return dir / name; }
};

} // namespace filo::img
