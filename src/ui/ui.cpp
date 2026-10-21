#include "ui/ui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstring>

namespace filo::ui {

using namespace filo::ds;

namespace {
constexpr float kFont = ds::dialog::bodyFontSize;
uint64_t Fnv(const void* d, size_t n, uint64_t h) {
  const uint8_t* p = (const uint8_t*)d;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
float Smooth(float t) { return t * t * (3 - 2 * t); }
std::wstring ClipboardText() {
  std::wstring out;
  if (OpenClipboard(nullptr)) {
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto* p = (const wchar_t*)GlobalLock(h)) { out = p; GlobalUnlock(h); } }
    CloseClipboard();
  }
  return out;
}
void SetClipboardText(const std::wstring& s) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
    memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
}
} // namespace

// ------------------------------------------------------------------------------------------------ frame
void Ui::BeginFrame(Painter& p, const InputState& in, float w, float h, double now, bool rtl) {
  p_ = &p; in_ = &in; pal_ = palOverride_ ? palOverride_ : &Theme::Get().Colors(); w_ = w; h_ = h; rtl_ = rtl;
  dt_ = last_ > 0 ? (float)std::clamp(now - last_, 0.0, 0.05) : 0.016f;
  last_ = now_ = now;
  needFrame_ = false; cursor_ = 0; closeBubble_={};
  deferred_.clear(); endActions_.clear();
  idStack_.assign(1, 1469598103934665603ull);
  autoCounters_.clear();
  col_.clear(); col_.push_back({Rect{0, 0, w, h}, 0, 8});
  prevFocusOrder_.swap(focusOrder_); focusOrder_.clear();
  scrollStack_.clear();
  tipId_ = tipId_;  // persists across frames
  hotNext_ = 0;
}

bool Ui::EndFrame() {
  // keyboard focus traversal
  for (const auto& k : in_->keys) {
    if (k.vk == VK_TAB && !focusOrder_.empty()) {
      auto it = std::find(focusOrder_.begin(), focusOrder_.end(), focus_);
      int idx = it == focusOrder_.end() ? (k.shift ? 0 : -1) : (int)(it - focusOrder_.begin());
      idx += k.shift ? -1 : 1;
      if (idx < 0) idx = (int)focusOrder_.size() - 1;
      if (idx >= (int)focusOrder_.size()) idx = 0;
      focus_ = focusOrder_[idx]; focusVisible_ = true; needFrame_ = true;
    }
  }
  hot_ = hotNext_;
  // deferred overlays (popups, tooltips)
  for (auto& f : deferred_) f(*p_);
  deferred_.clear();
  for (auto& f : endActions_) f();
  if (!in_->down) active_=0; // discard capture owned by a widget removed during this interaction
  if (tipId_ && hot_ != tipId_) { tipId_ = 0; }
  if (hot_ && hot_ == tipId_ && now_ - tipSince_ < 0.6) needFrame_ = true;     // waiting for the tooltip delay
  return needFrame_;
}

// ------------------------------------------------------------------------------------------------ ids / layout
uint64_t Ui::Id(std::string_view s) const { return Fnv(s.data(), s.size(), idStack_.back()); }
uint64_t Ui::AutoId(const char* kind) {
  int n = autoCounters_[idStack_.back()]++;
  uint64_t h = Fnv(kind, strlen(kind), idStack_.back());
  return Fnv(&n, sizeof n, h);
}
uint64_t Ui::Id(const std::wstring& s) const { return Fnv(s.data(), s.size() * sizeof(wchar_t), idStack_.back()); }
void Ui::PushId(uint64_t v) { idStack_.push_back(Fnv(&v, sizeof v, idStack_.back())); }
void Ui::PopId() { if (idStack_.size() > 1) idStack_.pop_back(); }

void Ui::BeginColumn(const Rect& r, float gap) { col_.push_back({r, r.y, gap}); }
void Ui::EndColumn() { if (col_.size() > 1) col_.pop_back(); }
Rect Ui::Take(float h) {
  ColState& c = col_.back();
  Rect r{c.r.x, c.y, c.r.w, h};
  c.y += h + c.gap;
  return r;
}
void Ui::Space(float h) { col_.back().y += h; }
Rect Ui::Remaining() const { const ColState& c = col_.back(); return {c.r.x, c.y, c.r.w, std::max(0.f, c.r.bottom() - c.y)}; }

std::vector<Rect> Ui::Cols(const Rect& r, std::initializer_list<Col> cols, float gap) const {
  float fixed = 0, flex = 0;
  for (auto& c : cols) { if (c.fixed >= 0) fixed += c.fixed; else flex += c.flex; }
  const float n = (float)cols.size();
  const float avail = std::max(0.f, r.w - fixed - gap * (n - 1));
  std::vector<Rect> out;
  float x = r.x;
  for (auto& c : cols) {
    float w = c.fixed >= 0 ? c.fixed : (flex > 0 ? avail * c.flex / flex : 0);
    out.push_back({x, r.y, w, r.h});
    x += w + gap;
  }
  if (rtl_) for (auto& q : out) q.x = r.x + (r.right() - (q.x + q.w));
  return out;
}
Rect Ui::Mirror(const Rect& q, const Rect& within) const {
  if (!rtl_) return q;
  return {within.x + (within.right() - (q.x + q.w)), q.y, q.w, q.h};
}

