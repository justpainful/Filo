// Minimal, valid PDF writer for "images -> PDF". One image per page; page size = image size at 96 dpi -> 72 pt
// (pt = px * 0.75). JPEG data can be embedded untouched (DCTDecode) with the EXIF orientation applied through the page
// matrix and the ICC profile attached; every other image is Flate-compressed (PNG predictors) with alpha as an SMask.
#pragma once
#include "engines/image/bitmap.h"

namespace filo::img {

struct PdfImagePage {
  // --- DCT passthrough (set `jpeg` non-empty) ---
  Bytes jpeg;                  // the original JPEG stream, written untouched
  JpegInfo jpegInfo;           // from ParseJpeg
  int orientation = 1;         // EXIF orientation 1..8 applied through the content matrix
  Bytes icc;                   // optional RGB/Gray ICC profile of the JPEG
  // --- decoded pixels (when `jpeg` is empty) ---
  const Bitmap* bitmap = nullptr;   // straight BGRA; alpha (if any pixel < 255) becomes an SMask
  double dpi = 96;             // pixel -> point mapping: pt = px * 72 / dpi
};

struct PdfOptions {
  std::string title;           // optional /Title (ASCII/UTF-8)
  int flateLevel = 0;          // 1..9; 0 = automatic (6 for small images, 2-4 for big ones)
};

// Can this JPEG be embedded without re-encoding?
bool JpegPassthroughOk(const JpegInfo& ji);

bool WritePdf(const fs::path& out, const std::vector<PdfImagePage>& pages, const PdfOptions& opt, std::wstring* err);

} // namespace filo::img
