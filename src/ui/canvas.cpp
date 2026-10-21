#include "ui/canvas.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace filo::ui {

CanvasMap MapCanvas(const Rect& view, int iw, int ih, const CanvasView& v, float pad) {
  CanvasMap m; m.view = view; m.imgW = iw; m.imgH = ih;
  if (iw <= 0 || ih <= 0) { m.image = view; return m; }
  const float fit = std::min((view.w - 2 * pad) / iw, (view.h - 2 * pad) / ih);
  m.scale = std::max(0.0001f, fit * v.zoom);
  const float w = iw * m.scale, h = ih * m.scale;
  m.image = {view.cx() - w / 2 + v.panX, view.cy() - h / 2 + v.panY, w, h};
  return m;
}

bool CanvasNavigate(Ui& ui, const Rect& view, CanvasView& v, int iw, int ih, uint64_t id, float pad, bool leftPan) {
  const InputState& in = ui.In();
  bool changed = false;
  Interaction it = ui.Behavior(view, id, true, false);
  // zoom toward the cursor
  if (it.hot && in.wheel != 0 && !in.ctrl) {
    CanvasMap before = MapCanvas(view, iw, ih, v, pad);
    const float ix = before.ToImageX(in.mx), iy = before.ToImageY(in.my);
    v.zoom = std::clamp(v.zoom * std::pow(1.15f, in.wheel), 1.f, 16.f);
    CanvasMap after = MapCanvas(view, iw, ih, v, pad);
    v.panX += in.mx - after.ToScreenX(ix);
    v.panY += in.my - after.ToScreenY(iy);
    if (v.zoom <= 1.0001f) v.Reset();
    changed = true;
  }
  // pan: left drag when zoomed in, or always with the right mouse / space
  static float lastX = 0, lastY = 0; static bool dragging = false;
  const bool wantPan = it.active && it.dragging && (v.zoom > 1.0001f) && (leftPan || in.shift);
  if (wantPan) {
    if (!dragging) { dragging = true; lastX = in.mx; lastY = in.my; }
    v.panX += in.mx - lastX; v.panY += in.my - lastY; lastX = in.mx; lastY = in.my;
    ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
    changed = true;
  } else if (!it.active) dragging = false;
  if (it.hot && in.dbl) { v.Reset(); changed = true; }
  if (it.hot && v.zoom > 1.0001f && !wantPan && leftPan) ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
  // keep the image from drifting completely off screen
  if (v.zoom > 1.f) {
    CanvasMap m = MapCanvas(view, iw, ih, v, pad);
    const float maxX = std::max(0.f, (m.image.w - view.w) / 2 + 80), maxY = std::max(0.f, (m.image.h - view.h) / 2 + 80);
    v.panX = std::clamp(v.panX, -maxX, maxX); v.panY = std::clamp(v.panY, -maxY, maxY);
  }
  return changed;
}

void DrawChecker(Painter& p, const Rect& r, float cell) {
  p.FillRect(r, ds::Color(1, 1, 1, 1));
  p.PushClip(r);
  for (float y = r.y, j = 0; y < r.bottom(); y += cell, ++j)
    for (float x = r.x + (std::fmod(j, 2.f) == 0 ? 0 : cell), i = 0; x < r.right(); x += cell * 2, ++i)
      p.FillRect({x, y, cell, cell}, ds::Color(0.82f, 0.82f, 0.84f, 1));
  p.PopClip();
}

} // namespace filo::ui
