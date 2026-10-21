// Global low-level mouse/keyboard observation on a dedicated thread (hook callbacks must return fast and the UI thread
// may be busy rendering). The hook never reads file data and never blocks input: it only tracks the left button and the
// modifier keys and posts small events to the UI thread. Nothing is swallowed except what the caller explicitly asks for.
#pragma once
#include "core/settings.h"
#include <windows.h>
#include <atomic>
#include <thread>

namespace filo::shell {

enum class InputEvent : uint8_t {
  LeftDown = 1,     // left button went down (position: read with GetCursorPos on the UI thread)
  LeftUp,           // left button released (drag finished or cancelled)
  DragMoved,        // button held and the cursor moved beyond the system drag threshold (sent once per press)
  ModsChanged,      // modifier mask changed (new mask in the event)
  KeyDown,          // non-modifier key pressed while the left button is down (vk in the event) - Esc handling
};

// Posted as PostMessage(hwnd, msg, wParam, lParam):  wParam = event | mods<<8 | vk<<16 ; lParam = packed down-point (x low, y high; signed 16)
inline InputEvent EventOf(WPARAM w) { return (InputEvent)(w & 0xFF); }
inline uint32_t ModsOf(WPARAM w) { return (uint32_t)((w >> 8) & 0xFF); }
inline uint32_t VkOf(WPARAM w) { return (uint32_t)((w >> 16) & 0xFFFF); }
inline POINT PointOf(LPARAM l) { return {(short)LOWORD(l), (short)HIWORD(l)}; }

class InputHooks {
 public:
  static InputHooks& Get();
  bool Start(HWND target, UINT message);
  void Stop();
  bool Running() const { return running_; }

  // Cheap snapshots, safe from any thread.
  bool LeftDown() const { return leftDown_.load(); }
  bool DragInProgress() const { return dragMoved_.load(); }
  uint32_t Mods() const { return mods_.load(); }          // Mod flags (ModShift...)
  POINT DownPoint() const { return {downX_.load(), downY_.load()}; }
  // While true the dragged-file wheel shortcuts are armed; the hook thread uses this to emit the "dummy key" that stops
  // Alt/Win taps from activating the Explorer menu bar / Start menu.
  void SetSuppressMenuActivation(bool on) { suppressMenu_ = on; }
  void SetDragThreshold(int px) { threshold_ = px; }

 private:
  InputHooks() = default;
  void ThreadMain();
  static LRESULT CALLBACK MouseProc(int, WPARAM, LPARAM);
  static LRESULT CALLBACK KeyProc(int, WPARAM, LPARAM);
  void Post(InputEvent e, uint32_t vk = 0);

  std::thread thread_;
  std::atomic<bool> running_{false};
  DWORD threadId_ = 0;
  HWND target_ = nullptr;
  UINT msg_ = 0;
  HHOOK mouseHook_ = nullptr, keyHook_ = nullptr;
  std::atomic<bool> leftDown_{false}, dragMoved_{false}, suppressMenu_{false};
  std::atomic<uint32_t> mods_{0};
  std::atomic<LONG> downX_{0}, downY_{0};
  std::atomic<int> threshold_{0};
  // per-vk physical state so L/R variants combine correctly
  bool keyState_[256] = {};
};

} // namespace filo::shell
