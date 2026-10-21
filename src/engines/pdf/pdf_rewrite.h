// Byte level rewrite of a PDF saved by PDFium: drops unreachable objects and (optionally) replaces raster image streams.
// Engine private; see pdf_outline.cpp for the implementation (shares the small PDF object parser).
#pragma once
#include "core/util.h"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace filo::pdf {

struct RewriteImage {
  int objNum = 0;
  int width = 0, height = 0;
  int comps = 0;                    // 1 (gray) or 3 (RGB) colour components, 8 bits each
  std::string filter;               // "" (raw), "FlateDecode" or "DCTDecode"
  int predictor = 1, predColors = 0, predColumns = 0;
  std::string_view data;            // the raw (still encoded) stream bytes
};
struct RewriteReplacement {
  std::string jpeg;                 // new DCT stream
  int width = 0, height = 0;
};
struct RewriteOptions {
  std::function<bool(const RewriteImage&, RewriteReplacement&)> imageHook;   // return true to replace the stream
  const std::atomic<bool>* cancel = nullptr;
};
struct RewriteStats { uint64_t before = 0, after = 0; int replaced = 0; int removedObjects = 0; bool cancelled = false; };

// Images that are not 8-bit DeviceGray/DeviceRGB/ICCBased/Cal*, have masks (SMask/Mask/ImageMask), /Decode arrays or filters other
// than Flate/DCT are never offered to the hook. The file is replaced only when the result is smaller.
bool RewritePdf(const fs::path& pdf, const RewriteOptions& opt, RewriteStats* stats, std::wstring* err);

} // namespace filo::pdf
