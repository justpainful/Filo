#include "tools/widgets_media.h"
#include "core/util.h"
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

std::wstring TimeText(double s) { return FormatTime(s, true); }

static float TimeToX(const Rect& r, double t, double dur) { return r.x + (float)(dur > 0 ? std::clamp(t / dur, 0.0, 1.0) : 0.0) * r.w; }
static double XToTime(const Rect& r, float x, double dur) { return std::clamp((double)((x - r.x) / std::max(1.f, r.w)), 0.0, 1.0) * dur; }

static void DrawBars(Painter& p, const Rect& r, const media::Waveform& wf, const Color& c, float barW, float gap, double t0 = 0, double t1 = 1e18, double dur = 0) {
  if (!wf.ok || wf.peaks.empty()) return;
  const int n = (int)wf.peaks.size();
  const float pitch = barW + gap;
  const int bars = std::max(1, (int)(r.w / pitch));
  for (int i = 0; i < bars; ++i) {
    const float x = r.x + i * pitch;
    const float f0 = (float)i / bars, f1 = (float)(i + 1) / bars;
    const int b0 = std::clamp((int)(f0 * n), 0, n - 1), b1 = std::clamp((int)std::ceil(f1 * n), b0 + 1, n);
    float pk = 0; for (int k = b0; k < b1; ++k) pk = std::max(pk, wf.peaks[k]);
    const float h = std::max(1.5f, pk * r.h * 0.92f);
    p.FillRoundRect({x, r.cy() - h / 2, barW, h}, barW / 2, c);
  }
  (void)t0; (void)t1; (void)dur;
}

TimelineResult Timeline(Ui& ui, const Rect& r, double dur, double& pos, double* a, double* b, const std::vector<double>* markers, const media::Waveform* wf, double frameStep) {
  TimelineResult res;
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const uint64_t id = ui.AutoId("timeline");
  ui::Interaction it = ui.Behavior(r, id, dur > 0, true);
  const float trackH = 8.f;
  Rect track{r.x, r.cy() - trackH / 2, r.w, trackH};
  if (wf && wf->ok) { Rect wr{r.x, r.y, r.w, r.h}; DrawBars(p, wr, *wf, Color(pal.text.r, pal.text.g, pal.text.b, 0.10f), 2.f, 1.5f); }
  p.FillRoundRect(track, trackH / 2, pal.trackInactive);
  // selection range
  if (a && b) {
    const float xa = TimeToX(r, *a, dur), xb = TimeToX(r, *b, dur);
    p.FillRoundRect({xa, track.y, std::max(2.f, xb - xa), trackH}, trackH / 2, accent::base.WithAlpha(0.85f));
  }
  if (markers) for (size_t i = 0; i < markers->size(); ++i) {
    const float x = TimeToX(r, (*markers)[i], dur);
    p.FillRoundRect({x - 1.5f, r.y + 4, 3, r.h - 8}, 1.5f, Color::Hex(0xFFB020));
  }
  // handles
  const float hw = 10, hh = 28;
  auto handleRect = [&](double t) { const float x = TimeToX(r, t, dur); return Rect{x - hw / 2, r.cy() - hh / 2, hw, hh}; };
  int active = 0;
  static int dragHandle = 0;
  if (a && b) {
    Rect ha = handleRect(*a), hb = handleRect(*b);
    Rect ga = ha.Inset(-4, -2), gb = hb.Inset(-4, -2);
    const InputState& in = ui.In();
    if (in.pressed && in.inWindow) { if (gb.Contains(in.mx, in.my)) dragHandle = 2; else if (ga.Contains(in.mx, in.my)) dragHandle = 1; }
    if (!in.down) dragHandle = 0;
    if (dragHandle && in.down) {
      const double t = XToTime(r, in.mx, dur);
      if (dragHandle == 1) *a = std::clamp(t, 0.0, *b - 0.05); else *b = std::clamp(t, *a + 0.05, dur);
      res.rangeChanged = true; res.handle = dragHandle; active = dragHandle;
      ui.RequestFrame();
    }
    for (int k = 0; k < 2; ++k) {
      Rect h = k == 0 ? ha : hb;
      const bool hot = (active == k + 1) || h.Inset(-4, -2).Contains(in.mx, in.my);
      p.Shadow(h, 4, 5, Color(0, 0, 0, 0.25f), 1);
      p.FillRoundRect(h, 4, hot ? accent::hover : accent::base);
      p.FillRoundRect({h.cx() - 0.75f, h.y + 7, 1.5f, h.h - 14}, 0.75f, Color(1, 1, 1, 0.85f));
    }
  }
  // playhead + seeking (only when not dragging a handle)
  if (!dragHandle && (it.pressed || it.active)) {
    const double t = XToTime(r, ui.In().mx, dur);
    // clicking a marker selects it instead of seeking
    if (markers && it.pressed) {
      for (size_t i = 0; i < markers->size(); ++i) if (std::fabs(TimeToX(r, (*markers)[i], dur) - ui.In().mx) < 6) { res.markerClicked = (int)i; break; }
    }
    if (res.markerClicked < 0) { pos = t; res.seeked = true; res.scrubbing = it.active; }
  }
  if (it.focused) for (const auto& k : ui.In().keys) {
    const double step = frameStep > 0 ? frameStep : std::max(0.04, dur / 200.0);
    if (k.vk == VK_LEFT) { pos = std::max(0.0, pos - step * (k.shift ? 10 : 1)); res.seeked = true; }
    if (k.vk == VK_RIGHT) { pos = std::min(dur, pos + step * (k.shift ? 10 : 1)); res.seeked = true; }
  }
  const float px = TimeToX(r, pos, dur);
  p.FillRoundRect({px - 1.25f, r.y + 2, 2.5f, r.h - 4}, 1.25f, pal.text);
  p.FillCircle(px, r.y + 4, 4.f, pal.text);
  return res;
}

