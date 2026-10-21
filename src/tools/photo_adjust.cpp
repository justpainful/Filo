#include "tools/photo_adjust.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace filo::tools {

namespace {
inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float Lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
inline float SmoothStep(float e0, float e1, float x) { float t = Clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }

template <class F> void ParallelRows(int h, F f) {
  const unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
  if (h < 64 || n == 1) { f(0, h); return; }
  std::vector<std::thread> th;
  for (unsigned i = 0; i < n; ++i) {
    int y0 = (int)((long long)h * i / n), y1 = (int)((long long)h * (i + 1) / n);
    th.emplace_back([=] { f(y0, y1); });
  }
  for (auto& t : th) t.join();
}

// Separable box blur on a float plane (3 passes ~ gaussian)
void BoxBlurPlane(std::vector<float>& src, int w, int h, int r) {
  if (r < 1) return;
  std::vector<float> tmp(src.size());
  auto pass = [&](const std::vector<float>& in, std::vector<float>& out, bool horiz) {
    const int len = horiz ? w : h, lines = horiz ? h : w;
    ParallelRows(lines, [&](int l0, int l1) {
      for (int l = l0; l < l1; ++l) {
        double sum = 0;
        auto at = [&](int i) -> float { i = std::clamp(i, 0, len - 1); return horiz ? in[(size_t)l * w + i] : in[(size_t)i * w + l]; };
        for (int i = -r; i <= r; ++i) sum += at(i);
        const float inv = 1.f / (2 * r + 1);
        for (int i = 0; i < len; ++i) {
          float v = (float)(sum * inv);
          if (horiz) out[(size_t)l * w + i] = v; else out[(size_t)i * w + l] = v;
          sum += at(i + r + 1) - at(i - r);
        }
      }
    });
  };
  for (int k = 0; k < 2; ++k) { pass(src, tmp, true); pass(tmp, src, false); }
}

struct Matrix3 { float m[9]; };
Matrix3 FilterMatrix(int f) {
  switch (f) {
    case 1: return {{1.15f, -0.08f, -0.07f, -0.05f, 1.15f, -0.10f, -0.05f, -0.10f, 1.15f}};   // vivid
    case 2: return {{1.08f, 0.04f, -0.02f, 0.02f, 1.0f, -0.02f, -0.06f, -0.04f, 0.90f}};      // warm
    case 3: return {{0.92f, 0.0f, 0.08f, 0.0f, 1.0f, 0.04f, 0.04f, 0.04f, 1.12f}};             // cool
    case 4: return {{0.299f, 0.587f, 0.114f, 0.299f, 0.587f, 0.114f, 0.299f, 0.587f, 0.114f}};  // mono
    case 5: return {{0.393f, 0.769f, 0.189f, 0.349f, 0.686f, 0.168f, 0.272f, 0.534f, 0.131f}};  // sepia
    case 6: return {{0.85f, 0.10f, 0.05f, 0.05f, 0.88f, 0.07f, 0.05f, 0.10f, 0.85f}};          // fade (lift added later)
    case 7: return {{1.25f, -0.12f, -0.13f, -0.10f, 1.18f, -0.08f, -0.10f, -0.12f, 1.22f}};     // dramatic
    default: return {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
  }
}
} // namespace

const wchar_t* const* FilterNames(int& count) {
  static const wchar_t* n[] = {L"None", L"Vivid", L"Warm", L"Cool", L"Mono", L"Sepia", L"Fade", L"Dramatic"};
  count = (int)(sizeof n / sizeof *n);
  return n;
}

bool PhotoAdjust::IsIdentity() const { return *this == PhotoAdjust{}; }
bool PhotoAdjust::operator==(const PhotoAdjust& o) const {
  return exposure == o.exposure && brightness == o.brightness && contrast == o.contrast && highlights == o.highlights && shadows == o.shadows &&
         whites == o.whites && blacks == o.blacks && gamma == o.gamma && temperature == o.temperature && tint == o.tint && saturation == o.saturation &&
         vibrance == o.vibrance && sharpness == o.sharpness && clarity == o.clarity && dehaze == o.dehaze && noiseReduction == o.noiseReduction &&
         grain == o.grain && vignette == o.vignette && filter == o.filter && filterAmount == o.filterAmount;
}

void ApplyPhotoAdjust(std::vector<uint8_t>& px, int w, int h, const PhotoAdjust& p, float previewScale, const std::atomic<bool>* cancel) {
  if (p.IsIdentity() || w <= 0 || h <= 0) return;
  auto cancelled = [&] { return cancel && cancel->load(); };
  const size_t n = (size_t)w * h;
  const float diag = std::sqrt((float)w * w + (float)h * h);
  const float rs = std::max(0.05f, previewScale);

  // ---- stage 0: noise reduction (before tone changes so it does not amplify noise)
  if (p.noiseReduction > 0.5f) {
    const float amt = p.noiseReduction / 100.f;
    std::vector<float> pl[3];
    for (int c = 0; c < 3; ++c) pl[c].resize(n);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) pl[c][i] = px[i * 4 + c] / 255.f;
    const int r = std::max(1, (int)std::lround((1.0f + 2.0f * amt) * std::sqrt(rs)));
    std::vector<float> bl[3] = {pl[0], pl[1], pl[2]};
    for (int c = 0; c < 3; ++c) BoxBlurPlane(bl[c], w, h, r);
    ParallelRows(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) {
        size_t i = (size_t)y * w + x;
        // edge-aware: blend toward the blur only where the local difference is small (noise), keep real edges
        float d = std::fabs(pl[0][i] - bl[0][i]) + std::fabs(pl[1][i] - bl[1][i]) + std::fabs(pl[2][i] - bl[2][i]);
        float k = amt * (1.f - SmoothStep(0.05f, 0.30f, d));
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = (uint8_t)std::lround(Clamp01(pl[c][i] + (bl[c][i] - pl[c][i]) * k) * 255.f);
      }
    });
    if (cancelled()) return;
  }

  // ---- stage 1: per-pixel tone & colour
  const float expo = std::pow(2.f, p.exposure);
  const float br = p.brightness / 100.f * 0.30f;
  const float ct = 1.f + p.contrast / 100.f * 0.9f;
  const float hi = p.highlights / 100.f, sh = p.shadows / 100.f, wh = p.whites / 100.f, bk = p.blacks / 100.f;
  const float gam = std::pow(2.f, -p.gamma / 100.f);                   // >0 brightens midtones
  const float temp = p.temperature / 100.f, tint = p.tint / 100.f;
  const float sat = 1.f + p.saturation / 100.f, vib = p.vibrance / 100.f;
  const Matrix3 fm = FilterMatrix(p.filter);
  const float fa = p.filter ? p.filterAmount / 100.f : 0.f;
  const float fadeLift = p.filter == 6 ? 0.06f * fa : 0.f;
  const float dramaticC = p.filter == 7 ? 0.12f * fa : 0.f;
  // dehaze base: dark-channel style haze estimate (single global value is enough for a preview-grade effect)
  float hazeA = 0.f;
  if (std::fabs(p.dehaze) > 0.5f) {
    double sum = 0; size_t cnt = 0;
    for (size_t i = 0; i < n; i += 37) { float m = std::min({px[i * 4] / 255.f, px[i * 4 + 1] / 255.f, px[i * 4 + 2] / 255.f}); sum += m; ++cnt; }
    hazeA = cnt ? (float)(sum / cnt) : 0.f;
  }
  const float dh = p.dehaze / 100.f;

  ParallelRows(h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) {
      uint8_t* q = &px[((size_t)y * w + x) * 4];
      float b = q[0] / 255.f, g = q[1] / 255.f, r = q[2] / 255.f;
      // exposure in (approximately) linear light
      if (p.exposure != 0) {
        r = std::pow(Clamp01(std::pow(r, 2.2f) * expo), 1.f / 2.2f); g = std::pow(Clamp01(std::pow(g, 2.2f) * expo), 1.f / 2.2f); b = std::pow(Clamp01(std::pow(b, 2.2f) * expo), 1.f / 2.2f);
      }
      // white balance
      if (temp != 0 || tint != 0) {
        r *= 1.f + 0.30f * temp + 0.06f * tint; b *= 1.f - 0.30f * temp + 0.06f * tint; g *= 1.f - 0.14f * tint;
        r = Clamp01(r); g = Clamp01(g); b = Clamp01(b);
      }
      // tonal ranges
      float L = Lum(r, g, b);
      if (hi != 0 || sh != 0 || wh != 0 || bk != 0) {
        const float shMask = (1.f - L) * (1.f - L), hiMask = L * L;
        float d = sh * 0.45f * shMask * (1.f - L) + hi * 0.45f * hiMask * L;
        d += wh * 0.30f * SmoothStep(0.55f, 1.f, L) + bk * 0.30f * (1.f - SmoothStep(0.f, 0.45f, L)) ;
        r = Clamp01(r + d); g = Clamp01(g + d); b = Clamp01(b + d);
      }
      if (br != 0) { r = Clamp01(r + br); g = Clamp01(g + br); b = Clamp01(b + br); }
      if (ct != 1.f) { r = Clamp01((r - 0.5f) * ct + 0.5f); g = Clamp01((g - 0.5f) * ct + 0.5f); b = Clamp01((b - 0.5f) * ct + 0.5f); }
      if (p.gamma != 0) { r = std::pow(r, gam); g = std::pow(g, gam); b = std::pow(b, gam); }
      if (dh != 0) {
        const float k = dh > 0 ? hazeA * dh * 0.9f : 0.f;
        const float c2 = 1.f + std::fabs(dh) * 0.25f * (dh > 0 ? 1.f : -1.f);
        r = (r - k) / (1.f - k); g = (g - k) / (1.f - k); b = (b - k) / (1.f - k);
        r = Clamp01((r - 0.5f) * c2 + 0.5f); g = Clamp01((g - 0.5f) * c2 + 0.5f); b = Clamp01((b - 0.5f) * c2 + 0.5f);
      }
      // saturation / vibrance
      if (sat != 1.f || vib != 0.f) {
        L = Lum(r, g, b);
        float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        float cur = mx > 0 ? (mx - mn) / mx : 0.f;
        float s = sat * (1.f + vib * (1.f - cur));      // low-saturation pixels get more of the vibrance boost
        r = Clamp01(L + (r - L) * s); g = Clamp01(L + (g - L) * s); b = Clamp01(L + (b - L) * s);
      }
      if (fa > 0) {
        float nr = fm.m[0] * r + fm.m[1] * g + fm.m[2] * b, ng = fm.m[3] * r + fm.m[4] * g + fm.m[5] * b, nb = fm.m[6] * r + fm.m[7] * g + fm.m[8] * b;
        nr = nr + fadeLift; ng = ng + fadeLift; nb = nb + fadeLift;
        if (dramaticC > 0) { nr = (nr - 0.5f) * (1.f + dramaticC * 2) + 0.5f; ng = (ng - 0.5f) * (1.f + dramaticC * 2) + 0.5f; nb = (nb - 0.5f) * (1.f + dramaticC * 2) + 0.5f; }
        r = Clamp01(r + (nr - r) * fa); g = Clamp01(g + (ng - g) * fa); b = Clamp01(b + (nb - b) * fa);
      }
      q[0] = (uint8_t)(b * 255.f + 0.5f); q[1] = (uint8_t)(g * 255.f + 0.5f); q[2] = (uint8_t)(r * 255.f + 0.5f);
    }
  });
  if (cancelled()) return;

  // ---- stage 2: local contrast (clarity) and sharpness via unsharp masking on luminance
  auto unsharp = [&](float amount, float radiusFrac, float rsc) {
    if (std::fabs(amount) < 0.5f) return;
    std::vector<float> Lp(n), Bp;
    for (size_t i = 0; i < n; ++i) Lp[i] = Lum(px[i * 4 + 2] / 255.f, px[i * 4 + 1] / 255.f, px[i * 4] / 255.f);
    Bp = Lp;
    const int r = std::max(1, (int)std::lround(radiusFrac * diag * rsc));
    BoxBlurPlane(Bp, w, h, r);
    const float a = amount / 100.f;
    ParallelRows(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) {
        size_t i = (size_t)y * w + x;
        float d = (Lp[i] - Bp[i]) * a * 1.4f;
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = (uint8_t)std::lround(Clamp01(px[i * 4 + c] / 255.f + d) * 255.f);
      }
    });
  };
  unsharp(p.clarity, 0.012f, 1.f);          // wide radius: local contrast
  if (cancelled()) return;
  unsharp(p.sharpness, 0.0012f, 1.f);        // small radius: edge sharpening
  (void)rs;

  // ---- stage 3: vignette and grain
  if (p.vignette != 0 || p.grain > 0.5f) {
    const float vg = p.vignette / 100.f, grainAmt = p.grain / 100.f * 0.12f;
    const float cx = w / 2.f, cy = h / 2.f, maxd = std::sqrt(cx * cx + cy * cy);
    ParallelRows(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* q = &px[((size_t)y * w + x) * 4];
        float v = 1.f;
        if (vg != 0) {
          float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxd;
          float m = SmoothStep(0.35f, 1.05f, d);
          v = vg > 0 ? 1.f - vg * 0.85f * m : 1.f;
          float add = vg < 0 ? -vg * 0.55f * m : 0.f;
          for (int c = 0; c < 3; ++c) q[c] = (uint8_t)std::lround(Clamp01(q[c] / 255.f * v + add) * 255.f);
        }
        if (grainAmt > 0) {
          uint32_t hsh = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u); hsh ^= hsh >> 13; hsh *= 1274126177u; hsh ^= hsh >> 16;
          float nse = ((hsh & 0xFFFF) / 65535.f - 0.5f) * 2.f * grainAmt;
          for (int c = 0; c < 3; ++c) q[c] = (uint8_t)std::lround(Clamp01(q[c] / 255.f + nse) * 255.f);
        }
      }
    });
  }
}

} // namespace filo::tools
