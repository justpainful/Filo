#include "tools/image_compose.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace filo::tools {

namespace {
inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

float RoundedBoxSdf(float px, float py, float cx, float cy, float hw, float hh, float r) {
  const float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
  return std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
}

void BoxBlur(std::vector<float>& a, int w, int h, int r) {
  if (r < 1) return;
  std::vector<float> t(a.size());
  auto pass = [&](const std::vector<float>& in, std::vector<float>& out, bool horiz) {
    const int len = horiz ? w : h, lines = horiz ? h : w;
    for (int l = 0; l < lines; ++l) {
      double sum = 0;
      auto at = [&](int i) { i = std::clamp(i, 0, len - 1); return horiz ? in[(size_t)l * w + i] : in[(size_t)i * w + l]; };
      for (int i = -r; i <= r; ++i) sum += at(i);
      const float inv = 1.f / (2 * r + 1);
      for (int i = 0; i < len; ++i) {
        float v = (float)(sum * inv);
        if (horiz) out[(size_t)l * w + i] = v; else out[(size_t)i * w + l] = v;
        sum += at(i + r + 1) - at(i - r);
      }
    }
  };
  for (int k = 0; k < 3; ++k) { pass(a, t, true); pass(t, a, false); }
}
} // namespace

void ComputeCanvasSize(int w, int h, const AddBgParams& p, int& cw, int& ch) {
  const float pad = std::max(0.f, p.padding);
  float W = w + 2 * pad, H = h + 2 * pad;
  static const float ratios[] = {0.f, 1.f, 16.f / 9.f, 4.f / 5.f};
  const float R = ratios[std::clamp(p.ratio, 0, 3)];
  if (R > 0) { if (W / H < R) W = H * R; else H = W / R; }
  cw = std::max(1, (int)std::lround(W)); ch = std::max(1, (int)std::lround(H));
}

