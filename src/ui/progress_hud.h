// Progress HUD: the small glass card that appears at the top of the screen while a wheel conversion runs
// ("Converting to MP4" / file name / thin progress bar / close button that cancels). It turns into "Done" (click to show the file)
// or an error message, then dismisses itself.
#pragma once
#include "ui/dialog_window.h"
#include <filesystem>
#include <functional>

namespace filo::ui {

class ProgressHud : public DialogWindow {
 public:
  enum class State { Running, Done, Failed };
  // Creates a self-owning HUD on the monitor containing `monitorPoint`; `show=false` keeps it hidden (visual tests).
  static ProgressHud* Create(POINT monitorPoint, bool show = true);

  void SetTexts(const std::wstring& title, const std::wstring& subtitle) { titleText_ = title; sub_ = subtitle; Invalidate(); }
  void SetProgress(float p) { progress_ = p < 0 ? 0 : (p > 1 ? 1 : p); Invalidate(); }
  void Finish(State s, const std::wstring& title, const std::wstring& subtitle, const std::filesystem::path& reveal = {});

  std::function<void()> onCancel;                      // close button while running
  std::function<void()> onGone;                        // the window is being destroyed (owner drops its pointer)
  bool TestShot(const std::filesystem::path& png) { return RenderToPng(png); }

 protected:
  void Draw(Ui& ui, const Rect& content, const Rect& body) override;
  bool NeedsContinuousFrames() const override { return state_ == State::Running && progress_ <= 0.001f; }   // indeterminate shimmer
  void OnCloseRequested() override;
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
  void OnOpened() override;

 private:
  std::wstring titleText_, sub_;
  float progress_ = 0, shown_ = 0;
  State state_ = State::Running;
  std::filesystem::path reveal_;
};

}  // namespace filo::ui
