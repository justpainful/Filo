#include "wheel/wheel_window.h"
#include "gfx/icons.h"
#include "design/theme.h"
#include "core/log.h"
#include "core/util.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "wheel_vs.h"
#include "wheel_ps.h"

namespace filo::wheel {

using namespace filo::ds;
using filo::gfx::Device;

// ------------------------------------------------------------------------------------------------ GPU side
struct alignas(16) WheelCB {
  float centerX, centerY, Rpx, n;
  float gapRad, corner, sectorOuter, sectorInner;
  float holeRadius, appear, glassMix, hasBackdrop;
  float fillColor[4], hoverColor[4], rimColor[4], holeColor[4];
  float shadowParams[4], glassParams[4], backRect[4], lightDir[4], fillGrad[4];
  float optics[4];
  float acrylic[4];
  struct { float hover, press, scale, alpha; } sec[kMaxItems];
};
static_assert(sizeof(WheelCB) % 16 == 0);

struct WheelWindow::Gpu {
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11Buffer> cb;
  ComPtr<ID3D11SamplerState> samp;
  ComPtr<ID3D11Texture2D> texSharp, texBlur;
  ComPtr<ID3D11ShaderResourceView> srvSharp, srvBlur;
  ComPtr<ID3D11BlendState> blendOff;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<ID2D1Bitmap1> thumb;
  std::unordered_map<std::wstring, ComPtr<IDWriteTextFormat>> formats;
  int sharpW = 0, sharpH = 0, blurW = 0, blurH = 0;