// ------------------------------------------------------------------------------------------------ behaviour
Interaction Ui::Behavior(const Rect& r, uint64_t id, bool enabled, bool focusable) {
  Interaction res;
  const InputState& in = *in_;
  const bool blocked = popup_ != 0 && popup_ != id;
  const bool over = enabled && !blocked && in.inWindow && r.Contains(in.mx, in.my);
  if (over && (active_ == 0 || active_ == id)) hotNext_ = id;
  res.hot = over && (active_ == 0 || active_ == id);
  res.mx = in.mx - r.x; res.my = in.my - r.y;
  if (focusable && enabled) focusOrder_.push_back(id);
  if (res.hot && in.pressed && active_ == 0) {
    active_ = id; pressX_ = in.mx; pressY_ = in.my; res.pressed = true;
    if (focusable) { focus_ = id; focusVisible_ = false; }
    needFrame_ = true;
  }
  if (active_ == id) {
    res.active = true;
    res.dx = in.mx - pressX_; res.dy = in.my - pressY_;
    res.dragging = std::fabs(res.dx) > 3 || std::fabs(res.dy) > 3;
    if (!in.down || in.released) { res.released = true; res.clicked = over && enabled; active_ = 0; needFrame_ = true; }
    else needFrame_ = true;
  }
  res.focused = focus_ == id && in.focused;
  if (res.hot && enabled) { lastId_ = id; }
  lastRect_ = r;
  if (focusable && res.focused) {
    for (const auto& k : in.keys) if ((k.vk == VK_SPACE || k.vk == VK_RETURN) && !res.clicked) { res.clicked = true; }
  }
  return res;
}

bool Ui::WantsEscape() const {
  if (popup_) return true;
  auto it = edit_.find(focus_);
  return it != edit_.end() && it->second.editing;
}

float Ui::Anim(uint64_t id, float target, float seconds) {
  AnimState& a = anim_[id];
  if (!a.init) { a.v = target; a.init = true; return a.v; }
  if (Theme::Get().MotionScale() == 0.f) { a.v = target; return a.v; }
  float step = dt_ / std::max(seconds, 0.001f);
  if (a.v < target) a.v = std::min(target, a.v + step); else if (a.v > target) a.v = std::max(target, a.v - step);
  if (a.v != target) needFrame_ = true;
  return Smooth(std::clamp(a.v, 0.f, 1.f));
}

// ------------------------------------------------------------------------------------------------ simple widgets
void Ui::Label(const Rect& r, const std::wstring& s, TextStyle st, const Color* color) {
  p_->Text(r, s, color ? *color : pal_->text, st);
}
void Ui::LabelSecondary(const Rect& r, const std::wstring& s, TextStyle st) { p_->Text(r, s, pal_->textSecondary, st); }
void Ui::SectionTitle(const Rect& r, const std::wstring& s) {
  TextStyle st; st.weight = 700; st.size = kFont;
  p_->Text(r, s, pal_->heading, st);
}
void Ui::Divider(const Rect& r) { p_->FillRect({r.x, std::floor(r.cy()), r.w, 1.f}, pal_->divider); }

static ds::Color HoverTint(const Palette& pal, float hover, float press, const Color& base) {
  Color c = Mix(base, accent::base.WithAlpha(0.22f), hover);
  return Mix(c, accent::base.WithAlpha(0.36f), press);
}