bool ComposeAddBackground(const img::Bitmap& src, const AddBgParams& p0, float scale, img::Bitmap& out) {
  if (src.Empty() || scale <= 0) return false;
  // sizes in "full" units, then scaled
  int cwF, chF; ComputeCanvasSize(src.info.fullWidth > 0 ? src.info.fullWidth : src.width, src.info.fullHeight > 0 ? src.info.fullHeight : src.height, p0, cwF, chF);
  const int cw = std::max(1, (int)std::lround(cwF * scale)), ch = std::max(1, (int)std::lround(chF * scale));
  out.Allocate(cw, ch);
  // picture rectangle: the picture keeps its aspect and is centred; with a fixed ratio it shrinks to leave the padding on the tight axis
  const float fw = (float)(src.info.fullWidth > 0 ? src.info.fullWidth : src.width), fh = (float)(src.info.fullHeight > 0 ? src.info.fullHeight : src.height);
  float pw = fw * scale, ph = fh * scale;
  const float padS = p0.padding * scale;
  const float availW = cw - 2 * padS, availH = ch - 2 * padS;
  if (availW > 1 && availH > 1) { const float k = std::min(1.f, std::min(availW / pw, availH / ph)); pw *= k; ph *= k; }
  const float x0 = (cw - pw) / 2, y0 = (ch - ph) / 2;
  const float radius = std::min(std::max(0.f, p0.corners * scale), std::min(pw, ph) * 0.5f);

  // ---- background
  for (int y = 0; y < ch; ++y) {
    uint8_t* row = out.Row(y);
    const float t = ch > 1 ? (float)y / (ch - 1) : 0.f;
    for (int x = 0; x < cw; ++x) {
      float r, g, b;
      if (p0.bg.kind == BgSpec::Kind::Image && p0.bg.image && !p0.bg.image->Empty()) {
        const img::Bitmap& im = *p0.bg.image;
        const float k = std::max((float)cw / im.width, (float)ch / im.height);
        const float sx = (x - cw / 2.f) / k + im.width / 2.f, sy = (y - ch / 2.f) / k + im.height / 2.f;
        const int ix = std::clamp((int)sx, 0, im.width - 1), iy = std::clamp((int)sy, 0, im.height - 1);
        const uint8_t* s = im.Row(iy) + ix * 4;
        b = s[0] / 255.f; g = s[1] / 255.f; r = s[2] / 255.f;
      } else if (p0.bg.kind == BgSpec::Kind::Solid) {
        r = p0.bg.top.r; g = p0.bg.top.g; b = p0.bg.top.b;
      } else {
        const float tt = Clamp01(t * 0.85f + (float)x / std::max(1, cw - 1) * 0.15f);
        r = p0.bg.top.r + (p0.bg.bottom.r - p0.bg.top.r) * tt; g = p0.bg.top.g + (p0.bg.bottom.g - p0.bg.top.g) * tt; b = p0.bg.top.b + (p0.bg.bottom.b - p0.bg.top.b) * tt;
      }
      uint8_t* q = row + x * 4;
      q[0] = (uint8_t)(Clamp01(b) * 255.f + 0.5f); q[1] = (uint8_t)(Clamp01(g) * 255.f + 0.5f); q[2] = (uint8_t)(Clamp01(r) * 255.f + 0.5f); q[3] = 255;
    }
  }

  // ---- soft shadow (blurred rounded rect, offset down like the reference)
  const float sh = std::max(0.f, p0.shadow * scale);
  if (sh > 0.5f) {
    const int blurR = std::max(1, (int)std::lround(sh * 0.5f));
    const float off = sh * 0.35f;
    std::vector<float> mask((size_t)cw * ch, 0.f);
    const int ya = std::max(0, (int)(y0 + off - blurR * 3)), yb = std::min(ch, (int)(y0 + off + ph + blurR * 3));
    const int xa = std::max(0, (int)(x0 - blurR * 3)), xb = std::min(cw, (int)(x0 + pw + blurR * 3));
    for (int y = ya; y < yb; ++y) for (int x = xa; x < xb; ++x) {
      float d = RoundedBoxSdf(x + 0.5f, y + 0.5f, x0 + pw / 2, y0 + off + ph / 2, pw / 2, ph / 2, radius);
      mask[(size_t)y * cw + x] = Clamp01(0.5f - d);
    }
    BoxBlur(mask, cw, ch, blurR);
    const float alpha = 0.38f;
    for (int y = ya; y < yb; ++y) for (int x = xa; x < xb; ++x) {
      const float a = mask[(size_t)y * cw + x] * alpha;
      if (a <= 0.001f) continue;
      uint8_t* q = out.Row(y) + x * 4;
      for (int c = 0; c < 3; ++c) q[c] = (uint8_t)(q[c] * (1.f - a));
    }
  }

  // ---- the picture with rounded corners (bilinear resample + SDF edge coverage)
  const int ya = std::max(0, (int)std::floor(y0) - 1), yb = std::min(ch, (int)std::ceil(y0 + ph) + 1);
  const int xa = std::max(0, (int)std::floor(x0) - 1), xb = std::min(cw, (int)std::ceil(x0 + pw) + 1);
  const float sxk = src.width / std::max(pw, 1.f), syk = src.height / std::max(ph, 1.f);
  for (int y = ya; y < yb; ++y) {
    uint8_t* row = out.Row(y);
    for (int x = xa; x < xb; ++x) {
      const float d = RoundedBoxSdf(x + 0.5f, y + 0.5f, x0 + pw / 2, y0 + ph / 2, pw / 2, ph / 2, radius);
      const float cov = Clamp01(0.5f - d);
      if (cov <= 0.f) continue;
      float fx = (x + 0.5f - x0) * sxk - 0.5f, fy = (y + 0.5f - y0) * syk - 0.5f;
      fx = std::clamp(fx, 0.f, (float)(src.width - 1)); fy = std::clamp(fy, 0.f, (float)(src.height - 1));
      const int ix = (int)fx, iy = (int)fy, jx = std::min(ix + 1, src.width - 1), jy = std::min(iy + 1, src.height - 1);
      const float tx = fx - ix, ty = fy - iy;
      uint8_t* q = row + x * 4;
      const uint8_t* s00 = src.Row(iy) + ix * 4; const uint8_t* s10 = src.Row(iy) + jx * 4;
      const uint8_t* s01 = src.Row(jy) + ix * 4; const uint8_t* s11 = src.Row(jy) + jx * 4;
      float a = ((s00[3] * (1 - tx) + s10[3] * tx) * (1 - ty) + (s01[3] * (1 - tx) + s11[3] * tx) * ty) / 255.f * cov;
      for (int c = 0; c < 3; ++c) {
        const float v = ((s00[c] * (1 - tx) + s10[c] * tx) * (1 - ty) + (s01[c] * (1 - tx) + s11[c] * tx) * ty);
        q[c] = (uint8_t)std::lround(q[c] * (1.f - a) + v * a);
      }
    }
  }
  return true;
}

} // namespace filo::tools
