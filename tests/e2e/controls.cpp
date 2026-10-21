// Owned-window regression tests. No Explorer hooks or global runtime instance.
#include "tools/tools.h"
#include "ui/settings_window.h"
#include "gfx/imageio.h"
#include "core/com.h"
#include "core/log.h"
#include "core/settings.h"
#include "design/theme.h"
#include <fstream>
#include <cstdio>
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
using namespace filo;
static int failed=0,checks=0;
static void Check(bool ok,const char* name) {++checks;if(!ok){++failed;std::printf("FAIL: %s\n",name);}}
static void Pump(int ms=60) {
  auto end=GetTickCount64()+ms;MSG m;
  do {while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(3);}while(GetTickCount64()<end);
}
static void Click(ui::DialogWindow* d,int x,int y) {
  HWND h=d->Hwnd();
  SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));Check(d->RenderNow(false),"render on press");
  SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(x,y));Check(d->RenderNow(false),"render on release");Pump();
}
class BubbleProbe:public ui::DialogWindow {
 public:
  bool disabled=false;
  bool CanClose() const override {return !disabled;}
  void Draw(ui::Ui& ui,const ui::Rect& c,const ui::Rect&) override {
    ui.Label({c.x+24,c.y+20,c.w-48,30},L"Live glass close button");
    ui.LabelSecondary({c.x+24,c.y+56,c.w-48,40},L"Transparency and blur are independent. Text stays sharp.");
    ui.P().GlassClose({c.x+24,c.y+115,28,28},ds::Theme::Get().IsDark(),0,0,false,false);
    ui.LabelSecondary({c.x+64,c.y+115,180,28},L"Disabled state");
  }
};
static void FloatRegression(const std::vector<fs::path>& files) {
  _wputenv_s(L"FILO_TEST_DPI",L"96");
  // Previously, resizing during Draw released the active render target and froze Float.
  auto* d=tools::OpenFloatShelf(files,false,false);Check(d!=nullptr,"open collapsed Float");if(!d)return;
  HWND h=d->Hwnd();int collapsedWidth=d->WidthPx();
  for(int i=0;i<8;++i) {
    Click(d,94,102);Check(IsWindow(h),"expanded Float survives");
    Check(d->WidthPx()>collapsedWidth,"expand resized the surface");Check(d->RenderNow(false),"expanded target remains renderable");
    Click(d,96,125);Check(d->WidthPx()==collapsedWidth,"collapse resized the surface");Check(d->RenderNow(false),"collapsed target remains renderable");
  }
  Click(d,48,48);Check(!IsWindow(h),"cold collapsed close works without hover");
  d=tools::OpenFloatShelf(files,true,false);Check(d!=nullptr,"open expanded Float");if(!d)return;h=d->Hwnd();int tall=d->HeightPx();
  Click(d,282,177);Check(IsWindow(h),"remove first reference keeps shelf open");
  Check(d->HeightPx()<tall,"removal fits panel after the frame");Check(d->RenderNow(false),"remove does not lose render target");
  Click(d,282,177);Check(!IsWindow(h),"remove last reference closes shelf");
  d=tools::OpenFloatShelf(files,true,false);if(!d)return;h=d->Hwnd();
  // A drag or another window can steal capture. It must not strand Ui::active_.
  SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,175));d->RenderNow(false);
  SendMessageW(h,WM_CAPTURECHANGED,0,0);d->RenderNow(false);
  Click(d,160,d->HeightPx()-52);Check(!IsWindow(h),"clear still responds after capture cancellation");
  d=tools::OpenFloatShelf(files,true,false);if(!d)return;h=d->Hwnd();
  Click(d,100,175);SendMessageW(h,WM_KEYDOWN,VK_DELETE,0);Pump();Check(d->RenderNow(false),"delete selected reference stays renderable");
  SendMessageW(h,WM_KEYDOWN,VK_ESCAPE,0);Pump();Check(!IsWindow(h),"Escape closes Float");
  std::vector<fs::path> many=files;
  for(int i=0;i<8;++i) {auto file=files.front().parent_path()/(L"extra-"+std::to_wstring(i)+L".txt");std::ofstream(file)<<"Filo shelf fixture\n";many.push_back(file);}
  d=tools::OpenFloatShelf(many,true,false);if(d) {h=d->Hwnd();Click(d,160,d->HeightPx()-52);Check(!IsWindow(h),"clear footer cannot be intercepted by a clipped row");}
  for(const auto& f:many)Check(fs::exists(f),"clearing a scrolled shelf preserves every original");
  for(const auto& f:files) {Check(fs::exists(f),"original fixture survives Float removal");std::ifstream input(f);std::string line;std::getline(input,line);Check(line=="Filo shelf fixture","original fixture content unchanged");}
}
static int backgroundMode=0;
static LRESULT CALLBACK Background(HWND h,UINT msg,WPARAM w,LPARAM l) {
  if(msg!=WM_PAINT)return DefWindowProcW(h,msg,w,l);
  PAINTSTRUCT paint;HDC dc=BeginPaint(h,&paint);RECT r;GetClientRect(h,&r);
  COLORREF warm=RGB(215,85,25),cool=RGB(25,85,225);
  HBRUSH brush=CreateSolidBrush(backgroundMode==1?cool:warm);FillRect(dc,&r,brush);DeleteObject(brush);
  if(backgroundMode==2)for(int y=0;y<r.bottom;y+=192)for(int x=0;x<r.right;x+=192) {
    RECT cell{x,y,x+192,y+192};brush=CreateSolidBrush(((x/192+y/192)%2)?RGB(25,40,65):RGB(240,244,250));FillRect(dc,&cell,brush);DeleteObject(brush);
  }
  EndPaint(h,&paint);return 0;
}
static COLORREF Pixel(HWND h,float x,float y,float scale) {
  POINT point{(LONG)std::lround(x*scale),(LONG)std::lround(y*scale)};ClientToScreen(h,&point);
  HDC dc=GetDC(nullptr);auto pixel=GetPixel(dc,point.x,point.y);ReleaseDC(nullptr,dc);return pixel;
}
static int Difference(COLORREF a,COLORREF b) {
  return std::max({std::abs((int)GetRValue(a)-GetRValue(b)),std::abs((int)GetGValue(a)-GetGValue(b)),std::abs((int)GetBValue(a)-GetBValue(b))});
}
static void NativeReview(const fs::path& out) {
  WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Background;cls.lpszClassName=L"Filo.ControlReviewBackground";RegisterClassW(&cls);
  HWND bg=CreateWindowExW(WS_EX_TOPMOST,cls.lpszClassName,L"Filo owned control test background",WS_POPUP|WS_VISIBLE,120,70,1080,850,nullptr,nullptr,cls.hInstance,nullptr);
  Json scenes=Json::MakeArray();
  for(int dpi:{96,120,144})for(bool dark:{false,true}) {
    _wputenv_s(L"FILO_TEST_DPI",std::to_wstring(dpi).c_str());
    ds::ThemeState state;state.mode=dark?ds::ThemeMode::Dark:ds::ThemeMode::Light;state.glassStyle=2;
    ds::Theme::Get().SetUserState(state);BubbleProbe p;ui::DialogOptions o;
    o.title=L"Filo glass control review";o.widthDip=520;o.heightDip=320;o.topmost=true;o.glassCloseButton=true;
    Check(p.Open(o,POINT{680,450},true),"open native Acrylic control scene");EnableWindow(p.Hwnd(),FALSE);Pump(250);
    float sc=dpi/96.f;RECT rect;GetWindowRect(p.Hwnd(),&rect);
    std::wstring prefix=(dark?L"dark-":L"light-")+std::to_wstring(dpi);
    std::vector<COLORREF> transparentSamples;
    for(float transparency:{0.f,.25f,.50f,.85f,1.f}) {
      backgroundMode=0;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);
      state.acrylicTransparency=transparency;ds::Theme::Get().SetUserState(state);p.RecaptureGlass();p.Invalidate();Pump(180);DwmFlush();
      auto warm=Pixel(p.Hwnd(),460,305,sc);auto warmBubble=Pixel(p.Hwnd(),52,57,sc);auto frames=p.RenderedFrames();
      gfx::CaptureScreenToPng(rect,out/(prefix+L"-t"+std::to_wstring((int)(transparency*100))+L"-warm.png"));
      backgroundMode=1;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(200);DwmFlush();
      auto cool=Pixel(p.Hwnd(),460,305,sc),coolBubble=Pixel(p.Hwnd(),52,57,sc);
      Check(p.RenderedFrames()==frames,"background and bubble update without foreground repaint");
      if(transparency==0)Check(Difference(warm,cool)<=1,"zero transparency makes Acrylic body opaque");
      if(transparency==1)Check(Difference(warm,cool)>=25,"maximum transparency transfers live desktop colour");
      Check(Difference(warmBubble,coolBubble)>=10,"close bubble has its own live background");
      transparentSamples.push_back(warm);
      gfx::CaptureScreenToPng(rect,out/(prefix+L"-t"+std::to_wstring((int)(transparency*100))+L"-cool.png"));
      Json scene=Json::MakeObject();scene["dpi"]=dpi;scene["dark"]=dark;scene["transparency"]=transparency;
      scene["surface_colour_difference"]=Difference(warm,cool);scene["bubble_colour_difference"]=Difference(warmBubble,coolBubble);scenes.Push(scene);
    }
    Check(Difference(transparentSamples.front(),transparentSamples.back())>=20,"transparency slider changes the material substantially");
    backgroundMode=2;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);state.acrylicTransparency=1;
    for(float blur:{0.f,12.f,30.f,48.f}) {
      state.acrylicBlur=blur;ds::Theme::Get().SetUserState(state);p.RecaptureGlass();p.Invalidate();Pump(180);
      Check(p.HasLiveBackdrop(),"every blur value retains the live compositor");
      gfx::CaptureScreenToPng(rect,out/(prefix+L"-blur"+std::to_wstring((int)blur)+L".png"));
    }
    state.reduceTransparency=true;ds::Theme::Get().SetUserState(state);p.RecaptureGlass();p.Invalidate();Pump(160);
    Check(!p.HasLiveBackdrop(),"reduced transparency disables the live material");
    gfx::CaptureScreenToPng(rect,out/(prefix+L"-reduced.png"));p.CloseWindow();Pump(40);
  }
  DestroyWindow(bg);_wputenv_s(L"FILO_TEST_DPI",L"96");
  std::ofstream(out/L"native-control-scenes.json")<<scenes.Dump(true);
}