bool Ui::Button(const Rect& r, const std::wstring& label, ButtonStyle st, bool enabled, const std::string& icon) {
  const uint64_t id = Id(label);
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  const float hov = Anim(id * 3 + 1, it.hot ? 1.f : 0.f), prs = Anim(id * 3 + 2, it.active && it.hot ? 1.f : 0.f, 0.06f);
  Color fill, text = pal_->text;
  switch (st) {
    case ButtonStyle::OnCanvas: fill = Mix(Color(1, 1, 1, 0.12f), accent::base.WithAlpha(0.50f), hov); text = Color::Hex(0xFFFFFF); break;
    case ButtonStyle::Primary: fill = Mix(Mix(accent::base, accent::hover, hov), accent::pressed, prs); text = accent::onAccent; break;
    case ButtonStyle::Quiet: fill = HoverTint(*pal_, hov, prs, Color(0, 0, 0, 0)); break;
    case ButtonStyle::Danger: fill = HoverTint(*pal_, hov, prs, pal_->controlFill); text = Color::Hex(0xE5484D); break;
    default: fill = HoverTint(*pal_, hov, prs, pal_->controlFill); break;
  }
  if (!enabled) { fill = pal_->controlFillDisabled; text = pal_->textDisabled; if (st == ButtonStyle::Primary) fill = Mix(accent::base, Color(1, 1, 1, 0), 0.6f).WithAlpha(0.35f); }
  p_->FillRoundRect(r, control::buttonRadius, fill);
  if (st == ButtonStyle::Secondary) p_->StrokeRoundRect(r, control::buttonRadius, pal_->divider.WithAlpha(enabled ? 0.10f : 0.04f), 1.f);
  if (it.focused && focusVisible_) p_->StrokeRoundRect(r.Inset(-2), control::buttonRadius + 2, accent::base.WithAlpha(0.9f), 2.f);
  TextStyle ts; ts.weight = 500; ts.align = Align::Center; ts.size = kFont;
  if (icon.empty()) p_->Text(r, label, text, ts);
  else {
    const float iw = 14, gap = 6, tw = p_->TextWidth(label, ts);
    float total = iw + gap + tw, x0 = r.cx() - total / 2;
    if (rtl_) { p_->Icon(icon, x0 + total - iw / 2, r.cy(), iw, text, 1.6f); p_->Text({x0, r.y, tw, r.h}, label, text, ts); }
    else { p_->Icon(icon, x0 + iw / 2, r.cy(), iw, text, 1.6f); p_->Text({x0 + iw + gap, r.y, tw + 2, r.h}, label, text, ts); }
  }
  return it.clicked && enabled;
}

bool Ui::IconButton(const Rect& r, const std::string& icon, bool enabled, const std::wstring& tip, bool filled) {
  const uint64_t id = Id(icon + "#ib");
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  const float hov = Anim(id * 3 + 1, it.hot ? 1.f : 0.f), prs = Anim(id * 3 + 2, it.active && it.hot ? 1.f : 0.f, 0.06f);
  Color base = filled ? pal_->controlFill : Color(0, 0, 0, 0);
  p_->FillRoundRect(r, std::min(r.w, r.h) * 0.5f > control::buttonRadius ? control::buttonRadius + 2 : std::min(r.w, r.h) * 0.5f, enabled ? HoverTint(*pal_, hov, prs, base) : Color(0, 0, 0, 0));
  p_->Icon(icon, r.cx(), r.cy(), std::min(r.w, r.h) * (filled ? 0.56f : 0.70f), enabled ? Mix(pal_->textSecondary, accent::base, hov) : pal_->textDisabled, 1.6f);
  if (it.focused && focusVisible_) p_->StrokeRoundRect(r.Inset(-2), 10, accent::base.WithAlpha(0.9f), 2.f);
  if (!tip.empty()) Tooltip(tip);
  return it.clicked && enabled;
}

bool Ui::CloseButton(const Rect& r,bool glass,bool enabled) {
  const uint64_t id=Id("close#x");auto it=Behavior(r,id,enabled,true);
  if(it.hot)cursor_=(int)(intptr_t)IDC_HAND;
  float hover=Anim(id*3+1,it.hot?1.f:0.f),pressed=Anim(id*3+2,it.active&&it.hot?1.f:0.f,.08f);
  if(glass) {
    float scale=Theme::Get().MotionScale()>0?(1+.03f*hover)*(1-pressed)+.97f*pressed:1.f;
    float size=std::min(r.w,r.h)*scale;Rect bubble{r.cx()-size/2,r.cy()-size/2,size,size};
    bool live=closeBubble_&&closeBubble_(bubble);
    p_->GlassClose(bubble,Theme::Get().IsDark(),hover,pressed,enabled,live);
  } else {
    float rad=std::min(r.w,r.h)*.5f;
    p_->FillCircle(r.cx(),r.cy(),rad,HoverTint(*pal_,hover,pressed,pal_->controlFill));
    p_->Icon("close",r.cx(),r.cy(),rad*.9f,pal_->text,1.7f);
  }
  if(it.focused&&focusVisible_)p_->StrokeCircle(r.cx(),r.cy(),r.w/2+3,accent::base.WithAlpha(.9f),1.5f);
  return enabled&&it.clicked;
}

