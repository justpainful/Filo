#include "gfx/live_composition.h"
#include "core/log.h"
#include <dwmapi.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include "design/theme.h"
#include <d2d1_1.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <windows.ui.composition.desktop.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <algorithm>
#include <vector>

namespace filo::gfx {
namespace {
namespace comp = winrt::Windows::UI::Composition;
namespace effects = winrt::Windows::Graphics::Effects;
namespace foundation = winrt::Windows::Foundation;
using EffectInterop = ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop;
using Mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING;

// Describes built-in D2D effects to the Windows compositor. This is metadata,
// not a custom shader, injection, or a dependency on Win2D activation classes.
struct Effect : winrt::implements<Effect, effects::IGraphicsEffect, effects::IGraphicsEffectSource, EffectInterop> {
  GUID id{};
  winrt::hstring name;
  std::vector<foundation::IInspectable> properties;
  std::vector<effects::IGraphicsEffectSource> sources;
  winrt::hstring Name() const { return name; }
  void Name(winrt::hstring const& value) { name = value; }
  HRESULT __stdcall GetEffectId(GUID* value) noexcept override { if (!value) return E_POINTER; *value = id; return S_OK; }
  HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, Mapping*) noexcept override { return E_INVALIDARG; }
  HRESULT __stdcall GetPropertyCount(UINT* value) noexcept override { if (!value) return E_POINTER; *value = (UINT)properties.size(); return S_OK; }
  HRESULT __stdcall GetSourceCount(UINT* value) noexcept override { if (!value) return E_POINTER; *value = (UINT)sources.size(); return S_OK; }
  HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
    if (!value) return E_POINTER; *value = nullptr;
    if (index >= properties.size()) return E_INVALIDARG;
    auto copy = properties[index].as<foundation::IPropertyValue>();
    *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(winrt::detach_abi(copy)); return S_OK;
  }
  HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override {
    if (!value) return E_POINTER; *value = nullptr;
    if (index >= sources.size()) return E_INVALIDARG;
    auto copy = sources[index];
    *value = reinterpret_cast<ABI::Windows::Graphics::Effects::IGraphicsEffectSource*>(winrt::detach_abi(copy)); return S_OK;
  }
};

effects::IGraphicsEffectSource Node(GUID id, std::vector<foundation::IInspectable> properties,
                                   std::vector<effects::IGraphicsEffectSource> sources = {}) {
  auto effect = winrt::make_self<Effect>();
  effect->id = id; effect->properties = std::move(properties); effect->sources = std::move(sources);
  return effect.as<effects::IGraphicsEffectSource>();
}
auto Floats(std::initializer_list<float> values) { return foundation::PropertyValue::CreateSingleArray(winrt::array_view<const float>(values.begin(), values.end())); }
auto Solid(float r, float g, float b, float a = 1.f) { return Node(CLSID_D2D1Flood, {Floats({r, g, b, a})}); }

