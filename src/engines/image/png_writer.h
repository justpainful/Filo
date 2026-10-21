// Own PNG encoder (miniz deflate) with adaptive row filters, lossless palette detection, optional lossy palette
// quantisation (median cut + Floyd-Steinberg) and eXIf / iTXt(XMP) / iCCP chunks. Also exposes the PNG row-filter pass
// that the PDF writer reuses for Flate predictors.
#pragma once
#include "engines/image/bitmap.h"

namespace filo::img {

struct PngOptions {
  int level = 6;                 // 1..9 (deflate effort)
  bool quantize = false;         // lossy palette when the image has more than maxColors colours
  int maxColors = 256;           // 2..256
  bool dither = true;
  bool srgbChunk = false;        // write an sRGB chunk (when no ICC profile is written)
  const Metadata* meta = nullptr;// EXIF / XMP / ICC to embed (nullptr: none)
};

// Encodes `bm`. Chooses the smallest lossless colour type automatically (gray / RGB / RGBA / exact palette).
bool EncodePng(const Bitmap& bm, const PngOptions& opt, Bytes& out, std::wstring* err = nullptr);

struct QuantResult {
  std::vector<uint32_t> palette;     // BGRA (little endian: B | G<<8 | R<<16 | A<<24), size <= maxColors
  std::vector<uint8_t> indices;      // width*height
  bool exact = false;                // palette holds every colour of the source (lossless)
};
// Median-cut quantiser. When the image has <= maxColors distinct colours the palette is exact.
bool QuantizeBitmap(const Bitmap& bm, int maxColors, bool dither, QuantResult& out);

// Applies PNG filter selection (min sum of absolute differences per row). `raw` = h rows of `rowBytes`; output rows get a
// leading filter-type byte. bpp = bytes per pixel (>=1).
Bytes PngFilterRows(const uint8_t* raw, int rowBytes, int h, int bpp);

// zlib-compress helper (level 1..9)
bool ZlibCompress(const uint8_t* data, size_t n, int level, Bytes& out);

} // namespace filo::img