int Ui::Segmented(const Rect& r, const std::vector<std::wstring>& labels, int sel, bool enabled) {
  if (labels.empty()) return sel;
  const uint64_t id = Id(labels[0] + L"#seg");
  const int n = (int)labels.size();
  p_->FillRoundRect(r, control::buttonRadius, enabled ? pal_->controlFill : pal_->controlFillDisabled);
  const float segW = r.w / n;
  int result = sel;
  PushId(id);
  for (int i = 0; i < n; ++i) {
    const int vi = rtl_ ? n - 1 - i : i;
    Rect sr{r.x + segW * vi, r.y, segW, r.h};
    PushId(i);
    Interaction it = Behavior(sr, Id("s"), enabled, true);
    if (it.hot) cursor_ = (int)(intptr_t)IDC_HAND;
    const float on = Anim(Id("sel"), i == sel ? 1.f : 0.f), hov = Anim(Id("hov"), it.hot && i != sel ? 1.f : 0.f);
    Rect in = sr.Inset(2);
    if (on > 0.01f) {
      p_->Shadow(in, control::buttonRadius - 2, 3, Color(0, 0, 0, 0.12f * on), 1);
      p_->FillRoundRect(in, control::buttonRadius - 2, Color(pal_->controlFillPressed.r, pal_->controlFillPressed.g, pal_->controlFillPressed.b, pal_->controlFillPressed.a * on));
    }
    if (hov > 0.01f) p_->FillRoundRect(in, control::buttonRadius - 2, accent::base.WithAlpha(0.16f * hov));
    TextStyle ts; ts.weight = 500; ts.align = Align::Center; ts.size = kFont;
    p_->Text(sr, labels[i], enabled ? pal_->text : pal_->textDisabled, ts);
    if (it.focused && focusVisible_) p_->StrokeRoundRect(in, control::buttonRadius - 2, accent::base, 2.f);
    if (it.clicked && enabled) result = i;
    PopId();
    if (i + 1 < n && i != sel && i + 1 != sel) p_->FillRect({sr.right() - 0.5f, r.y + 7, 1.f, r.h - 14}, pal_->divider);
  }
  PopId();
  return result;
}

bool Ui::Toggle(const Rect& r0, bool& v, bool enabled) {
  const uint64_t id = AutoId("toggle");
  const float w = 38, h = 22;
  Rect r{r0.x + (r0.w - w) * (rtl_ ? 1.f : 0.f), r0.cy() - h / 2, w, h};
  if (rtl_) r.x = r0.right() - w;
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  bool changed = false;
  if (it.clicked && enabled) { v = !v; changed = true; }
  const float on = Anim(id * 3 + 1, v ? 1.f : 0.f, 0.14f), hov = Anim(id * 3 + 2, it.hot ? 1.f : 0.f);
  Color off = pal_->trackInactive;
  Color track = Mix(off, Mix(accent::base, accent::hover, hov), on);
  if (!enabled) track.a *= 0.5f;
  p_->FillRoundRect(r, h / 2, track);
  const float kx = r.x + 2 + (w - h) * on + (h - 4) / 2;
  p_->Shadow({kx - (h - 4) / 2, r.y + 2, h - 4, h - 4}, (h - 4) / 2, 4, Color(0, 0, 0, 0.35f), 1);
  p_->FillCircle(kx, r.cy(), (h - 4) / 2, Color(1, 1, 1, enabled ? 1.f : 0.7f));
  if (it.focused && focusVisible_) p_->StrokeRoundRect(r.Inset(-2), h / 2 + 2, accent::base.WithAlpha(0.9f), 2.f);
  return changed;
}

bool Ui::Checkbox(const Rect& r, const std::wstring& label, bool& v, bool enabled) {
  const uint64_t id = Id(label + L"#chk");
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  bool changed = false;
  if (it.clicked && enabled) { v = !v; changed = true; }
  const float on = Anim(id * 3 + 1, v ? 1.f : 0.f, 0.1f), hov = Anim(id * 3 + 2, it.hot ? 1.f : 0.f);
  const float s = control::checkbox + 2;
  Rect box{rtl_ ? r.right() - s : r.x, r.cy() - s / 2, s, s};
  Color off = Mix(pal_->controlFillPressed, accent::base.WithAlpha(0.30f), hov);
  p_->FillRoundRect(box, 5, enabled ? Mix(off, accent::base, on) : pal_->controlFillDisabled);
  if (on > 0.02f) p_->Icon("check", box.cx(), box.cy(), s * 0.78f, Color(1, 1, 1, on), 2.2f);
  TextStyle ts; ts.size = kFont;
  Rect tr{rtl_ ? r.x : r.x + s + 8, r.y, r.w - s - 8, r.h};
  p_->Text(tr, label, enabled ? pal_->text : pal_->textDisabled, ts);
  if (it.focused && focusVisible_) p_->StrokeRoundRect(box.Inset(-2), 7, accent::base.WithAlpha(0.9f), 2.f);
  return changed;
}

