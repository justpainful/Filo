// Filo settings window: General / Wheels / Appearance / Files. Every change is applied and persisted immediately
// (SettingsStore::Update notifies the runtime, which re-registers shortcuts, theme, autostart...).
#pragma once
#include "ui/dialog_window.h"
#include "core/settings.h"

namespace filo::ui {

class SettingsWindow : public DialogWindow {
 public:
  // Shows the single settings window (brings it forward when it is already open). `show=false` keeps it hidden for visual tests.
  static SettingsWindow* Show(bool show = true, int tab = 0);
  bool TestShot(const std::filesystem::path& png) { return RenderToPng(png); }

 protected:
  void Draw(Ui& ui, const Rect& content, const Rect& body) override;
  bool NeedsContinuousFrames() const override { return capturing_ != 0; }

 private:
  void Tab(Ui& ui, const Rect& vp, Settings& s);
  void General(Ui& ui, const Rect& vp, Settings& s, float& y);
  void Wheels(Ui& ui, const Rect& vp, Settings& s, float& y);
  void Appearance(Ui& ui, const Rect& vp, Settings& s, float& y);
  void Files(Ui& ui, const Rect& vp, Settings& s, float& y);
  bool ShortcutRow(Ui& ui, const Rect& vp, float& y, const wchar_t* label, int slot, Shortcut& sc, bool needsKey);
  void PollCapture(Shortcut* target, bool needsKey);

  int tab_ = 0;
  float contentH_ = 600;
  int capturing_ = 0;              // 1-based slot of the shortcut being recorded (0 = none)
  uint32_t capMods_ = 0, capVk_ = 0;
  int lastNoteSlot_ = 0;
  bool resetPresets_ = false;
};

} // namespace filo::ui
