// Drag session state machine. Pure logic (no Win32 calls) so it can be unit-tested with fake services.
//
//   Idle --LeftDown--> Pressed --DragMoved--> Dragging --(shortcut modifiers settle)--> WheelOpen --LeftUp--> Closing --> Idle
//
// Rules (from the product spec):
//  * the wheel only opens while a real OLE drag is in flight (a drag-image window exists), never for rubber-band selection
//  * the wheel stays open until drop / cancel; releasing the modifier does not close it (setting)
//  * pressing another wheel's modifiers switches wheels
//  * Esc / releasing the button outside the wheel cancels; the original files are never touched by cancelling
//  * LeftUp does not close immediately: OLE delivers Drop() *after* the hook sees the button release, so we wait a grace
//    period for Drop/DragLeave before dismissing.
#pragma once
#include "core/settings.h"
#include "wheel/wheel_window.h"
#include <vector>
#include <filesystem>

namespace filo::shell {

struct IDragServices {
  virtual ~IDragServices() = default;
  virtual bool DragImageWindowExists() = 0;                       // OLE drag helper window present => a real drag is running
  virtual bool IsShellViewAt(POINT screenPt) = 0;
  virtual std::vector<std::filesystem::path> PrefetchSelection(POINT screenPt) = 0;
  virtual POINT CursorPos() = 0;
  virtual void OpenWheel(wheel::WheelKind kind, POINT at, const std::vector<std::filesystem::path>& files) = 0;
  virtual void SwitchWheel(wheel::WheelKind kind) = 0;
  virtual void CloseWheel(bool animated) = 0;
  virtual void ArmTimer(int id, unsigned ms) = 0;
  virtual void KillTimer(int id) = 0;
  virtual void SuppressMenuActivation(bool on) = 0;
};

class DragSession {
 public:
  enum class State { Idle, Pressed, Dragging, WheelOpen, Closing };
  enum TimerId { kSettle = 1, kGrace = 2, kProbe = 3 };

  explicit DragSession(IDragServices* s) : svc_(s) {}
  void SetShortcuts(const Shortcut& convert, const Shortcut& tools, const Shortcut& actions, bool keepOpenOnRelease) {
    conv_ = convert; tools_ = tools; actions_ = actions; keepOpen_ = keepOpenOnRelease;
  }
  void SetEnabled(bool e) { enabled_ = e; if (!e) Reset(false); }

  void OnLeftDown(POINT pt);
  void OnDragMoved();
  void OnLeftUp();
  void OnModsChanged(uint32_t mods);
  void OnKeyDown(uint32_t vk);
  void OnTimer(int id);
  // Called by the drop target when the OLE drop happened / the drag left / the wheel was dismissed by other means.
  void OnDropHandled();
  void OnWheelClosedExternally();

  State state() const { return state_; }
  bool WheelOpen() const { return state_ == State::WheelOpen || state_ == State::Closing; }

 private:
  bool MatchWheel(uint32_t mods, wheel::WheelKind* out) const;
  void Reset(bool animate);
  void TryOpen(uint32_t mods);

  IDragServices* svc_;
  Shortcut conv_{ModShift, 0}, tools_{ModAlt, 0}, actions_{ModCtrl, 0};
  bool keepOpen_ = true, enabled_ = true;
  State state_ = State::Idle;
  POINT down_{};
  uint32_t mods_ = 0;
  uint32_t settledMods_ = 0;
  wheel::WheelKind kind_ = wheel::WheelKind::Convert;
  bool downOnShell_ = false;
  bool sawDragImage_ = false;      // sticky: the OLE drag-image window was seen at least once during this press
  std::vector<std::filesystem::path> prefetched_;
};

} // namespace filo::shell
