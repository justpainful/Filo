// Render the actual Direct2D plugin pipeline, including caption sizes and DPI scaling.
#include "gfx/icons.h"
#include "gfx/icon_packs.h"
#include "gfx/imageio.h"
#include "ui/painter.h"
#include "design/theme.h"
#include "core/com.h"
#include <fstream>
#include <cstdio>
#include <cstring>
using namespace filo;
class Canvas {
 public:
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<ID2D1Bitmap1> target;
  int w,h;float dpi;
  Canvas(int width,int height,float resolution):w(width),h(height),dpi(resolution) {
    auto& dev=gfx::Device::Get();dev.Ensure();
    dev.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&dc);
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
    dc->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,&props,&target);dc->SetTarget(target.Get());dc->SetDpi(dpi,dpi);
  }
  std::vector<uint8_t> Capture() {
    auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
    ComPtr<ID2D1Bitmap1> copy;
    if(FAILED(dc->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,&props,&copy))||FAILED(copy->CopyFromBitmap(nullptr,target.Get(),nullptr)))return {};
    D2D1_MAPPED_RECT map{};if(FAILED(copy->Map(D2D1_MAP_OPTIONS_READ,&map)))return {};
    std::vector<uint8_t> pixels(w*h*4);
    for(int y=0;y<h;++y)memcpy(pixels.data()+y*w*4,map.bits+y*map.pitch,w*4);
    copy->Unmap();return pixels;
  }
};
static bool Gallery(const fs::path& out,int dpi,bool dark,bool atlas) {
  const float scale=dpi/96.f;
  const auto all=gfx::IconCache::Names();
  const std::vector<std::string> sample={"compress","metadata","edit","crop","trim","join","share","float","copy","move","terminal","folder"};
  const auto& names=atlas?all:sample;
  const int rows=(int)((names.size()+11)/12),height=88+4*(68+rows*64);
  Canvas c((int)(1120*scale),(int)(height*scale),(float)dpi);
  c.dc->BeginDraw();c.dc->Clear(dark?D2D1::ColorF(.055f,.065f,.085f):D2D1::ColorF(.965f,.97f,.98f));
  ui::Painter p;p.Begin(c.dc.Get(),(float)dpi,false);
  const ds::Color ink=dark?ds::Color::Hex(0xF2F3F8):ds::Color::Hex(0x20283C);
  ui::TextStyle heading;heading.size=24;heading.weight=600;
  p.Text({30,14,1060,36},L"Filo / Four icon languages",ink,heading);
  ui::TextStyle caption;caption.size=11;
  p.Text({30,50,1060,24},L"Classic preserved  ·  Pebble / soft mass  ·  Circuit / signal flow  ·  Facet / folded planes",ink,caption);
  size_t i=0;
  for(const char* id:{"classic","pebble","circuit","facet"}) {
    const auto& pack=gfx::IconPacks::Get().Resolve(id);
    float top=88.f+i++*(68+rows*64);
    p.FillRoundRect({18,top,1084.f,(float)(60+rows*64)},16,dark?ds::Color(1,1,1,.055f):ds::Color(1,1,1,.85f));
    ui::TextStyle title;title.size=17;title.weight=600;p.Text({34,top+6,1052,25},pack.name,ink,title);
    p.Text({34,top+30,1052,20},pack.description,ink.WithAlpha(.72f),caption);
    for(size_t n=0;n<names.size();++n) {
      float x=60.f+(n%12)*90, y=top+76+(n/12)*64;
      if(!gfx::IconCache::Get().DrawPack(c.dc.Get(),id,names[n],x,y,28,p.Brush(ink).Get()))return false;
      caption.align=ui::Align::Center;
      p.Text({x-42,y+20,84,18},Widen(names[n]),ink.WithAlpha(.8f),caption);
    }
    caption.align=ui::Align::Start;
  }
  p.End();if(FAILED(c.dc->EndDraw()))return false;
  auto pixels=c.Capture();return !pixels.empty()&&gfx::SavePng(out,pixels.data(),c.w,c.h,true);
}
static std::vector<uint8_t> Glyph(std::string_view pack,std::string_view name,int dpi,int size,bool active=false) {
  const float scale=dpi/96.f;Canvas c((int)(48*scale),(int)(48*scale),(float)dpi);
  c.dc->BeginDraw();c.dc->Clear(D2D1::ColorF(0,0));
  ComPtr<ID2D1SolidColorBrush> brush;c.dc->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&brush);brush->SetOpacity(.63f);
  bool ok=active?gfx::IconCache::Get().Draw(c.dc.Get(),name,24,24,(float)size,brush.Get()):gfx::IconCache::Get().DrawPack(c.dc.Get(),pack,name,24,24,(float)size,brush.Get());
  if(brush->GetOpacity()!=.63f)ok=false;
  if(FAILED(c.dc->EndDraw())||!ok)return {};return c.Capture();
}
int wmain(int argc,wchar_t** argv) {
  ComInit com;gfx::Device::Get().Ensure();
  fs::path out=argc>1?argv[1]:L"out/icon-packs";fs::create_directories(out);
  Json report=Json::MakeObject();int failures=0,rendered=0;
  const auto names=gfx::IconCache::Names();
  for(const char* id:{"classic","pebble","circuit","facet"}) {
    if(gfx::IconPacks::Get().Resolve(id).id!=id)++failures;
    for(int dpi:{96,120,144})for(int size:{16,24,32})for(const auto& n:names) {
      auto pixels=Glyph(id,n,dpi,size);bool ink=false;
      for(size_t k=3;k<pixels.size();k+=4)ink|=pixels[k]>0;
      if(!ink) {std::printf("Empty icon %s/%s/%d/%d\n",id,n.c_str(),dpi,size);++failures;}
      ++rendered;
    }
  }
  // Cache isolation, runtime theme changes, missing-pack inheritance, device cache recreation.
  auto classic=Glyph("classic","copy",96,24);auto a=Glyph("pebble","copy",96,24);auto b=Glyph("circuit","copy",96,24);auto a2=Glyph("pebble","copy",96,24);
  bool isolated=a==a2&&a!=b&&a!=classic;
  bool fallback=Glyph("removed-pack","copy",96,24)==classic;
  auto theme=ds::Theme::Get().State();theme.iconPack="facet";ds::Theme::Get().SetUserState(theme);
  bool follows=Glyph("","copy",96,24,true)==Glyph("facet","copy",96,24);
  gfx::IconCache::Get().Clear();bool restored=Glyph("classic","copy",96,24)==classic&&Glyph("pebble","copy",96,24)==a;
  bool partial=false;
  const auto probe=ExeDir()/L"icon-packs"/(L"99-qa-partial-"+std::to_wstring(GetCurrentProcessId())+L".json");
  if(!fs::exists(probe)) {
    {std::ofstream file(probe);file<<R"({"schema":1,"id":"qa-partial","name":"QA partial","icons":{"image":[{"path":"M3 3h18v18H3z","fill":true}]}})";}
    gfx::IconPacks::Get().Reload();
    partial=gfx::IconPacks::Get().Resolve("qa-partial").id=="qa-partial" &&
      Glyph("qa-partial","copy",96,24)==classic && Glyph("qa-partial","image",96,24)!=Glyph("classic","image",96,24);
    fs::remove(probe);gfx::IconPacks::Get().Reload();
    partial&=gfx::IconPacks::Get().Resolve("qa-partial").id=="classic";
  }
  report["partial_plugin_discovery_and_removal"]=partial;if(!partial)++failures;
  report["cache_isolated"]=isolated;report["fallback_classic"]=fallback;report["theme_switch"]=follows;report["cache_rebuild"]=restored;
  if(!isolated||!fallback||!follows||!restored)++failures;
  for(int dpi:{96,120,144})for(bool dark:{false,true}) {
    auto file=out/(std::wstring(dark?L"comparison-dark-":L"comparison-light-")+std::to_wstring(dpi)+L".png");
    if(!Gallery(file,dpi,dark,false))++failures;
  }
  for(bool dark:{false,true})if(!Gallery(out/(dark?L"atlas-dark.png":L"atlas-light.png"),96,dark,true))++failures;
  report["glyph_renders"]=rendered;report["failures"]=failures;report["passed"]=failures==0;
  std::ofstream(out/L"render-verification.json")<<report.Dump(true);
  std::printf("%d glyph renders, %d failures\n",rendered,failures);gfx::Shutdown();return failures?1:0;
}

