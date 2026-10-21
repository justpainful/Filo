#include "shell/input_hooks.h"
#include "core/log.h"
#include <cstdlib>

namespace filo::shell {

static constexpr ULONG_PTR kOwnInput = 0x46494C4F;   // "FILO": marks input we inject ourselves

InputHooks& InputHooks::Get() { static InputHooks* h = new InputHooks(); return *h; }

bool InputHooks::Start(HWND target, UINT message) {
  if (running_) return true;
  target_ = target; msg_ = message;
  std::atomic<bool> ready{false};
  thread_ = std::thread([this, &ready] {
    threadId_ = GetCurrentThreadId();
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &InputHooks::MouseProc, GetModuleHandleW(nullptr), 0);
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputHooks::KeyProc, GetModuleHandleW(nullptr), 0);
    running_ = mouseHook_ && keyHook_;
    if (!running_) FILO_LOG(Error, "hooks", "SetWindowsHookEx failed: %lu", GetLastError());
    ready = true;
    ThreadMain();
  });
  while (!ready) Sleep(1);
  if (!running_) { if (thread_.joinable()) thread_.join(); return false; }
  FILO_LOG(Info, "hooks", "global input observation started");
  return true;
}

void InputHooks::Stop() {
  if (!thread_.joinable()) return;
  PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
  thread_.join();
  running_ = false;
}

void InputHooks::ThreadMain() {
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
  if (keyHook_) UnhookWindowsHookEx(keyHook_);
  mouseHook_ = keyHook_ = nullptr;
}

void InputHooks::Post(InputEvent e, uint32_t vk) {
  if (!target_) return;
  const uint32_t mods = mods_.load();
  const LONG dx = downX_.load(), dy = downY_.load();
  PostMessageW(target_, msg_, (WPARAM)((uint32_t)e | (mods << 8) | (vk << 16)), MAKELPARAM((short)dx, (short)dy));
}

LRESULT CALLBACK InputHooks::MouseProc(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION) {
    auto& self = Get();
    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
    switch (w) {
      case WM_LBUTTONDOWN:
        self.downX_ = ms->pt.x; self.downY_ = ms->pt.y;
        self.dragMoved_ = false; self.leftDown_ = true;
        self.Post(InputEvent::LeftDown);
        break;
      case WM_LBUTTONUP:
        self.leftDown_ = false;
        self.dragMoved_ = false;
        self.Post(InputEvent::LeftUp);
        break;
      case WM_MOUSEMOVE:
        if (self.leftDown_ && !self.dragMoved_) {
          int th = self.threshold_.load();
          int tx = th > 0 ? th : GetSystemMetrics(SM_CXDRAG), ty = th > 0 ? th : GetSystemMetrics(SM_CYDRAG);
          if (std::abs(ms->pt.x - self.downX_.load()) >= tx || std::abs(ms->pt.y - self.downY_.load()) >= ty) {
            self.dragMoved_ = true;
            self.Post(InputEvent::DragMoved);
          }
        }
        break;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

static uint32_t ModBit(DWORD vk) {
  switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return ModShift;
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return ModCtrl;
    case VK_LMENU: case VK_RMENU: case VK_MENU: return ModAlt;
    case VK_LWIN: case VK_RWIN: return ModWin;
  }
  return 0;
}

LRESULT CALLBACK InputHooks::KeyProc(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION) {
    auto& self = Get();
    const auto* ks = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
    const bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
    const bool up = (w == WM_KEYUP || w == WM_SYSKEYUP);
    if ((down || up) && ks->dwExtraInfo != kOwnInput) {
      const DWORD vk = ks->vkCode;
      if (uint32_t bit = ModBit(vk)) {
        // physical state per concrete key so Left+Right Shift behave
        self.keyState_[vk & 0xFF] = down;
        uint32_t m = 0;
        if (self.keyState_[VK_LSHIFT] || self.keyState_[VK_RSHIFT]) m |= ModShift;
        if (self.keyState_[VK_LCONTROL] || self.keyState_[VK_RCONTROL]) m |= ModCtrl;
        if (self.keyState_[VK_LMENU] || self.keyState_[VK_RMENU]) m |= ModAlt;
        if (self.keyState_[VK_LWIN] || self.keyState_[VK_RWIN]) m |= ModWin;
        uint32_t old = self.mods_.exchange(m);
        if (old != m) {
          // A lone Alt/Win tap would otherwise open the menu bar / Start menu after a wheel was used: inject a harmless
          // unassigned key (VK_E8) between the modifier going down and up, exactly as AutoHotkey does.
          if (down && self.suppressMenu_ && (bit == ModAlt || bit == ModWin) && self.leftDown_) {
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = 0xE8;
            in[0].ki.dwExtraInfo = in[1].ki.dwExtraInfo = kOwnInput;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
          }
          self.Post(InputEvent::ModsChanged);
        }
      } else if (down && self.leftDown_.load()) {
        self.Post(InputEvent::KeyDown, vk);
      }
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

} // namespace filo::shell
