// Immediate-mode widget layer. Every Filo tool dialog, the settings window, the floating shelf and the progress HUD are
// written against this one toolkit so they share exactly the same metrics, states and motion (design tokens only).
//
// Usage inside a DialogWindow::Draw():
//     Rect row = ui.Take(27);
//     auto c = ui.Cols(row, {Fixed(90), Flex(1), Fixed(60)});
//     ui.Label(c[0], L"Exposure");
//     ui.Slider(c[1], v, -5, 5);
//
// Widget identity comes from ui.Id()/PushId() scopes. Persistent per-widget state (hover animation, text caret...) lives in
// the Ui object, never in the caller.
#pragma once
#include "ui/painter.h"
#include "design/theme.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filo::ui {

struct KeyEvent { uint32_t vk; bool ctrl, shift, alt; };

struct InputState {
  float mx = -10000, my = -10000;            // DIP, window-relative
  bool down = false, pressed = false, released = false, dbl = false;
  bool rpressed = false;
  float wheel = 0;                           // positive = scroll up (in lines)
  std::wstring chars;                        // text typed this frame (UTF-16)
  std::vector<KeyEvent> keys;                // key presses this frame (with auto-repeat)
  bool ctrl = false, shift = false, alt = false;
  bool inWindow = false;
  bool focused = true;                       // window has keyboard focus
  void ClearTransient() { pressed = released = dbl = rpressed = false; wheel = 0; chars.clear(); keys.clear(); }
};

struct Col { float fixed = -1, flex = 0; };
inline Col Fixed(float w) { return {w, 0}; }
inline Col Flex(float f = 1) { return {-1, f}; }

enum class ButtonStyle { Secondary, Primary, Quiet, Danger, OnCanvas };

struct Interaction {
  bool hot = false;        // mouse over
  bool active = false;     // mouse captured by this widget
  bool pressed = false;    // became active this frame
  bool released = false;   // mouse released this frame (may be outside)
  bool clicked = false;    // released while still over the widget after pressing on it
  bool dragging = false;
  float mx = 0, my = 0;    // mouse position relative to the rect
  float dx = 0, dy = 0;    // drag delta since the press
  bool focused = false;
};

class Ui {
 public:
  // ---- frame plumbing
  void BeginFrame(Painter& p, const InputState& in, float w, float h, double now, bool rtl);
  // Optional surface palette; null restores the selected theme palette.
  void SetPaletteOverride(const ds::Palette* p) { palOverride_ = p; }
  // Returns true when another frame must be rendered (animation running or a state change needs a repaint).
  bool EndFrame();
  void RequestFrame() { needFrame_ = true; }
  float Width() const { return w_; }
  float Height() const { return h_; }
  Painter& P() { return *p_; }
  const ds::Palette& Pal() const { return *pal_; }
  const InputState& In() const { return *in_; }
  bool Rtl() const { return rtl_; }
  double Now() const { return now_; }

  // ---- ids
  uint64_t Id(std::string_view s) const;
  uint64_t Id(const std::wstring& s) const;
  uint64_t AutoId(const char* kind);                 // stable id from call order within the current id scope
  void PushId(uint64_t v);
  void PopId();
  void PushId(int i) { PushId((uint64_t)(uint32_t)i * 0x9E3779B97F4A7C15ull); }

  // ---- layout (vertical cursor in the current column)
  void BeginColumn(const Rect& r, float gap = 8);
  void EndColumn();
  Rect Take(float h);                                // next row of height h
  void Space(float h);
  Rect Remaining() const;                            // rect from the cursor to the bottom of the column
  float CursorY() const { return col_.back().y; }
  std::vector<Rect> Cols(const Rect& r, std::initializer_list<Col> cols, float gap = 8) const;   // RTL mirrors the order
  Rect Mirror(const Rect& r, const Rect& within) const;  // flips x inside `within` when RTL

  // ---- low-level behaviour for custom widgets
  Interaction Behavior(const Rect& r, uint64_t id, bool enabled = true, bool focusable = false);
  bool Hot(uint64_t id) const { return hot_ == id; }
  bool Active(uint64_t id) const { return active_ == id; }
  void Focus(uint64_t id) { focus_ = id; focusVisible_ = false; RequestFrame(); }
  void CancelPointer() { active_=hot_=hotNext_=0; RequestFrame(); }
  bool Focused(uint64_t id) const { return focus_ == id; }
  float Anim(uint64_t id, float target, float seconds = ds::motion::uiFast);   // eased 0..1 follow
  bool WantsEscape() const;                                                    // a popup or text field consumes Esc/Enter
  void SetCursor(int idcCursor) { cursor_ = idcCursor; }                       // IDC_HAND etc., applied by the window
  int CursorWanted() const { return cursor_; }

