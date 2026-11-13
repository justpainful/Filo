// Icons for native Win32 menus (the tray menu): renders a Filo icon of the current icon pack into a 32-bit premultiplied bitmap that
// MENUITEMINFO::hbmpItem accepts. The ink follows the system menu text colour, so it reads in light and dark menus alike.
#pragma once
#include <windows.h>
#include <string>

namespace filo::ui {

// Caller owns the bitmap (DeleteObject). Returns nullptr when the GPU device or the icon is unavailable.
// `ink`: a fixed colour (e.g. the accent for "on"); the default follows the system menu text colour.
HBITMAP MenuIconBitmap(const std::string& icon, int sizePx, COLORREF ink = CLR_INVALID);

}  // namespace filo::ui
