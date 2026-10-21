// Headless image conversion used by the registered capabilities (and callable directly by tools).
#pragma once
#include "engines/image/bitmap.h"
#include "registry/capability.h"

namespace filo::img {

struct ConvertOptions {
  int quality = 90;                 // 1..100 for lossy outputs (jpg webp avif heic)
  bool lossless = false;            // webp / avif / heic lossless mode
  bool preserveMetadata = true;     // keep EXIF / XMP where the output format can carry it
  int frame = 0;                    // frame / page to convert for animated or multi-page inputs
  bool keepAnimation = true;        // gif <-> webp keep every frame
  int svgLongSide = 0;              // rasterised SVG size (0 = auto)
};

// Converts `in` to `outFormat` ("jpg","png","webp","heic","tiff","avif","bmp","gif","pdf"), writing `out` directly
// (callers pass the OutputTransaction temp path). Detects the real input format from content.
EngineResult ConvertImageFile(const fs::path& in, const fs::path& out, const std::wstring& outFormat, const ConvertOptions& opt, JobContext& ctx);

ConvertOptions ConvertOptionsFromJson(const Json& options);

} // namespace filo::img
