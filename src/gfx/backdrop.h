// Bitmap backdrop fixtures for hidden visual regression renders.
// Visible windows use LiveComposition; this API cannot capture the desktop.
#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>

namespace filo::gfx {

struct BackdropImage {
  RECT screenRect{};                 // physical pixels (virtual-screen coordinates) that were captured
  int w = 0, h = 0;
  std::vector<uint32_t> sharp;       // BGRA, w*h
  int bw = 0, bh = 0;                // blurred copy is stored at reduced resolution
  std::vector<uint32_t> blurred;     // BGRA, bw*bh
  bool valid() const { return w > 0 && h > 0 && !sharp.empty(); }
  bool validBlur() const { return bw > 0 && bh > 0 && !blurred.empty(); }
};

// In-place separable box-blur x3 (~gaussian). Exposed for tests.
void BlurBgra(std::vector<uint32_t>& px, int w, int h, float sigma);

} // namespace filo::gfx
