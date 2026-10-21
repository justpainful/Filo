// Filo design tokens - the ONLY place visual constants live. Everything is expressed in DIPs (1 dip = 1/96 in) so that
// DPI scaling is a single multiplication at the edge. Values marked [M] were measured from the reference screenshots
// (see docs/REFERENCE-AUDIT.md); [E] are estimates to be refined by overlay comparison; [W] are Windows-specific additions.
#pragma once
#include <cstdint>
#include <cmath>

namespace filo::ds {

struct Color {
  float r = 0, g = 0, b = 0, a = 1;   // straight (non-premultiplied) sRGB
  constexpr Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
  static constexpr Color Hex(uint32_t rgb, float a = 1) {
    return Color(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
  }
  constexpr Color WithAlpha(float na) const { return Color(r, g, b, na); }
  constexpr Color Premul() const { return Color(r * a, g * a, b * a, a); }
};

constexpr Color Mix(const Color& x, const Color& y, float t) {
  return Color(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t);
}

// ------------------------------------------------------------------------------------------------ brand / accent
// The reference uses orange as its accent; Filo's official accent is a friendly medium blue (not navy).        [W]
namespace accent {
constexpr Color base     = Color::Hex(0x3B8BFF);   // Filo Blue
constexpr Color hover    = Color::Hex(0x5BA0FF);
constexpr Color pressed  = Color::Hex(0x2E78EA);
constexpr Color soft     = Color::Hex(0x3B8BFF, 0.18f);
constexpr Color onAccent = Color::Hex(0xFFFFFF);
}

// ------------------------------------------------------------------------------------------------ surfaces / text
struct Palette {
  Color text, textSecondary, textTertiary, textDisabled;
  Color windowTint;          // tint laid over the blurred backdrop
  Color windowTintBottom;    // vertical gradient end
  Color windowBorder;        // 1px hairline
  Color divider;
  Color controlFill;         // buttons / segmented background
  Color controlFillHover;
  Color controlFillPressed;
  Color controlFillDisabled;
  Color fieldFill;           // list rows, thumbnails wells
  Color trackInactive;       // slider / progress background
  Color darkCanvas;          // image preview well (reference: near-black warm)
  Color sectorFill, sectorFillHover, sectorText, sectorTextHover;
  Color rimFill, holeFill;
  Color heading;             // section headings ("Light", "Color"): deeper / more saturated than body text in the reference
};

constexpr Palette kLight = {
  /*text*/ Color::Hex(0x1C1C1E),
  /*textSecondary*/ Color::Hex(0x3B424D),
  /*textTertiary*/ Color::Hex(0x3F4650),
  /*textDisabled*/ Color::Hex(0x1C1C1E, 0.28f),
  /*windowTint*/ Color::Hex(0xFFFFFF, 0.64f),
  /*windowTintBottom*/ Color::Hex(0xF4F6FA, 0.74f),
  /*windowBorder*/ Color::Hex(0xFFFFFF, 0.55f),
  /*divider*/ Color::Hex(0x000000, 0.10f),
  /*controlFill*/ Color::Hex(0xFFFFFF, 0.48f),
  /*controlFillHover*/ Color::Hex(0x000000, 0.11f),
  /*controlFillPressed*/ Color::Hex(0xFFFFFF, 0.94f),
  /*controlFillDisabled*/ Color::Hex(0x000000, 0.04f),
  /*fieldFill*/ Color::Hex(0xFFFFFF, 0.66f),
  /*trackInactive*/ Color::Hex(0x000000, 0.14f),
  /*darkCanvas*/ Color::Hex(0x12100D),
  /*sectorFill*/ Color::Hex(0xFFFFFF, 0.58f),       // [M] reference sectors ~ white @ 55-60% over backdrop
  /*sectorFillHover*/ accent::base.WithAlpha(0.68f),
  /*sectorText*/ Color::Hex(0x111111),
  /*sectorTextHover*/ Color::Hex(0xFFFFFF),
  /*rimFill*/ Color::Hex(0xCCBBB2, 0.36f),          // [M] fitted on the gap/rim pixels of conversion-wheel.png
  /*holeFill*/ Color::Hex(0xE8E4E1, 0.40f),
  /*heading*/ Color::Hex(0x1C1C1E, 0.88f),
};

constexpr Palette kDark = {
  /*text*/ Color::Hex(0xF5F5F7),
  /*textSecondary*/ Color::Hex(0xC2C9D4),
  /*textTertiary*/ Color::Hex(0xB5BECC),
  /*textDisabled*/ Color::Hex(0xF5F5F7, 0.28f),
  /*windowTint*/ Color::Hex(0x26272B, 0.66f),
  /*windowTintBottom*/ Color::Hex(0x1C1D20, 0.76f),
  /*windowBorder*/ Color::Hex(0xFFFFFF, 0.16f),
  /*divider*/ Color::Hex(0xFFFFFF, 0.12f),
  /*controlFill*/ Color::Hex(0xFFFFFF, 0.10f),
  /*controlFillHover*/ Color::Hex(0xFFFFFF, 0.15f),
  /*controlFillPressed*/ Color::Hex(0xFFFFFF, 0.21f),
  /*controlFillDisabled*/ Color::Hex(0xFFFFFF, 0.05f),
  /*fieldFill*/ Color::Hex(0xFFFFFF, 0.07f),
  /*trackInactive*/ Color::Hex(0xFFFFFF, 0.18f),
  /*darkCanvas*/ Color::Hex(0x0B0A09),
  /*sectorFill*/ Color::Hex(0x2C2D31, 0.55f),
  /*sectorFillHover*/ accent::base.WithAlpha(0.85f),
  /*sectorText*/ Color::Hex(0xF5F5F7),
  /*sectorTextHover*/ Color::Hex(0xFFFFFF),
  /*rimFill*/ Color::Hex(0xFFFFFF, 0.10f),
  /*holeFill*/ Color::Hex(0x000000, 0.22f),
  /*heading*/ Color::Hex(0xFFFFFF, 0.92f),
};

// ------------------------------------------------------------------------------------------------ the radial wheel
// Radii are fractions of the outer rim radius R. Measured on conversion-wheel.png (R = 530 px at 2x):          [M]
//   sector outer 506/530, sector inner 206/530, gap between sectors 4.5 deg (constant angle => wedge gaps).
namespace wheel {
constexpr float radiusDip       = 265.f;   // [M] reference 530px radius at 2x; capped to the monitor work area
constexpr float rimOuter        = 1.000f;  // [M]
constexpr float sectorOuter     = 0.955f;  // [M]
constexpr float sectorInner     = 0.389f;  // [M]
constexpr float holeRadius      = 0.362f;  // [E] inner disc edge
constexpr float gapDegrees      = 4.5f;    // [M]
constexpr float cornerRadius    = 0.095f;  // [E] rounded sector corners (fraction of R)
constexpr float hoverScale      = 1.045f;  // [E]
constexpr float centerPillW     = 0.560f;  // [M] "2.2 MB" pill: 297/530 px
constexpr float centerPillH     = 0.285f;  // [M] 151/530 px
constexpr float centerThumb     = 0.43f;   // [E]
constexpr float labelSize       = 0.071f;  // [E] cap-height-ish; font size = labelSize*R*1.28
constexpr float iconSize        = 0.150f;  // [E]
constexpr float shadowBlur      = 0.075f;  // [E]
constexpr float shadowOffsetY   = 0.030f;  // [E]
constexpr float shadowAlpha     = 0.10f;   // [E] reference wheel casts almost no outer shadow
constexpr float fillAlphaOuter  = 0.40f;   // [M] sector white-mix alpha at the outer edge (JPG sector top: 0.42)
constexpr float fillAlphaInner  = 0.62f;   // [M] ... and near the hole (0.60-0.66)
constexpr float margin          = 0.22f;   // window padding around R for shadow, as a fraction of R
}

// ------------------------------------------------------------------------------------------------ dialog windows
// Measured on edit-photo.png / add-background.png / audio-tools.png (2x retina, so dip = px/2):               [M/E]
namespace dialog {
constexpr float cornerRadius    = 26.f;    // [M] ~ 55px @2x
constexpr float titleBarHeight  = 54.5f;   // [M] 109px @2x
constexpr float closeButton     = 32.f;    // [M] 64px @2x
constexpr float closeInset      = 16.5f;   // [M] 33px @2x
constexpr float titleFontSize   = 18.5f;   // [M] "Edit Photo" spans ~99 dip, bold
constexpr float bodyFontSize    = 14.5f;   // [M] "Exposure" spans ~60 dip
constexpr float smallFontSize   = 13.f;
constexpr float contentInset    = 16.5f;   // [M] preview well starts 33px @2x from the window edge
constexpr float sectionGap      = 14.f;
constexpr float rowHeight       = 30.f;    // [M] slider rows: 59.5px @2x
constexpr float footerHeight    = 58.f;    // [E]
constexpr float shadowBlur      = 40.f;
constexpr float shadowAlpha     = 0.30f;
constexpr float wellRadius      = 24.f;    // [M] dark preview well
constexpr float borderWidth     = 1.f;
}

namespace control {
constexpr float buttonHeight    = 26.f;    // [E] reference "Apply" pill: ~49px @2x
constexpr float buttonRadius    = 8.f;     // [E]
constexpr float buttonPadX      = 14.f;
constexpr float segmentedHeight = 26.f;
constexpr float sliderTrack     = 5.f;     // [M] ~ 10px @2x incl. antialiasing
constexpr float sliderThumbW    = 24.f;    // [M] 47px @2x pill
constexpr float sliderThumbH    = 18.f;    // [M] ~ 36px @2x
constexpr float swatch          = 32.f;    // [M] background swatch ~ 64px @2x
constexpr float swatchRadius    = 9.f;
constexpr float checkbox        = 16.f;
constexpr float inputHeight     = 26.f;
constexpr float listRowRadius   = 14.f;    // [E] Join Videos rows
constexpr float listRowHeight   = 64.f;    // [E]
}

// ------------------------------------------------------------------------------------------------ typography
// SF Pro cannot be redistributed; Segoe UI Variable is the closest shipped Windows face.                      [W]
namespace type {
constexpr const wchar_t* family        = L"Segoe UI Variable Text";
constexpr const wchar_t* familyDisplay = L"Segoe UI Variable Display";
constexpr const wchar_t* familyFallback= L"Segoe UI";
constexpr const wchar_t* mono          = L"Cascadia Mono";
}

// ------------------------------------------------------------------------------------------------ motion
// Durations in seconds. Spring-ish ease-out; refined from the official demo video (docs/REFERENCE-AUDIT.md).  [E]
namespace motion {
constexpr float wheelAppear     = 0.200f;
constexpr float wheelDisappear  = 0.130f;
constexpr float wheelSwitch     = 0.180f;
constexpr float hoverIn         = 0.090f;
constexpr float hoverOut        = 0.140f;
constexpr float dropConfirm     = 0.160f;
constexpr float appearScaleFrom = 0.86f;
constexpr float uiFast          = 0.120f;
constexpr float uiNormal        = 0.200f;
}

// ------------------------------------------------------------------------------------------------ glass
namespace glass {
constexpr float blurSigmaDip     = 22.f;    // frosting of the backdrop                                        [E]
constexpr float dialogBlurDip    = 28.f;    // [W] diffuse small desktop icons before tinting
constexpr float lightTint        = 0.025f;  // [W] quiet white veil, retaining reference-like backdrop colour
constexpr float lightLift        = 0.20f;   // [E] additive light lift, bounded by the readability floor below
constexpr float lightFloor       = 0.72f;   // [W] minimum weighted sRGB brightness for dark labels
constexpr float darkTint         = 0.72f;   // [W]
constexpr float darkCeiling      = 0.29f;   // [W] maximum channel brightness for light labels
constexpr float refractStrength  = 0.030f;  // edge displacement, fraction of R                                [E]
constexpr float bevelWidth       = 0.020f;  // [W] narrow, quiet edge highlight
constexpr float specularStrength = 0.035f;
constexpr float fresnelPower     = 2.6f;
constexpr float saturationBoost  = 1.18f;
}

// Linear interpolation helpers shared by animation code.
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float EaseOutCubic(float t) { t = Clamp01(t); float u = 1 - t; return 1 - u * u * u; }
inline float EaseOutBack(float t, float s = 1.25f) { t = Clamp01(t) - 1; return t * t * ((s + 1) * t + s) + 1; }
inline float SmoothStep(float e0, float e1, float x) { float t = Clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }

} // namespace filo::ds
