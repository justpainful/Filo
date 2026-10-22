#include "ui/dialog_window.h"
#include "design/material.h"
#include "core/log.h"
#include "core/util.h"
#include "design/theme.h"
#include "shell/explorer.h"
#include "gfx/imageio.h"
#include <cstdlib>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace filo::ui {

using namespace filo::ds;

// File drops onto dialogs (add clips to Join Videos, add PDFs to Merge...).
struct DialogWindow::DropTargetImpl : IDropTarget {
  DialogWindow* w;
  LONG ref = 1;
  explicit DropTargetImpl(DialogWindow* d) : w(d) {}
  STDMETHODIMP QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IDropTarget) { *p = static_cast<IDropTarget*>(this); AddRef(); return S_OK; } *p = nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD* e) override { *e = (w && w->AcceptsFileDrop()) ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
  STDMETHODIMP DragOver(DWORD, POINTL, DWORD* e) override { *e = (w && w->AcceptsFileDrop()) ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
  STDMETHODIMP DragLeave() override { return S_OK; }
  STDMETHODIMP Drop(IDataObject* d, DWORD, POINTL, DWORD* e) override {
    *e = DROPEFFECT_NONE;
    if (!w || !w->AcceptsFileDrop()) return S_OK;
    auto files = shell::PathsFromDataObject(d);
    if (!files.empty()) { *e = DROPEFFECT_COPY; w->OnFilesDropped(files); w->Invalidate(); }
    return S_OK;
  }
};

DialogWindow::DialogWindow() = default;
DialogWindow::~DialogWindow() {
  if (drop_) { if (Valid() && dropRegistered_) RevokeDragDrop(Hwnd()); drop_->w = nullptr; drop_->Release(); drop_ = nullptr; }
}

static float MonitorScale(POINT pt) {
  if (const wchar_t* e = _wgetenv(L"FILO_TEST_DPI")) return (float)_wtof(e) / 96.f;
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy);
  return dx / 96.f;
}

bool DialogWindow::Open(const DialogOptions& o, POINT anchor, bool show) {
  opts_ = o; title_ = o.title;
  liveAllowed_ = show && !_wgetenv(L"FILO_TEST_BACKDROP");
  const float sc = MonitorScale(anchor);
  const float M = kMargin;
  HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
  const RECT wa = mi.rcWork;
  if (!_wgetenv(L"FILO_TEST_DPI")) {      // never taller/wider than the screen: footer buttons must stay reachable
    const float maxH = (wa.bottom - wa.top) / sc - 2 * M + 2 * 14, maxW = (wa.right - wa.left) / sc - 2 * M + 2 * 14;
    opts_.heightDip = std::max(std::min(opts_.heightDip, maxH), std::min(opts_.minHeightDip, maxH));
    opts_.widthDip = std::max(std::min(opts_.widthDip, maxW), std::min(opts_.minWidthDip, maxW));
  }
  const int wPx = (int)std::lround((opts_.widthDip + 2 * M) * sc), hPx = (int)std::lround((opts_.heightDip + 2 * M) * sc);
  // centre the body on the anchor, clamp inside the work area (never partly off-screen: footer buttons stay reachable)
  int x = anchor.x - wPx / 2, y = anchor.y - hPx / 2;
  const int mpx = (int)(M * sc);
  if (!_wgetenv(L"FILO_TEST_DPI")) {      // visual tests place windows exactly where the reference has them
    x = std::clamp(x, (int)wa.left - mpx, std::max((int)wa.left - mpx, (int)wa.right - wPx + mpx));
    y = std::clamp(y, (int)wa.top - mpx, std::max((int)wa.top - mpx, (int)wa.bottom - hPx + mpx));
  }
  gfx::SurfaceOptions so;
  so.style = WS_POPUP | WS_MINIMIZEBOX | (o.resizable ? WS_THICKFRAME : 0);
  so.topmost = o.topmost; so.toolWindow = false; so.noActivate = false; so.title = title_.c_str();
  if (!Create(so, x, y, wPx, hPx)) return false;
  scale_ = Dpi() / 96.f;
  if (!drop_) {
    drop_ = new DropTargetImpl(this);
    dropRegistered_ = SUCCEEDED(RegisterDragDrop(Hwnd(), drop_));
  }
  ApplyBlur();
  open_ = true;
  OnOpened();
  RenderNow(false);
  if (show) {
    ShowWindow(Hwnd(), o.activate ? SW_SHOW : SW_SHOWNA);
    if (o.activate) { SetForegroundWindow(Hwnd()); SetFocus(Hwnd()); }
    Invalidate();
  }
  return true;
}