void EnsureQueue() {
  if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
  // The dispatcher is owned by the UI thread for its lifetime; never synchronously
  // wait for its shutdown from inside its own message dispatch.
  static thread_local winrt::Windows::System::DispatcherQueueController controller{nullptr};
  if (!controller) {
    DispatcherQueueOptions options{sizeof options, DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::check_hresult(CreateDispatcherQueueController(options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
  }
}
}

struct LiveComposition::Impl {
  comp::Compositor compositor{nullptr};
  comp::Desktop::DesktopWindowTarget target{nullptr};
  comp::ContainerVisual root{nullptr}, opticalRoot{nullptr};
  comp::SpriteVisual backdrop{nullptr}, content{nullptr}, bubble{nullptr};
  comp::CompositionEllipseGeometry bubbleGeometry{nullptr};
  float bubbleScale=-1;
  comp::CompositionRoundedRectangleGeometry geometry{nullptr};
  bool dark = false, configured = false;
  float intensity = -1;
  int width=0,height=0,maskWidth=0,maskHeight=0;
  bool liquid=false;
  int material=-1;
  float acrylicSigma=-1;
  float opticalScale=-1,opticalShift=-1;
  LiquidMasks masks;
  ~Impl() { try { if (target) { target.Root(nullptr); target.Close(); } } catch (...) {} }
};

LiveComposition::LiveComposition() = default;
LiveComposition::~LiveComposition() = default;

bool LiveComposition::Create(HWND window, IDXGISwapChain1* swap, int width, int height) {
  const BOOL enabled = TRUE;
  if (FAILED(DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &enabled, sizeof enabled))) return false;
  try {
    EnsureQueue();
    auto p = std::make_unique<Impl>();
    p->compositor = comp::Compositor();
    auto interop = p->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(window, TRUE, reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(p->target))));
    comp::ICompositionSurface surface{nullptr};
    winrt::check_hresult(p->compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateCompositionSurfaceForSwapChain(swap, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface))));
    p->root = p->compositor.CreateContainerVisual();
    p->backdrop = p->compositor.CreateSpriteVisual();
    p->backdrop.IsVisible(false);
    p->geometry = p->compositor.CreateRoundedRectangleGeometry();
    p->backdrop.Clip(p->compositor.CreateGeometricClip(p->geometry));
    p->content = p->compositor.CreateSpriteVisual();
    auto brush = p->compositor.CreateSurfaceBrush(surface);
    brush.Stretch(comp::CompositionStretch::None);
    p->content.Brush(brush);
    p->root.Children().InsertAtBottom(p->backdrop);
    p->opticalRoot=p->compositor.CreateContainerVisual();
    p->opticalRoot.IsVisible(false);
    p->root.Children().InsertAtTop(p->opticalRoot);
    p->bubble=p->compositor.CreateSpriteVisual();p->bubble.IsVisible(false);
    p->bubbleGeometry=p->compositor.CreateEllipseGeometry();
    p->bubble.Clip(p->compositor.CreateGeometricClip(p->bubbleGeometry));
    p->root.Children().InsertAtTop(p->bubble);
    p->root.Children().InsertAtTop(p->content);
    p->target.Root(p->root);
    impl_ = std::move(p);
    Resize(width, height);
    FILO_LOG(Info, "glass", "native host backdrop composition ready");
    return true;
  } catch (winrt::hresult_error const& e) {
    FILO_LOG(Warn, "glass", "host composition unavailable: 0x%08x", (unsigned)e.code().value);
    return false;
  }
}