  // ---- widgets (return true when the user changed/activated them)
  void Label(const Rect& r, const std::wstring& s, TextStyle st = {}, const ds::Color* color = nullptr);
  void LabelSecondary(const Rect& r, const std::wstring& s, TextStyle st = {});
  void SectionTitle(const Rect& r, const std::wstring& s);                        // "Light", "Color" headings
  void Divider(const Rect& r);                                                    // horizontal hairline, vertically centred
  bool Button(const Rect& r, const std::wstring& label, ButtonStyle st = ButtonStyle::Secondary, bool enabled = true, const std::string& icon = {});
  bool IconButton(const Rect& r, const std::string& icon, bool enabled = true, const std::wstring& tip = L"", bool filled = true);
  void SetCloseBubbleProvider(std::function<bool(const Rect&)> callback) { closeBubble_=std::move(callback); }
  bool CloseButton(const Rect& r, bool glass = true, bool enabled = true);                                                // the circular title-bar close button
  int Segmented(const Rect& r, const std::vector<std::wstring>& labels, int selected, bool enabled = true);   // returns new selection
  bool Toggle(const Rect& r, bool& v, bool enabled = true);
  bool Checkbox(const Rect& r, const std::wstring& label, bool& v, bool enabled = true);
  bool Slider(const Rect& r, float& v, float lo, float hi, float step = 0, float neutral = NAN, bool enabled = true);
  // label | slider | value text | reset button  (the Edit Photo row)
  bool SliderRow(const Rect& row, const std::wstring& label, float& v, float lo, float hi, float step, float def,
                 const std::function<std::wstring(float)>& fmt, bool enabled = true, float labelW = 135.f, float valueW = 70.f);
  bool TextInput(const Rect& r, std::wstring& s, const std::wstring& placeholder = L"", bool numericOnly = false, bool enabled = true);
  bool Stepper(const Rect& r, double& v, double lo, double hi, double step, int decimals = 0, const std::wstring& suffix = L"", bool enabled = true);
  bool Dropdown(const Rect& r, const std::vector<std::wstring>& items, int& sel, bool enabled = true);
  bool Swatch(const Rect& r, const ds::Color& top, const ds::Color& bottom, bool selected, const std::wstring& tip = L"");
  void ProgressBar(const Rect& r, float v, bool indeterminate = false);
  void Tooltip(const std::wstring& text);                                          // for the widget just drawn
  // Vertical scroll area: returns the y offset to subtract from content; draws a thin scroll bar.
  float BeginScroll(const Rect& viewport, float contentH, uint64_t id);
  void EndScroll();

  // Deferred drawing on top of everything (popups, tooltips).
  void Defer(std::function<void(Painter&)> fn) { deferred_.push_back(std::move(fn)); }

  // Text editing helpers needed by TextInput (public for dialogs with custom inputs)
  struct EditState { int caret = 0, anchor = 0; float scroll = 0; std::wstring original; bool editing = false; double blink = 0; };

 private:
  struct AnimState { float v = 0; bool init = false; };
  struct Scroll { float y = 0; float contentH = 0; float viewH = 0; };

  std::function<bool(const Rect&)> closeBubble_;
  Painter* p_ = nullptr;
  const InputState* in_ = nullptr;
  const ds::Palette* pal_ = nullptr;
  const ds::Palette* palOverride_ = nullptr;
  float w_ = 0, h_ = 0;
  double now_ = 0, last_ = 0;
  float dt_ = 0;
  bool rtl_ = false;
  bool needFrame_ = false;
  int cursor_ = 0;

  std::vector<uint64_t> idStack_;
  std::unordered_map<uint64_t, int> autoCounters_;
  struct ColState { Rect r; float y; float gap; };
  std::vector<ColState> col_;

  uint64_t hot_ = 0, hotNext_ = 0, active_ = 0, focus_ = 0, popup_ = 0;
  bool focusVisible_ = false;
  float pressX_ = 0, pressY_ = 0;
  std::vector<uint64_t> focusOrder_, prevFocusOrder_;
  std::unordered_map<uint64_t, AnimState> anim_;
  std::unordered_map<uint64_t, EditState> edit_;
  std::unordered_map<uint64_t, Scroll> scroll_;
  std::vector<uint64_t> scrollStack_;
  std::vector<std::function<void(Painter&)>> deferred_;
  uint64_t tipId_ = 0; double tipSince_ = 0; std::wstring tipText_; Rect tipRect_{};
  Rect lastRect_{};
  uint64_t lastId_ = 0;
  // dropdown popup request recorded this frame
  struct PopupReq { uint64_t id; Rect anchor; const std::vector<std::wstring>* items; int* sel; bool* changed; };
  std::vector<std::function<void()>> endActions_;
};

} // namespace filo::ui