bool DialogWindow::RenderToPng(const std::filesystem::path& png, int frames) {
  std::vector<uint8_t> px; int w = 0, h = 0;
  for (int i = 0; i < std::max(1, frames); ++i) {
    // pump the queue so async results (thumbnails, probes) posted to this window are applied before the next frame
    const double until = NowSeconds() + 0.35;
    MSG m;
    while (NowSeconds() < until) {
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
      Sleep(10);
    }
    px.clear();
    if (!RenderNow(false, &px, &w, &h)) return false;
  }
  return gfx::SavePng(png, px.data(), w, h, true);
}

void DialogWindow::CloseWindow() {
  if (!open_) return;
  open_ = false;
  if (drop_ && dropRegistered_) { RevokeDragDrop(Hwnd()); dropRegistered_ = false; }
  if (InDispatch()) {
    Hide();
    PostMessageW(Hwnd(), gfx::WM_FILO_DISPOSE, 0, 0);
  } else OnDeferredClose();
}

void DialogWindow::OnDeferredClose() {
  auto cb = std::move(onClosed);
  Destroy();
  if (cb) cb();
}

void DialogWindow::OnDeviceReset(bool rebuilt) {
  if (!rebuilt) { dc_.Reset(); painter_.ResetCaches(); }
  else ApplyBlur();
}

Rect DialogWindow::BodyRect() const { return {kMargin, kMargin, opts_.widthDip, opts_.heightDip}; }

void DialogWindow::SetBodySize(float w, float h) {
  opts_.widthDip = w; opts_.heightDip = h;
  if (!Valid()) return;
  RECT r; GetWindowRect(Hwnd(), &r);
  SetWindowPos(Hwnd(), nullptr, r.left, r.top, (int)std::lround((w + 2 * kMargin) * scale_), (int)std::lround((h + 2 * kMargin) * scale_), SWP_NOZORDER | SWP_NOACTIVATE);
  Invalidate();
}
void DialogWindow::RecaptureGlass() { ApplyBlur(); }

// The visible app uses a compositor-owned host backdrop, sampled before Filo is
// drawn. The bitmap path exists only for deterministic, hidden reference renders.
static void ApplyGlassyInk(Palette& p) {
  // glassyui --lg-foreground / --lg-muted-foreground
  const bool d = Theme::Get().IsDark();
  p.text = d ? Color::Hex(0xF5F5F7) : Color::Hex(0x222224);
  p.textSecondary = d ? Color::Hex(0xE5E5EA, .86f) : Color::Hex(0x505055);
  p.textTertiary = d ? Color::Hex(0xE5E5EA, .62f) : Color::Hex(0x505055, .82f);
}

void DialogWindow::ApplyBlur() {
  if (!Valid()) return;
  SetBlurBehind(-1);
  const float intensity = Theme::Get().EffectiveGlass();
  if (liveAllowed_ && opts_.glassBody && intensity > 0) {
    const float sc = Dpi() / 96.f;
    const int m = (int)std::lround(kMargin * sc);
    RECT bounds{m, m, WidthPx() - m, HeightPx() - m};
    const float radius = opts_.cornerRadius > 0 ? opts_.cornerRadius : dialog::cornerRadius;
    if (SetLiveBackdrop(bounds, radius * sc, Theme::Get().IsDark(), intensity)) {
      back_ = {}; backBmp_.Reset();
      dyn_ = Theme::Get().Colors();
      if (Theme::Get().State().glassStyle == 3) ApplyGlassyInk(dyn_);
      else
      if (!Theme::Get().IsDark()) {
        dyn_.textSecondary = Color::Hex(0x25282F);
        dyn_.textTertiary = dyn_.textSecondary;
      }
      dynValid_ = true;
      return;
    }
  }
  DisableLiveBackdrop();
  back_ = {}; backBmp_.Reset(); dynValid_ = false;
  if (opts_.glassBody && intensity > 0 && _wgetenv(L"FILO_TEST_BACKDROP")) CaptureGlass();
}

