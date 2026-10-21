// Image compression tool (headless; not a conversion, so nothing is registered in the capability registry).
//
//   TargetBytes      binary search over quality (then optional downscale) so the file ends up <= targetBytes
//   PercentReduction same, with target = input size * (100 - percent) / 100
//   Preset           one encode: Low (gentle) / Medium / High (strong); PNG uses lossless -> palette quantisation
//
// The output keeps the input format unless `outFormat` is given. If the result is not smaller than the input,
// `notSmaller` is set and NO output file is written - the caller keeps the original.
#pragma once
#include "engines/image/bitmap.h"
#include <optional>

namespace filo::img {

enum class CompressMode { TargetBytes, PercentReduction, Preset };
enum class CompressPreset { Low, Medium, High };   // amount of compression: Low = gentle, High = strong

struct CompressRequest {
  fs::path input;
  fs::path output;                       // written directly (callers pass an OutputTransaction temp path)
  CompressMode mode = CompressMode::Preset;
  uint64_t targetBytes = 0;              // TargetBytes
  int percentReduction = 0;              // PercentReduction: 1..99
  CompressPreset preset = CompressPreset::Medium;
  std::wstring outFormat;                // empty = keep the input format (jpg png webp avif heic tiff)
  int maxSide = 0;                       // optional resize: longer side <= maxSide (never upscales)
  int resizePercent = 0;                 // optional resize: 1..99 % of the original size (used when maxSide == 0)
  bool allowDownscale = true;            // TargetBytes / PercentReduction: reduce dimensions when quality alone cannot reach the target
  int minQuality = 35;                   // quality floor of the search before dimensions are reduced
  bool preserveMetadata = true;          // EXIF / XMP (the ICC profile is always kept: it is colour data)
};

struct CompressResult {
  bool ok = false;
  std::wstring error;
  std::vector<std::wstring> warnings;
  bool notSmaller = false;               // no output written: keep the original
  bool targetMet = true;                 // TargetBytes / PercentReduction: output <= target
  uint64_t inputBytes = 0, outputBytes = 0, targetBytesUsed = 0;
  int widthIn = 0, heightIn = 0, widthOut = 0, heightOut = 0;
  int qualityUsed = 0;                   // 0 for lossless output
  int colorsUsed = 0;                    // PNG palette size when quantised (0 = true colour / lossless)
  bool downscaled = false;
  std::wstring format;                   // output format
};

CompressResult CompressImage(const CompressRequest& req, JobContext& ctx);

struct SizeEstimate {
  bool ok = false;
  uint64_t bytes = 0;
  bool exact = false;                    // small images are really encoded; large ones are extrapolated from a full-resolution crop
  std::wstring error;
};
// Fast prediction of the output size for the same request (does not write req.output).
SizeEstimate EstimateCompressedSize(const CompressRequest& req, JobContext* ctx = nullptr);

} // namespace filo::img