bool Ui::Slider(const Rect& r, float& v, float lo, float hi, float step, float neutral, bool enabled) {
  const uint64_t id = AutoId("slider");
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  const float tw = control::sliderThumbW, th = control::sliderThumbH;
  const float x0 = r.x + tw / 2, x1 = r.right() - tw / 2;
  bool changed = false;
  auto setFromX = [&](float mx) {
    float t = std::clamp((mx - x0) / std::max(1.f, x1 - x0), 0.f, 1.f);
    if (rtl_) t = 1.f - t;
    float nv = lo + t * (hi - lo);
    if (step > 0) nv = std::round((nv - lo) / step) * step + lo;
    nv = std::clamp(nv, lo, hi);
    if (nv != v) { v = nv; changed = true; }
  };
  if (enabled) {
    if (it.pressed || it.active) setFromX(in_->mx);
    if (it.focused) for (const auto& k : in_->keys) {
      float st = step > 0 ? step : (hi - lo) / 100.f;
      if (k.vk == VK_LEFT || k.vk == VK_DOWN) { v = std::max(lo, v - st * (k.shift ? 10 : 1)); changed = true; }
      if (k.vk == VK_RIGHT || k.vk == VK_UP) { v = std::min(hi, v + st * (k.shift ? 10 : 1)); changed = true; }
    }
  }
  float t = hi > lo ? (v - lo) / (hi - lo) : 0.f;
  if (rtl_) t = 1.f - t;
  const float tx = x0 + (x1 - x0) * t;
  const float ty = r.cy();
  const float trackH = control::sliderTrack;
  Rect track{r.x + 1, ty - trackH / 2, r.w - 2, trackH};
  p_->FillRoundRect(track, trackH / 2, enabled ? pal_->trackInactive : Color(pal_->trackInactive.r, pal_->trackInactive.g, pal_->trackInactive.b, pal_->trackInactive.a * 0.5f));
  Rect fill = rtl_ ? Rect{tx, track.y, track.right() - tx, trackH} : Rect{track.x, track.y, tx - track.x, trackH};
  const float hov = Anim(id * 3 + 1, (it.hot || it.active) ? 1.f : 0.f);
  Color fc = Mix(accent::base, accent::hover, hov);
  if (!enabled) fc.a *= 0.4f;
  p_->FillRoundRect(fill, trackH / 2, fc);
  Rect thumb{tx - tw / 2, ty - th / 2, tw, th};
  p_->Shadow(thumb, th / 2, 5, Color(0, 0, 0, 0.30f), 1.5f);
  p_->FillRoundRect(thumb, th / 2, Color(1, 1, 1, enabled ? 1.f : 0.75f));
  if (hov > 0.01f) p_->StrokeRoundRect(thumb, th / 2, accent::base.WithAlpha(0.85f * hov), 1.5f);
  if (it.focused && focusVisible_) p_->StrokeRoundRect(thumb.Inset(-3), th / 2 + 3, accent::base.WithAlpha(0.9f), 2.f);
  (void)neutral;
  return changed;
}

bool Ui::SliderRow(const Rect& row, const std::wstring& label, float& v, float lo, float hi, float step, float def,
                   const std::function<std::wstring(float)>& fmt, bool enabled, float labelW, float valueW) {
  auto c = Cols(row, {Fixed(labelW), Flex(1), Fixed(valueW), Fixed(32)}, 12);   // [M] label column 135 dip, reset glyph ~18 dip
  PushId(Id(label));
  TextStyle ts; ts.size = kFont;
  p_->Text(c[0], label, enabled ? pal_->text : pal_->textDisabled, ts);
  bool ch = Slider(c[1], v, lo, hi, step, def, enabled);
  TextStyle vs; vs.size = kFont; vs.align = Align::End;
  p_->Text(c[2], fmt ? fmt(v) : std::to_wstring((int)std::lround(v)), enabled ? pal_->text : pal_->textDisabled, vs);
  const bool atDef = std::fabs(v - def) < 1e-4f;
  Rect rb{c[3].x + 3, c[3].cy() - 13, 26, 26};
  if (IconButton(rb, "reset", enabled && !atDef, L"Reset", false)) { v = def; ch = true; }
  PopId();
  return ch;
}