  bool Create() {
    auto& dev = Device::Get();
    if (FAILED(dev.d3d->CreateVertexShader(g_wheel_vs, sizeof g_wheel_vs, nullptr, &vs))) return false;
    if (FAILED(dev.d3d->CreatePixelShader(g_wheel_ps, sizeof g_wheel_ps, nullptr, &ps))) return false;
    D3D11_BUFFER_DESC bd{sizeof(WheelCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(dev.d3d->CreateBuffer(&bd, nullptr, &cb))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev.d3d->CreateSamplerState(&sd, &samp))) return false;
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev.d3d->CreateBlendState(&bl, &blendOff))) return false;
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(dev.d3d->CreateRasterizerState(&rd, &rs))) return false;
    ComPtr<ID2D1DeviceContext> d;
    if (FAILED(dev.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    return true;
  }

  bool UploadTexture(const uint32_t* px, int w, int h, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv, int& curW, int& curH) {
    auto& dev = Device::Get();
    if (!tex || curW != w || curH != h) {
      tex.Reset(); srv.Reset();
      D3D11_TEXTURE2D_DESC td{};
      td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sr{px, (UINT)w * 4, 0};
      if (FAILED(dev.d3d->CreateTexture2D(&td, &sr, &tex))) return false;
      if (FAILED(dev.d3d->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
      curW = w; curH = h;
    } else {
      dev.ctx->UpdateSubresource(tex.Get(), 0, nullptr, px, (UINT)w * 4, 0);
    }
    return true;
  }

  IDWriteTextFormat* Format(float sizePx, DWRITE_FONT_WEIGHT weight, bool center = true) {
    wchar_t key[64]; swprintf_s(key, L"%.1f/%d/%d", sizePx, (int)weight, center ? 1 : 0);
    auto it = formats.find(key);
    if (it != formats.end()) return it->second.Get();
    auto& dev = Device::Get();
    ComPtr<IDWriteTextFormat> f;
    const wchar_t* fams[] = {type::family, type::familyFallback};
    for (auto fam : fams) {
      if (SUCCEEDED(dev.dwrite->CreateTextFormat(fam, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizePx, L"en-us", &f))) break;
    }
    if (!f) return nullptr;
    if (center) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return formats.emplace(key, f).first->second.Get();
  }
};

// ------------------------------------------------------------------------------------------------ window
WheelWindow::WheelWindow() = default;
WheelWindow::~WheelWindow() = default;

bool WheelWindow::Init() {
  gfx::SurfaceOptions o;
  o.style = WS_POPUP; o.topmost = true; o.noActivate = true; o.toolWindow = true; o.title = L"Filo Wheel";
  if (!Create(o, 0, 0, 400, 400)) return false;
  gpu_ = std::make_unique<Gpu>();
  if (!gpu_->Create()) { FILO_LOG(Error, "wheel", "GPU resources failed"); return false; }
  return true;
}

void WheelWindow::OnDeviceReset(bool rebuilt) {
  if (!rebuilt) { if (gpu_) { gpu_->dc.Reset(); } gpu_.reset(); gfx::IconCache::Get().Clear(); }
  else { gpu_ = std::make_unique<Gpu>(); if (!gpu_->Create()) gpu_.reset(); backdropDirty_ = !backdrop_.sharp.empty(); thumbDirty_ = !thumb_.empty(); }
}

Layout WheelWindow::MakeLayout() const {
  Layout L;
  L.n = std::max(1, (int)content_.items.size());
  L.R = Rpx_;
  return L;
}

RECT WheelWindow::ScreenRect() const {
  return {origin_.x, origin_.y, origin_.x + (LONG)(w_c_ * 2), origin_.y + (LONG)(h_c_ * 2)};
}

void WheelWindow::LayoutWindow(POINT cursor, bool clampToWorkArea) {
  HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
  const float scale = dx / 96.f * Theme::Get().State().wheelScale;
  const float availableRadius = std::max(48.f, (float)std::min(mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top) * 0.5f - 12.f);
  Rpx_ = radiusOverridePx_ > 0 ? radiusOverridePx_ : std::min(ds::wheel::radiusDip * scale, availableRadius);
  const float half = Rpx_ * (1.f + ds::wheel::margin);
  const int size = (int)std::ceil(half * 2);
  // keep the whole rim inside the monitor work area (taskbar excluded); never jump when far from edges
  const RECT& wa = mi.rcWork;
  float cx = (float)cursor.x, cy = (float)cursor.y;
  if (clampToWorkArea) {
    cx = std::clamp(cx, wa.left + Rpx_ + 4.f, wa.right - Rpx_ - 4.f);
    cy = std::clamp(cy, wa.top + Rpx_ + 4.f, wa.bottom - Rpx_ - 4.f);
  }
  origin_.x = (LONG)std::lround(cx - half);
  origin_.y = (LONG)std::lround(cy - half);
  w_c_ = h_c_ = (float)size * 0.5f;
  SetBounds(origin_.x, origin_.y, size, size, false);
}

void WheelWindow::ShowAt(POINT cursorPx, bool liveBackdrop) {
  if (!Valid() || !gpu_) return;
  LayoutWindow(cursorPx);
  const double now = NowSeconds();
  const float glass = Theme::Get().EffectiveGlass();
  const RECT bounds{(LONG)std::lround(w_c_ - Rpx_), (LONG)std::lround(h_c_ - Rpx_), (LONG)std::lround(w_c_ + Rpx_), (LONG)std::lround(h_c_ + Rpx_)};
  liveAllowed_ = liveBackdrop;
  if (!liveAllowed_ || glass <= 0 || !SetLiveBackdrop(bounds, Rpx_, Theme::Get().IsDark(), glass)) DisableLiveBackdrop();
  backdrop_ = {}; backdropDirty_ = false; // the visible wheel never photographs the desktop
  shown_ = true; dismissing_ = false; tShow_ = now; tLast_ = now; appear_ = 0;
  const bool animate = Theme::Get().MotionScale() > 0.f;
  if (!animate) { appear_ = 1; tShow_ = now - 10; }
  for (auto& a : anim_) { a.hover = a.hoverT = a.press = a.pressT = 0; a.alpha = 1; a.flash = 0; }
  hover_ = pressed_ = -1;
  RenderNow(false);             // prime the first frame so the window never flashes empty
  ShowWindow(Hwnd(), SW_SHOWNOACTIVATE);
  SetWindowPos(Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  Invalidate();
}

void WheelWindow::PrepareOffscreen(POINT cursorPx, bool fullyShown) {
  liveAllowed_ = false;
  DisableLiveBackdrop();
  LayoutWindow(cursorPx, false);
  shown_ = true; dismissing_ = false; appear_ = fullyShown ? 1.f : 0.f; tShow_ = NowSeconds() - 10; tLast_ = NowSeconds();
  for (auto& a : anim_) { a.hover = a.hoverT = a.press = a.pressT = 0; a.alpha = 1; a.flash = 0; }
  tSwitch_ = -1; switchP_ = 1;
  hover_ = pressed_ = -1;
}

void WheelWindow::SetHoverImmediate(int idx) {
  SetHoverIndex(idx);
  for (int k = 0; k < kMaxItems; ++k) anim_[k].hover = anim_[k].hoverT;
}

void WheelWindow::SetActivatable(bool on) {
  if (!Valid()) return;
  LONG_PTR ex = GetWindowLongPtrW(Hwnd(), GWL_EXSTYLE);
  ex = on ? (ex & ~(LONG_PTR)WS_EX_NOACTIVATE) : (ex | WS_EX_NOACTIVATE);
  SetWindowLongPtrW(Hwnd(), GWL_EXSTYLE, ex);
}
void WheelWindow::FocusForKeyboard() {
  if (!Valid()) return;
  // The hotkey/tray handler that opened us owns the foreground right, so this normally succeeds.
  SetForegroundWindow(Hwnd());
  SetFocus(Hwnd());
}

void WheelWindow::Dismiss(bool animated) {
  if (!shown_) return;
  if (!animated || Theme::Get().MotionScale() == 0.f) {
    shown_ = false; dismissing_ = false; Hide();
    if (onDismissed) onDismissed();
    return;
  }
  if (dismissing_) return;
  dismissing_ = true; tDismiss_ = NowSeconds();
  Invalidate();
}

void WheelWindow::SetContent(WheelContent c, bool animateSwitch) {
  if (animateSwitch && shown_ && Theme::Get().MotionScale() > 0.f) {
    outgoing_ = content_; tSwitch_ = NowSeconds(); switchP_ = 0;
    for (auto& a : anim_) a.alpha = 0;
  } else {
    tSwitch_ = -1; switchP_ = 1; outgoing_ = {};
    for (auto& a : anim_) a.alpha = 1;
  }
  content_ = std::move(c);
  if ((int)content_.items.size() > kMaxItems) content_.items.resize(kMaxItems);
  hover_ = pressed_ = -1;
  for (auto& a : anim_) { a.hover = a.hoverT = a.press = a.pressT = 0; }
  Invalidate();
}

void WheelWindow::SetThumbnail(int w, int h, const uint8_t* px, size_t stride) {
  thumbW_ = w; thumbH_ = h;
  thumb_.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) memcpy(thumb_.data() + (size_t)y * w * 4, px + (size_t)y * stride, (size_t)w * 4);
  thumbDirty_ = true;
  Invalidate();
}
void WheelWindow::ClearThumbnail() { thumb_.clear(); thumbW_ = thumbH_ = 0; thumbDirty_ = true; if (gpu_) gpu_->thumb.Reset(); }

void WheelWindow::SetHoverIndex(int idx) {
  if (idx >= (int)content_.items.size()) idx = -1;
  if (idx >= 0 && !content_.items[idx].enabled) idx = -1;
  if (idx == hover_) return;
  hover_ = idx;
  for (int k = 0; k < kMaxItems; ++k) anim_[k].hoverT = (k == idx) ? 1.f : 0.f;
  if (onHoverChanged) onHoverChanged(idx);
  Invalidate();
}
void WheelWindow::SetPressed(int idx) {
  pressed_ = idx;
  for (int k = 0; k < kMaxItems; ++k) anim_[k].pressT = (k == idx) ? 1.f : 0.f;
  Invalidate();
}
void WheelWindow::ConfirmFlash(int idx) { if (idx >= 0 && idx < kMaxItems) anim_[idx].flash = 1.f; Invalidate(); }

void WheelWindow::MoveSelection(int dir) {
  const int n = (int)content_.items.size();
  if (n == 0) return;
  int cur = hover_;
  for (int step = 0; step < n; ++step) {
    cur = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir + n) % n;
    if (content_.items[cur].enabled) { SetHoverIndex(cur); return; }
  }
}

Hit WheelWindow::HitTestScreen(POINT pt) const {
  Layout L = MakeLayout();
  const float s = Lerp(ds::motion::appearScaleFrom, 1.f, appear_);
  Vec2 p{((float)(pt.x - origin_.x) - w_c_) / s, ((float)(pt.y - origin_.y) - h_c_) / s};
  float scales[kMaxItems];
  for (int k = 0; k < kMaxItems; ++k) scales[k] = Lerp(1.f, ds::wheel::hoverScale, SmoothStep(0, 1, anim_[k].hover));
  Hit h = HitTest(L, p, scales);
  if (h.zone == Zone::Sector && h.sector >= (int)content_.items.size()) h = {Zone::Rim, -1};
  return h;
}

bool WheelWindow::WantsContinuousFrames() const {
  if (!shown_) return false;
  if (dismissing_ || appear_ < 1.f || switchP_ < 1.f) return true;
  for (int k = 0; k < kMaxItems; ++k) {
    const auto& a = anim_[k];
    if (a.hover != a.hoverT || a.press != a.pressT || a.flash > 0.f || a.alpha < 1.f) return true;
  }
  return false;
}

void WheelWindow::UploadBackdrop() {
  if (!gpu_ || !backdrop_.valid()) return;
  gpu_->UploadTexture(backdrop_.sharp.data(), backdrop_.w, backdrop_.h, gpu_->texSharp, gpu_->srvSharp, gpu_->sharpW, gpu_->sharpH);
  gpu_->UploadTexture(backdrop_.blurred.data(), backdrop_.bw, backdrop_.bh, gpu_->texBlur, gpu_->srvBlur, gpu_->blurW, gpu_->blurH);
  backdropDirty_ = false;
}

void WheelWindow::StepAnimation(double now) {
  float dt = (float)std::clamp(now - tLast_, 0.0, 0.1);
  tLast_ = now;
  const bool anim = Theme::Get().MotionScale() > 0.f;
  // appear / disappear
  if (dismissing_) {
    float p = anim ? (float)((now - tDismiss_) / ds::motion::wheelDisappear) : 1.f;
    appear_ = 1.f - EaseOutCubic(p);
    if (p >= 1.f) { shown_ = false; dismissing_ = false; appear_ = 0; Hide(); if (onDismissed) onDismissed(); return; }
  } else if (appear_ < 1.f) {
    float p = anim ? (float)((now - tShow_) / ds::motion::wheelAppear) : 1.f;
    appear_ = EaseOutCubic(p);
    if (p >= 1.f) appear_ = 1.f;
  }
  // wheel switch cross-fade
  if (tSwitch_ >= 0) {
    switchP_ = std::clamp((float)((now - tSwitch_) / ds::motion::wheelSwitch), 0.f, 1.f);
    for (int k = 0; k < kMaxItems; ++k) anim_[k].alpha = EaseOutCubic(std::clamp(switchP_ * 1.25f - 0.03f * k, 0.f, 1.f));
    if (switchP_ >= 1.f) { tSwitch_ = -1; outgoing_ = {}; for (auto& a : anim_) a.alpha = 1.f; }
  }
  for (auto& a : anim_) {
    auto approach = [&](float& v, float t, float dur) {
      if (!anim) { v = t; return; }
      float step = dt / std::max(dur, 0.001f);
      if (v < t) v = std::min(t, v + step); else if (v > t) v = std::max(t, v - step);
    };
    approach(a.hover,a.hoverT,Theme::Get().State().glassStyle==0?(a.hoverT>a.hover?.16f:.18f):(a.hoverT>a.hover?ds::motion::hoverIn:ds::motion::hoverOut));
    approach(a.press, a.pressT, 0.06f);
    if (a.flash > 0.f) a.flash = anim ? std::max(0.f, a.flash - dt / ds::motion::dropConfirm) : 0.f;
  }
}

static void SetColor(float* dst, const Color& c) { dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = c.a; }

void WheelWindow::OnRender(ID3D11RenderTargetView* rtv, ID3D11Texture2D* bb, int w, int h) {
  auto& dev = Device::Get();
  if (!gpu_) return;
  StepAnimation(NowSeconds());
  if (backdropDirty_) UploadBackdrop();
  if (liveAllowed_ && Theme::Get().EffectiveGlass() > 0.f) {
    const float r = Rpx_ * Lerp(ds::motion::appearScaleFrom, 1.f, appear_);
    const RECT clip{(LONG)std::lround(w_c_ - r), (LONG)std::lround(h_c_ - r), (LONG)std::lround(w_c_ + r), (LONG)std::lround(h_c_ + r)};
    gfx::LiquidShape shape{};const auto layout=MakeLayout();
    shape.cx=w_c_;shape.cy=h_c_;shape.halfW=shape.halfH=Rpx_;shape.radius=Rpx_;
    shape.count=(float)layout.n;shape.gap=layout.gapRad;shape.corner=layout.corner;
    shape.outer=layout.sectorOuter;shape.inner=layout.sectorInner;shape.hole=layout.holeRadius;
    shape.appearScale=Lerp(ds::motion::appearScaleFrom,1.f,appear_);shape.dpiScale=Scale();
    for(int k=0;k<kMaxItems;++k) shape.scales[k]= k<(int)content_.items.size()?Lerp(1.f,ds::wheel::hoverScale,SmoothStep(0,1,anim_[k].hover))+.02f*anim_[k].flash:0.f;
    SetLiveBackdrop(clip, r, Theme::Get().IsDark(), Theme::Get().EffectiveGlass(), &shape);
    SetLiveBackdropOpacity(appear_);
  } else DisableLiveBackdrop();

  const float clear[4] = {0, 0, 0, 0};
  dev.ctx->ClearRenderTargetView(rtv, clear);
  if (!shown_ && appear_ <= 0.f) return;

  const Palette& pal = Theme::Get().Colors();
  const float glassMix = Theme::Get().EffectiveGlass();
  Layout L = MakeLayout();

  WheelCB cb{};
  cb.centerX = w_c_; cb.centerY = h_c_; cb.Rpx = Rpx_; cb.n = (float)L.n;
  cb.gapRad = L.gapRad; cb.corner = L.corner; cb.sectorOuter = L.sectorOuter; cb.sectorInner = L.sectorInner;
  cb.holeRadius = L.holeRadius; cb.appear = appear_; cb.glassMix = glassMix;
  cb.hasBackdrop = HasLiveBackdrop() ? -1.f : ((backdrop_.valid() && gpu_->srvSharp && glassMix > 0.f) ? 1.f : 0.f);
  SetColor(cb.fillColor, pal.sectorFill);
  SetColor(cb.hoverColor, pal.sectorFillHover);
  SetColor(cb.rimColor, pal.rimFill);
  SetColor(cb.holeColor, pal.holeFill);
  if (glassMix <= 0.f) {                       // reduced transparency: opaque, still pretty
    Color solid = Theme::Get().IsDark() ? Color::Hex(0x2B2C30, 0.97f) : Color::Hex(0xF3F4F7, 0.97f);
    SetColor(cb.fillColor, solid);
    SetColor(cb.rimColor, Mix(solid, Color::Hex(0x808590), 0.35f).WithAlpha(0.97f));
    SetColor(cb.holeColor, Mix(solid, Color::Hex(0x000000), 0.15f).WithAlpha(0.97f));
  }
  cb.shadowParams[0] = ds::wheel::shadowBlur * Rpx_ * 2.f;
  cb.shadowParams[1] = ds::wheel::shadowOffsetY * Rpx_;
  cb.shadowParams[2] = ds::wheel::shadowAlpha;
  cb.shadowParams[3] = ds::motion::appearScaleFrom;
  cb.glassParams[0] = ds::glass::refractStrength * Rpx_;
  cb.glassParams[1] = ds::glass::bevelWidth * Rpx_;
  cb.glassParams[2] = ds::glass::specularStrength;
  cb.glassParams[3] = ds::glass::fresnelPower;
  cb.backRect[0] = (float)(backdrop_.screenRect.left - origin_.x);
  cb.backRect[1] = (float)(backdrop_.screenRect.top - origin_.y);
  cb.backRect[2] = (float)std::max(backdrop_.w, 1);
  cb.backRect[3] = (float)std::max(backdrop_.h, 1);
  cb.lightDir[0] = -0.55f; cb.lightDir[1] = -0.83f;                 // light from the upper left
  cb.lightDir[2] = ds::glass::saturationBoost; cb.lightDir[3] = 1.f;
  cb.optics[0]=HasLiveBackdrop()?(Theme::Get().State().glassStyle==0?1.f:Theme::Get().State().glassStyle==2?2.f:0.f):0.f;
  cb.acrylic[0]=Theme::Get().State().acrylicTransparency;
  cb.optics[1]=Theme::Get().State().glassReflections;
  cb.optics[2]=Theme::Get().State().glassRefraction;cb.optics[3]=Scale();
  if(cb.optics[0]>.5f) {cb.shadowParams[0]=22.f*Scale();cb.shadowParams[1]=6.f*Scale();cb.shadowParams[2]=.18f;}
  cb.fillGrad[0] = glassMix > 0.f ? ds::wheel::fillAlphaOuter : 0.f; cb.fillGrad[1] = ds::wheel::fillAlphaInner; cb.fillGrad[2] = Theme::Get().IsDark() ? 1.f : 0.f;
  for (int k = 0; k < kMaxItems; ++k) {
    const auto& a = anim_[k];
    const bool present = k < (int)content_.items.size();
    const float hv = SmoothStep(0, 1, a.hover);
    cb.sec[k].hover = std::clamp(hv + a.flash * 0.5f, 0.f, 1.f);
    cb.sec[k].press = SmoothStep(0, 1, a.press);
    cb.sec[k].scale = Lerp(1.f, ds::wheel::hoverScale, hv) + 0.02f * a.flash;
    float alpha = present ? a.alpha : 0.f;
    if (present && !content_.items[k].enabled) alpha *= 0.55f;
    cb.sec[k].alpha = alpha;
  }
  if (content_.items.empty()) cb.sec[0].alpha = 0;

  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(dev.ctx->Map(gpu_->cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof cb); dev.ctx->Unmap(gpu_->cb.Get(), 0); }
  dev.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  dev.ctx->VSSetShader(gpu_->vs.Get(), nullptr, 0);
  dev.ctx->PSSetShader(gpu_->ps.Get(), nullptr, 0);
  ID3D11Buffer* cbs[] = {gpu_->cb.Get()}; dev.ctx->PSSetConstantBuffers(0, 1, cbs);
  ID3D11SamplerState* ss[] = {gpu_->samp.Get()}; dev.ctx->PSSetSamplers(0, 1, ss);
  ID3D11ShaderResourceView* srvs[] = {gpu_->srvSharp.Get(), gpu_->srvBlur.Get()};
  dev.ctx->PSSetShaderResources(0, 2, srvs);
  dev.ctx->OMSetBlendState(gpu_->blendOff.Get(), nullptr, 0xFFFFFFFF);
  dev.ctx->RSSetState(gpu_->rs.Get());
  dev.ctx->Draw(3, 0);
  ID3D11ShaderResourceView* none[2] = {nullptr, nullptr}; dev.ctx->PSSetShaderResources(0, 2, none);
  dev.ctx->OMSetRenderTargets(0, nullptr, nullptr);

  // ---- Direct2D overlay: labels, icons, centre preview
  ComPtr<IDXGISurface> surf;
  if (FAILED(bb->QueryInterface(__uuidof(IDXGISurface), surf.AsVoid()))) return;
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(gpu_->dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return;
  gpu_->dc->SetTarget(target.Get());
  gpu_->dc->BeginDraw();
  DrawOverlay(gpu_->dc.Get(), bb, appear_);
  HRESULT hr = gpu_->dc->EndDraw();
  gpu_->dc->SetTarget(nullptr);
  if (hr == D2DERR_RECREATE_TARGET) Device::Get().HandleDeviceLost(hr);
}

void WheelWindow::DrawOverlay(ID2D1DeviceContext* dc, ID3D11Texture2D*, float appear) {
  const Palette& pal = Theme::Get().Colors();
  const Layout L = MakeLayout();
  const float R = Rpx_;
  const float s = Lerp(ds::motion::appearScaleFrom, 1.f, appear);
  const D2D1_POINT_2F C{w_c_, h_c_};
  dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, C));
  auto brush = [&](const Color& c, float alphaMul = 1.f) {
    ComPtr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(D2D1::ColorF(c.r, c.g, c.b, c.a * alphaMul), &b);
    return b;
  };
  const float labelAlphaAppear = SmoothStep(0.25f, 1.f, appear);

  // ---- centre: thumbnail + info pill
  {
    const float boxHalf = ds::wheel::holeRadius * R * 0.78f;
    if (thumbDirty_ && !thumb_.empty()) {
      gpu_->thumb.Reset();
      D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
      dc->CreateBitmap(D2D1::SizeU(thumbW_, thumbH_), thumb_.data(), thumbW_ * 4, &bp, &gpu_->thumb);
      thumbDirty_ = false;
    }
    const bool emptyMsg = content_.items.empty() && !content_.message.empty();
    if (gpu_->thumb && !emptyMsg) {
      float aw = (float)thumbW_, ah = (float)thumbH_;
      float k = std::min(boxHalf * 2 / aw, boxHalf * 2 / ah * 1.15f);
      float tw = aw * k, th = ah * k;
      D2D1_ROUNDED_RECT rr{D2D1::RectF(C.x - tw / 2, C.y - th / 2, C.x + tw / 2, C.y + th / 2), R * 0.045f, R * 0.045f};
      // soft shadow
      auto shadow = brush(Color(0, 0, 0, 0.22f), labelAlphaAppear);
      D2D1_ROUNDED_RECT sr = rr; sr.rect.top += R * 0.02f; sr.rect.bottom += R * 0.02f;
      dc->FillRoundedRectangle(sr, shadow.Get());
      ComPtr<ID2D1ImageBrush> ib;  // clip the bitmap to the rounded rect
      ComPtr<ID2D1BitmapBrush1> bb2;
      dc->CreateBitmapBrush(gpu_->thumb.Get(), &bb2);
      bb2->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP); bb2->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
      bb2->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(rr.rect.left, rr.rect.top));
      bb2->SetOpacity(labelAlphaAppear);
      dc->FillRoundedRectangle(rr, bb2.Get());
    } else if (!content_.centerIcon.empty() && !emptyMsg) {
      auto b = brush(pal.sectorText, 0.55f * labelAlphaAppear);
      gfx::IconCache::Get().Draw(dc, content_.centerIcon, C.x, C.y, R * 0.30f, b.Get(), 1.5f);
    }
    if (!content_.centerPrimary.empty() && !emptyMsg) {
      const float pw = ds::wheel::centerPillW * R, ph = ds::wheel::centerPillH * R;
      D2D1_ROUNDED_RECT pill{D2D1::RectF(C.x - pw / 2, C.y - ph / 2, C.x + pw / 2, C.y + ph / 2), ph / 2, ph / 2};
      auto fill = brush(Theme::Get().IsDark() ? Color::Hex(0x20242C, 0.94f) : Color(1, 1, 1, 0.84f), labelAlphaAppear);
      auto edge = brush(Color(1, 1, 1, 0.70f), labelAlphaAppear);
      dc->FillRoundedRectangle(pill, fill.Get());
      dc->DrawRoundedRectangle(pill, edge.Get(), 1.f);
      auto* f = gpu_->Format(R * 0.098f * 1.18f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      auto tb = brush(pal.sectorText, labelAlphaAppear);
      if (f) dc->DrawText(content_.centerPrimary.c_str(), (UINT32)content_.centerPrimary.size(), f, pill.rect, tb.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  // ---- empty wheel: explain why instead of showing nothing
  if (content_.items.empty() && !content_.message.empty()) {
    auto* f = gpu_->Format(R * 0.062f, DWRITE_FONT_WEIGHT_MEDIUM);
    auto b = brush(pal.sectorText, 0.85f * labelAlphaAppear);
    const float half = R * 0.33f;
    D2D1_RECT_F rc{C.x - half, C.y - R * 0.30f, C.x + half, C.y + R * 0.30f};
    if (f) {
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      dc->DrawText(content_.message.c_str(), (UINT32)content_.message.size(), f, rc, b.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  // ---- sector content
  const Vec2 box = SectorTextBox(L);
  auto drawItems = [&](const WheelContent& wc, float fade) {
    const int n = (int)wc.items.size();
    Layout Lc = L; Lc.n = std::max(1, n);
    for (int k = 0; k < n; ++k) {
      const WheelItem& it = wc.items[k];
      const auto& a = anim_[k];
      const float hv = SmoothStep(0, 1, a.hover);
      const float alpha = labelAlphaAppear * fade * (it.enabled ? 1.f : 0.40f);
      if (alpha <= 0.002f) continue;
      Vec2 anchor = SectorAnchor(Lc, k, (Lc.sectorInner + Lc.sectorOuter) * 0.5f - (it.icon.empty() ? 0.f : 0.01f));
      // hovered sectors grow about their centroid; move the content with them
      const float sc = Lerp(1.f, ds::wheel::hoverScale, hv);
      D2D1_POINT_2F p{C.x + anchor.x, C.y + anchor.y};
      Color tc = Mix(pal.sectorText, pal.sectorTextHover, hv);
      auto b = brush(tc, alpha);
      if (it.icon.empty()) {
        float fs = std::min(R * ds::wheel::labelSize / 0.70f, box.y * 1.5f) * sc;
        auto* f = gpu_->Format(fs, DWRITE_FONT_WEIGHT_BOLD);
        if (!f) continue;
        std::wstring up = it.label; CharUpperBuffW(up.data(), (DWORD)up.size());
        ComPtr<IDWriteTextLayout> lay;
        Device::Get().dwrite->CreateTextLayout(up.c_str(), (UINT32)up.size(), f, 1000.f, 200.f, &lay);
        DWRITE_TEXT_METRICS tm{};
        float scaleFit = 1.f;
        if (lay) { lay->GetMetrics(&tm); if (tm.widthIncludingTrailingWhitespace > box.x) scaleFit = box.x / tm.widthIncludingTrailingWhitespace; }
        if (scaleFit < 1.f) f = gpu_->Format(std::max(fs * scaleFit, R * 0.05f), DWRITE_FONT_WEIGHT_BOLD);
        const float dy = -0.016f * R;      // centre on cap-height rather than the line box
        D2D1_RECT_F rc{p.x - box.x, p.y - box.y + dy, p.x + box.x, p.y + box.y + dy};
        dc->DrawText(up.c_str(), (UINT32)up.size(), f, rc, b.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
      } else {
        const float iconPx = ds::wheel::iconSize * R * 1.45f * sc;
        const float gap = R * 0.055f;
        gfx::IconCache::Get().Draw(dc, it.icon, p.x, p.y - gap - R * 0.012f, iconPx, b.Get(), 1.6f);
        float fs = R * 0.0625f * sc;
        auto* f = gpu_->Format(fs, DWRITE_FONT_WEIGHT_BOLD);
        std::wstring up = it.label; CharUpperBuffW(up.data(), (DWORD)up.size());
        ComPtr<IDWriteTextLayout> lay;
        Device::Get().dwrite->CreateTextLayout(up.c_str(), (UINT32)up.size(), f, 1000.f, 100.f, &lay);
        if (lay) {
          DWRITE_TEXT_RANGE all{0, (UINT32)up.size()};
          ComPtr<IDWriteTextLayout1> lay1; lay->QueryInterface(__uuidof(IDWriteTextLayout1), lay1.AsVoid());
          if (lay1) lay1->SetCharacterSpacing(0.f, fs * 0.05f, 0.f, all);
          DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
          if (tm.widthIncludingTrailingWhitespace > box.x) { lay->SetFontSize(fs * box.x / tm.widthIncludingTrailingWhitespace, all); lay->GetMetrics(&tm); }
          lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
          lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);      // the shared format centres vertically: that pushed the label ~half the layout height below its icon
          dc->DrawTextLayout(D2D1::Point2F(p.x - tm.widthIncludingTrailingWhitespace / 2, p.y + gap * 0.30f), lay.Get(), b.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
      }
    }
  };
  if (switchP_ < 1.f && !outgoing_.items.empty()) drawItems(outgoing_, 1.f - std::clamp(switchP_ * 2.f, 0.f, 1.f));
  drawItems(content_, 1.f);
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

LRESULT WheelWindow::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
      Theme::Get().RefreshFromSystem(); Invalidate(); break;
    case WM_NCHITTEST: SetHandled(true); return HTCLIENT;
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ClientToScreen(Hwnd(), &pt);
      Hit h = HitTestScreen(pt);
      SetHoverIndex(h.zone == Zone::Sector ? h.sector : -1);
      TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, Hwnd(), 0}; TrackMouseEvent(&te);
      SetHandled(true); return 0; }
    case WM_MOUSELEAVE: SetHoverIndex(-1); SetHandled(true); return 0;
    case WM_LBUTTONDOWN: {
      if (hover_ >= 0) SetPressed(hover_);
      SetCapture(Hwnd()); SetHandled(true); return 0; }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      int idx = pressed_; SetPressed(-1);
      if (idx >= 0 && idx == hover_ && onChoose) onChoose(idx);
      SetHandled(true); return 0; }
    case WM_KEYDOWN:
      switch (wp) {
        case VK_RIGHT: case VK_DOWN: MoveSelection(+1); SetHandled(true); return 0;
        case VK_LEFT: case VK_UP: MoveSelection(-1); SetHandled(true); return 0;
        case VK_RETURN: if (hover_ >= 0 && onChoose) onChoose(hover_); SetHandled(true); return 0;
        case VK_ESCAPE: Dismiss(true); SetHandled(true); return 0;
      }
      break;
  }
  SetHandled(false);
  return 0;
}

} // namespace filo::wheel
