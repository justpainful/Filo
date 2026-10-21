#include "tools/crop_frame.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

RectF FitAspect(float w, float h, float ratio) {
  if (ratio <= 0) return {0, 0, w, h};
  float rw = w, rh = w / ratio;
  if (rh > h) { rh = h; rw = h * ratio; }
  return {(w - rw) / 2, (h - rh) / 2, rw, rh};
}

namespace {
struct Drag { uint64_t id = 0; int mode = 0; RectF start; float mx = 0, my = 0; };
Drag g_drag;

// handle ids: 1 TL 2 T 3 TR 4 R 5 BR 6 B 7 BL 8 L, 9 = move
void HandlePos(const Rect& sr, int h, float& x, float& y) {
  const float cx = sr.cx(), cy = sr.cy();
  switch (h) {
    case 1: x = sr.x; y = sr.y; break; case 2: x = cx; y = sr.y; break; case 3: x = sr.right(); y = sr.y; break;
    case 4: x = sr.right(); y = cy; break; case 5: x = sr.right(); y = sr.bottom(); break; case 6: x = cx; y = sr.bottom(); break;
    case 7: x = sr.x; y = sr.bottom(); break; default: x = sr.x; y = cy; break;
  }
}
int Cursor(int mode) {
  switch (mode) {
    case 1: case 5: return (int)(intptr_t)IDC_SIZENWSE; case 3: case 7: return (int)(intptr_t)IDC_SIZENESW;
    case 2: case 6: return (int)(intptr_t)IDC_SIZENS; case 4: case 8: return (int)(intptr_t)IDC_SIZEWE; case 9: return (int)(intptr_t)IDC_SIZEALL;
  }
  return 0;
}
} // namespace

RectEdit RectHandles(Ui& ui, const CanvasMap& m, RectF& r, float ratio, bool showHandles, uint64_t id, float minSize) {
  RectEdit res;
  Painter& p = ui.P();
  const InputState& in = ui.In();
  Rect sr = m.ToScreen(r.x, r.y, r.w, r.h);
  const float hs = 8.f, hit = 12.f;
  int over = 0;
  if (in.inWindow) {
    if (showHandles) for (int h = 1; h <= 8 && !over; ++h) { float x, y; HandlePos(sr, h, x, y); if (std::fabs(in.mx - x) <= hit && std::fabs(in.my - y) <= hit) over = h; }
    if (!over && sr.Contains(in.mx, in.my)) over = 9;
  }
  const bool mine = g_drag.id == id;
  res.hot = over != 0 && (g_drag.id == 0 || mine);
  if (over && in.pressed && g_drag.id == 0 && m.view.Contains(in.mx, in.my)) {
    g_drag = {id, over, r, in.mx, in.my};
    res.pressed = true;
  }
  if (g_drag.id == id) {
    res.active = true;
    if (!in.down) g_drag = {};
    else {
      const float dx = (in.mx - g_drag.mx) / m.scale, dy = (in.my - g_drag.my) / m.scale;
      RectF n = g_drag.start;
      const int md = g_drag.mode;
      if (md == 9) { n.x += dx; n.y += dy; }
      else {
        float l = n.x, t = n.y, rr = n.right(), b = n.bottom();
        if (md == 1 || md == 7 || md == 8) l += dx;
        if (md == 3 || md == 4 || md == 5) rr += dx;
        if (md == 1 || md == 2 || md == 3) t += dy;
        if (md == 5 || md == 6 || md == 7) b += dy;
        if (ratio > 0) {                              // keep the aspect: the dragged edge/corner drives, the rest follows
          if (md == 2 || md == 6) { float w = (b - t) * ratio, cx = (l + rr) / 2; l = cx - w / 2; rr = cx + w / 2; }
          else if (md == 4 || md == 8) { float h = (rr - l) / ratio, cy = (t + b) / 2; t = cy - h / 2; b = cy + h / 2; }
          else {
            float w = std::max(minSize, rr - l), h = w / ratio;
            if (md == 1 || md == 7) l = rr - w; else rr = l + w;
            if (md == 1 || md == 3) t = b - h; else b = t + h;
          }
        }
        if (rr - l < minSize) { if (md == 1 || md == 7 || md == 8) l = rr - minSize; else rr = l + minSize; }
        if (b - t < minSize) { if (md == 1 || md == 2 || md == 3) t = b - minSize; else b = t + minSize; }
        n = {l, t, rr - l, b - t};
      }
      // clamp inside the image (shrinking toward the anchor when a resize would leave it)
      const float W = (float)m.imgW, H = (float)m.imgH;
      if (md == 9) { n.x = std::clamp(n.x, 0.f, W - n.w); n.y = std::clamp(n.y, 0.f, H - n.h); }
      else {
        float l = std::clamp(n.x, 0.f, W), t = std::clamp(n.y, 0.f, H), rr = std::clamp(n.right(), 0.f, W), b = std::clamp(n.bottom(), 0.f, H);
        if (ratio > 0) {
          float w = rr - l, h = b - t;
          if (w / std::max(h, 1.f) > ratio) { float nw = h * ratio; if (md == 1 || md == 7 || md == 8) l = rr - nw; else rr = l + nw; }
          else { float nh = w / ratio; if (md == 1 || md == 2 || md == 3) t = b - nh; else b = t + nh; }
        }
        n = {l, t, std::max(minSize, rr - l), std::max(minSize, b - t)};
      }
      if (n.x != r.x || n.y != r.y || n.w != r.w || n.h != r.h) { r = n; res.changed = true; }
      ui.SetCursor(Cursor(md));
      ui.RequestFrame();
    }
  } else if (res.hot) ui.SetCursor(Cursor(over));
  sr = m.ToScreen(r.x, r.y, r.w, r.h);
  if (showHandles) {
    for (int h = 1; h <= 8; ++h) {
      float x, y; HandlePos(sr, h, x, y);
      p.Shadow({x - hs / 2, y - hs / 2, hs, hs}, hs / 2, 4, Color(0, 0, 0, 0.35f), 1);
      p.FillCircle(x, y, hs / 2 + 1, Color(1, 1, 1, 1));
      p.StrokeCircle(x, y, hs / 2 + 1, accent::base, 1.5f);
    }
  }
  return res;
}

RectEdit CropFrame(Ui& ui, const CanvasMap& m, RectF& r, float ratio, uint64_t id, bool grid) {
  Painter& p = ui.P();
  const Rect img = m.image, sr = m.ToScreen(r.x, r.y, r.w, r.h);
  const Color shade(0, 0, 0, 0.52f);
  p.FillRect({img.x, img.y, img.w, std::max(0.f, sr.y - img.y)}, shade);
  p.FillRect({img.x, sr.bottom(), img.w, std::max(0.f, img.bottom() - sr.bottom())}, shade);
  p.FillRect({img.x, sr.y, std::max(0.f, sr.x - img.x), sr.h}, shade);
  p.FillRect({sr.right(), sr.y, std::max(0.f, img.right() - sr.right()), sr.h}, shade);
  p.StrokeRoundRect(sr, 0, Color(1, 1, 1, 0.95f), 1.5f);
  if (grid) for (int i = 1; i <= 2; ++i) {
    p.Line(sr.x + sr.w * i / 3, sr.y, sr.x + sr.w * i / 3, sr.bottom(), Color(1, 1, 1, 0.35f), 1.f);
    p.Line(sr.x, sr.y + sr.h * i / 3, sr.right(), sr.y + sr.h * i / 3, Color(1, 1, 1, 0.35f), 1.f);
  }
  return RectHandles(ui, m, r, ratio, true, id);
}

} // namespace filo::tools
