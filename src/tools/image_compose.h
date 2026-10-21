// Software compositing used by Add Background and Collage: gradient / solid / image backgrounds, rounded corners with true
// anti-aliasing, soft drop shadow. Straight-alpha BGRA in, straight-alpha BGRA out (opaque canvas).
#pragma once
#include "engines/image/bitmap.h"
#include "design/tokens.h"
#include <cstdint>
#include <vector>

namespace filo::tools {

struct BgSpec {
  enum class Kind { Gradient, Solid, Image } kind = Kind::Gradient;
  ds::Color top{1, 1, 1, 1}, bottom{0.56f, 0.85f, 1.f, 1};   // gradient / solid (top used)
  const img::Bitmap* image = nullptr;                         // Kind::Image: scaled to cover the canvas
};

struct AddBgParams {
  BgSpec bg;
  float padding = 0;       // px of the full-resolution canvas around the picture
  float corners = 24;      // px
  float shadow = 30;       // px (blur radius); 0 = none
  int ratio = 0;           // 0 auto, 1 = 1:1, 2 = 16:9, 3 = 4:5
};

// Canvas size for a picture of w x h under these params.
void ComputeCanvasSize(int w, int h, const AddBgParams& p, int& cw, int& ch);

// Renders the picture onto the styled canvas. `scale` multiplies every length (preview <1). Returns false on failure.
bool ComposeAddBackground(const img::Bitmap& src, const AddBgParams& p, float scale, img::Bitmap& out);

} // namespace filo::tools
