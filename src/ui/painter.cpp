#include "ui/painter.h"
#include "gfx/icons.h"
#include "design/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace filo::ui {

using namespace filo::ds;

void Painter::Begin(ID2D1DeviceContext* dc, float dpi, bool rtl) {
  if (dc != dc_) { brush_.Reset(); }
  dc_ = dc; dpi_ = dpi; rtl_ = rtl;
  dc_->SetDpi(dpi, dpi);
  dc_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
  dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType is invalid on premultiplied-alpha targets
  dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  if (!brush_) dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
  clips_.clear(); xforms_.clear();
}
void Painter::End() { while (!clips_.empty()) PopClip(); while (!xforms_.empty()) PopTransform(); dc_ = nullptr; }
void Painter::ClearRoundRect(const Rect& r, float radius) {
  const auto previous = dc_->GetPrimitiveBlend();
  dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
  dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), radius, radius), Brush(Color(0, 0, 0, 0)).Get());
  dc_->SetPrimitiveBlend(previous);
}
void Painter::PushRotate(float cx, float cy, float degrees) {
  D2D1_MATRIX_3X2_F old; dc_->GetTransform(&old); xforms_.push_back(old);
  dc_->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, D2D1::Point2F(cx, cy)) * old);
}
void Painter::PopTransform() { if (xforms_.empty()) return; dc_->SetTransform(xforms_.back()); xforms_.pop_back(); }
void Painter::ResetCaches() {
  roundStroke_.Reset(); brush_.Reset(); acrylicGrain_.Reset(); formats_.clear(); gfx::IconCache::Get().Clear(); }

ComPtr<ID2D1SolidColorBrush> Painter::Brush(const Color& c) {
  brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
  return brush_;
}

