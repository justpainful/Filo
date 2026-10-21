#include "gfx/surface.h"
#include "gfx/live_composition.h"
#include "gfx/icons.h"
#include "core/log.h"
#include "core/util.h"
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <algorithm>

namespace filo::gfx {

static const wchar_t* kClass = L"Filo.Surface";
static constexpr UINT_PTR kFrameTimer = 0xF140;
static std::atomic<bool> g_classRegistered{false};

static void EnsureClass() {
  bool expected = false;
  if (!g_classRegistered.compare_exchange_strong(expected, true)) return;
  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = &Surface::WndProcThunk;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
}

Surface::Surface() = default;
Surface::~Surface() { Destroy(); }

bool Surface::Create(const SurfaceOptions& opt, int x, int y, int wPx, int hPx) {
  if (!Device::Get().Ensure()) return false;
  EnsureClass();
  DWORD ex = opt.exStyle | WS_EX_NOREDIRECTIONBITMAP;
  if (opt.topmost) ex |= WS_EX_TOPMOST;
  if (opt.noActivate) ex |= WS_EX_NOACTIVATE;
  if (opt.toolWindow) ex |= WS_EX_TOOLWINDOW;
  hwnd_ = CreateWindowExW(ex, kClass, opt.title, opt.style, x, y, wPx, hPx, nullptr, nullptr, GetModuleHandleW(nullptr), this);
  if (!hwnd_) { FILO_LOG(Error, "gfx", "CreateWindowEx failed: %ls", LastErrorMessage().c_str()); return false; }
  dpi_ = GetDpiForWindow(hwnd_);
  if (!dpi_) dpi_ = 96;
  if (const wchar_t* e = _wgetenv(L"FILO_TEST_DPI")) dpi_ = (UINT)_wtoi(e);     // visual tests render at the reference scale (192 = 2x)
  w_ = wPx; h_ = hPx;
  if (!CreateSwapChain(w_, h_)) { Destroy(); return false; }
  lostListener_ = Device::Get().AddLostListener([this](bool rebuilt) {
    if (!rebuilt) { live_.reset(); liveBackdrop_ = false; rtv_.Reset(); dcompVisual_.Reset(); dcompTarget_.Reset(); swap_.Reset(); OnDeviceReset(false); }
    else { if (hwnd_ && CreateSwapChain(w_, h_)) { OnDeviceReset(true); Invalidate(); } }
  });
  return true;
}

void Surface::Destroy() {
  if (lostListener_) { Device::Get().RemoveLostListener(lostListener_); lostListener_ = 0; }
  DestroySwapChain();
  if (hwnd_) { HWND h = hwnd_; hwnd_ = nullptr; DestroyWindow(h); }
}

bool Surface::CreateSwapChain(int w, int h) {
  auto& dev = Device::Get();
  w = std::max(w, 1); h = std::max(h, 1);
  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = w; d.Height = h;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.Scaling = DXGI_SCALING_STRETCH;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  HRESULT hr = dev.factory->CreateSwapChainForComposition(dev.d3d.Get(), &d, nullptr, &swap_);
  if (FAILED(hr)) { FILO_LOG(Error, "gfx", "CreateSwapChainForComposition: %ls", HResultMessage(hr).c_str()); return false; }
  live_ = std::make_unique<LiveComposition>();
  if (live_->Create(hwnd_, swap_.Get(), w, h)) return BuildTarget();
  live_.reset();
  if (FAILED(dev.dcomp->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
  if (FAILED(dev.dcomp->CreateVisual(&dcompVisual_))) return false;
  dcompVisual_->SetContent(swap_.Get());
  dcompTarget_->SetRoot(dcompVisual_.Get());
  dev.dcomp->Commit();
  return BuildTarget();
}

bool Surface::BuildTarget() {
  auto& dev = Device::Get();
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), bb.AsVoid()))) return false;
  return SUCCEEDED(dev.d3d->CreateRenderTargetView(bb.Get(), nullptr, &rtv_));
}

void Surface::DestroySwapChain() {
  live_.reset(); liveBackdrop_ = false;
  rtv_.Reset(); dcompVisual_.Reset(); dcompTarget_.Reset(); swap_.Reset();
}

void Surface::ResizeSwapChain(int w, int h) {
  if (!swap_) return;
  w = std::max(w, 1); h = std::max(h, 1);
  Device::Get().ctx->OMSetRenderTargets(0, nullptr, nullptr);
  rtv_.Reset();
  HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr)) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) Device::Get().HandleDeviceLost(hr);
    return;
  }
  BuildTarget();
  if (live_) live_->Resize(w, h);
}

bool Surface::SetLiveBackdrop(const RECT& bounds, float radius, bool dark, float intensity, const LiquidShape* shape) {
  LiquidShape rectangle{};
  if (!shape) {
    rectangle.cx=(bounds.left+bounds.right)*.5f;rectangle.cy=(bounds.top+bounds.bottom)*.5f;
    rectangle.halfW=(bounds.right-bounds.left)*.5f;rectangle.halfH=(bounds.bottom-bounds.top)*.5f;
    rectangle.radius=radius;rectangle.dpiScale=Scale();shape=&rectangle;
  }
  liveBackdrop_ = live_ && live_->Configure(bounds, radius, dark, intensity, shape);
  return liveBackdrop_;
}
bool Surface::SetCloseBubble(const RECT& bounds,bool enabled) {return live_&&live_->CloseBubble(bounds,Scale(),enabled);}
void Surface::DisableLiveBackdrop() { if (live_) live_->Disable(); liveBackdrop_ = false; }
unsigned Surface::BackdropMaskUpdates() const {return live_?live_->MaskUpdates():0;}
void Surface::SetLiveBackdropOpacity(float value) { if (live_) live_->SetOpacity(value); }