// ------------------------------------------------------------------------------------------------ text input
bool Ui::TextInput(const Rect& r, std::wstring& s, const std::wstring& placeholder, bool numericOnly, bool enabled) {
  const uint64_t id = AutoId("input");
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_IBEAM;
  EditState& es = edit_[id];
  const bool focused = it.focused;
  bool changed = false;
  TextStyle ts; ts.size = kFont; ts.ellipsis = false; ts.vcenter = true;
  const float pad = 9;
  Rect tr{r.x + pad, r.y, r.w - pad * 2, r.h};
  auto clampCaret = [&] { es.caret = std::clamp(es.caret, 0, (int)s.size()); es.anchor = std::clamp(es.anchor, 0, (int)s.size()); };
  if (focused && !es.editing) { es.editing = true; es.original = s; es.blink = now_; es.caret = es.anchor = (int)s.size(); }
  if (!focused && es.editing) es.editing = false;
  if (enabled && it.pressed) {
    float x = in_->mx - tr.x + es.scroll;
    es.caret = p_->HitIndex(s, ts, x); es.anchor = es.caret; es.blink = now_;
  }
  if (enabled && it.active && it.dragging) { es.caret = p_->HitIndex(s, ts, in_->mx - tr.x + es.scroll); }
  if (enabled && it.hot && in_->dbl) { es.anchor = 0; es.caret = (int)s.size(); }
  if (focused && enabled) {
    clampCaret();
    auto eraseSel = [&] { int a = std::min(es.anchor, es.caret), b = std::max(es.anchor, es.caret); if (a != b) { s.erase(a, b - a); es.caret = es.anchor = a; changed = true; return true; } return false; };
    for (wchar_t ch : in_->chars) {
      if (ch < 32 || ch == 127) continue;
      if (numericOnly && !(iswdigit(ch) || ch == L'.' || ch == L'-' || ch == L'x' || ch == L':' || ch == L'%')) continue;
      eraseSel(); s.insert(s.begin() + es.caret, ch); ++es.caret; es.anchor = es.caret; changed = true; es.blink = now_;
    }
    for (const auto& k : in_->keys) {
      es.blink = now_;
      switch (k.vk) {
        case VK_LEFT: if (es.caret > 0) --es.caret; if (!k.shift) es.anchor = es.caret; break;
        case VK_RIGHT: if (es.caret < (int)s.size()) ++es.caret; if (!k.shift) es.anchor = es.caret; break;
        case VK_HOME: es.caret = 0; if (!k.shift) es.anchor = 0; break;
        case VK_END: es.caret = (int)s.size(); if (!k.shift) es.anchor = es.caret; break;
        case VK_BACK: if (!eraseSel() && es.caret > 0) { s.erase(es.caret - 1, 1); --es.caret; es.anchor = es.caret; changed = true; } break;
        case VK_DELETE: if (!eraseSel() && es.caret < (int)s.size()) { s.erase(es.caret, 1); changed = true; } break;
        case 'A': if (k.ctrl) { es.anchor = 0; es.caret = (int)s.size(); } break;
        case 'C': if (k.ctrl) { int a = std::min(es.anchor, es.caret), b = std::max(es.anchor, es.caret); if (a != b) SetClipboardText(s.substr(a, b - a)); } break;
        case 'X': if (k.ctrl) { int a = std::min(es.anchor, es.caret), b = std::max(es.anchor, es.caret); if (a != b) { SetClipboardText(s.substr(a, b - a)); eraseSel(); } } break;
        case 'V': if (k.ctrl) {
          std::wstring t = ClipboardText();
          std::wstring clean; for (wchar_t ch : t) if (ch >= 32 && !(numericOnly && !(iswdigit(ch) || ch == L'.' || ch == L'-'))) clean.push_back(ch);
          eraseSel(); s.insert(es.caret, clean); es.caret += (int)clean.size(); es.anchor = es.caret; changed = true; } break;
        case VK_ESCAPE: if (es.original != s) { s = es.original; es.caret = es.anchor = (int)s.size(); changed = true; } focus_ = 0; break;
        case VK_RETURN: focus_ = 0; break;
      }
    }
    clampCaret();
    needFrame_ = true;      // caret blink
  }
  // field visuals
  const float foc = Anim(id * 3 + 1, focused ? 1.f : 0.f, 0.1f), hov = Anim(id * 3 + 2, it.hot ? 1.f : 0.f);
  p_->FillRoundRect(r, control::buttonRadius, Mix(pal_->fieldFill, Color(1, 1, 1, std::min(1.f, pal_->fieldFill.a + 0.15f)), foc));
  p_->StrokeRoundRect(r, control::buttonRadius, Mix(Mix(pal_->divider, accent::base.WithAlpha(0.45f), hov), accent::base, foc), 1.f + foc * 0.5f);
  p_->PushClip(tr);
  const float caretX = p_->CaretX(s, ts, es.caret);
  if (focused) {
    if (caretX - es.scroll > tr.w - 4) es.scroll = caretX - tr.w + 4;
    if (caretX - es.scroll < 0) es.scroll = caretX;
  } else es.scroll = 0;
  Rect text{tr.x - es.scroll, tr.y, std::max(tr.w, p_->TextWidth(s, ts) + 8), tr.h};
  if (focused && es.anchor != es.caret) {
    float a = p_->CaretX(s, ts, std::min(es.anchor, es.caret)), b = p_->CaretX(s, ts, std::max(es.anchor, es.caret));
    p_->FillRect({text.x + a, r.y + 5, b - a, r.h - 10}, accent::base.WithAlpha(0.28f));
  }
  if (s.empty() && !placeholder.empty()) p_->Text(text, placeholder, pal_->textTertiary, ts);
  else p_->Text(text, s, enabled ? pal_->text : pal_->textDisabled, ts);
  if (focused && std::fmod(now_ - es.blink, 1.0) < 0.55) p_->FillRect({text.x + caretX, r.y + 6, 1.2f, r.h - 12}, pal_->text);
  p_->PopClip();
  if (!enabled) p_->FillRoundRect(r, control::buttonRadius, Color(1, 1, 1, 0.25f));
  return changed;
}

