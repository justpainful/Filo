// Link zone: the small glass card that appears under the cursor when Right Ctrl is pressed while a link is dragged out of a browser.
// It is an OLE drop target; dropping a link on it hands the address to the owner (which opens the download window).
#pragma once
#include "ui/dialog_window.h"
#include <functional>

namespace filo::ui {

// A download arrow drawn from lines (not part of the icon packs, so the packs stay complete at their fixed icon set).
inline void DrawDownloadGlyph(Painter& p, float cx, float cy, float size, const ds::Color& c, float w) {
  const float h = size * 0.5f;
  p.Line(cx, cy - h * 0.8f, cx, cy + h * 0.35f, c, w, true);
  p.Polyline({{cx - h * 0.5f, cy - h * 0.1f}, {cx, cy + h * 0.4f}, {cx + h * 0.5f, cy - h * 0.1f}}, c, w);
  p.Line(cx - h * 0.8f, cy + h * 0.85f, cx + h * 0.8f, cy + h * 0.85f, c, w, true);
}

class LinkZone : public DialogWindow {
 public:
  // Creates a self-owning zone centred on `at` (so the dragged link is already over it). `show=false` keeps it hidden (visual tests).
  static LinkZone* Create(POINT at, bool show = true);
  // Dismisses the zone (fades out and destroys itself). Safe to call twice.
  void Dismiss();

  // Decides whether dragged text is a link the app can download: fills `url` and `platform` ("TikTok"). Supplied by the owner (the
  // UI module knows nothing about sites).
  std::function<bool(const std::vector<std::wstring>& texts, std::wstring* url, std::wstring* platform)> classify;
  // Called with the address when a link is dropped. Runs inside the OLE Drop() callback: only post work, never run it.
  std::function<void(const std::wstring& url)> onDrop;
  std::function<void()> onGone;                         // the window was destroyed (owner drops its pointer)
  bool TestShot(const std::filesystem::path& png, int state) { state_ = state; return RenderToPng(png); }

 protected:
  void Draw(Ui& ui, const Rect& content, const Rect& body) override;
  bool AcceptsRawDrop(IDataObject* d) override;
  void OnRawDragState(bool over, bool acceptable) override;
  void OnRawDrop(IDataObject* d) override;
  void OnCloseRequested() override { Dismiss(); }

 private:
  int state_ = 0;                                      // 0 waiting, 1 a link is over the zone, 2 the data is not a link
  std::wstring platform_;
  std::wstring url_;
  bool dismissing_ = false;
};

}  // namespace filo::ui
