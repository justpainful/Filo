// The radial wheel as a GPU-rendered, click-through-free popup. Owns animation state; knows nothing about files or jobs:
// it shows a WheelContent, reports hover/choice through callbacks and answers hit-test queries for the OLE drop target.
#pragma once
#include "gfx/surface.h"
#include "gfx/backdrop.h"
#include "wheel/geometry.h"
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <array>
#include <memory>

namespace filo::wheel {

enum class WheelKind { Convert, Tools, Actions };

struct WheelItem {
  std::wstring id;          // stable machine id, e.g. "convert:webp" or "tool:compress"
  std::wstring label;       // shown text (already localised)
  std::string icon;         // icon name (gfx/icons.cpp) - empty for text-only items
  bool enabled = true;
  std::wstring tooltip;     // reason text when disabled
};

struct WheelContent {
  WheelKind kind = WheelKind::Convert;
  std::vector<WheelItem> items;       // at most kMaxItems; pagination is handled by the controller
  std::wstring centerPrimary;         // e.g. "2.2 MB" or "3 files"
  std::wstring centerSecondary;       // e.g. file name (optional)
  std::string  centerIcon;            // used when no thumbnail is available
  std::wstring message;               // shown inside the hole when `items` is empty ("Folders have no conversions...")
};

constexpr int kMaxItems = 16;

class WheelWindow : public gfx::Surface {
 public:
  WheelWindow();
  ~WheelWindow() override;

  bool Init();                                         // creates the window (hidden)
  void SetContent(WheelContent c, bool animateSwitch); // replaces items; animateSwitch cross-fades like a wheel change
  const WheelContent& Content() const { return content_; }

  // Shows the wheel centred on screen point `cursorPx` (physical px), clamped inside the monitor work area.
  // `liveBackdrop` enables compositor-owned sampling behind the HWND.
  void ShowAt(POINT cursorPx, bool liveBackdrop = true);
  // Offscreen/test path: lays the wheel out for `cursorPx`, jumps the animation to its end state and leaves the window
  // hidden, so RenderNow(false, &pixels) can read the frame back.
  void PrepareOffscreen(POINT cursorPx, bool fullyShown = true);
  void SetBackdropImage(gfx::BackdropImage img) { backdrop_ = std::move(img); backdropDirty_ = true; }
  void SetHoverImmediate(int idx);       // test helper: no animation
  void SetRadiusOverridePx(float r) { radiusOverridePx_ = r; }   // visual-comparison helper (fixed R in px)
  void Dismiss(bool animated = true);
  bool Shown() const { return shown_; }
  void SetActivatable(bool on);          // keyboard-invoked wheels take focus; drag wheels never do
  void FocusForKeyboard();

  // Thumbnail for the centre hole; BGRA premultiplied, copied.
  void SetThumbnail(int w, int h, const uint8_t* bgraPremul, size_t stride);
  void ClearThumbnail();

  // Hover / selection
  void SetHoverIndex(int idx);                          // -1 = none
  int  HoverIndex() const { return hover_; }
  void SetPressed(int idx);
  // Screen point (physical px) -> zone/sector, using the exact rendered geometry (including hover scale).
  Hit HitTestScreen(POINT screenPx) const;
  // Keyboard navigation: step to the next/previous enabled item (wraps).
  void MoveSelection(int dir);
  void ConfirmFlash(int idx);                           // brief "drop accepted" pulse before dismissal

  // Geometry the shell layer needs.
  POINT CenterScreen() const { return {origin_.x + (LONG)std::lround(w_c_), origin_.y + (LONG)std::lround(h_c_)}; }
  float RimRadiusPx() const { return Rpx_; }
  RECT  ScreenRect() const;

  std::function<void(int idx)> onChoose;                // set by the controller; not called by hover
  std::function<void()> onDismissed;                    // after the disappear animation completes
  std::function<void(int idx)> onHoverChanged;

 protected:
  void OnRender(ID3D11RenderTargetView* rtv, ID3D11Texture2D* bb, int w, int h) override;
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
  void OnDeviceReset(bool rebuilt) override;
  bool WantsContinuousFrames() const override;

 private:
  struct Gpu;
  Layout MakeLayout() const;
  void LayoutWindow(POINT cursorPx, bool clampToWorkArea = true);
  void UploadBackdrop();
  void DrawOverlay(ID2D1DeviceContext* dc, ID3D11Texture2D* bb, float appear);
  void StepAnimation(double now);

  struct SectorAnim { float hover = 0, hoverT = 0, press = 0, pressT = 0, alpha = 1, flash = 0; };

  WheelContent content_;
  WheelContent outgoing_;                       // previous content during a wheel switch
  std::array<SectorAnim, kMaxItems> anim_{};
  int hover_ = -1, pressed_ = -1;
  bool shown_ = false, dismissing_ = false, liveAllowed_ = false;
  double tShow_ = 0, tDismiss_ = 0, tSwitch_ = -1, tLast_ = 0;
  float appear_ = 0, switchP_ = 1;
  POINT origin_{};                              // window top-left, screen px
  float radiusOverridePx_ = 0;
  float Rpx_ = 150, w_c_ = 0, h_c_ = 0;         // rim radius and window-local centre, px
  gfx::BackdropImage backdrop_;
  bool backdropDirty_ = false;
  std::vector<uint8_t> thumb_; int thumbW_ = 0, thumbH_ = 0;
  bool thumbDirty_ = false;
  std::unique_ptr<Gpu> gpu_;
};

} // namespace filo::wheel
