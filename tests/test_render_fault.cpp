#include "test.h"
#include "ui/dialog_window.h"
#include "core/com.h"

namespace {
class ProbeDialog : public filo::ui::DialogWindow {
 public:
  bool crashNextFrame = false;
  bool renderInsideDraw = false;
  bool innerRendered = true;
  int draws = 0;
  void Draw(filo::ui::Ui&, const filo::ui::Rect&, const filo::ui::Rect&) override {
    ++draws;
    if (crashNextFrame) { volatile int* p = nullptr; *p = 1; }          // a drawing bug
    if (renderInsideDraw) { renderInsideDraw = false; innerRendered = RenderNow(false); }   // what a message pump inside a widget callback does
  }
};

bool OpenHidden(ProbeDialog& d) {
  filo::ui::DialogOptions o;
  o.widthDip = 340; o.heightDip = 280; o.glassBody = false; o.title = L"Filo render fault test";
  return d.Open(o, POINT{400, 400}, false);
}
}

TEST(render_a_fault_while_drawing_drops_one_frame_and_the_window_recovers) {
  _putenv_s("FILO_TEST_NO_FAULT_REPORTS", "1");          // the deliberate fault must not leave a dump in the user's logs folder
  filo::ComInit com;
  OleInitialize(nullptr);
  ProbeDialog d;
  CHECK(OpenHidden(d));
  CHECK(d.RenderNow(false));
  d.crashNextFrame = true;
  CHECK(!d.RenderNow(false));              // the access violation was caught: no crash, the frame is dropped
  d.crashNextFrame = false;
  CHECK(d.RenderNow(false));               // and the very next frame draws normally (Direct2D state was closed cleanly)
  CHECK(d.RenderNow(false));
  OleUninitialize();
}

TEST(render_a_frame_requested_inside_a_frame_is_refused_not_nested) {
  filo::ComInit com;
  OleInitialize(nullptr);
  ProbeDialog d;
  CHECK(OpenHidden(d));
  d.renderInsideDraw = true;
  CHECK(d.RenderNow(false));               // the outer frame completes
  CHECK(!d.innerRendered);                 // the nested request did not start a second render on the same device context
  CHECK(d.RenderNow(false));
  OleUninitialize();
}
