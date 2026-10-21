// Photo adjustment pipeline (Edit Photo). Pure CPU float pipeline on straight-alpha BGRA; the same function renders the live
// preview (downscaled copy, spatial radii scale with the image) and the final full-resolution export.
#pragma once
#include <cstdint>
#include <vector>
#include <atomic>

namespace filo::tools {

struct PhotoAdjust {
  // Light
  float exposure = 0;      // EV  -5 .. +5
  float brightness = 0;    // -100 .. 100
  float contrast = 0;
  float highlights = 0;
  float shadows = 0;
  float whites = 0;
  float blacks = 0;
  float gamma = 0;
  // Color
  float temperature = 0;
  float tint = 0;
  float saturation = 0;
  float vibrance = 0;
  // Details
  float sharpness = 0;     // 0 .. 100
  float clarity = 0;       // -100 .. 100
  float dehaze = 0;        // -100 .. 100
  float noiseReduction = 0;// 0 .. 100
  float grain = 0;         // 0 .. 100
  // Additional effects
  float vignette = 0;      // -100 (light) .. 100 (dark)
  int filter = 0;          // 0 none, see FilterNames()
  float filterAmount = 100;// 0 .. 100

  bool IsIdentity() const;
  bool operator==(const PhotoAdjust& o) const;
};

const wchar_t* const* FilterNames(int& count);

// Applies `p` to `px` (w*h BGRA straight alpha) in place. `previewScale` = preview long side / full long side (1.0 for export):
// radius-based effects (clarity, sharpness, noise reduction, dehaze, grain size) are multiplied by it so the preview matches.
// Honours `cancel` between stages.
void ApplyPhotoAdjust(std::vector<uint8_t>& px, int w, int h, const PhotoAdjust& p, float previewScale, const std::atomic<bool>* cancel = nullptr);

} // namespace filo::tools
