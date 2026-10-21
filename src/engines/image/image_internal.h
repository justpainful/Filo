// Internal helpers shared between the image engine's translation units. Not part of the public API.
#pragma once
#include "engines/image/bitmap.h"
#include "core/com.h"

namespace filo::img {

// Direct2D SVG rasteriser (svg.cpp). `longSide` = pixels of the longer side (0 = auto).
bool RasterizeSvg(const Bytes& svg, int longSide, Bitmap& out, std::wstring* err);
// Reads the intrinsic size of an SVG document from its root element (width/height/viewBox). Returns false if it has none.
bool SvgIntrinsicSize(const Bytes& svg, double& w, double& h);

// ffmpeg based codecs (ffcodec.cpp)
// ff::HasMuxer() of the shared helper parses `-muxers` with a separator that ffmpeg does not print for that listing (always false),
// so the image engine asks ffmpeg directly (`-h muxer=<name>`), cached.
bool FfHasMuxer(const std::string& name);
struct FfEncodeParams {
  int quality = 90;
  bool lossless = false;
  bool hasAlpha = false;
  int avifSpeed = 6;
};
// `pngIn` is a PNG written by us. Output is written to `out` (temp). ctx may be null.
bool FfEncodeWebp(const fs::path& pngIn, const fs::path& out, const FfEncodeParams& p, JobContext* ctx, std::wstring* err);
bool FfEncodeAvif(const fs::path& pngIn, const fs::path& out, const FfEncodeParams& p, JobContext* ctx, std::wstring* err);
// Decodes frame `frame` (or alpha-merged AVIF) of any ffmpeg-readable image into an 8-bit RGBA PNG at `pngOut`.
bool FfDecodeToPng(const fs::path& in, const fs::path& pngOut, int frame, bool avifAlphaMerge, JobContext* ctx, std::wstring* err);
// Animated GIF <-> animated WebP (keeps all frames, delays, loop count and transparency). `toWebp` selects the direction.
bool FfAnimatedConvert(const fs::path& in, const fs::path& out, bool toWebp, int quality, bool lossless, JobContext* ctx, std::wstring* err);
// Copies the input to an ASCII-named file in `scratch` when its path could confuse ffmpeg (non-ASCII, very long, odd characters).
fs::path StageForFfmpeg(const fs::path& in, const fs::path& scratchDir, const wchar_t* stagedName);

} // namespace filo::img
