// Widget gallery: every control in every state on one dialog. Used for design review against the reference screenshots and
// as a manual smoke test of the toolkit (Filo.exe --gallery).
#pragma once
#include "ui/dialog_window.h"

namespace filo::ui {

class GalleryDialog : public DialogWindow {
 public:
  static GalleryDialog* Show(POINT at, bool rtl = false, bool show = true);
 protected:
  void Draw(Ui& ui, const Rect& content, const Rect& body) override;
  bool NeedsContinuousFrames() const override { return false; }
 private:
  float v_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int seg_ = 1, ratio_ = 0, swatch_ = 4, drop_ = 1;
  bool chk_ = true, tog_ = true;
  std::wstring text_ = L"photo.png";
  double step_ = 24;
  float progress_ = 0.62f;
  float scrollSeed_ = 0;
};

} // namespace filo::ui