bool Ui::Stepper(const Rect& r, double& v, double lo, double hi, double step, int decimals, const std::wstring& suffix, bool enabled) {
  const uint64_t id = AutoId("stepper");
  PushId(id);
  const float bw = 22;
  Rect field{r.x, r.y, r.w - bw - 4, r.h};
  Rect up{r.right() - bw, r.y, bw, r.h / 2 - 1}, dn{r.right() - bw, r.y + r.h / 2 + 1, bw, r.h / 2 - 1};
  wchar_t buf[64]; swprintf_s(buf, L"%.*f", decimals, v);
  std::wstring txt = buf;
  bool changed = false;
  std::wstring before = txt;
  if (TextInput(field, txt, L"", true, enabled) && txt != before) {
    wchar_t* end = nullptr; double nv = wcstod(txt.c_str(), &end);
    if (end != txt.c_str()) { nv = std::clamp(nv, lo, hi); if (nv != v) { v = nv; changed = true; } }
  }
  if (!suffix.empty()) {
    TextStyle ss; ss.size = ds::dialog::smallFontSize; ss.align = Align::End;
    p_->Text({field.x, field.y, field.w - 8, field.h}, suffix, pal_->textTertiary, ss);
  }
  if (IconButton(up, "chevronup", enabled && v < hi, L"", true)) { v = std::min(hi, v + step); changed = true; }
  if (IconButton(dn, "chevrondown", enabled && v > lo, L"", true)) { v = std::max(lo, v - step); changed = true; }
  PopId();
  return changed;
}

bool Ui::Dropdown(const Rect& r, const std::vector<std::wstring>& items, int& sel, bool enabled) {
  const uint64_t id = AutoId("dropdown");
  const bool open = popup_ == id;
  Interaction it = Behavior(r, id, enabled, true);
  if (it.hot && enabled) cursor_ = (int)(intptr_t)IDC_HAND;
  bool changed = false;
  const float hov = Anim(id * 3 + 1, it.hot ? 1.f : 0.f);
  p_->FillRoundRect(r, control::buttonRadius, enabled ? HoverTint(*pal_, hov, 0, pal_->controlFill) : pal_->controlFillDisabled);
  TextStyle ts; ts.size = kFont; ts.weight = 500;
  Rect tr{r.x + 10, r.y, r.w - 34, r.h};
  if (rtl_) tr = {r.x + 24, r.y, r.w - 34, r.h};
  p_->Text(tr, sel >= 0 && sel < (int)items.size() ? items[sel] : L"", enabled ? pal_->text : pal_->textDisabled, ts);
  p_->Icon("chevrondown", rtl_ ? r.x + 14 : r.right() - 14, r.cy(), 12, enabled ? pal_->textSecondary : pal_->textDisabled, 1.8f);
  if (it.clicked && enabled && !open) { popup_ = id; needFrame_ = true; }
  else if (it.clicked && open) { popup_ = 0; }
  if (open) {
    const float rowH = 28;
    Rect list{r.x, r.bottom() + 4, r.w, rowH * items.size() + 8};
    if (list.bottom() > h_ - 4) list.y = std::max(4.f, r.y - 4 - list.h);
    needFrame_ = true;
    int hover = -1;
    if (in_->inWindow && list.Contains(in_->mx, in_->my)) hover = (int)((in_->my - list.y - 4) / rowH);
    if (in_->pressed) {
      if (hover >= 0 && hover < (int)items.size()) { sel = hover; changed = true; popup_ = 0; }
      else if (!r.Contains(in_->mx, in_->my)) popup_ = 0;
    }
    for (const auto& k : in_->keys) if (k.vk == VK_ESCAPE) popup_ = 0;
    const Palette* pal = pal_;
    std::vector<std::wstring> itemsCopy = items;
    const int selCopy = sel;
    const bool dark = Theme::Get().IsDark();
    Defer([=](Painter& p) {
      p.Shadow(list, 12, 18, Color(0, 0, 0, 0.35f), 6);
      p.FillRoundRect(list, 12, dark ? Color::Hex(0x2A2B2F, 0.98f) : Color::Hex(0xFFFFFF, 0.98f));
      p.StrokeRoundRect(list, 12, pal->divider, 1.f);
      for (int i = 0; i < (int)itemsCopy.size(); ++i) {
        Rect rr{list.x + 4, list.y + 4 + i * rowH, list.w - 8, rowH};
        if (i == hover) p.FillRoundRect(rr, 8, accent::base.WithAlpha(0.22f));
        TextStyle s2; s2.size = kFont; s2.weight = i == selCopy ? 600 : 400;
        p.Text({rr.x + 8, rr.y, rr.w - 32, rr.h}, itemsCopy[i], pal->text, s2);
        if (i == selCopy) p.Icon("check", rr.right() - 14, rr.cy(), 13, accent::base, 2.f);
      }
    });
  }
  return changed;
}