void DialogWindow::CaptureGlass() {
  const wchar_t* testImage = _wgetenv(L"FILO_TEST_BACKDROP");
  if (!Valid() || !testImage) return;
  const bool glassy = Theme::Get().State().glassStyle == 3;
  const float sigma = (glassy ? 3.f : ds::glass::dialogBlurDip) * (Dpi() / 96.f);
  gfx::BackdropImage img;
  std::vector<uint8_t> tmp; int iw = 0, ih = 0;
  if (!gfx::LoadBgra(testImage, tmp, iw, ih) || !gfx::BackdropFromImage(testImage, RECT{0, 0, iw, ih}, sigma, img)) return;
  for (auto& px : img.blurred) {
    Color bg(((px >> 16) & 255) / 255.f, ((px >> 8) & 255) / 255.f, (px & 255) / 255.f);
    Color c = ds::FrostedBackdrop(bg, Theme::Get().IsDark(), Theme::Get().EffectiveGlass());
    if (glassy) {
      // glassyui slab: saturate(1.8 dark / 1.6 light) then brightness(.38 dark / 1 light)
      const bool d = Theme::Get().IsDark();
      const float s = d ? 1.8f : 1.6f, k = d ? .38f : 1.f;
      auto cl = [](float v) { return std::clamp(v, 0.f, 1.f); };
      c = Color(cl(k * ((.213f + .787f * s) * bg.r + (.715f - .715f * s) * bg.g + (.072f - .072f * s) * bg.b)),
                cl(k * ((.213f - .213f * s) * bg.r + (.715f + .285f * s) * bg.g + (.072f - .072f * s) * bg.b)),
                cl(k * ((.213f - .213f * s) * bg.r + (.715f - .715f * s) * bg.g + (.072f + .928f * s) * bg.b)));
    }
    px = 0xFF000000u | (uint32_t)(c.b * 255 + 0.5f) | ((uint32_t)(c.g * 255 + 0.5f) << 8) | ((uint32_t)(c.r * 255 + 0.5f) << 16);
  }
  back_ = std::move(img); backBmp_.Reset(); UpdateGlassPalette(); Invalidate();
}

void DialogWindow::UpdateGlassPalette() {
  // Theme selection is explicit; desktop luminance must never flip text polarity.
  dyn_ = Theme::Get().Colors();
  if (Theme::Get().State().glassStyle == 3) ApplyGlassyInk(dyn_);
  dynValid_ = back_.validBlur();
}