bool WaveformTrim(Ui& ui, const Rect& r, const media::Waveform& wf, double dur, double& start, double& end, double playhead, bool enabled) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const float radius = 8, border = 2;
  Rect box = r;
  p.FillRoundRect(box, radius, accent::base.WithAlpha(0.14f));
  p.PushClipRoundRect(box, radius);
  Rect inner = box.Inset(border + 8, 14);
  DrawBars(p, inner, wf, accent::base, 2.4f, 1.9f);
  // dim outside the selection
  const float xa = TimeToX(box, start, dur), xb = TimeToX(box, end, dur);
  p.FillRect({box.x, box.y, std::max(0.f, xa - box.x), box.h}, Color(0.5f, 0.5f, 0.55f, 0.35f));
  p.FillRect({xb, box.y, std::max(0.f, box.right() - xb), box.h}, Color(0.5f, 0.5f, 0.55f, 0.35f));
  // end blocks with a white grip (the reference trim handles)
  const float bw = 14;
  Rect ha{xa - 0.f, box.y, bw, box.h}, hb{xb - bw, box.y, bw, box.h};
  if (xa < box.x + bw) ha.x = box.x;
  p.FillRect(ha, accent::base); p.FillRect(hb, accent::base);
  p.FillRoundRect({ha.cx() - 1.5f, box.cy() - 14, 3, 28}, 1.5f, Color(1, 1, 1, 0.92f));
  p.FillRoundRect({hb.cx() - 1.5f, box.cy() - 14, 3, 28}, 1.5f, Color(1, 1, 1, 0.92f));
  if (playhead >= 0) p.FillRect({TimeToX(box, playhead, dur) - 1.f, box.y, 2.f, box.h}, Color(1, 1, 1, 0.95f));
  p.PopClip();
  p.StrokeRoundRect(box, radius, accent::base, border);
  // interaction
  static int drag = 0;
  ui::Interaction it = ui.Behavior(box, ui.AutoId("wavetrim"), enabled, true);
  const InputState& in = ui.In();
  bool changed = false;
  if (enabled && it.pressed) {
    const float d1 = std::fabs(in.mx - (ha.x + bw / 2)), d2 = std::fabs(in.mx - (hb.x + bw / 2));
    drag = (d1 <= d2) ? 1 : 2;
    if (std::min(d1, d2) > 24) drag = in.mx < (xa + xb) / 2 ? 1 : 2;
  }
  if (!in.down) drag = 0;
  if (drag && in.down && enabled) {
    const double t = XToTime(box, in.mx, dur);
    if (drag == 1) { const double v = std::clamp(t, 0.0, end - 0.05); if (v != start) { start = v; changed = true; } }
    else { const double v = std::clamp(t, start + 0.05, dur); if (v != end) { end = v; changed = true; } }
    ui.RequestFrame();
  }
  if (it.hot) ui.SetCursor((int)(intptr_t)IDC_SIZEWE);
  (void)pal;
  return changed;
}