void Surface::SetBounds(int x, int y, int wPx, int hPx, bool show) {
  if (!hwnd_) return;
  UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
  if (show) flags |= SWP_SHOWWINDOW;
  SetWindowPos(hwnd_, nullptr, x, y, wPx, hPx, flags);
}
void Surface::SetBlurBehind(float radiusPx, int insetPx) {
  blurRadius_ = radiusPx; blurInset_ = insetPx; blurHasRect_ = false;
  ApplyBlurRegion();
}
void Surface::SetBlurRect(const RECT& r, float radiusPx) {
  blurRadius_ = radiusPx; blurInset_ = 0; blurRect_ = r; blurHasRect_ = true;
  ApplyBlurRegion();
}
void Surface::ApplyBlurRegion() {
  if (!hwnd_) return;
  DWM_BLURBEHIND bb{};
  if (blurRadius_ < 0) { bb.dwFlags = DWM_BB_ENABLE; bb.fEnable = FALSE; DwmEnableBlurBehindWindow(hwnd_, &bb); return; }
  const int r = std::max(2, (int)std::lround(blurRadius_ * 2));
  HRGN rgn = blurHasRect_ ? CreateRoundRectRgn(blurRect_.left + 1, blurRect_.top + 1, blurRect_.right, blurRect_.bottom, r, r)
                          : CreateRoundRectRgn(blurInset_, blurInset_, w_ - blurInset_ + 1, h_ - blurInset_ + 1, r, r);
  bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
  bb.fEnable = TRUE; bb.hRgnBlur = rgn;
  DwmEnableBlurBehindWindow(hwnd_, &bb);
  DeleteObject(rgn);
}

void Surface::Show(bool activate) { if (hwnd_) ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNA); }
void Surface::Hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
bool Surface::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void Surface::Invalidate() {
  if (!hwnd_ || framePending_) return;
  framePending_ = true;
  // Posted messages precede hardware input in PeekMessage. Reposting a frame
  // during caret blinking/animation can therefore starve clicks and keystrokes.
  // WM_TIMER runs after pending input and bounds animation to about 60 Hz.
  if (!SetTimer(hwnd_, kFrameTimer, 16, nullptr)) framePending_ = false;
}

static bool ReadBack(ID3D11Texture2D* bb, std::vector<uint8_t>& out, int& w, int& h) {
  auto& dev = Device::Get();
  D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev.d3d->CreateTexture2D(&td, nullptr, &st))) return false;
  dev.ctx->CopyResource(st.Get(), bb);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(dev.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  w = (int)td.Width; h = (int)td.Height;
  out.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) memcpy(out.data() + (size_t)y * w * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
  dev.ctx->Unmap(st.Get(), 0);
  return true;
}

bool Surface::RenderNow(bool present, std::vector<uint8_t>* capture, int* cw, int* ch) {
  if (!hwnd_ || !swap_ || !rtv_) return false;
  auto& dev = Device::Get();
  ComPtr<ID3D11Texture2D> bb;
  swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), bb.AsVoid());
  D3D11_VIEWPORT vp{0, 0, (float)w_, (float)h_, 0, 1};
  dev.ctx->RSSetViewports(1, &vp);
  ID3D11RenderTargetView* rtv = rtv_.Get();
  dev.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  rendering_ = true;
  ++renderedFrames_;
  OnRender(rtv_.Get(), bb.Get(), w_, h_);
  rendering_ = false;
  dev.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  if (capture) { int w = 0, h = 0; ReadBack(bb.Get(), *capture, w, h); if (cw) *cw = w; if (ch) *ch = h; }
  if (present) {
    HRESULT hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { dev.HandleDeviceLost(hr); return false; }
  }
  return true;
}

LRESULT Surface::OnMessage(UINT, WPARAM, LPARAM) { SetHandled(false); return 0; }

LRESULT Surface::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TIMER && wp == kFrameTimer) {
    KillTimer(hwnd_, kFrameTimer);
    msg = WM_FILO_FRAME;
  }
  switch (msg) {
    case WM_FILO_FRAME:
      framePending_ = false;
      if (IsVisible()) {
        RenderNow(true);
        if (WantsContinuousFrames()) Invalidate();
      }
      return 0;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w != w_ || h != h_) { w_ = w; h_ = h; ResizeSwapChain(w, h); ApplyBlurRegion(); OnResized(w, h); Invalidate(); }
      }
      break;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Invalidate();
      break; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: {
      LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
      if (ex & WS_EX_NOACTIVATE) return MA_NOACTIVATE;
      break; }
  }
  handledFlag_ = false;
  LRESULT r = OnMessage(msg, wp, lp);
  if (handledFlag_) return r;
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK Surface::WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
  Surface* self = nullptr;
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    self = static_cast<Surface*>(cs->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) self->hwnd_ = h;
  } else {
    self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  }
  if (!self) return DefWindowProcW(h, m, w, l);
  if (m == WM_NCDESTROY) { SetWindowLongPtrW(h, GWLP_USERDATA, 0); self->hwnd_ = nullptr; return DefWindowProcW(h, m, w, l); }
  if (m == WM_FILO_DISPOSE) { self->OnDeferredClose(); return 0; }
  ++self->dispatchDepth_;
  const LRESULT result = self->WndProc(m, w, l);
  --self->dispatchDepth_;
  return result;
}

void Shutdown() {
  IconCache::Get().Clear();
  Device::Get().Release();
}

} // namespace filo::gfx