void Painter::FillRect(const Rect& r, const Color& c) { if (c.a > 0) dc_->FillRectangle(r.D2D(), Brush(c).Get()); }
void Painter::FillRoundRect(const Rect& r, float rad, const Color& c) {
  if (c.a <= 0) return;
  rad = std::min(rad, std::min(r.w, r.h) * 0.5f);
  dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), rad, rad), Brush(c).Get());
}
void Painter::StrokeRoundRect(const Rect& r, float rad, const Color& c, float width) {
  if (c.a <= 0) return;
  Rect in = r.Inset(width * 0.5f);
  rad = std::min(std::max(0.f, rad - width * 0.5f), std::min(in.w, in.h) * 0.5f);
  dc_->DrawRoundedRectangle(D2D1::RoundedRect(in.D2D(), rad, rad), Brush(c).Get(), width);
}
void Painter::FillRoundRectGradient(const Rect& r, float rad, const Color& top, const Color& bottom) {
  D2D1_GRADIENT_STOP stops[2] = {{0.f, D2D1::ColorF(top.r, top.g, top.b, top.a)}, {1.f, D2D1::ColorF(bottom.r, bottom.g, bottom.b, bottom.a)}};
  ComPtr<ID2D1GradientStopCollection> gc;
  if (FAILED(dc_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gc))) return;
  ComPtr<ID2D1LinearGradientBrush> b;
  if (FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.bottom())), gc.Get(), &b))) return;
  rad = std::min(rad, std::min(r.w, r.h) * 0.5f);
  dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), rad, rad), b.Get());
}
void Painter::FillCircle(float cx, float cy, float r, const Color& c) { if (c.a > 0) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(c).Get()); }
void Painter::GlassClose(const Rect& r,bool dark,float hover,float pressed,bool enabled,bool live) {
  const float radius=r.w*.5f;
  Shadow(r,radius,4,Color(0,0,0,enabled?.09f:.04f),1.5f);
  if(live)ClearRoundRect(r,radius);
  float alpha=live?(dark?.45f:.35f):1.f;
  alpha=std::min(1.f,alpha+.04f*hover+.07f*pressed+(enabled?0.f:.15f));
  auto tint=dark?Color::Hex(0x1A222C):Color::Hex(0xF7FAFD);
  FillRoundRectGradient(r,radius,tint.WithAlpha(alpha),tint.WithAlpha(std::min(1.f,alpha+.06f)));
  if(live) {
    D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(tint.r,tint.g,tint.b,.45f)},{.65f,D2D1::ColorF(tint.r,tint.g,tint.b,.32f)},{1,D2D1::ColorF(tint.r,tint.g,tint.b,0)}};
    ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1RadialGradientBrush> centre;
    if(SUCCEEDED(dc_->CreateGradientStopCollection(stops,3,&collection))&&SUCCEEDED(dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({r.cx()-radius*.08f,r.cy()-radius*.12f},{0,0},radius*.95f,radius*.95f),collection.Get(),&centre)))
      dc_->FillEllipse(D2D1::Ellipse({r.cx(),r.cy()},radius,radius),centre.Get());
  }
  float rim=(enabled?(.45f+.12f*hover):.09f)*(1-.55f*pressed);
  FillRoundRectGradient({r.x+radius*.28f,r.y+radius*.12f,r.w-radius*.56f,r.h*.42f},radius*.65f,Color(1,1,1,.10f*(1-pressed)),Color(1,1,1,0));
  StrokeRoundRect(r.Inset(.5f),radius-.5f,Color(0,0,0,dark?.22f:.10f),.65f);
  // Directional reflection: luminous above, quiet on the opposite edge.
  D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(1,1,1,rim)},{.55f,D2D1::ColorF(1,1,1,rim*.32f)},{1,D2D1::ColorF(1,1,1,.025f)}};
  ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> edge;
  if(SUCCEEDED(dc_->CreateGradientStopCollection(stops,3,&collection))&&SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.x,r.y},{r.right(),r.bottom()}),collection.Get(),&edge)))
    dc_->DrawEllipse(D2D1::Ellipse({r.cx(),r.cy()},radius-.75f,radius-.75f),edge.Get(),.8f);
  const Color ink=dark?Color(1,1,1,enabled?.94f:.38f):Color::Hex(0x2F3A45,enabled?.95f:.36f);
  float half=r.w*.17f;
  Line(r.cx()-half,r.cy()-half,r.cx()+half,r.cy()+half,ink,1.25f,true);
  Line(r.cx()+half,r.cy()-half,r.cx()-half,r.cy()+half,ink,1.25f,true);
}
void Painter::AcrylicBody(const Rect& r,float radius,bool dark,float intensity) {
  const Color tint=dark?Color::Hex(0x151922):Color::Hex(0xF7F8FA);
  FillRoundRect(r,radius,tint.WithAlpha(1-intensity*Theme::Get().State().acrylicTransparency));
  if(intensity<=0)return;
  if(!acrylicGrain_) {
    // A tiny deterministic premultiplied tile. No capture and no animated noise.
    uint32_t pixels[64*64];uint32_t seed=0x73e29u;
    for(auto& pixel:pixels) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;pixel=(seed&1)?0x02020202u:0x02000000u;}
    ComPtr<ID2D1Bitmap> tile;
    auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    if(SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(64,64),pixels,64*4,&props,&tile))) {
      auto repeat=D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,D2D1_EXTEND_MODE_WRAP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
      dc_->CreateBitmapBrush(tile.Get(),repeat,&acrylicGrain_);
    }
  }
  if(acrylicGrain_) {acrylicGrain_->SetOpacity(intensity);dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(),radius,radius),acrylicGrain_.Get());}
}
void Painter::LiquidBody(const Rect& r, float radius, bool dark, float intensity) {
  const Color tint=dark?Color::Hex(0x171B22):Color(1,1,1);
  const float base=1-intensity*(dark?.50f:.66f);
  const float centre=1-intensity*(dark?.17f:.24f);
  FillRoundRect(r,radius,tint.WithAlpha(base));
  const float a=std::clamp((centre-base)/std::max(1-base,.001f),0.f,1.f);
  FillRoundRectGradient(r.Inset(2.8f),std::max(0.f,radius-2.8f),tint.WithAlpha(std::min(1.f,a+.025f)),tint.WithAlpha(a));
}
void Painter::LiquidRim(const Rect& r, float radius, bool dark, float strength) {
  const float k=std::clamp(strength,0.f,1.f);
  StrokeRoundRect(r,radius,Color(0,0,0,dark?.30f:.15f),.9f);
  auto stroke=[&](float inset,float width,const D2D1_GRADIENT_STOP* stops,UINT count) {
    ComPtr<ID2D1GradientStopCollection> gc;ComPtr<ID2D1LinearGradientBrush> brush;
    if(FAILED(dc_->CreateGradientStopCollection(stops,count,D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,&gc)))return;
    if(FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x,r.y),D2D1::Point2F(r.right()*.7f+r.x*.3f,r.bottom())),gc.Get(),&brush)))return;
    const auto box=r.Inset(inset);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(box.D2D(),std::max(0.f,radius-inset),std::max(0.f,radius-inset)),brush.Get(),width);
  };
  D2D1_GRADIENT_STOP shoulder[]={
    {0,D2D1::ColorF(1,1,1,.22f*k)}, {.30f,D2D1::ColorF(1,1,1,.045f*k)},
    {.58f,D2D1::ColorF(0,0,0,.14f*k)}, {.82f,D2D1::ColorF(1,1,1,.035f*k)}, {1,D2D1::ColorF(1,1,1,.15f*k)}};
  stroke(2.3f,3.4f,shoulder,5);
  D2D1_GRADIENT_STOP lip[]={
    {0,D2D1::ColorF(1,1,1,.85f*k)}, {.27f,D2D1::ColorF(1,1,1,.42f*k)},
    {.52f,D2D1::ColorF(1,1,1,.055f*k)}, {.72f,D2D1::ColorF(1,1,1,.10f*k)}, {1,D2D1::ColorF(1,1,1,.52f*k)}};
  stroke(.85f,1.2f,lip,5);
}
// ---- Glassy UI material -------------------------------------------------------------------------------------------------
// Values are the ones in the glassyui library (styles/liquid.css) for a dialog slab (GlassPanel preset "prominent", raised, lens):
//   --lg-slab-tint        dark rgb(28 28 30 / .30)   light rgb(255 255 255 / .80)
//   --lg-sheen-alpha      dark .05                    light .08          (linear-gradient 180deg white -> transparent 45%)
//   --lg-shadow-raised    dark 0 2px 6px .20, 0 30px 60px -18px .55    light 0 2px 6px .06, 0 28px 56px -18px .26
//   rim:   --lg-glow-alpha .06/.14, --lg-reflex-light .42/1, --lg-reflex-dark 1.4/.8, --lg-rim-ca-alpha .06/.05 (blue 80 160 255 / pink 255 90 150, 2px)
//   ring:  --lg-specular-peak .45/.70, --lg-specular-floor .07/.35, --lg-specular-counter .22/.55, --lg-rim-width 1px
// The 30 dip window margin cannot hold the 60px blur of the second shadow, so it is shortened to fit; nothing else is changed.
void Painter::GlassyShadow(const Rect& r, float radius, bool dark) {
  Shadow(r, radius, 6.f, Color(0, 0, 0, dark ? .20f : .06f), 2.f);
  Shadow(r.Inset(10.f), std::max(0.f, radius - 10.f), 30.f, Color(0, 0, 0, dark ? .55f : .26f), 11.f);
}
void Painter::GlassyBody(const Rect& r, float radius, bool dark) {
  const Color tint = dark ? Color(28 / 255.f, 28 / 255.f, 30 / 255.f, .30f) : Color(1, 1, 1, .80f);
  FillRoundRect(r, radius, tint);
  const float sheen = dark ? .05f : .08f;
  D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(1, 1, 1, sheen)}, {.45f, D2D1::ColorF(1, 1, 1, 0)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
  ComPtr<ID2D1GradientStopCollection> gc; ComPtr<ID2D1LinearGradientBrush> b;
  if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gc)) &&
      SUCCEEDED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.bottom())), gc.Get(), &b)))
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), radius, radius), b.Get());
}
void Painter::GlassyRim(const Rect& r, float radius, bool dark) {
  const float glow = dark ? .06f : .14f, rimLight = dark ? .42f : 1.f, rimDark = dark ? 1.4f : .8f, ca = dark ? .06f : .05f;
  const float rimAlpha = dark ? .05f : .109f;                              // .05 + light-strength*.12 + fresnel*.35 + specular*.32
  auto stroke = [&](float inset, float width, const D2D1_GRADIENT_STOP* stops, UINT count, D2D1_POINT_2F p0, D2D1_POINT_2F p1) {
    ComPtr<ID2D1GradientStopCollection> gc; ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(dc_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gc))) return;
    if (FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), gc.Get(), &brush))) return;
    const Rect box = r.Inset(inset);
    const float rr = std::max(0.f, radius - inset);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(box.D2D(), rr, rr), brush.Get(), width);
  };
  const D2D1_POINT_2F tl{r.x, r.y}, br{r.right(), r.bottom()};
  // inset 0 0 12px -4px white / glow: a soft glow hugging the inside of the edge
  for (int i = 0; i < 6; ++i) {
    const float t = i / 6.f;
    StrokeRoundRect(r.Inset(1.f + i * 1.5f), std::max(0.f, radius - 1.f - i * 1.5f), Color(1, 1, 1, glow * (1 - t) * (1 - t) * .8f), 1.6f);
  }
  // inset -0.3px -1px 4px / inset -1.5px 2.5px 0 -2px / inset 0 3px 4px -2px : the shaded inner edge (black * rim-dark)
  { D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(0, 0, 0, rimDark * .14f)}, {.30f, D2D1::ColorF(0, 0, 0, 0)}, {.62f, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, rimDark * .20f)}};
    stroke(2.2f, 2.4f, s, 4, tl, br); }
  // inset 1.5px 2.5px 1px -2px white * rim-light * .7 (top and left) and inset -1.5px -2px 1px -2px white * rim-light * .55 (bottom and right)
  { D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(1, 1, 1, std::min(1.f, rimLight * .70f))}, {.45f, D2D1::ColorF(1, 1, 1, 0)}, {.55f, D2D1::ColorF(1, 1, 1, 0)}, {1, D2D1::ColorF(1, 1, 1, std::min(1.f, rimLight * .55f))}};
    stroke(1.3f, 1.4f, s, 4, tl, br); }
  // inset 0 0 3px white / --lg-rim-alpha
  StrokeRoundRect(r.Inset(1.f), std::max(0.f, radius - 1.f), Color(1, 1, 1, rimAlpha), 2.f);
  // chromatic aberration at the vertical edges: blue on the left, pink on the right (2px, 3px blur)
  { D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(80 / 255.f, 160 / 255.f, 1, ca)}, {1, D2D1::ColorF(80 / 255.f, 160 / 255.f, 1, 0)}};
    stroke(2.f, 2.4f, s, 2, D2D1::Point2F(r.x, r.cy()), D2D1::Point2F(r.x + 9.f, r.cy())); }
  { D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(1, 90 / 255.f, 150 / 255.f, ca)}, {1, D2D1::ColorF(1, 90 / 255.f, 150 / 255.f, 0)}};
    stroke(2.f, 2.4f, s, 2, D2D1::Point2F(r.right(), r.cy()), D2D1::Point2F(r.right() - 9.f, r.cy())); }
  // specular ring (::after, 1px): linear-gradient(to bottom right, peak 0%, floor 32%, floor 68%, counter 100%)
  { const float peak = dark ? .45f : .70f, floor = dark ? .07f : .35f, counter = dark ? .22f : .55f;
    D2D1_GRADIENT_STOP s[] = {{0, D2D1::ColorF(1, 1, 1, peak)}, {.32f, D2D1::ColorF(1, 1, 1, floor)}, {.68f, D2D1::ColorF(1, 1, 1, floor)}, {1, D2D1::ColorF(1, 1, 1, counter)}};
    stroke(.5f, 1.f, s, 4, tl, br); }
  // hairline dark outline so the slab separates from bright backdrops
  StrokeRoundRect(r.Inset(-.5f), radius + .5f, Color(0, 0, 0, dark ? .30f : .12f), .8f);
}
void Painter::StrokeCircle(float cx, float cy, float r, const Color& c, float w) { if (c.a > 0) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - w * 0.5f, r - w * 0.5f), Brush(c).Get(), w); }
void Painter::Line(float x0, float y0, float x1, float y1, const Color& c, float w, bool round) {
  if (c.a <= 0) return;
  ID2D1StrokeStyle* ss = nullptr;
  if (round) {
    if (!roundStroke_) {
      ComPtr<ID2D1Factory> f; dc_->GetFactory(&f);
      ID2D1Factory1* f1 = nullptr; f->QueryInterface(__uuidof(ID2D1Factory1), (void**)&f1);
      if (f1) { f1->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &roundStroke_); f1->Release(); }
    }
    ss = roundStroke_.Get();
  }
  dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush(c).Get(), w, ss);
}
void Painter::Polyline(const std::vector<std::pair<float, float>>& pts, const Color& c, float w) {
  if (c.a <= 0 || pts.empty()) return;
  if (pts.size() == 1) { FillCircle(pts[0].first, pts[0].second, w * 0.5f, c); return; }
  ComPtr<ID2D1Factory> f; dc_->GetFactory(&f);
  ComPtr<ID2D1PathGeometry> g; if (FAILED(f->CreatePathGeometry(&g))) return;
  ComPtr<ID2D1GeometrySink> s; if (FAILED(g->Open(&s))) return;
  s->BeginFigure(D2D1::Point2F(pts[0].first, pts[0].second), D2D1_FIGURE_BEGIN_HOLLOW);
  for (size_t i = 1; i < pts.size(); ++i) s->AddLine(D2D1::Point2F(pts[i].first, pts[i].second));
  s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close();
  if (!roundStroke_) { ID2D1Factory1* f1 = nullptr; f->QueryInterface(__uuidof(ID2D1Factory1), (void**)&f1); if (f1) { f1->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &roundStroke_); f1->Release(); } }
  dc_->DrawGeometry(g.Get(), Brush(c).Get(), w, roundStroke_.Get());
}
void Painter::StrokeEllipse(float cx, float cy, float rx, float ry, const Color& c, float w) {
  if (c.a > 0) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(0.5f, rx), std::max(0.5f, ry)), Brush(c).Get(), w);
}

