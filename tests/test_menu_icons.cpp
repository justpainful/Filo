#include "test.h"
#include "ui/menu_icons.h"
#include "core/com.h"
#include <vector>

TEST(menu_icons_render_visible_ink_for_every_tray_menu_item) {
  filo::ComInit com;
  OleInitialize(nullptr);
  for (const char* name : {"circle", "check", "download", "settings", "folder", "close"}) {
    HBITMAP bmp = filo::ui::MenuIconBitmap(name, 16);
    CHECK(bmp != nullptr);
    if (!bmp) continue;
    BITMAP info{};
    GetObjectW(bmp, sizeof info, &info);
    CHECK_EQ((int)info.bmWidth, 16);
    int visible = 0;
    if (info.bmBits) for (int i = 0; i < 16 * 16; ++i) visible += ((const uint32_t*)info.bmBits)[i] >> 24 > 40;
    CHECK(visible > 12);                       // the glyph left pixels with alpha, i.e. the icon was drawn, not an empty bitmap
    DeleteObject(bmp);
  }
  CHECK(filo::ui::MenuIconBitmap("no-such-icon-name", 16) == nullptr);
  OleUninitialize();
}
