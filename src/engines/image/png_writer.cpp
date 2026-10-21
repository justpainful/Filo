#include "engines/image/png_writer.h"
#include "miniz.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace filo::img {

bool ZlibCompress(const uint8_t* data, size_t n, int level, Bytes& out) {
  mz_ulong bound = mz_compressBound((mz_ulong)n);
  out.resize(bound);
  mz_ulong dl = bound;
  int r = mz_compress2(out.data(), &dl, data, (mz_ulong)n, std::clamp(level, 1, 10));
  if (r != MZ_OK) return false;
  out.resize(dl);
  return true;
}

Bytes PngFilterRows(const uint8_t* raw, int rowBytes, int h, int bpp) {
  Bytes out((size_t)h * (rowBytes + 1));
  std::vector<uint8_t> cand[5];
  for (auto& c : cand) c.resize(rowBytes);
  std::vector<uint8_t> zero(rowBytes, 0);
  for (int y = 0; y < h; ++y) {
    const uint8_t* cur = raw + (size_t)y * rowBytes;
    const uint8_t* prev = y ? raw + (size_t)(y - 1) * rowBytes : zero.data();
    for (int i = 0; i < rowBytes; ++i) {
      int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      cand[0][i] = cur[i];
      cand[1][i] = (uint8_t)(cur[i] - a);
      cand[2][i] = (uint8_t)(cur[i] - b);
      cand[3][i] = (uint8_t)(cur[i] - ((a + b) >> 1));
      int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
      int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      cand[4][i] = (uint8_t)(cur[i] - pr);
    }
    int best = 0; uint64_t bestSum = ~0ull;
    for (int f = 0; f < 5; ++f) {
      uint64_t s = 0;
      for (int i = 0; i < rowBytes; ++i) { int v = (int8_t)cand[f][i]; s += (uint64_t)(v < 0 ? -v : v); }
      if (s < bestSum) { bestSum = s; best = f; }
    }
    uint8_t* o = out.data() + (size_t)y * (rowBytes + 1);
    o[0] = (uint8_t)best;
    memcpy(o + 1, cand[best].data(), rowBytes);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// colour analysis + quantiser
// ---------------------------------------------------------------------------------------------------------------------
static inline uint32_t PixelAt(const Bitmap& bm, size_t i) { uint32_t v; memcpy(&v, bm.px.data() + i * 4, 4); return v; }

// Returns true when the image has <= limit distinct colours (list filled in first-seen order).
static bool DistinctColors(const Bitmap& bm, int limit, std::vector<uint32_t>& list) {
  std::unordered_map<uint32_t, int> seen; seen.reserve(512);
  size_t n = (size_t)bm.width * bm.height;
  uint32_t last = 0; bool haveLast = false;
  for (size_t i = 0; i < n; ++i) {
    uint32_t v = PixelAt(bm, i);
    if (haveLast && v == last) continue;
    last = v; haveLast = true;
    if (seen.emplace(v, (int)list.size()).second) {
      list.push_back(v);
      if ((int)list.size() > limit) return false;
    }
  }
  return true;
}

namespace {
struct Q {
  bool alpha;
  int Key(int r, int g, int b, int a) const {
    int ak = alpha ? (a == 0 ? 0 : 1 + (a * 14 / 255)) : 15;
    return ((r >> 3) << 14) | ((g >> 3) << 9) | ((b >> 3) << 4) | ak;
  }
};
struct Bin { int key; uint64_t n = 0, r = 0, g = 0, b = 0, a = 0; int cr, cg, cb, ca; };
struct Box { std::vector<int> bins; uint64_t count = 0; int lo[4], hi[4]; };
}

bool QuantizeBitmap(const Bitmap& bm, int maxColors, bool dither, QuantResult& out) {
  maxColors = std::clamp(maxColors, 2, 256);
  size_t n = (size_t)bm.width * bm.height;
  if (!n) return false;
  std::vector<uint32_t> distinct;
  if (DistinctColors(bm, maxColors, distinct)) {
    out.palette = distinct; out.exact = true;
    std::unordered_map<uint32_t, uint8_t> idx; for (size_t i = 0; i < distinct.size(); ++i) idx[distinct[i]] = (uint8_t)i;
    out.indices.resize(n);
    uint32_t last = 0; uint8_t lastIdx = 0; bool have = false;
    for (size_t i = 0; i < n; ++i) { uint32_t v = PixelAt(bm, i); if (!have || v != last) { last = v; lastIdx = idx[v]; have = true; } out.indices[i] = lastIdx; }
    return true;
  }
  out.exact = false;
  Q q{UsesAlpha(bm)};
  std::vector<int32_t> compact(1 << 19, -1);
  std::vector<Bin> bins;
  const uint8_t* p = bm.px.data();
  for (size_t i = 0; i < n; ++i, p += 4) {
    int key = q.Key(p[2], p[1], p[0], p[3]);
    int32_t& c = compact[key];
    if (c < 0) { c = (int32_t)bins.size(); bins.emplace_back(); bins.back().key = key; }
    Bin& b = bins[c]; ++b.n; b.r += p[2]; b.g += p[1]; b.b += p[0]; b.a += p[3];
  }
  for (auto& b : bins) { b.cr = (int)(b.r / b.n); b.cg = (int)(b.g / b.n); b.cb = (int)(b.b / b.n); b.ca = (int)(b.a / b.n); }
  auto chan = [&](const Bin& b, int c) { return c == 0 ? b.cr : c == 1 ? b.cg : c == 2 ? b.cb : b.ca; };
  auto fit = [&](Box& bx) {
    for (int c = 0; c < 4; ++c) { bx.lo[c] = 255; bx.hi[c] = 0; }
    bx.count = 0;
    for (int bi : bx.bins) { const Bin& b = bins[bi]; bx.count += b.n; for (int c = 0; c < 4; ++c) { int v = chan(b, c); bx.lo[c] = std::min(bx.lo[c], v); bx.hi[c] = std::max(bx.hi[c], v); } }
  };
  std::vector<Box> boxes(1);
  for (int i = 0; i < (int)bins.size(); ++i) boxes[0].bins.push_back(i);
  fit(boxes[0]);
  const double wt[4] = {1.0, 1.0, 1.0, q.alpha ? 1.0 : 0.0};
  while ((int)boxes.size() < maxColors) {
    int bestBox = -1; double bestScore = 0;
    for (int i = 0; i < (int)boxes.size(); ++i) {
      if (boxes[i].bins.size() < 2) continue;
      double range = 0; for (int c = 0; c < 4; ++c) range = std::max(range, (boxes[i].hi[c] - boxes[i].lo[c]) * wt[c]);
      double score = range * std::sqrt((double)boxes[i].count);
      if (score > bestScore) { bestScore = score; bestBox = i; }
    }
    if (bestBox < 0) break;
    Box& bx = boxes[bestBox];
    int ch = 0; double br = -1;
    for (int c = 0; c < 4; ++c) { double r = (bx.hi[c] - bx.lo[c]) * wt[c]; if (r > br) { br = r; ch = c; } }
    std::sort(bx.bins.begin(), bx.bins.end(), [&](int a, int b) { return chan(bins[a], ch) < chan(bins[b], ch); });
    uint64_t half = bx.count / 2, acc = 0; size_t cut = 1;
    for (size_t i = 0; i < bx.bins.size(); ++i) { acc += bins[bx.bins[i]].n; if (acc >= half) { cut = std::clamp<size_t>(i + 1, 1, bx.bins.size() - 1); break; } }
    Box nb; nb.bins.assign(bx.bins.begin() + cut, bx.bins.end());
    bx.bins.resize(cut);
    fit(bx); fit(nb);
    boxes.push_back(std::move(nb));
  }
  out.palette.clear();
  for (auto& bx : boxes) {
    uint64_t cnt = 0, r = 0, g = 0, b = 0, a = 0;
    for (int bi : bx.bins) { cnt += bins[bi].n; r += bins[bi].r; g += bins[bi].g; b += bins[bi].b; a += bins[bi].a; }
    if (!cnt) continue;
    uint32_t pr = (uint32_t)(r / cnt), pg = (uint32_t)(g / cnt), pb = (uint32_t)(b / cnt), pa = (uint32_t)(a / cnt);
    if (!q.alpha) pa = 255;
    out.palette.push_back(pb | (pg << 8) | (pr << 16) | (pa << 24));
  }
  // if the image has fully transparent pixels make sure one palette entry is exactly transparent
  if (q.alpha) {
    int bestT = -1; int bestA = 256;
    for (size_t i = 0; i < out.palette.size(); ++i) { int a = out.palette[i] >> 24; if (a < bestA) { bestA = a; bestT = (int)i; } }
    bool hasTransparentPixels = false;
    for (auto& b : bins) if (b.ca == 0 || (b.key & 15) == 0) { hasTransparentPixels = true; break; }
    if (hasTransparentPixels && bestT >= 0) out.palette[bestT] = 0;
  }
  // mapping with a lazily filled LUT
  std::vector<int16_t> lut(1 << 19, -1);
  auto nearest = [&](int r, int g, int b, int a) -> int {
    int key = q.Key(r, g, b, a);
    if (lut[key] >= 0) return lut[key];
    long best = LONG_MAX; int bi = 0;
    for (size_t i = 0; i < out.palette.size(); ++i) {
      uint32_t c = out.palette[i];
      long db = (int)(c & 255) - b, dg = (int)((c >> 8) & 255) - g, dr = (int)((c >> 16) & 255) - r, da = q.alpha ? (int)(c >> 24) - a : 0;
      long d = dr * dr * 3 + dg * dg * 4 + db * db * 2 + da * da * 3;
      if (d < best) { best = d; bi = (int)i; }
    }
    lut[key] = (int16_t)bi;
    return bi;
  };
  out.indices.assign(n, 0);
  int w = bm.width, h = bm.height;
  if (!dither) {
    const uint8_t* s = bm.px.data();
    for (size_t i = 0; i < n; ++i, s += 4) out.indices[i] = (uint8_t)nearest(s[2], s[1], s[0], s[3]);
  } else {
    std::vector<int> errCur((size_t)(w + 2) * 3, 0), errNext((size_t)(w + 2) * 3, 0);
    for (int y = 0; y < h; ++y) {
      std::fill(errNext.begin(), errNext.end(), 0);
      bool ltr = (y & 1) == 0;
      for (int step = 0; step < w; ++step) {
        int x = ltr ? step : w - 1 - step;
        const uint8_t* s = bm.px.data() + ((size_t)y * w + x) * 4;
        int a = s[3];
        int r = s[2], g = s[1], b = s[0];
        if (a != 0) {
          r = std::clamp(r + errCur[(x + 1) * 3 + 0] / 16, 0, 255);
          g = std::clamp(g + errCur[(x + 1) * 3 + 1] / 16, 0, 255);
          b = std::clamp(b + errCur[(x + 1) * 3 + 2] / 16, 0, 255);
        }
        int idx = nearest(r, g, b, a);
        out.indices[(size_t)y * w + x] = (uint8_t)idx;
        if (a != 0) {
          uint32_t c = out.palette[idx];
          int e[3] = {r - (int)((c >> 16) & 255), g - (int)((c >> 8) & 255), b - (int)(c & 255)};
          int dir = ltr ? 1 : -1;
          for (int k = 0; k < 3; ++k) {
            errCur[(x + 1 + dir) * 3 + k] += e[k] * 7;
            errNext[(x + 1 - dir) * 3 + k] += e[k] * 3;
            errNext[(x + 1) * 3 + k] += e[k] * 5;
            errNext[(x + 1 + dir) * 3 + k] += e[k] * 1;
          }
        }
      }
      errCur.swap(errNext);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// PNG writer
// ---------------------------------------------------------------------------------------------------------------------
static void PutBe32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void Chunk(Bytes& out, const char* type, const uint8_t* d, size_t n) {
  PutBe32(out, (uint32_t)n);
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  if (n) out.insert(out.end(), d, d + n);
  uint32_t crc = Crc32(out.data() + start, out.size() - start);
  PutBe32(out, crc);
}

bool EncodePng(const Bitmap& bm, const PngOptions& opt, Bytes& out, std::wstring* err) {
  if (bm.Empty()) { if (err) *err = L"There is no image to save."; return false; }
  int w = bm.width, h = bm.height;
  size_t n = (size_t)w * h;
  bool alpha = UsesAlpha(bm);
  // analysis
  bool gray = true;
  { const uint8_t* p = bm.px.data(); for (size_t i = 0; i < n; ++i, p += 4) if (p[0] != p[1] || p[1] != p[2]) { gray = false; break; } }

  Bytes ihdr; PutBe32(ihdr, w); PutBe32(ihdr, h);
  int colorType = 6, bitDepth = 8, bpp = 4;
  Bytes raw, plte, trns;
  QuantResult qr;
  bool usePalette = false;
  if (opt.quantize || !gray || alpha) {
    // palette if it is exactly representable, or when lossy quantisation was requested
    std::vector<uint32_t> probe;
    bool few = DistinctColors(bm, 256, probe);
    if (few || opt.quantize) { if (QuantizeBitmap(bm, opt.quantize ? opt.maxColors : 256, opt.dither && !few, qr)) usePalette = true; }
  } else {
    // gray image: palette only if it is tiny (<=16 shades) - bit packing wins
    std::vector<uint32_t> probe;
    if (DistinctColors(bm, 16, probe)) { if (QuantizeBitmap(bm, 16, false, qr)) usePalette = true; }
  }
  if (usePalette && gray && !alpha && qr.palette.size() > 16) usePalette = false;   // plain 8-bit gray is simpler
  if (usePalette) {
    colorType = 3;
    int cnt = (int)qr.palette.size();
    bitDepth = cnt <= 2 ? 1 : cnt <= 4 ? 2 : cnt <= 16 ? 4 : 8;
    for (uint32_t c : qr.palette) { plte.push_back((c >> 16) & 255); plte.push_back((c >> 8) & 255); plte.push_back(c & 255); trns.push_back(c >> 24); }
    while (!trns.empty() && trns.back() == 255) trns.pop_back();
    int rowBytes = (w * bitDepth + 7) / 8;
    raw.assign((size_t)rowBytes * h, 0);
    for (int y = 0; y < h; ++y) {
      uint8_t* r = raw.data() + (size_t)y * rowBytes;
      const uint8_t* idx = qr.indices.data() + (size_t)y * w;
      if (bitDepth == 8) memcpy(r, idx, w);
      else for (int x = 0; x < w; ++x) r[(x * bitDepth) >> 3] |= (uint8_t)(idx[x] << (8 - bitDepth - ((x * bitDepth) & 7)));
    }
    bpp = 1;
  } else if (gray) {
    colorType = alpha ? 4 : 0; bpp = alpha ? 2 : 1;
    raw.resize(n * bpp);
    const uint8_t* p = bm.px.data(); uint8_t* o = raw.data();
    for (size_t i = 0; i < n; ++i, p += 4) { *o++ = p[2]; if (alpha) *o++ = p[3]; }
  } else if (!alpha) {
    colorType = 2; bpp = 3;
    raw.resize(n * 3);
    const uint8_t* p = bm.px.data(); uint8_t* o = raw.data();
    for (size_t i = 0; i < n; ++i, p += 4) { *o++ = p[2]; *o++ = p[1]; *o++ = p[0]; }
  } else {
    colorType = 6; bpp = 4;
    raw.resize(n * 4);
    const uint8_t* p = bm.px.data(); uint8_t* o = raw.data();
    for (size_t i = 0; i < n; ++i, p += 4) { *o++ = p[2]; *o++ = p[1]; *o++ = p[0]; *o++ = p[3]; }
  }
  ihdr.push_back((uint8_t)bitDepth); ihdr.push_back((uint8_t)colorType); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);

  int rowBytes = (int)(raw.size() / h);
  Bytes filtered = PngFilterRows(raw.data(), rowBytes, h, bitDepth < 8 ? 1 : bpp);
  Bytes z;
  if (!ZlibCompress(filtered.data(), filtered.size(), opt.level, z)) { if (err) *err = L"Compression failed."; return false; }

  out.clear();
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.insert(out.end(), sig, sig + 8);
  Chunk(out, "IHDR", ihdr.data(), ihdr.size());
  const Metadata* m = opt.meta;
  if (m && !m->icc.empty()) {
    Bytes ic; const char* nm = "ICC profile"; ic.insert(ic.end(), nm, nm + strlen(nm)); ic.push_back(0); ic.push_back(0);
    Bytes zi; if (ZlibCompress(m->icc.data(), m->icc.size(), 9, zi)) { ic.insert(ic.end(), zi.begin(), zi.end()); Chunk(out, "iCCP", ic.data(), ic.size()); }
  } else if (opt.srgbChunk) { uint8_t intent = 0; Chunk(out, "sRGB", &intent, 1); }
  if (m && !m->exif.empty()) Chunk(out, "eXIf", m->exif.data(), m->exif.size());
  if (colorType == 3) { Chunk(out, "PLTE", plte.data(), plte.size()); if (!trns.empty()) Chunk(out, "tRNS", trns.data(), trns.size()); }
  Chunk(out, "IDAT", z.data(), z.size());
  if (m && !m->xmp.empty()) {
    Bytes x; const char* kw = "XML:com.adobe.xmp"; x.insert(x.end(), kw, kw + strlen(kw)); x.push_back(0); x.push_back(0); x.push_back(0); x.push_back(0); x.push_back(0);
    x.insert(x.end(), m->xmp.begin(), m->xmp.end());
    Chunk(out, "iTXt", x.data(), x.size());
  }
  Chunk(out, "IEND", nullptr, 0);
  return true;
}

} // namespace filo::img