void DialogWindow::OnRender(ID3D11RenderTargetView*, ID3D11Texture2D* bb, int w, int h) {
  auto& dev = gfx::Device::Get();
  if (!dc_) { if (FAILED(dev.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return; }
  const float dpi = (float)Dpi();
  scale_ = dpi / 96.f;
  ComPtr<IDXGISurface> surf;
  if (FAILED(bb->QueryInterface(__uuidof(IDXGISurface), surf.AsVoid()))) return;
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return;
  dc_->SetTarget(target.Get());
  dc_->BeginDraw();
  dc_->Clear(D2D1::ColorF(0, 0, 0, 0));

  const float glass = Theme::Get().EffectiveGlass();
  const bool liveGlass = HasLiveBackdrop() && opts_.glassBody;
  const bool haveGlass = glass > 0 && (liveGlass || back_.validBlur()) && dynValid_ && opts_.glassBody;
  const Palette& pal = haveGlass ? dyn_ : Theme::Get().Colors();
  ui_.SetPaletteOverride(haveGlass ? &dyn_ : nullptr);
  const float W = w / scale_, H = h / scale_;
  painter_.Begin(dc_.Get(), dpi, opts_.rtl);
  in_.focused = GetForegroundWindow() == Hwnd();
  ui_.BeginFrame(painter_, in_, W, H, NowSeconds(), opts_.rtl);
  SetCloseBubble(RECT{},false);
  ui_.SetCloseBubbleProvider([this,glass](const Rect& circle) {
    if(!liveAllowed_||glass<=0)return false;
    RECT px{(LONG)std::floor(circle.x*scale_),(LONG)std::floor(circle.y*scale_),
      (LONG)std::ceil(circle.right()*scale_),(LONG)std::ceil(circle.bottom()*scale_)};
    return SetCloseBubble(px,true);
  });

  const Rect body = BodyRect();
  const float R = opts_.cornerRadius > 0 ? opts_.cornerRadius : dialog::cornerRadius;
  if (opts_.glassBody) {
    if (Theme::Get().State().glassStyle == 3 && (liveGlass || haveGlass)) painter_.GlassyShadow(body, R, Theme::Get().IsDark());
    else painter_.Shadow(body, R, dialog::shadowBlur, Color(0, 0, 0, dialog::shadowAlpha * 0.75f), 10);
  }
  if (liveGlass) {
    painter_.ClearRoundRect(body, R); // keep the outer shadow out of the transparent reading surface
    // The composition visual below this swap chain supplies the live material.
    if (Theme::Get().State().glassStyle == 0) painter_.LiquidBody(body,R,Theme::Get().IsDark(),glass);
    else if (Theme::Get().State().glassStyle == 2) painter_.AcrylicBody(body,R,Theme::Get().IsDark(),glass);
    else if (Theme::Get().State().glassStyle == 3) painter_.GlassyBody(body,R,Theme::Get().IsDark());
    else painter_.FillRoundRectGradient(body, R, Color(1, 1, 1, Theme::Get().IsDark() ? 0.01f : 0.04f), Color(1, 1, 1, 0.f));
  } else if (haveGlass) {
    if (!backBmp_) {
      D2D1_BITMAP_PROPERTIES1 bp2 = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
      ComPtr<ID2D1Bitmap1> b1;
      if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(back_.bw, back_.bh), back_.blurred.data(), back_.bw * 4, &bp2, &b1))) backBmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b1.Get()));
    }
    RECT wr; GetWindowRect(Hwnd(), &wr);
    // world-fixed backdrop: stays put while the window is dragged, like a real transparent window
    const Rect dst{(back_.screenRect.left - wr.left) / scale_, (back_.screenRect.top - wr.top) / scale_,
                   (float)(back_.screenRect.right - back_.screenRect.left) / scale_, (float)(back_.screenRect.bottom - back_.screenRect.top) / scale_};
    painter_.PushClipRoundRect(body, R);
    if (backBmp_) painter_.Bitmap(backBmp_.Get(), dst);
    // Subtle vertical sheen, clipped to the same body as the material.
    if (Theme::Get().State().glassStyle == 3) painter_.GlassyBody(body, R, Theme::Get().IsDark());
    else painter_.FillRoundRectGradient(body, R, Color(1, 1, 1, Theme::Get().IsDark() ? 0.01f : 0.07f), Color(1, 1, 1, 0.f));
    painter_.PopClip();
  } else if (opts_.glassBody) {
    // opaque fallback (reduce transparency / high contrast / capture failed)
    const bool dark = Theme::Get().IsDark();
    painter_.FillRoundRectGradient(body, R, dark ? Color::Hex(0x2B2C30) : Color::Hex(0xF6F7FA), dark ? Color::Hex(0x232427) : Color::Hex(0xECEEF3));
  }
  Rect content = body;
  if (opts_.showTitleBar) {
    const float TH = dialog::titleBarHeight;
    painter_.PushClipRoundRect(body, R); painter_.FillRoundRectGradient({body.x, body.y, body.w, TH}, 0, Color(1, 1, 1, haveGlass ? 0.05f : 0.10f), Color(1, 1, 1, haveGlass ? 0.0f : 0.06f)); painter_.PopClip();
    painter_.FillRect({body.x, body.y + TH, body.w, 1.f}, pal.divider);
    const float cb = opts_.glassCloseButton?28.f:dialog::closeButton;
    Rect closeR{opts_.glassCloseButton || !opts_.rtl ? body.x+dialog::closeInset : body.right()-dialog::closeInset-cb, body.y + (TH - cb) / 2, cb, cb};
    TextStyle ts; ts.size = dialog::titleFontSize; ts.weight = 700; ts.align = Align::Center;
    painter_.Text({body.x + cb + dialog::closeInset * 2, body.y, body.w - 2 * (cb + dialog::closeInset * 2), TH}, title_, pal.text, ts);
    content = {body.x, body.y + TH + 1, body.w, body.h - TH - 1};
    ui_.PushId(0xC105E);
    if (ui_.CloseButton(closeR,opts_.glassCloseButton,CanClose())) OnCloseRequested();
    ui_.PopId();
  }
  if (opts_.glassBody) painter_.PushClipRoundRect(body, R);
  Draw(ui_, content, body);
  if (opts_.glassBody) painter_.PopClip();
  if (opts_.glassBody) {
    // border last, over everything
    if ((liveGlass || haveGlass) && Theme::Get().State().glassStyle == 3) {
      painter_.GlassyRim(body,R,Theme::Get().IsDark());
    } else if (liveGlass && Theme::Get().State().glassStyle == 0) {
      painter_.LiquidRim(body,R,Theme::Get().IsDark(),Theme::Get().State().glassReflections*glass);
    } else {
    painter_.StrokeRoundRect(body.Inset(-0.5f), R + 0.5f, Color(0, 0, 0, 0.20f), 1.f);          // thin dark outline (as in the reference edge)
    painter_.StrokeRoundRect(body.Inset(0.6f), R - 0.6f, Color(1, 1, 1, 0.28f), 1.f);            // inner specular rim
    }
  }

  const bool more = ui_.EndFrame();
  painter_.End();
  dc_->EndDraw();
  dc_->SetTarget(nullptr);
  in_.ClearTransient();
  OnTick(NowSeconds());
  if (more || NeedsContinuousFrames()) Invalidate();
}

