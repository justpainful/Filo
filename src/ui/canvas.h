// Zoom/pan image canvas helpers shared by Edit Photo, Crop, Annotate, Redact, Add Background, video crop/redact previews.
#pragma once
#include "ui/ui.h"

namespace filo::ui {

struct CanvasView {
  float zoom = 1.f;            // 1 = "fit"; >1 zooms in around the cursor
  float panX = 0, panY = 0;    // pan offset in DIP (applied after fit/zoom)
  bool IsFit() const { return zoom <= 1.0001f && panX == 0 && panY == 0; }
  void Reset() { zoom = 1.f; panX = panY = 0; }
};

struct CanvasMap {
  Rect view;                   // widget rectangle (DIP)
  Rect image;                  // where the image lands on screen (DIP), already including zoom/pan
  float scale = 1;             // screen DIP per image pixel
  int imgW = 0, imgH = 0;
  float ToScreenX(float ix) const { return image.x + ix * scale; }
  float ToScreenY(float iy) const { return image.y + iy * scale; }
  float ToImageX(float sx) const { return (sx - image.x) / scale; }
  float ToImageY(float sy) const { return (sy - image.y) / scale; }
  Rect ToScreen(float ix, float iy, float iw, float ih) const { return {ToScreenX(ix), ToScreenY(iy), iw * scale, ih * scale}; }
  float ZoomPercent(float dpr) const { return scale * dpr * 100.f; }   // 100% = one image pixel per device pixel
};

CanvasMap MapCanvas(const Rect& view, int imgW, int imgH, const CanvasView& v, float padding = 14.f);

// Mouse wheel = zoom toward the cursor, left drag pans when zoomed in (or Shift+left drag when `leftPan` is false, for canvases
// where the plain left drag draws), double click = fit. Returns true when changed.
bool CanvasNavigate(Ui& ui, const Rect& view, CanvasView& v, int imgW, int imgH, uint64_t id, float padding = 14.f, bool leftPan = true);

// Checkerboard under transparent images.
void DrawChecker(Painter& p, const Rect& r, float cell = 10.f);

} // namespace filo::ui