bool WaveformRegions(Ui& ui, const Rect& r, const media::Waveform& wf, double dur, std::vector<WaveRegion>& regions, int& selected, double playhead, double minLen) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  p.FillRoundRect(r, 10, Color(pal.text.r, pal.text.g, pal.text.b, 0.06f));
  p.PushClipRoundRect(r, 10);
  DrawBars(p, r.Inset(6, 8), wf, Color(pal.text.r, pal.text.g, pal.text.b, 0.55f), 2.2f, 1.8f);
  for (size_t i = 0; i < regions.size(); ++i) {
    const float xa = TimeToX(r, regions[i].start, dur), xb = TimeToX(r, regions[i].end, dur);
    const bool sel = (int)i == selected;
    p.FillRect({xa, r.y, std::max(2.f, xb - xa), r.h}, accent::base.WithAlpha(sel ? 0.40f : 0.26f));
    p.FillRect({xa, r.y, 3, r.h}, accent::base); p.FillRect({xb - 3, r.y, 3, r.h}, accent::base);
  }
  if (playhead >= 0) p.FillRect({TimeToX(r, playhead, dur) - 1.f, r.y, 2.f, r.h}, pal.text);
  p.PopClip();
  p.StrokeRoundRect(r, 10, pal.divider, 1.f);

  static int mode = 0, dragIdx = -1; static double grabOff = 0, anchor = 0;   // 1 create, 2 left edge, 3 right edge, 4 move
  ui::Interaction it = ui.Behavior(r, ui.AutoId("regions"), true, true);
  const InputState& in = ui.In();
  bool changed = false;
  if (it.pressed) {
    const double t = XToTime(r, in.mx, dur);
    mode = 0; dragIdx = -1;
    for (int i = (int)regions.size() - 1; i >= 0 && !mode; --i) {
      const float xa = TimeToX(r, regions[i].start, dur), xb = TimeToX(r, regions[i].end, dur);
      if (std::fabs(in.mx - xa) < 7) { mode = 2; dragIdx = i; }
      else if (std::fabs(in.mx - xb) < 7) { mode = 3; dragIdx = i; }
      else if (in.mx > xa && in.mx < xb) { mode = 4; dragIdx = i; grabOff = t - regions[i].start; }
    }
    if (!mode) { mode = 1; anchor = t; regions.push_back({t, t}); dragIdx = (int)regions.size() - 1; }
    selected = dragIdx;
  }
  if (!in.down) {
    if (mode == 1 && dragIdx >= 0 && dragIdx < (int)regions.size() && regions[dragIdx].end - regions[dragIdx].start < minLen) { regions.erase(regions.begin() + dragIdx); selected = -1; }
    mode = 0; dragIdx = -1;
  }
  if (mode && in.down && dragIdx >= 0 && dragIdx < (int)regions.size()) {
    const double t = XToTime(r, in.mx, dur);
    WaveRegion& g = regions[dragIdx];
    if (mode == 1) { g.start = std::min(anchor, t); g.end = std::max(anchor, t); }
    else if (mode == 2) g.start = std::clamp(t, 0.0, g.end - minLen);
    else if (mode == 3) g.end = std::clamp(t, g.start + minLen, dur);
    else { const double len = g.end - g.start; g.start = std::clamp(t - grabOff, 0.0, dur - len); g.end = g.start + len; }
    changed = true; ui.RequestFrame();
  }
  if (it.focused && selected >= 0 && selected < (int)regions.size())
    for (const auto& k : in.keys) if (k.vk == VK_DELETE || k.vk == VK_BACK) { regions.erase(regions.begin() + selected); selected = -1; changed = true; break; }
  if (it.hot) ui.SetCursor((int)(intptr_t)IDC_IBEAM);
  return changed;
}

} // namespace filo::tools
