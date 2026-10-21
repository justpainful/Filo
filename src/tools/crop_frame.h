// Interactive rectangle on a zoom/pan canvas: move, resize by 8 handles, optional fixed aspect ratio, clamped to the image.
// Used by Crop Images (shaded frame + thirds grid) and Redact Photos (many independent regions).
#pragma once
#include "ui/canvas.h"
#include <cstdint>

namespace filo::tools {

struct RectF { float x = 0, y = 0, w = 0, h = 0; float right() const { return x + w; } float bottom() const { return y + h; } };

struct RectEdit {
  bool changed = false;   // geometry modified this frame
  bool pressed = false;   // the user pressed on this rectangle (select it)
  bool hot = false;
  bool active = false;    // being dragged
};

// `r` is in IMAGE pixels. `ratio` > 0 locks width/height. `showHandles` draws the handles (selected rectangle). `minSize` in image px.
RectEdit RectHandles(ui::Ui& ui, const ui::CanvasMap& m, RectF& r, float ratio, bool showHandles, uint64_t id, float minSize = 16.f);

// Crop frame: dims everything outside `r`, draws the border, the rule-of-thirds grid and handles.
RectEdit CropFrame(ui::Ui& ui, const ui::CanvasMap& m, RectF& r, float ratio, uint64_t id, bool grid = true);

// Largest rectangle of aspect `ratio` (> 0) centred in w x h (the starting frame when a preset is chosen).
RectF FitAspect(float w, float h, float ratio);

} // namespace filo::tools
