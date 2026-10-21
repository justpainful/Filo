// A top-level popup window rendered through DirectComposition with a premultiplied-alpha flip swap chain.
// Base class for the wheel, the floating shelf and every tool dialog. Subclasses implement OnRender.
#pragma once
#include "gfx/device.h"
#include <string>
#include <memory>
#include "gfx/liquid_mask.h"

namespace filo::gfx {
class LiveComposition;

struct SurfaceOptions {
  DWORD style = WS_POPUP;
  DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;
  bool topmost = false;
  bool noActivate = false;
  bool toolWindow = true;          // WS_EX_TOOLWINDOW: no taskbar button / alt-tab entry
  const wchar_t* title = L"";
};

class Surface {
 public:
  Surface();
  virtual ~Surface();
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  // Window rectangle in physical pixels (screen coordinates).
  bool Create(const SurfaceOptions& opt, int x, int y, int wPx, int hPx);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  bool Valid() const { return hwnd_ != nullptr; }
  UINT Dpi() const { return dpi_; }
  float Scale() const { return dpi_ / 96.0f; }
  int WidthPx() const { return w_; }
  int HeightPx() const { return h_; }

  void SetBounds(int x, int y, int wPx, int hPx, bool show = false);
  // Legacy DWM region helper (disabled for the host brush), clipped to a rounded rectangle covering the whole window; radius in px.
  // The region is kept in sync on resize. Call with radiusPx < 0 to turn it off.
  void SetBlurBehind(float radiusPx, int insetPx = 1);
  // Same, but for a sub-rectangle of the window (client px) - dialogs keep a transparent margin for their drop shadow.
  void SetBlurRect(const RECT& clientRectPx, float radiusPx);
  void Show(bool activate = false);
  void Hide();
  bool IsVisible() const;
  bool SetLiveBackdrop(const RECT& boundsPx, float radiusPx, bool dark, float intensity, const LiquidShape* shape = nullptr);
  void DisableLiveBackdrop();
  bool SetCloseBubble(const RECT& bounds, bool enabled);
  bool HasLiveBackdrop() const { return liveBackdrop_; }
  void SetLiveBackdropOpacity(float opacity);
  unsigned BackdropMaskUpdates() const;
  unsigned RenderedFrames() const { return renderedFrames_; }

  // Marks the surface dirty; a low-priority frame timer runs OnRender on the UI thread after pending input.
  void Invalidate();
  // Renders synchronously (used for the first frame before showing, and by tests). When `capture` is given, the
  // freshly rendered frame (BGRA premultiplied, w*h*4 bytes) is read back before presenting - used by visual tests.
  bool RenderNow(bool present = true, std::vector<uint8_t>* capture = nullptr, int* cw = nullptr, int* ch = nullptr);

 protected:
  virtual void OnRender(ID3D11RenderTargetView* rtv, ID3D11Texture2D* backBuffer, int wPx, int hPx) = 0;
  virtual LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);   // return handled-ness via *handled
  virtual void OnResized(int wPx, int hPx) {}
  virtual void OnDeviceReset(bool rebuilt) {}                  // GPU resources owned by subclass
  // A self-owning dialog must finish rendering/message dispatch before deletion.
  bool InDispatch() const { return dispatchDepth_ > 0 || rendering_; }
  virtual void OnDeferredClose() { Destroy(); }
  // Subclasses return true while animating so the frame loop keeps running.
  virtual bool WantsContinuousFrames() const { return false; }

  bool handledFlag_ = false;                                     // set by OnMessage overrides when they fully handled a message
  void SetHandled(bool h) { handledFlag_ = h; }

 public:
  static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
 private:
  LRESULT WndProc(UINT, WPARAM, LPARAM);
  unsigned renderedFrames_=0;
  bool CreateSwapChain(int w, int h);
  void DestroySwapChain();
  void ResizeSwapChain(int w, int h);
  bool BuildTarget();

  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  int w_ = 0, h_ = 0;
  bool framePending_ = false;
  int lostListener_ = 0;
  int dispatchDepth_ = 0;
  bool rendering_ = false;
  float blurRadius_ = -1; int blurInset_ = 1; RECT blurRect_{0, 0, 0, 0}; bool blurHasRect_ = false;
  void ApplyBlurRegion();

  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<IDCompositionTarget> dcompTarget_;
  ComPtr<IDCompositionVisual> dcompVisual_;
  std::unique_ptr<LiveComposition> live_;
  bool liveBackdrop_ = false;
};

constexpr UINT WM_FILO_FRAME = WM_APP + 0x40;
constexpr UINT WM_FILO_DISPOSE = WM_APP + 0x41;

} // namespace filo::gfx
