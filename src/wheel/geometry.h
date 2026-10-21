// Exact geometry of the radial wheel. The *same* signed-distance formulation is implemented in HLSL
// (src/gfx/shaders/wheel.hlsl) so that what is drawn and what is hit-tested are identical.
// Coordinates: pixels relative to the wheel centre, +x right, +y DOWN. Sector 0 is centred at 12 o'clock, clockwise.
#pragma once
#include <cmath>
#include <algorithm>
#include "design/tokens.h"

namespace filo::wheel {

constexpr float kPi = 3.14159265358979f;

struct Vec2 { float x = 0, y = 0; };

struct Layout {
  int   n = 8;                       // number of sectors (>=1)
  float R = 150.f;                   // outer rim radius, in the same unit as the points you pass in
  // fractions of R (copied from design tokens; kept here so tests / alternative skins can override)
  float sectorOuter  = ds::wheel::sectorOuter;
  float sectorInner  = ds::wheel::sectorInner;
  float holeRadius   = ds::wheel::holeRadius;
  float gapRad       = ds::wheel::gapDegrees * kPi / 180.f;
  float corner       = ds::wheel::cornerRadius;
  float rim          = ds::wheel::rimOuter;

  float Pitch() const { return 2.f * kPi / (float)n; }
  // Half angular width of a sector's body (after removing half of the gap on each side).
  float HalfWidth() const { return n == 1 ? kPi : Pitch() * 0.5f - gapRad * 0.5f; }
  float CenterAngle(int k) const { return Pitch() * (float)k; }          // clockwise from 12 o'clock
  float LabelRadius() const { return (sectorInner + sectorOuter) * 0.5f * R; }
};

// Signed distance (in the units of `p`) from p to sector k's rounded body; negative inside. `scale` (>=1) grows the
// sector about its own centroid (hover feedback).
float SectorSdf(const Layout& L, int k, Vec2 p, float scale = 1.f);

// The sector whose angular slice contains p (regardless of whether p is inside its body). Never -1.
int NearestSector(const Layout& L, Vec2 p);

enum class Zone { Outside, Rim, Hole, Sector };
struct Hit { Zone zone = Zone::Outside; int sector = -1; };

// Hit test with optional per-sector hover scales (size n, may be null).
Hit HitTest(const Layout& L, Vec2 p, const float* scales = nullptr);

// Centre of sector k's label/icon group.
Vec2 SectorAnchor(const Layout& L, int k, float radiusFraction /*of R, e.g. 0.67*/);

// Largest axis-aligned text box (w,h) that fits inside sector k around its anchor with a safety margin; used to decide
// font size / truncation so labels never overlap neighbours or leave the sector.
Vec2 SectorTextBox(const Layout& L);

} // namespace filo::wheel
