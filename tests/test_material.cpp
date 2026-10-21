#include "test.h"
#include "design/material.h"
#include <algorithm>

using namespace filo::ds;

static float RelativeLuminance(Color c) {
  auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
  return 0.2126f * linear(c.r) + 0.7152f * linear(c.g) + 0.0722f * linear(c.b);
}
static float Contrast(Color text, Color surface) {
  const float t = RelativeLuminance(text), b = RelativeLuminance(surface);
  return (std::max(t, b) + 0.05f) / (std::min(t, b) + 0.05f);
}

TEST(material_text_contrast_across_desktop_colours_and_intensities) {
  for (bool dark : {false, true}) {
    const Palette& p = dark ? kDark : kLight;
    for (int r = 0; r <= 8; ++r) for (int g = 0; g <= 8; ++g) for (int b = 0; b <= 8; ++b) {
      for (float intensity : {0.f, 0.25f, 0.5f, 1.f}) {
        const Color surface = FrostedBackdrop(Color(r / 8.f, g / 8.f, b / 8.f), dark, intensity);
        CHECK(Contrast(p.text, surface) >= 4.5f);
        CHECK(Contrast(p.textSecondary, surface) >= 4.5f);
        CHECK(Contrast(p.textTertiary, surface) >= 4.5f);
        CHECK_EQ(surface.a, 1.f);
      }
    }
  }
}

TEST(material_preserves_backdrop_colour_and_respects_opaque_mode) {
  Color red = FrostedBackdrop(Color(1, 0, 0), false, 1);
  Color blue = FrostedBackdrop(Color(0, 0, 1), false, 1);
  CHECK(red.r > red.b);
  CHECK(blue.b > blue.r);
  for (bool dark : {false, true}) {
    Color a = FrostedBackdrop(Color(1, 0, 0), dark, 0);
    Color b = FrostedBackdrop(Color(0, 0, 1), dark, 0);
    CHECK_EQ(a.r, b.r); CHECK_EQ(a.g, b.g); CHECK_EQ(a.b, b.b);
  }
}
