// DialogWindow: the Filo tool/settings window chrome - large rounded glass window, circular close button, centred title,
// header divider - plus input routing into the immediate-mode Ui. Subclasses only implement Draw().
#pragma once
#include "gfx/surface.h"
#include "gfx/backdrop.h"
#include "ui/ui.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace filo::ui {

struct DialogOptions {
  std::wstring title;
  float widthDip = 460, heightDip = 420;      // size of the window BODY (the shadow margin is added automatically)
  bool resizable = false;
  float minWidthDip = 0, minHeightDip = 0;
  bool showTitleBar = true;
  bool glassCloseButton = true;               // the circular glass close control; every Filo window uses it
  bool rtl = false;
  bool topmost = false;
  bool activate = true;
  bool glassBody = true;                      // false: fully transparent window; Draw() paints everything (Float stack)
  float cornerRadius = 0;                     // 0 = dialog::cornerRadius
};

class DialogWindow : public gfx::Surface {
 public:
  DialogWindow();
  ~DialogWindow() override;

  // Creates and shows the window near `anchor` (screen px; typically the cursor), clamped into the monitor work area.
  bool Open(const DialogOptions& o, POINT anchor, bool show = true);
  // Renders the dialog without showing it and writes the premultiplied frame to PNG (visual regression / reference comparison).
  bool RenderToPng(const std::filesystem::path& png, int frames = 3);
  void CloseWindow();                                  // destroys the window; onClosed fires
  std::function<void()> onClosed;
  void SetTitle(const std::wstring& t) { title_ = t; Invalidate(); }
  void SetGlassBody(bool on) { opts_.glassBody = on; ApplyBlur(); Invalidate(); }
  void SetCornerRadius(float r) { opts_.cornerRadius = r; ApplyBlur(); Invalidate(); }
  // Resizes the BODY (keeps its top-left corner); the shadow margin is added automatically.
  void SetBodySize(float widthDip, float heightDip);
  void RecaptureGlass();                              // refreshes material configuration; the compositor updates the background

  // Lays out a child rect below the title bar. Valid inside Draw().
  static constexpr float kMargin = 30.f;               // transparent margin around the body for the drop shadow (DIP)

 protected:
  // Draw the content. `content` is the area below the title bar; `body` the full window body.
  virtual void Draw(Ui& ui, const Rect& content, const Rect& body) = 0;
  virtual bool CanClose() const { return true; }
  void CancelPointerInput();
  virtual void OnCloseRequested() { if(CanClose()) CloseWindow(); }
  virtual bool OnEnter() { return false; }             // Enter pressed while no text field has focus; return true if handled
  virtual bool AcceptsFileDrop() const { return false; }
  virtual void OnFilesDropped(const std::vector<std::filesystem::path>& files) {}
  virtual void OnOpened() {}
  virtual void OnTick(double now) {}                   // called every rendered frame (video/audio preview timers)
  virtual bool NeedsContinuousFrames() const { return false; }

  void OnRender(ID3D11RenderTargetView* rtv, ID3D11Texture2D* bb, int w, int h) override;
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
  bool WantsContinuousFrames() const override { return NeedsContinuousFrames(); }
  void OnDeviceReset(bool rebuilt) override;
  void OnDeferredClose() override;

  Ui ui_;
  std::wstring title_;
  DialogOptions opts_;
  ComPtr<ID2D1DeviceContext> dc_;

 private:
  void ApplyBlur();                                   // configures the native host backdrop or the opaque fallback
  void CaptureGlass();                               // explicit FILO_TEST_BACKDROP fixture only
  void UpdateGlassPalette();                          // stable text colours from the selected theme
  gfx::BackdropImage back_;
  ComPtr<ID2D1Bitmap> backBmp_;
  ds::Palette dyn_{};
  bool dynValid_ = false;
  Rect BodyRect() const;
  InputState in_;
  Painter painter_;
  bool open_ = false;
  bool liveAllowed_ = false;
  bool dropRegistered_ = false;
  struct DropTargetImpl; DropTargetImpl* drop_ = nullptr;
  float scale_ = 1.f;
};

} // namespace filo::ui
