#include "gfx/backdrop.h"
#include <algorithm>
#include <cmath>

namespace filo::gfx {

static void BoxBlurH(const uint32_t* src, uint32_t* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int y = 0; y < h; ++y) {
    const uint32_t* s = src + (size_t)y * w;
    uint32_t* d = dst + (size_t)y * w;
    int64_t sum[4] = {0, 0, 0, 0};
    auto add = [&](int x, int sgn) {
      uint32_t c = s[std::clamp(x, 0, w - 1)];
      sum[0] += sgn * (int)(c & 255); sum[1] += sgn * (int)((c >> 8) & 255); sum[2] += sgn * (int)((c >> 16) & 255); sum[3] += sgn * (int)(c >> 24);
    };
    for (int x = -r; x <= r; ++x) add(x, 1);
    for (int x = 0; x < w; ++x) {
      d[x] = (uint32_t)(sum[0] / win) | ((uint32_t)(sum[1] / win) << 8) | ((uint32_t)(sum[2] / win) << 16) | ((uint32_t)(sum[3] / win) << 24);
      add(x + r + 1, 1);
      add(x - r, -1);
    }
  }
}
static void BoxBlurV(const uint32_t* src, uint32_t* dst, int w, int h, int r) {
  const int win = 2 * r + 1;
  for (int x = 0; x < w; ++x) {
    int64_t sum[4] = {0, 0, 0, 0};
    auto add = [&](int y, int sgn) {
      uint32_t c = src[(size_t)std::clamp(y, 0, h - 1) * w + x];
      sum[0] += sgn * (int)(c & 255); sum[1] += sgn * (int)((c >> 8) & 255); sum[2] += sgn * (int)((c >> 16) & 255); sum[3] += sgn * (int)(c >> 24);
    };
    for (int y = -r; y <= r; ++y) add(y, 1);
    for (int y = 0; y < h; ++y) {
      dst[(size_t)y * w + x] = (uint32_t)(sum[0] / win) | ((uint32_t)(sum[1] / win) << 8) | ((uint32_t)(sum[2] / win) << 16) | ((uint32_t)(sum[3] / win) << 24);
      add(y + r + 1, 1);
      add(y - r, -1);
    }
  }
}

void BlurBgra(std::vector<uint32_t>& px, int w, int h, float sigma) {
  if (w < 2 || h < 2 || sigma < 0.5f) return;
  // 3 box passes approximate a gaussian: box radius r ~ sigma * sqrt(12/3) / 2
  int r = std::max(1, (int)std::lround(sigma * 1.0f));
  std::vector<uint32_t> tmp(px.size());
  for (int i = 0; i < 3; ++i) {
    BoxBlurH(px.data(), tmp.data(), w, h, r);
    BoxBlurV(tmp.data(), px.data(), w, h, r);
  }
}

} // namespace filo::gfx
