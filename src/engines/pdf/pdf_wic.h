// WIC helpers private to the PDF engine (image encode/decode for page export, compression and image->PDF).
#pragma once
#include "core/util.h"
#include <cstdint>
#include <string>
#include <vector>

namespace filo::pdf::wic {

// Initialises COM on the calling thread for the lifetime of the object (tolerates an already initialised apartment).
class ComScope {
 public:
  ComScope();
  ~ComScope();
  bool ok() const { return ok_; }
 private:
  bool ok_ = false, uninit_ = false;
};

// bgra: top-down, stride = w*4. hasAlpha=false writes a 24-bit PNG.
bool EncodePng(const fs::path& out, const uint8_t* bgra, int w, int h, bool hasAlpha, double dpi, std::wstring* err);
// Opaque pixels only (alpha ignored). 4:4:4 chroma for quality >= 90.
bool EncodeJpeg(const fs::path& out, const uint8_t* bgra, int w, int h, int quality, double dpi, std::wstring* err);
bool EncodeJpegMemory(std::vector<uint8_t>& out, const uint8_t* bgra, int w, int h, int quality, bool gray = false);

struct Decoded {
  int w = 0, h = 0;
  std::vector<uint8_t> bgra;          // straight alpha
  int orientation = 1;                // EXIF orientation 1..8 (1 = upright)
  bool isJpeg = false;
  bool cmyk = false;
  bool hasAlpha = false;
  double dpiX = 0, dpiY = 0;
  int frames = 1;
};
// Reads only size/format/orientation (no pixels).
bool Probe(const fs::path& in, Decoded& out, std::wstring* err);
// Full decode of frame 0 to BGRA (EXIF orientation is reported, NOT applied).
bool Decode(const fs::path& in, Decoded& out, std::wstring* err);
// Decodes an encoded image held in memory (JPEG/PNG/...) to BGRA.
bool DecodeMemory(const uint8_t* data, size_t size, Decoded& out, std::wstring* err);
// Downscale (box filter) to exactly w2 x h2.
std::vector<uint8_t> Downscale(const uint8_t* bgra, int w, int h, int w2, int h2);

} // namespace filo::pdf::wic