void Painter::Shadow(const Rect& r, float radius, float blur, const Color& c, float dy) {
  if (c.a <= 0 || blur <= 0) return;
  const int N = 14;
  for (int i = N - 1; i >= 0; --i) {
    float t = (float)(i + 1) / N;                 // 1 = outermost
    float grow = blur * t;
    float a = c.a * (1.f - t) * (1.f - t) * 2.2f / N;
    Rect rr{r.x - grow, r.y - grow + dy, r.w + 2 * grow, r.h + 2 * grow};
    FillRoundRect(rr, radius + grow, Color(c.r, c.g, c.b, a));
  }
}

void Painter::PushClip(const Rect& r) {
  dc_->PushAxisAlignedClip(r.D2D(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  clips_.push_back({false});
}
void Painter::PushClipRoundRect(const Rect& r, float radius) {
  ComPtr<ID2D1RoundedRectangleGeometry> g;
  gfx::Device::Get().d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.D2D(), radius, radius), &g);
  dc_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
  clips_.push_back({true});
}
void Painter::PopClip() {
  if (clips_.empty()) return;
  if (clips_.back().layer) dc_->PopLayer(); else dc_->PopAxisAlignedClip();
  clips_.pop_back();
}

// ------------------------------------------------------------------------------------------------ text
IDWriteTextFormat* Painter::Format(const TextStyle& st) {
  char key[96];
  snprintf(key, sizeof key, "%.2f/%d/%d/%d/%d/%d", st.size, st.weight, (int)st.align, st.mono, st.wrap, rtl_);
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.Get();
  auto& dev = gfx::Device::Get();
  ComPtr<IDWriteTextFormat> f;
  const wchar_t* fams[] = {st.mono ? type::mono : type::family, type::familyFallback};
  for (auto fam : fams)
    if (SUCCEEDED(dev.dwrite->CreateTextFormat(fam, nullptr, (DWRITE_FONT_WEIGHT)st.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, st.size, L"en-us", &f))) break;
  if (!f) return nullptr;
  f->SetTextAlignment(st.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : (st.align == Align::End ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING));
  f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  f->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  f->SetReadingDirection(rtl_ ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
  return formats_.emplace(key, f).first->second.Get();
}

ComPtr<IDWriteTextLayout> Painter::Layout(const std::wstring& s, const TextStyle& st, float w, float h) {
  ComPtr<IDWriteTextLayout> l;
  IDWriteTextFormat* f = Format(st);
  if (!f) return l;
  if (FAILED(gfx::Device::Get().dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(w, 1.f), std::max(h, 1.f), &l))) return {};
  if (st.tracking != 0) {
    ComPtr<IDWriteTextLayout1> l1;
    if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), l1.AsVoid()))) l1->SetCharacterSpacing(0, st.tracking, 0, DWRITE_TEXT_RANGE{0, (UINT32)s.size()});
  }
  if (st.ellipsis && !st.wrap) {
    ComPtr<IDWriteInlineObject> sign;
    gfx::Device::Get().dwrite->CreateEllipsisTrimmingSign(l.Get(), &sign);
    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    l->SetTrimming(&tr, sign.Get());
  }
  return l;
}

