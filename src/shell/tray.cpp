#include "shell/tray.h"
#include <shellapi.h>
#include <windowsx.h>
#include <cstring>

namespace filo::shell {

bool TrayIcon::Create(HWND owner, HICON icon, const wchar_t* tooltip) {
  nid_ = {};
  nid_.cbSize = sizeof nid_;
  nid_.hWnd = owner;
  nid_.uID = 1;
  nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid_.uCallbackMessage = kCallbackMessage;
  nid_.hIcon = icon;
  wcsncpy_s(nid_.szTip, tooltip, _TRUNCATE);
  nid_.uVersion = NOTIFYICON_VERSION_4;
  created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  if (created_) Shell_NotifyIconW(NIM_SETVERSION, &nid_);
  return created_;
}

void TrayIcon::Recreate() {
  if (!nid_.hWnd) return;
  created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  if (created_) Shell_NotifyIconW(NIM_SETVERSION, &nid_);
}

void TrayIcon::Destroy() {
  if (created_) Shell_NotifyIconW(NIM_DELETE, &nid_);
  created_ = false;
}

void TrayIcon::SetTooltip(const std::wstring& text) {
  if (!created_) return;
  NOTIFYICONDATAW n = nid_;
  n.uFlags = NIF_TIP | NIF_SHOWTIP;
  wcsncpy_s(n.szTip, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayIcon::Balloon(const std::wstring& title, const std::wstring& text, bool error, std::function<void()> onClick) {
  if (!created_) return;
  balloonClick_ = std::move(onClick);
  NOTIFYICONDATAW n = nid_;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
  wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool TrayIcon::HandleMessage(WPARAM wp, LPARAM lp, const std::function<void(POINT)>& onContextMenu, const std::function<void()>& onLeftClick) {
  const UINT ev = LOWORD(lp);
  switch (ev) {
    case WM_CONTEXTMENU: case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
      if (ev == WM_RBUTTONUP) GetCursorPos(&pt);
      if (onContextMenu) onContextMenu(pt);
      return true; }
    case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP:
      if (onLeftClick) onLeftClick();
      return true;
    case NIN_BALLOONUSERCLICK:
      if (balloonClick_) { auto f = std::move(balloonClick_); balloonClick_ = nullptr; f(); }
      return true;
    case NIN_BALLOONTIMEOUT: balloonClick_ = nullptr; return true;
  }
  return false;
}

} // namespace filo::shell
