#pragma once
#include "design/tokens.h"
#include <algorithm>

namespace filo::ds {

// A frosted material must provide a predictable reading surface even over black
// Explorer windows, white documents, and small saturated desktop icons.
inline Color FrostedBackdrop(Color background, bool dark, float intensity) {
  const Color solid = dark ? Color::Hex(0x252830) : Color::Hex(0xF2F4F8);
  Color c;
  if (dark) {
    c = Mix(background, solid, glass::darkTint);
    const float peak = std::max({c.r, c.g, c.b, glass::darkCeiling});
    const float scale = glass::darkCeiling / peak;
    c = Color(c.r * scale, c.g * scale, c.b * scale);
  } else {
    c = Color(std::min(1.f, background.r + glass::lightLift), std::min(1.f, background.g + glass::lightLift), std::min(1.f, background.b + glass::lightLift));
    c = Mix(c, Color(1, 1, 1), glass::lightTint);
    const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    const float wash = std::clamp((glass::lightFloor - lum) / std::max(1.f - lum, 0.001f), 0.f, 1.f);
    c = Mix(c, Color(1, 1, 1), wash);
  }
  c = Mix(solid, c, std::clamp(intensity, 0.f, 1.f));
  c.a = 1;
  return c;
}
} // namespace filo::ds