void Painter::Text(const Rect& r, const std::wstring& s, const Color& c, const TextStyle& st) {
  if (s.empty() || c.a <= 0 || r.w <= 0 || r.h <= 0) return;
  auto l = Layout(s, st, r.w, r.h);
  if (!l) return;
  if (!st.vcenter) l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), l.Get(), Brush(c).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

Painter::Metrics Painter::Measure(const std::wstring& s, const TextStyle& st, float maxW) {
  TextStyle ns = st; ns.ellipsis = false;
  auto l = Layout(s.empty() ? L" " : s, ns, maxW > 0 ? maxW : 10000.f, 10000.f);
  if (!l) return {0, 0, 0};
  DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
  return {tm.widthIncludingTrailingWhitespace, tm.height, (int)tm.lineCount};
}
float Painter::TextWidth(const std::wstring& s, const TextStyle& st) { return Measure(s, st, 0).w; }

float Painter::CaretX(const std::wstring& s, const TextStyle& st, int pos) {
  TextStyle ns = st; ns.ellipsis = false;
  auto l = Layout(s.empty() ? L" " : s, ns, 10000.f, 100.f);
  if (!l) return 0;
  float x = 0, y = 0; DWRITE_HIT_TEST_METRICS hm{};
  l->HitTestTextPosition((UINT32)std::clamp<int>(pos, 0, (int)s.size()), FALSE, &x, &y, &hm);
  return x;
}
int Painter::HitIndex(const std::wstring& s, const TextStyle& st, float x) {
  TextStyle ns = st; ns.ellipsis = false;
  auto l = Layout(s.empty() ? L" " : s, ns, 10000.f, 100.f);
  if (!l) return 0;
  BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS hm{};
  l->HitTestPoint(x, 10.f, &trailing, &inside, &hm);
  int idx = (int)hm.textPosition + (trailing ? 1 : 0);
  return std::clamp(idx, 0, (int)s.size());
}

void Painter::Icon(const std::string& name, float cx, float cy, float size, const Color& c, float stroke) {
  if (c.a <= 0) return;
  gfx::IconCache::Get().Draw(dc_, name, cx, cy, size, Brush(c).Get(), stroke);
}

void Painter::Bitmap(ID2D1Bitmap* bmp, const Rect& dst, float opacity, float cornerRadius, bool linear) {
  if (!bmp) return;
  if (cornerRadius <= 0) {
    dc_->DrawBitmap(bmp, dst.D2D(), opacity, linear ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return;
  }
  D2D1_SIZE_F sz = bmp->GetSize();
  ComPtr<ID2D1BitmapBrush> b;
  if (FAILED(dc_->CreateBitmapBrush(bmp, &b))) return;
  b->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP); b->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
  b->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  b->SetTransform(D2D1::Matrix3x2F::Scale(dst.w / sz.width, dst.h / sz.height) * D2D1::Matrix3x2F::Translation(dst.x, dst.y));
  b->SetOpacity(opacity);
  dc_->FillRoundedRectangle(D2D1::RoundedRect(dst.D2D(), cornerRadius, cornerRadius), b.Get());
}

} // namespace filo::ui
