// Notification-area icon: status tooltip, balloon notifications (success/failure with click-to-reveal) and the context menu.
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

namespace filo::shell {

class TrayIcon {
 public:
  static constexpr UINT kCallbackMessage = WM_APP + 0x30;
  bool Create(HWND owner, HICON icon, const wchar_t* tooltip);
  void Destroy();
  void SetTooltip(const std::wstring& text);
  // `onClick` runs when the user clicks the balloon (e.g. reveal the converted file).
  void Balloon(const std::wstring& title, const std::wstring& text, bool error, std::function<void()> onClick = {});
  // Call from the owner's WndProc for kCallbackMessage. Returns true when handled.
  bool HandleMessage(WPARAM wp, LPARAM lp, const std::function<void(POINT)>& onContextMenu, const std::function<void()>& onLeftClick);
  void Recreate();      // after Explorer restarts (TaskbarCreated)
 private:
  NOTIFYICONDATAW nid_{};
  bool created_ = false;
  std::function<void()> balloonClick_;
};

} // namespace filo::shell