bool LiveComposition::Configure(const RECT& r, float radius, bool dark, float intensity, const LiquidShape* requestedShape) {
  if (!impl_) return false;
  auto& p = *impl_;
  intensity = std::clamp(intensity, 0.f, 1.f);
  const char* stage = "setup";
  try {
    const auto& theme=ds::Theme::Get().State();
    if(theme.glassStyle==0 || theme.glassStyle==2) {
      const bool acrylic=theme.glassStyle==2;
      LiquidShape shape{};
      if(requestedShape) shape=*requestedShape;
      else {
        shape.cx=(r.left+r.right)*.5f;shape.cy=(r.top+r.bottom)*.5f;
        shape.halfW=(r.right-r.left)*.5f;shape.halfH=(r.bottom-r.top)*.5f;
        shape.radius=radius;shape.dpiScale=radius/26.f;
      }
      shape.dpiScale=std::max(.5f,shape.dpiScale);shape.refract=acrylic?0.f:theme.glassRefraction;
      if(acrylic)shape.refractionPx=0.f;
      stage="optical-masks";
      if(!p.masks.Update(p.width,p.height,shape)) {Disable();return false;}
      if(!p.liquid || p.material!=theme.glassStyle || (acrylic&&(p.dark!=dark||p.acrylicSigma!=theme.acrylicBlur)) || p.maskWidth!=p.width || p.maskHeight!=p.height || p.opticalScale!=shape.dpiScale || p.opticalShift!=shape.refractionPx) {
        p.opticalRoot.Children().RemoveAll();
        const float shift=shape.refractionPx*shape.dpiScale;
        const float dx[5]={0,-shift,shift,0,0},dy[5]={0,0,0,-shift,shift};
        auto interop=p.compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        auto live=Node(CLSID_D2D1GaussianBlur,{foundation::PropertyValue::CreateSingle((acrylic?theme.acrylicBlur:14.f)*shape.dpiScale),foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED),foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)},{comp::CompositionEffectSourceParameter(L"desktop")});
        if(acrylic) {
          // Windows Acrylic ingredients: live host blur, desaturation, exclusion,
          // with tint and stable grain in the foreground, clipped to Filo's geometry.
          live=Node(CLSID_D2D1Saturation,{foundation::PropertyValue::CreateSingle(.95f)},{live});
          live=Node(CLSID_D2D1Blend,{foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_EXCLUSION)},{live,Solid(.025f,.025f,.025f)});
          if(dark) {
            // Tone the live image itself, retaining its colour. A thick fixed tint
            // would hide the desktop and turn Acrylic into a nearly solid panel.
            live=Node(CLSID_D2D1ColorMatrix,{Floats({.24f,0,0,0, 0,.24f,0,0, 0,0,.24f,0, 0,0,0,1, .02f,.02f,.02f,0}),foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED),foundation::PropertyValue::CreateBoolean(true)},{live});
            live=Node(CLSID_D2D1Blend,{foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_DARKEN)},{live,Solid(.265f,.265f,.265f)});
          } else {
            auto luminanceSource=Node(CLSID_D2D1GaussianBlur,{foundation::PropertyValue::CreateSingle(theme.acrylicBlur*shape.dpiScale),foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED),foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)},{comp::CompositionEffectSourceParameter(L"desktop")});
            luminanceSource=Node(CLSID_D2D1Saturation,{foundation::PropertyValue::CreateSingle(.95f)},{luminanceSource});
            luminanceSource=Node(CLSID_D2D1Blend,{foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_EXCLUSION)},{luminanceSource,Solid(.025f,.025f,.025f)});
            auto luminosity=Node(CLSID_D2D1ColorMatrix,{Floats({.2126f,.2126f,.2126f,0, .7152f,.7152f,.7152f,0, .0722f,.0722f,.0722f,0, 0,0,0,1, 0,0,0,0}),foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED),foundation::PropertyValue::CreateBoolean(true)},{luminanceSource});
            auto floor=Node(CLSID_D2D1Blend,{foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_LIGHTEN)},{luminosity,Solid(.68f,.68f,.68f)});
            live=Node(CLSID_D2D1Blend,{foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_LUMINOSITY)},{floor,live});
          }
        }
        auto liveBrush=p.compositor.CreateEffectFactory(live.as<effects::IGraphicsEffect>()).CreateBrush();
        liveBrush.SetSourceParameter(L"desktop",p.compositor.CreateHostBackdropBrush());
        for(int k=0;k<(acrylic?1:5);++k) {
          auto child=p.compositor.CreateSpriteVisual();child.Size({(float)p.width,(float)p.height});child.Brush(liveBrush);
          auto layer=p.compositor.CreateLayerVisual();layer.Size({(float)p.width,(float)p.height});layer.Children().InsertAtTop(child);
          auto moved=Node(CLSID_D2D12DAffineTransform,{foundation::PropertyValue::CreateUInt32(D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR),foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD),Floats({1,0,0,1,-dx[k],-dy[k]}),foundation::PropertyValue::CreateSingle(0.f)},{comp::CompositionEffectSourceParameter(L"Backdrop")});
          auto masked=Node(CLSID_D2D1AlphaMask,{}, {moved,comp::CompositionEffectSourceParameter(L"mask")});
          stage="layer-refraction-effect";
          auto brush=p.compositor.CreateEffectFactory(masked.as<effects::IGraphicsEffect>()).CreateBrush();
          comp::ICompositionSurface surface{nullptr};
          winrt::check_hresult(interop->CreateCompositionSurfaceForSwapChain(p.masks.Swap(k),reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface))));
          auto mask=p.compositor.CreateSurfaceBrush(surface);mask.Stretch(comp::CompositionStretch::None);
          brush.SetSourceParameter(L"mask",mask);layer.Effect(brush);
          p.opticalRoot.Children().InsertAtTop(layer);
        }
        p.maskWidth=p.width;p.maskHeight=p.height;p.opticalScale=shape.dpiScale;p.opticalShift=shape.refractionPx;p.material=theme.glassStyle;p.dark=dark;p.acrylicSigma=theme.acrylicBlur;
      }
      p.backdrop.IsVisible(false);p.opticalRoot.IsVisible(true);p.opticalRoot.Opacity(1.f);
      p.liquid=true;p.configured=false;return true;
    }
    p.opticalRoot.IsVisible(false);
    if(p.liquid) {p.liquid=false;p.backdrop.Clip(p.compositor.CreateGeometricClip(p.geometry));}

    if (!p.configured || p.dark != dark || p.intensity != intensity) {
      comp::CompositionEffectSourceParameter source(L"desktop");
      auto blur = Node(CLSID_D2D1GaussianBlur, {foundation::PropertyValue::CreateSingle(18.f), foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED), foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)}, {source});
      const float amount = dark ? 0.28f : 1.f, lift = dark ? 0.108f : 0.20f;
      auto lifted = Node(CLSID_D2D1ColorMatrix, {Floats({amount,0,0,0, 0,amount,0,0, 0,0,amount,0, 0,0,0,1, lift,lift,lift,0}), foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED), foundation::PropertyValue::CreateBoolean(true)}, {blur});
      // Bound brightness on the GPU while retaining the live desktop colours.
      effects::IGraphicsEffectSource bounded{nullptr};
      if (dark) {
        bounded = Node(CLSID_D2D1Blend, {foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_DARKEN)}, {lifted, Solid(0.29f, 0.29f, 0.29f)});
      } else {
        // Replace only brightness below the reading floor, retaining chrominance.
        // Repeat the source branch: Composition rejects shared effect-node DAGs.
        auto maskBlur = Node(CLSID_D2D1GaussianBlur, {foundation::PropertyValue::CreateSingle(18.f), foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED), foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)}, {comp::CompositionEffectSourceParameter(L"desktop")});
        auto maskLift = Node(CLSID_D2D1ColorMatrix, {Floats({1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, .20f,.20f,.20f,0}), foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED), foundation::PropertyValue::CreateBoolean(true)}, {maskBlur});
        auto luminance = Node(CLSID_D2D1ColorMatrix, {Floats({.2126f,.2126f,.2126f,0, .7152f,.7152f,.7152f,0, .0722f,.0722f,.0722f,0, 0,0,0,1, 0,0,0,0}), foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED), foundation::PropertyValue::CreateBoolean(true)}, {maskLift});
        auto safeLuminance = Node(CLSID_D2D1Blend, {foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_LIGHTEN)}, {luminance, Solid(.72f,.72f,.72f)});
        bounded = Node(CLSID_D2D1Blend, {foundation::PropertyValue::CreateUInt32(D2D1_BLEND_MODE_LUMINOSITY)}, {safeLuminance, lifted});
      }
      auto solid = dark ? Solid(0.145f, 0.157f, 0.188f, 1.f - intensity) : Solid(0.949f, 0.957f, 0.973f, 1.f - intensity);
      auto graph = Node(CLSID_D2D1Composite, {foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER)}, {bounded, solid});
      stage = "mix"; auto brush = p.compositor.CreateEffectFactory(graph.as<effects::IGraphicsEffect>()).CreateBrush();
      stage = "host-source";
      brush.SetSourceParameter(L"desktop", p.compositor.CreateHostBackdropBrush());
      p.backdrop.Brush(brush);
      p.dark = dark; p.intensity = intensity; p.configured = true;
    }
    p.backdrop.Offset({(float)r.left, (float)r.top, 0});
    p.backdrop.Size({(float)(r.right - r.left), (float)(r.bottom - r.top)});
    p.geometry.Size(p.backdrop.Size());
    p.geometry.CornerRadius({radius, radius});
    p.backdrop.IsVisible(true);
    return true;
  } catch (winrt::hresult_error const& e) {
    FILO_LOG(Warn, "glass", "live effect unavailable (%s): 0x%08x", stage, (unsigned)e.code().value);
    Disable(); return false;
  }
}
bool LiveComposition::CloseBubble(const RECT& r,float scale,bool enabled) {
  if(!impl_)return false;auto& p=*impl_;
  if(!enabled) {p.bubble.IsVisible(false);return false;}
  try {
    if(p.bubbleScale!=scale) {
      auto blur=Node(CLSID_D2D1GaussianBlur,{foundation::PropertyValue::CreateSingle(8.f*scale),foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED),foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)},{comp::CompositionEffectSourceParameter(L"desktop")});
      auto brush=p.compositor.CreateEffectFactory(blur.as<effects::IGraphicsEffect>()).CreateBrush();
      brush.SetSourceParameter(L"desktop",p.compositor.CreateHostBackdropBrush());p.bubble.Brush(brush);p.bubbleScale=scale;
    }
    float w=(float)(r.right-r.left),h=(float)(r.bottom-r.top);
    p.bubble.Offset({(float)r.left,(float)r.top,0});p.bubble.Size({w,h});
    p.bubbleGeometry.Center({w/2,h/2});p.bubbleGeometry.Radius({w/2,h/2});p.bubble.IsVisible(true);return true;
  } catch(winrt::hresult_error const&) {p.bubble.IsVisible(false);return false;}
}
void LiveComposition::Resize(int w, int h) {
  if (!impl_) return;
  impl_->width=w;impl_->height=h;
  impl_->root.Size({(float)w, (float)h});
  impl_->content.Size({(float)w, (float)h});
}
void LiveComposition::Disable() { if (impl_) {impl_->backdrop.IsVisible(false);impl_->opticalRoot.IsVisible(false);} }
void LiveComposition::SetOpacity(float value) { if (impl_) {impl_->backdrop.Opacity(std::clamp(value,0.f,1.f));impl_->opticalRoot.Opacity(std::clamp(value,0.f,1.f));} }
unsigned LiveComposition::MaskUpdates() const {return impl_?impl_->masks.Updates():0;}

}