void DialogWindow::CancelPointerInput() {
  in_.down=in_.pressed=in_.released=in_.dbl=false;
  ui_.CancelPointer();
  if(GetCapture()==Hwnd())ReleaseCapture();
  Invalidate();
}

LRESULT DialogWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  auto toDip = [&](LPARAM l) { return D2D1::Point2F(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_); };
  switch (msg) {
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(Hwnd(), &p);
      const float x = p.x / scale_, y = p.y / scale_;
      const Rect body = BodyRect();
      const float edge = 7;
      if (opts_.resizable) {
        const bool L = x >= body.x - edge && x < body.x + edge, Rr = x > body.right() - edge && x <= body.right() + edge;
        const bool T = y >= body.y - edge && y < body.y + edge, B = y > body.bottom() - edge && y <= body.bottom() + edge;
        if (T && L) { SetHandled(true); return HTTOPLEFT; } if (T && Rr) { SetHandled(true); return HTTOPRIGHT; }
        if (B && L) { SetHandled(true); return HTBOTTOMLEFT; } if (B && Rr) { SetHandled(true); return HTBOTTOMRIGHT; }
        if (L) { SetHandled(true); return HTLEFT; } if (Rr) { SetHandled(true); return HTRIGHT; }
        if (T) { SetHandled(true); return HTTOP; } if (B) { SetHandled(true); return HTBOTTOM; }
      }
      if (!body.Contains(x, y)) { SetHandled(true); return HTTRANSPARENT; }
      if (opts_.showTitleBar && y < body.y + dialog::titleBarHeight) {
        const float cb = opts_.glassCloseButton?28.f:dialog::closeButton;
        const float cx0 = opts_.glassCloseButton || !opts_.rtl ? body.x+dialog::closeInset : body.right()-dialog::closeInset-cb;
        if (x >= cx0 - 4 && x <= cx0 + cb + 4) { SetHandled(true); return HTCLIENT; }
        SetHandled(true); return HTCAPTION;
      }
      SetHandled(true); return HTCLIENT; }
    case WM_NCCALCSIZE: if (wp) { SetHandled(true); return 0; } break;       // no non-client area at all
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      if (opts_.resizable) {
        mm->ptMinTrackSize.x = (LONG)((std::max(opts_.minWidthDip, 300.f) + 2 * kMargin) * scale_);
        mm->ptMinTrackSize.y = (LONG)((std::max(opts_.minHeightDip, 240.f) + 2 * kMargin) * scale_);
      }
      SetHandled(true); return 0; }
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && open_) {
        scale_ = Dpi() / 96.f;
        opts_.widthDip = LOWORD(lp) / scale_ - 2 * kMargin; opts_.heightDip = HIWORD(lp) / scale_ - 2 * kMargin;
        ApplyBlur();
      }
      break;
    case WM_SETCURSOR: {
      if (LOWORD(lp) == HTCLIENT) {
        int c = ui_.CursorWanted();
        SetCursor(LoadCursorW(nullptr, c ? MAKEINTRESOURCEW(c) : IDC_ARROW));
        SetHandled(true); return TRUE;
      }
      break; }
    case WM_CANCELMODE: CancelPointerInput();SetHandled(true);return 0;
    case WM_CAPTURECHANGED: if(in_.down)CancelPointerInput();SetHandled(true);return 0;
    case WM_MOUSEMOVE: {
      auto p = toDip(lp); in_.mx = p.x; in_.my = p.y; in_.inWindow = true; Invalidate();
      TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, Hwnd(), 0}; TrackMouseEvent(&te);
      SetHandled(true); return 0; }
    case WM_MOUSELEAVE: in_.inWindow = false; in_.mx = in_.my = -10000; Invalidate(); SetHandled(true); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
      auto p = toDip(lp); in_.mx = p.x; in_.my = p.y; in_.inWindow = true;
      in_.ctrl=(wp&MK_CONTROL)!=0;in_.shift=(wp&MK_SHIFT)!=0;
      in_.down = true; in_.pressed = true; if (msg == WM_LBUTTONDBLCLK) in_.dbl = true;
      SetCapture(Hwnd()); SetFocus(Hwnd()); Invalidate(); SetHandled(true); return 0; }
    case WM_LBUTTONUP: {
      auto p = toDip(lp); in_.mx = p.x; in_.my = p.y;
      in_.down = false; in_.released = true; ReleaseCapture(); Invalidate(); SetHandled(true); return 0; }
    case WM_RBUTTONDOWN: in_.rpressed = true; Invalidate(); SetHandled(true); return 0;
    case WM_MOUSEWHEEL: in_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; Invalidate(); SetHandled(true); return 0;
    case WM_CHAR: {
      wchar_t c = (wchar_t)wp;
      if (c >= 32 && c != 127) in_.chars.push_back(c);
      Invalidate(); SetHandled(true); return 0; }
    case WM_UNICHAR: {
      if (wp == UNICODE_NOCHAR) { SetHandled(true); return TRUE; }
      if (wp >= 32 && wp <= 0x10FFFF && !(wp >= 0xD800 && wp <= 0xDFFF)) {
        if (wp <= 0xFFFF) in_.chars.push_back((wchar_t)wp);
        else { const uint32_t cp = (uint32_t)wp - 0x10000; in_.chars.push_back((wchar_t)(0xD800 + (cp >> 10))); in_.chars.push_back((wchar_t)(0xDC00 + (cp & 0x3FF))); }
      }
      Invalidate(); SetHandled(true); return 0; }
    case WM_PASTE: case WM_COPY: case WM_CUT:
      in_.keys.push_back(KeyEvent{static_cast<uint32_t>(msg == WM_PASTE ? 'V' : (msg == WM_COPY ? 'C' : 'X')), true, false, false});
      Invalidate(); SetHandled(true); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      KeyEvent k{(uint32_t)wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0};
      in_.ctrl = k.ctrl; in_.shift = k.shift; in_.alt = k.alt;
      in_.keys.push_back(k);
      if (wp == VK_ESCAPE) { if (!ui_.WantsEscape()) OnCloseRequested(); }
      else if (wp == VK_RETURN) { if (!ui_.WantsEscape()) OnEnter(); }
      Invalidate();
      if (msg == WM_SYSKEYDOWN && wp != VK_F4) { SetHandled(true); return 0; }
      break; }
    case WM_ACTIVATE: case WM_SETFOCUS: case WM_KILLFOCUS: Invalidate(); break;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: Theme::Get().RefreshFromSystem(); ApplyBlur(); Invalidate(); break;
    case WM_DPICHANGED: ApplyBlur(); break;
    case WM_EXITSIZEMOVE: ApplyBlur(); break;
    case WM_CLOSE: OnCloseRequested(); SetHandled(true); return 0;
    case WM_DESTROY: break;
  }
  return Surface::OnMessage(msg, wp, lp);
}

} // namespace filo::ui