int wmain(int argc,wchar_t** argv) {
  ComInit com;OleInitialize(nullptr);log::Init(L"controls-test.log");
  fs::path out=argc>1?argv[1]:L"out/controls";fs::create_directories(out/L"fixtures");
  std::vector<fs::path> files={fs::absolute(out/L"fixtures/first.txt"),fs::absolute(out/L"fixtures/second.txt")};
  for(const auto& f:files)std::ofstream(f)<<"Filo shelf fixture\n";
  bool preview=argc>2&&std::wstring(argv[2])==L"--preview";
  if(preview) {
    SettingsStore::Get().Load();const auto s=SettingsStore::Get().Snapshot();ds::ThemeState state;
    state.mode=s.theme==2?ds::ThemeMode::Dark:ds::ThemeMode::Light;state.glassStyle=s.glassStyle;state.iconPack=s.iconPack;
    state.acrylicTransparency=s.acrylicTransparency;state.acrylicBlur=s.acrylicBlur;ds::Theme::Get().SetUserState(state);
    auto* shelf=tools::OpenFloatShelf(files,false,true);HWND h=shelf?shelf->Hwnd():nullptr;
    while(IsWindow(h))Pump(50);gfx::Shutdown();OleUninitialize();return 0;
  }
  {
    BubbleProbe button;ui::DialogOptions o;o.title=L"Close button interaction test";o.widthDip=340;o.heightDip=260;o.glassCloseButton=true;
    Check(button.Open(o,POINT{500,400},false),"open bubble interaction test");HWND h=button.Hwnd();
    button.disabled=true;Click(&button,60,57);Check(IsWindow(h),"disabled bubble ignores clicks");
    button.disabled=false;Click(&button,60,57);Check(!IsWindow(h),"enabled bubble closes on release");
  }
  FloatRegression(files);
  if(!(argc>2&&std::wstring(argv[2])==L"--regression-only"))NativeReview(out);
  Json report=Json::MakeObject();report["checks"]=checks;report["failures"]=failed;report["passed"]=failed==0;
  std::ofstream(out/L"float-regression.json")<<report.Dump(true);
  std::printf("%d checks, %d failures\n",checks,failed);gfx::Shutdown();OleUninitialize();return failed?1:0;
}
