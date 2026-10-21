// Image encode/decode helpers (WIC).
#pragma once
#include "engines/doc/docutil.h"

namespace filo::doc {

enum class ImgFormat { Png, Jpeg };
// Encodes a top-down BGRA bitmap. JPEG ignores alpha. quality 1..100 (JPEG only).
bool EncodeBgra(const uint8_t* bgra, int width, int height, int stride, ImgFormat fmt, int jpegQuality, Bytes& out);
// Decodes any WIC-supported image to top-down BGRA.
bool DecodeToBgra(const Bytes& in, int& w, int& h, Bytes& bgra);

} // namespace filo::doc
