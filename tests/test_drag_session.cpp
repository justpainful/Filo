#include "test.h"
#include "shell/drag_session.h"

using namespace filo;
using namespace filo::shell;
using filo::wheel::WheelKind;

namespace {
struct Fake : IDragServices {
  bool dragImage = true, shell = true;
  std::vector<std::filesystem::path> sel{L"C:\\a.png"};
  POINT cursor{500, 400};
  int opens = 0, switches = 0, closes = 0, prefetches = 0;
  WheelKind lastKind = WheelKind::Convert;
  bool menuSuppressed = false;
  std::vector<int> timers;
  bool DragImageWindowExists() override { return dragImage; }
  bool IsShellViewAt(POINT) override { return shell; }
  std::vector<std::filesystem::path> PrefetchSelection(POINT) override { ++prefetches; return sel; }
  POINT CursorPos() override { return cursor; }
  void OpenWheel(WheelKind k, POINT, const std::vector<std::filesystem::path>&) override { ++opens; lastKind = k; }
  void SwitchWheel(WheelKind k) override { ++switches; lastKind = k; }
  void CloseWheel(bool) override { ++closes; }
  void ArmTimer(int id, unsigned) override { timers.push_back(id); }
  void KillTimer(int id) override { for (auto it = timers.begin(); it != timers.end();) it = (*it == id) ? timers.erase(it) : it + 1; }
  void SuppressMenuActivation(bool on) override { menuSuppressed = on; }
};
void Settle(DragSession& s) { s.OnTimer(DragSession::kSettle); }
}

TEST(drag_shift_during_drag_opens_convert_wheel) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({100, 100}); s.OnDragMoved();
  CHECK(f.menuSuppressed);
  s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 1); CHECK(f.lastKind == WheelKind::Convert);
  CHECK(s.state() == DragSession::State::WheelOpen);
  CHECK_EQ(f.prefetches, 1);
}

TEST(drag_no_wheel_without_ole_drag_image_rubber_band) {
  Fake f; f.dragImage = false; DragSession s(&f);
  s.OnLeftDown({100, 100}); s.OnDragMoved();
  s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 0);
}

TEST(drag_no_wheel_without_button) {
  Fake f; DragSession s(&f);
  s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 0);
}

TEST(drag_modifier_held_before_drag_starts_still_opens) {
  Fake f; DragSession s(&f);
  s.OnModsChanged(ModShift);
  s.OnLeftDown({10, 10}); s.OnDragMoved(); Settle(s);
  CHECK_EQ(f.opens, 1);
}

TEST(drag_switching_wheels_with_other_modifier) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved();
  s.OnModsChanged(ModShift); Settle(s);
  s.OnModsChanged(0);                                  // key released: wheel stays
  Settle(s);
  CHECK_EQ(f.closes, 0); CHECK(s.WheelOpen());
  s.OnModsChanged(ModAlt); Settle(s);
  CHECK_EQ(f.switches, 1); CHECK(f.lastKind == WheelKind::Tools);
  s.OnModsChanged(ModCtrl); Settle(s);
  CHECK_EQ(f.switches, 2); CHECK(f.lastKind == WheelKind::Actions);
}

TEST(drag_unrelated_chord_does_not_open_or_switch) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved();
  s.OnModsChanged(ModShift | ModCtrl | ModAlt); Settle(s);
  CHECK_EQ(f.opens, 0);
}

TEST(drag_leftup_waits_for_drop_then_closes) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  s.OnLeftUp();
  CHECK(s.state() == DragSession::State::Closing);
  CHECK_EQ(f.closes, 0);                               // not yet: Drop() has not arrived
  s.OnDropHandled();
  CHECK_EQ(f.closes, 1); CHECK(s.state() == DragSession::State::Idle);
  CHECK(!f.menuSuppressed);
}

TEST(drag_leftup_without_drop_closes_after_grace) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  s.OnLeftUp();
  s.OnTimer(DragSession::kGrace);
  CHECK_EQ(f.closes, 1); CHECK(s.state() == DragSession::State::Idle);
}

TEST(drag_escape_cancels_immediately) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  s.OnKeyDown(VK_ESCAPE);
  CHECK_EQ(f.closes, 1); CHECK(s.state() == DragSession::State::Idle);
}

TEST(drag_new_press_during_closing_resets) {
  Fake f; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  s.OnLeftUp();
  s.OnLeftDown({5, 5});
  CHECK(s.state() == DragSession::State::Pressed);
  CHECK_EQ(f.closes, 1);
}

TEST(drag_non_shell_source_has_no_prefetch_but_opens) {
  Fake f; f.shell = false; DragSession s(&f);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 1); CHECK_EQ(f.prefetches, 0);
}

TEST(drag_custom_shortcuts_respected) {
  Fake f; DragSession s(&f);
  s.SetShortcuts(Shortcut{ModCtrl | ModShift, 0}, Shortcut{ModAlt | ModShift, 0}, Shortcut{ModCtrl | ModAlt, 0}, true);
  s.OnLeftDown({0, 0}); s.OnDragMoved();
  s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 0);                                // plain Shift is no longer a trigger
  s.OnModsChanged(ModAlt | ModShift); Settle(s);
  CHECK_EQ(f.opens, 1); CHECK(f.lastKind == WheelKind::Tools);
}

TEST(drag_disabled_ignores_everything) {
  Fake f; DragSession s(&f); s.SetEnabled(false);
  s.OnLeftDown({0, 0}); s.OnDragMoved(); s.OnModsChanged(ModShift); Settle(s);
  CHECK_EQ(f.opens, 0);
}