bool Ui::Swatch(const Rect& r, const Color& top, const Color& bottom, bool selected, const std::wstring& tip) {
  const uint64_t id = AutoId("swatch");
  Interaction it = Behavior(r, id, true, true);
  if (it.hot) cursor_ = (int)(intptr_t)IDC_HAND;
  const float hov = Anim(id * 3 + 1, it.hot ? 1.f : 0.f), sel = Anim(id * 3 + 2, selected ? 1.f : 0.f, 0.12f);
  Rect rr = r.Inset(-1.5f * hov);
  p_->FillRoundRectGradient(rr, control::swatchRadius, top, bottom);
  p_->StrokeRoundRect(rr, control::swatchRadius, Color(0, 0, 0, 0.10f), 1.f);
  if (sel > 0.01f) p_->StrokeRoundRect(r.Inset(-3), control::swatchRadius + 3, accent::base.WithAlpha(sel), 2.5f);
  if (it.focused && focusVisible_) p_->StrokeRoundRect(r.Inset(-4), control::swatchRadius + 4, accent::base.WithAlpha(0.6f), 1.5f);
  if (!tip.empty()) Tooltip(tip);
  return it.clicked;
}

void Ui::ProgressBar(const Rect& r, float v, bool indeterminate) {
  const float h = std::min(r.h, 6.f);
  Rect t{r.x, r.cy() - h / 2, r.w, h};
  p_->FillRoundRect(t, h / 2, pal_->trackInactive);
  if (indeterminate) {
    const float seg = t.w * 0.28f;
    float ph = (float)std::fmod(now_ * 0.9, 1.0);
    float x = t.x - seg + (t.w + seg) * ph;
    p_->PushClip(t);
    p_->FillRoundRect({x, t.y, seg, h}, h / 2, accent::base);
    p_->PopClip();
    needFrame_ = true;
  } else {
    float w = t.w * std::clamp(v, 0.f, 1.f);
    if (w > 0) p_->FillRoundRect({rtl_ ? t.right() - w : t.x, t.y, std::max(w, h), h}, h / 2, accent::base);
  }
}

void Ui::Tooltip(const std::wstring& text) {
  if (text.empty()) return;
  if (hot_ == lastId_ && lastId_ != 0 && lastRect_.Contains(in_->mx, in_->my) && popup_ == 0) {
    if (tipId_ != lastId_) { tipId_ = lastId_; tipSince_ = now_; }
    if (now_ - tipSince_ >= 0.6) {
      const Rect anchor = lastRect_;
      const bool dark = Theme::Get().IsDark();
      const float W = w_, H = h_;
      Defer([=](Painter& p) {
        TextStyle ts; ts.size = ds::dialog::smallFontSize; ts.ellipsis = false; ts.wrap = true;
        auto m = p.Measure(text, ts, std::max(40.f, std::min(520.f, W - 44)));
        Rect bubble{anchor.cx() - (m.w + 20) / 2, anchor.bottom() + 8, m.w + 20, m.h + 12};
        bubble.x = std::clamp(bubble.x, 4.f, std::max(4.f, W - bubble.w - 4));
        if (bubble.bottom() > H - 4) bubble.y = anchor.y - 8 - bubble.h;
        p.Shadow(bubble, 8, 12, Color(0, 0, 0, 0.25f), 3);
        p.FillRoundRect(bubble, 8, dark ? Color::Hex(0x3A3B40, 0.98f) : Color::Hex(0x2B2C30, 0.96f));
        p.Text(bubble, text, Color(1, 1, 1, 0.95f), [&] { TextStyle t = ts; t.align = Align::Center; return t; }());
      });
    }
  }
}

float Ui::BeginScroll(const Rect& vp, float contentH, uint64_t id) {
  Scroll& s = scroll_[id];
  s.contentH = contentH; s.viewH = vp.h;
  const float maxY = std::max(0.f, contentH - vp.h);
  if (in_->inWindow && vp.Contains(in_->mx, in_->my) && popup_ == 0 && in_->wheel != 0) { s.y -= in_->wheel * 44.f; needFrame_ = true; }
  s.y = std::clamp(s.y, 0.f, maxY);
  scrollStack_.push_back(id);
  p_->PushClip(vp);
  // scroll bar
  if (maxY > 0) {
    const float trackH = vp.h - 8, thumbH = std::max(28.f, trackH * vp.h / contentH);
    const float ty = vp.y + 4 + (trackH - thumbH) * (s.y / maxY);
    const float vis = Anim(id * 7 + 5, (in_->inWindow && vp.Contains(in_->mx, in_->my)) ? 1.f : 0.f, 0.2f);
    Rect thumb{vp.right() - 7, ty, 4, thumbH};
    deferred_.push_back([thumb, vis](Painter& p) { p.FillRoundRect(thumb, 2, Color(0.5f, 0.5f, 0.55f, 0.25f + 0.35f * vis)); });
  }
  return s.y;
}
void Ui::EndScroll() {
  if (!scrollStack_.empty()) scrollStack_.pop_back();
  p_->PopClip();
}

} // namespace filo::ui
