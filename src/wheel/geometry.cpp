#include "wheel/geometry.h"

namespace filo::wheel {

static inline float Len(float x, float y) { return std::sqrt(x * x + y * y); }

int NearestSector(const Layout& L, Vec2 p) {
  if (L.n <= 1) return 0;
  float phi = std::atan2(p.x, -p.y);              // 0 at 12 o'clock, clockwise positive
  if (phi < 0) phi += 2.f * kPi;
  int k = (int)std::floor(phi / L.Pitch() + 0.5f);
  return k % L.n;
}

float SectorSdf(const Layout& L, int k, Vec2 p, float scale) {
  const float ang = L.CenterAngle(k);
  const float rm = (L.sectorInner + L.sectorOuter) * 0.5f * L.R;
  // Scale about the sector centroid.
  if (scale != 1.f) {
    const float cx = std::sin(ang) * rm, cy = -std::cos(ang) * rm;
    p.x = cx + (p.x - cx) / scale;
    p.y = cy + (p.y - cy) / scale;
  }
  const float r = Len(p.x, p.y);
  const float rc = L.corner * L.R;
  const float Ro = L.sectorOuter * L.R, Ri = L.sectorInner * L.R;
  const float dOut = r - Ro, dIn = Ri - r;
  float dEdge = -1e9f;
  if (L.n > 1) {
    // rotate into the sector frame: axis a (radial, along sector centre) and lateral l
    float phi = std::atan2(p.x, -p.y) - ang;
    while (phi > kPi) phi -= 2 * kPi;
    while (phi < -kPi) phi += 2 * kPi;
    const float a = r * std::cos(phi), l = r * std::sin(phi);
    const float h = L.HalfWidth();
    dEdge = std::fabs(l) * std::cos(h) - a * std::sin(h);
  }
  const float qx = dOut + rc, qy = dIn + rc, qz = dEdge + rc;
  const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f), oz = std::max(qz, 0.f);
  const float outside = std::sqrt(ox * ox + oy * oy + oz * oz);
  const float inside = std::min(std::max(qx, std::max(qy, qz)), 0.f);
  return outside + inside - rc;
}

Hit HitTest(const Layout& L, Vec2 p, const float* scales) {
  Hit h;
  const float r = Len(p.x, p.y);
  if (r > L.rim * L.R) return h;
  // Hovered (scaled) sectors take priority: they may extend past their angular slice.
  if (scales) {
    for (int j = 0; j < L.n; ++j) {
      if (scales[j] > 1.001f && SectorSdf(L, j, p, scales[j]) <= 0.f) { h.zone = Zone::Sector; h.sector = j; return h; }
    }
  }
  if (L.n >= 1) {
    int k = NearestSector(L, p);
    float s = scales ? scales[k] : 1.f;
    if (SectorSdf(L, k, p, s) <= 0.f) { h.zone = Zone::Sector; h.sector = k; return h; }
  }
  if (r <= L.holeRadius * L.R) { h.zone = Zone::Hole; return h; }
  h.zone = Zone::Rim;
  return h;
}

Vec2 SectorAnchor(const Layout& L, int k, float radiusFraction) {
  const float a = L.CenterAngle(k), r = radiusFraction * L.R;
  return {std::sin(a) * r, -std::cos(a) * r};
}

Vec2 SectorTextBox(const Layout& L) {
  // Chord width at the label radius, limited by the body half-width and the radial thickness.
  const float rl = L.LabelRadius();
  const float h = L.HalfWidth();
  float chord = L.n == 1 ? 2.f * rl : 2.f * rl * std::sin(std::min(h, kPi * 0.5f)) * 0.82f;  // 0.82 = margin for the gap/corners
  const float thick = (L.sectorOuter - L.sectorInner) * L.R - 2.f * L.corner * L.R * 0.5f;
  return {std::max(chord, 10.f), std::max(thick * 0.55f, 10.f)};
}

} // namespace filo::wheel
