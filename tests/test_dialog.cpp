#include "test.h"
#include "ui/dialog_window.h"
#include "core/com.h"

namespace {
class ClosingDialog : public filo::ui::DialogWindow {
 public:
  bool closeInDraw = false;
  std::wstring received;
  void Draw(filo::ui::Ui& ui, const filo::ui::Rect&, const filo::ui::Rect&) override {
    received += ui.In().chars;
    if (closeInDraw) CloseWindow();
  }
};
void PumpMessages() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
void CheckClose(bool render) {
  filo::ComInit com;
  OleInitialize(nullptr);
  int deleted = 0;
  auto* d = new ClosingDialog;
  d->onClosed = [d, &deleted] { ++deleted; delete d; };
  filo::ui::DialogOptions options;
  options.widthDip = 340; options.heightDip = 280; options.glassBody = false;
  options.title = L"Filo hidden lifetime test";
  const bool opened = d->Open(options, POINT{400, 400}, false);
  CHECK(opened);
  if (opened) {
    if (render) { d->closeInDraw = true; CHECK(d->RenderNow(false)); }
    else SendMessageW(d->Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK_EQ(deleted, 0); // object must survive the entire frame/message dispatch
    PumpMessages();
    CHECK_EQ(deleted, 1);
    PumpMessages();
    CHECK_EQ(deleted, 1);
  } else delete d;
  OleUninitialize();
}
}
TEST(dialog_escape_defers_self_deletion_until_dispatch_returns) { CheckClose(false); }
TEST(dialog_close_during_render_defers_self_deletion_until_frame_ends) { CheckClose(true); }

TEST(dialog_accepts_unicode_codepoints_from_native_input) {
  filo::ComInit com;
  OleInitialize(nullptr);
  ClosingDialog d;
  filo::ui::DialogOptions options;
  options.widthDip = 340; options.heightDip = 280; options.glassBody = false;
  CHECK(d.Open(options, POINT{400, 400}, false));
  CHECK_EQ(SendMessageW(d.Hwnd(), WM_UNICHAR, UNICODE_NOCHAR, 0), TRUE);
  SendMessageW(d.Hwnd(), WM_UNICHAR, 0x0641, 0);
  SendMessageW(d.Hwnd(), WM_UNICHAR, 0x1F34A, 0);
  CHECK(d.RenderNow(false));
  CHECK_EQ(d.received, L"\u0641\U0001F34A");
  d.CloseWindow();
  OleUninitialize();
}
