// Native compositor acceptance harness. Owns both the changing background and
// Filo foreground; captures only this synthetic test scene, never user windows.
#include "ui/dialog_window.h"
#include "ui/settings_window.h"
#include "gfx/imageio.h"
#include "core/com.h"
#include "core/log.h"
#include "design/theme.h"
#include "tools/tools.h"
#include "wheel/wheel_window.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <dwmapi.h>

using namespace filo;
static bool grid=false;
static int gridCell=48;
static COLORREF colour = RGB(0, 0, 0);
static std::vector<uint8_t> wallpaper;
static int wallpaperW = 0, wallpaperH = 0;
static LRESULT CALLBACK Background(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
    HBRUSH b = CreateSolidBrush(colour); FillRect(dc, &r, b); DeleteObject(b);
    if (!wallpaper.empty()) {
      BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), wallpaperW, -wallpaperH, 1, 32, BI_RGB};
      SetStretchBltMode(dc, HALFTONE);
      StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, wallpaperW, wallpaperH, wallpaper.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    if(grid) {
      for(int y=0;y<r.bottom;y+=gridCell)for(int x=0;x<r.right;x+=gridCell) {
        RECT cell{x,y,x+gridCell,y+gridCell};HBRUSH brush=CreateSolidBrush(((x/gridCell+y/gridCell)%2)?RGB(36,58,80):RGB(228,233,240));
        FillRect(dc,&cell,brush);DeleteObject(brush);
      }
    }
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    if (wallpaper.empty()&&!grid) TextOutW(dc, 24, 24, L"Filo live backdrop — synthetic changing scene", 44);
    EndPaint(h, &ps); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
class Probe : public ui::DialogWindow {
 public:
  int frames = 0;
  void Draw(ui::Ui& ui, const ui::Rect& content, const ui::Rect&) override {
    ++frames;
    ui::TextStyle st; st.size = 20; st.weight = 600;
    ui.Label({content.x + 24, content.y + 20, content.w - 48, 32}, L"Live background • ثابت وواضح", st);
    ui.LabelSecondary({content.x + 24, content.y + 62, content.w - 48, 28}, L"The desktop changes; Filo does not repaint.");
    ui.Button({content.x + 24, content.bottom() - 58, 150, 30}, L"Save a Copy", ui::ButtonStyle::Primary);
  }
};
class OpticalProbe : public gfx::Surface {
 void OnRender(ID3D11RenderTargetView* rtv,ID3D11Texture2D*,int,int) override {
  float clear[4]{};gfx::Device::Get().ctx->ClearRenderTargetView(rtv,clear);
 }
};
static void Pump(unsigned ms) {
  const auto end = GetTickCount64() + ms; MSG m;
  while (GetTickCount64() < end) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Sleep(5);
  }
}
static bool Calibrate(HWND bg,const std::filesystem::path& out) {
    wallpaper.clear();grid=true;SetWindowPos(bg,HWND_TOP,100,100,1110,855,SWP_SHOWWINDOW);
    _wputenv_s(L"FILO_TEST_DPI",L"96");
    gridCell=160;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(250);
    gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"refraction-calibration-background.png");
    OpticalProbe probe;gfx::SurfaceOptions options;options.title=L"Filo optical calibration";
    ds::ThemeState light;light.mode=ds::ThemeMode::Light;light.glassStyle=0;light.glassRefraction=1;ds::Theme::Get().SetUserState(light);
    if(probe.Create(options,310,245,600,500)) {
      gfx::LiquidShape shape{};shape.cx=300;shape.cy=250;shape.halfW=220;shape.halfH=180;shape.radius=44;
      RECT clip{80,70,520,430};
      bool calibrated=true;
      for(float offset:{2.4f,24.f,0.f}) {
        shape.refractionPx=offset;
        calibrated&=probe.SetLiveBackdrop(clip,44,false,1,&shape);probe.RenderNow(true);probe.Show();Pump(350);
        gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(offset==0?L"refraction-calibration-none.png":offset>10?L"refraction-calibration-24.png":L"refraction-calibration-2.4.png"));
      }
      grid=false;colour=RGB(220,60,30);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(350);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"refraction-calibration-live-red.png");
      colour=RGB(25,80,225);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(350);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"refraction-calibration-live-blue.png");
      if(!calibrated)return false;
      probe.Destroy();
    } else return false;
    return true;
}
static bool IconScenes(HWND bg,const fs::path& out) {
  bool ok=true;int cases=0;Json report=Json::MakeObject();
  wallpaper.clear();grid=true;gridCell=48;
  SetWindowPos(bg,HWND_TOPMOST,100,100,1110,855,SWP_SHOWWINDOW);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(200);
  gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"icons-background.png");
  for(int dpi:{96,120,144}) {
    _wputenv_s(L"FILO_TEST_DPI",std::to_wstring(dpi).c_str());
    wheel::WheelWindow wheel;if(!wheel.Init())return false;
    EnableWindow(wheel.Hwnd(),FALSE);
    wheel::WheelContent c;c.kind=wheel::WheelKind::Actions;c.centerPrimary=L"1 file";
    const wchar_t* labels[]={L"Share",L"Float",L"Copy",L"Move",L"Copy Path",L"Terminal"};
    const char* icons[]={"share","float","copy","move","copypath","terminal"};
    for(int k=0;k<6;++k)c.items.push_back({Widen(icons[k]),labels[k],icons[k],true,L""});
    wheel.SetContent(std::move(c),false);wheel.SetRadiusOverridePx(220*dpi/96.f);
    wheel.ShowAt(POINT{610,503});Pump(500);
    for(int style:{0,1,2})for(const char* pack:{"classic","pebble","circuit","facet"}) {
      auto state=ds::Theme::Get().State();state.mode=ds::ThemeMode::Light;state.glassStyle=style;state.iconPack=pack;
      state.reduceTransparency=false;state.glassIntensity=1;ds::Theme::Get().SetUserState(state);wheel.Invalidate();Pump(220);
      ok&=wheel.HasLiveBackdrop();++cases;
      std::wstring prefix=L"wheel-"+std::to_wstring(style)+L"-"+Widen(pack)+L"-"+std::to_wstring(dpi);
      ok&=gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L".png"));
      if(style==2&&std::string(pack)=="facet") {
        auto frames=wheel.RenderedFrames(), masks=wheel.BackdropMaskUpdates();
        grid=false;colour=RGB(230,85,25);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(250);
        gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-warm.png"));
        colour=RGB(20,85,230);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(250);
        gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-cool.png"));
        bool idle=frames==wheel.RenderedFrames()&&masks==wheel.BackdropMaskUpdates();ok&=idle;
        report["idle-"+std::to_string(dpi)]=idle;
        grid=true;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(100);
      }
    }
    wheel.Dismiss(false);wheel.Destroy();Pump(100);
  }
  _wputenv_s(L"FILO_TEST_DPI",L"");report["native_wheel_cases"]=cases;report["passed"]=ok;
  std::ofstream(out/L"native-icons.json")<<report.Dump(true);return ok;
}

int wmain(int argc, wchar_t** argv) {
  ComInit com; OleInitialize(nullptr);
  log::Init(L"live-test.log");
  if(argc>1 && std::wstring(argv[1])==L"--settings-preview") {
    SettingsStore::Get().Load();
    auto apply=[] {
      const auto s=SettingsStore::Get().Snapshot();ds::ThemeState t;
      t.mode=s.theme==2?ds::ThemeMode::Dark:s.theme==0?ds::ThemeMode::System:ds::ThemeMode::Light;
      t.acrylicTransparency=s.acrylicTransparency;t.acrylicBlur=s.acrylicBlur;t.iconPack=s.iconPack;t.glassStyle=s.glassStyle;t.glassIntensity=s.glassIntensity;t.glassReflections=s.glassReflections;t.glassRefraction=s.glassRefraction;
      t.reduceTransparency=s.reduceTransparency;t.reduceMotion=s.reduceMotion;ds::Theme::Get().SetUserState(t);
    };
    apply();auto* settings=ui::SettingsWindow::Show(true,2);if(!settings)return 13;
    const HWND hwnd=settings->Hwnd();
    SettingsStore::Get().AddListener([](const Settings&){PostThreadMessageW(GetCurrentThreadId(),WM_APP+0x700,0,0);});
    MSG msg;while(IsWindow(hwnd)) {while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_APP+0x700){apply();PostMessageW(hwnd,WM_THEMECHANGED,0,0);}else{TranslateMessage(&msg);DispatchMessageW(&msg);}}MsgWaitForMultipleObjects(0,nullptr,FALSE,16,QS_ALLINPUT);}
    gfx::Shutdown();OleUninitialize();return 0;
  }

  const std::filesystem::path out = argc > 1 ? argv[1] : L"out/live-glass";
  std::filesystem::create_directories(out);
  const bool soft=argc>2 && std::wstring(argv[2])==L"--soft";
  const bool acrylic=argc>2 && std::wstring(argv[2])==L"--acrylic";
  const int style=acrylic?2:soft?1:0;
  WNDCLASSW wc{}; wc.lpfnWndProc = Background; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Filo.LiveBackgroundTest";
  RegisterClassW(&wc);
  HWND bg = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Filo synthetic background", WS_POPUP | WS_VISIBLE, 200, 100, 1040, 820, nullptr, nullptr, wc.hInstance, nullptr);
  if(argc>2 && std::wstring(argv[2])==L"--icons") {
    bool ok=IconScenes(bg,out);DestroyWindow(bg);gfx::Shutdown();OleUninitialize();return ok?0:14;
  }
  if(argc>2 && std::wstring(argv[2])==L"--calibration-only") {
    const bool ok=Calibrate(bg,out);DestroyWindow(bg);gfx::Shutdown();OleUninitialize();return ok?0:12;
  }
  int result = 0;
  std::ofstream sweep(out / "samples.csv");
  sweep << "dark,intensity,inputR,inputG,inputB,R,G,B\n";
  std::ofstream log(out / "result.json");
  log << "{\n  \"themes\": [\n";
  for (int theme = 0; theme < 2; ++theme) {
    ds::ThemeState state; state.glassStyle=style; state.mode = theme ? ds::ThemeMode::Dark : ds::ThemeMode::Light; ds::Theme::Get().SetUserState(state);
    Probe p; ui::DialogOptions o; o.title = L"Filo Live Glass Check"; o.topmost=true; o.glassCloseButton=true; o.widthDip = 520; o.heightDip = 450;
    if (!p.Open(o, POINT{720, 510}, true)) return 2;
    EnableWindow(p.Hwnd(),FALSE); Pump(650);
    bool live = p.HasLiveBackdrop();
    if(!live && argc>2 && std::wstring(argv[2])==L"--quick") return 3;
    log << "    {\"dark\":" << (theme ? "true" : "false") << ",\"live\":" << (live ? "true" : "false") << ",\"frames\":[";
    if (!live) result = 3;
    COLORREF colours[] = {RGB(0,0,0), RGB(220,70,30), RGB(10,95,230), RGB(255,255,255)};
    for (int phase = 0; phase < 4; ++phase) {
      colour = colours[phase]; InvalidateRect(bg, nullptr, FALSE); UpdateWindow(bg);
      Pump(600);
      RECT r; GetWindowRect(p.Hwnd(), &r);
      const auto file = out / ((theme ? L"dark-" : L"light-") + std::to_wstring(phase) + L".png");
      if (!gfx::CaptureScreenToPng(r, file)) result = 4;
      if (phase) log << ',';
      log << p.frames;
    }
    log << "],\"mask_updates\":" << p.BackdropMaskUpdates();
    RECT sampleRect; GetWindowRect(p.Hwnd(), &sampleRect);
    for (float intensity : {.25f,.5f,1.f}) {
      state.glassIntensity = intensity;if(acrylic)state.acrylicTransparency=.85f*intensity; ds::Theme::Get().SetUserState(state); p.RecaptureGlass(); p.Invalidate(); Pump(150);
      for (int red : {0,128,255}) for (int green : {0,128,255}) for (int blue : {0,128,255}) {
        colour = RGB(red,green,blue); InvalidateRect(bg,nullptr,FALSE); UpdateWindow(bg); Pump(60); DwmFlush();
        HDC screen = GetDC(nullptr);
        COLORREF pixel = GetPixel(screen,sampleRect.left+290,sampleRect.top+280); ReleaseDC(nullptr,screen);
        if (pixel == CLR_INVALID) result = 9;
        sweep << theme << ',' << intensity << ',' << red << ',' << green << ',' << blue << ',' << (int)GetRValue(pixel) << ',' << (int)GetGValue(pixel) << ',' << (int)GetBValue(pixel) << '\n';
      }
    }
    state.glassIntensity = .5f;if(acrylic)state.acrylicTransparency=.425f; ds::Theme::Get().SetUserState(state); p.RecaptureGlass(); p.Invalidate();
    colour = RGB(0,0,0); InvalidateRect(bg,nullptr,FALSE); UpdateWindow(bg); Pump(300);
    RECT r; GetWindowRect(p.Hwnd(),&r);
    gfx::CaptureScreenToPng(r, out / (theme ? L"dark-half.png" : L"light-half.png"));
    state.reduceTransparency = true; ds::Theme::Get().SetUserState(state); p.RecaptureGlass(); p.Invalidate(); Pump(250);
    bool opaque = !p.HasLiveBackdrop();
    gfx::CaptureScreenToPng(r, out / (theme ? L"dark-opaque-0.png" : L"light-opaque-0.png"));
    colour = RGB(255,255,255); InvalidateRect(bg,nullptr,FALSE); UpdateWindow(bg); Pump(250);
    gfx::CaptureScreenToPng(r, out / (theme ? L"dark-opaque-1.png" : L"light-opaque-1.png"));
    state.reduceTransparency = false; state.glassIntensity = 1.f;if(acrylic)state.acrylicTransparency=.85f;
    ds::Theme::Get().SetUserState(state); p.RecaptureGlass(); p.SetBodySize(570,410); Pump(300);
    bool resized = p.HasLiveBackdrop();
    gfx::Device::Get().HandleDeviceLost(DXGI_ERROR_DEVICE_RESET); Pump(400);
    bool recovered = p.HasLiveBackdrop();
    GetWindowRect(p.Hwnd(), &r);
    gfx::CaptureScreenToPng(r, out / (theme ? L"dark-recovered.png" : L"light-recovered.png"));
    log << ",\"opaque\":" << (opaque ? "true":"false") << ",\"resized\":" << (resized ? "true":"false") << ",\"device_recovered\":" << (recovered ? "true":"false") << "}" << (theme ? "\n" : ",\n");
    if (!opaque || !resized || !recovered) result = 7;
    p.CloseWindow(); Pump(100);
  }
  log << "  ],\n";
  ds::ThemeState light; light.glassStyle=style; light.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(light);
  _wputenv_s(L"FILO_TEST_DPI", L"96");
  gfx::LoadBgra(L"reference/backdrop-edit.png", wallpaper, wallpaperW, wallpaperH);
  SetWindowPos(bg, HWND_TOP, 100, 100, 1110, 866, SWP_SHOWWINDOW);
  InvalidateRect(bg, nullptr, FALSE); UpdateWindow(bg);
  tools::RegisterAllTools(nullptr);
  auto selection = Selection::FromPaths({std::filesystem::absolute(L"testdata/ui/flower.png")});
  auto* photo = tools::OpenTool("edit-photo", selection);
  bool photoLive = photo && photo->HasLiveBackdrop();
  if (photo) {
    // Align the measured reference window in the half-resolution background.
    SetWindowPos(photo->Hwnd(), HWND_TOPMOST, 301, 93, 708, 960, SWP_SHOWWINDOW);
    Pump(1200);
    gfx::CaptureScreenToPng(RECT{100,100,1210,966}, out / L"reference-photo-live.png");
    photo->CloseWindow(); Pump(100);
  }
  log << "  \"reference_photo_live\":" << (photoLive ? "true" : "false") << ",\n";
  if (!photoLive) result = 5;
  wallpaper.clear();
  gfx::LoadBgra(L"reference/backdrop-conv.png", wallpaper, wallpaperW, wallpaperH);
  SetWindowPos(bg, HWND_TOP, 100, 100, 1110, 855, SWP_SHOWWINDOW);
  InvalidateRect(bg, nullptr, FALSE); UpdateWindow(bg);
  wheel::WheelWindow wheel;
  bool wheelLive = wheel.Init();
  if (wheelLive) {
    wheel::WheelContent content; content.kind = wheel::WheelKind::Convert; content.centerPrimary = L"2.2 MB";
    for (auto label : {L"JPG",L"WEBP",L"HEIC",L"TIFF",L"AVIF",L"BMP",L"PDF",L"DOCX"}) content.items.push_back({label,label,"",true,L""});
    wheel.SetContent(std::move(content), false); wheel.SetRadiusOverridePx(265); wheel.ShowAt(POINT{610,503});
    Pump(600); wheelLive = wheel.HasLiveBackdrop();
    gfx::CaptureScreenToPng(RECT{100,100,1210,955}, out / L"reference-wheel-live.png");
    wallpaper.clear(); colour = RGB(20,85,220); InvalidateRect(bg, nullptr, FALSE); UpdateWindow(bg); Pump(600);
    gfx::CaptureScreenToPng(RECT{100,100,1210,955}, out / L"wheel-changing-background.png");
    wheel.SetHoverImmediate(1); Pump(300);
    gfx::CaptureScreenToPng(RECT{100,100,1210,955}, out / L"wheel-hover-live.png");
    colour=RGB(255,255,255);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(400);
    gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"wheel-hover-white.png");
    light.reduceTransparency = true; ds::Theme::Get().SetUserState(light); wheel.Invalidate(); Pump(300);
    bool wheelOpaque = !wheel.HasLiveBackdrop();
    light.reduceTransparency = false; ds::Theme::Get().SetUserState(light);
    gfx::Device::Get().HandleDeviceLost(DXGI_ERROR_DEVICE_RESET); Pump(400);
    bool wheelRecovered = wheel.HasLiveBackdrop();
    gfx::CaptureScreenToPng(RECT{100,100,1210,955}, out / L"wheel-recovered.png");
    log << "  \"wheel_opaque\":" << (wheelOpaque ? "true":"false") << ",\n  \"wheel_device_recovered\":" << (wheelRecovered ? "true":"false") << ",\n";
    if (!wheelOpaque || !wheelRecovered) result = 8;
    wheel.Dismiss(false); wheel.Destroy();
  }
  log << "  \"wheel_live\":" << (wheelLive ? "true" : "false") << "\n}\n";
  if (!wheelLive) result = 6;
  log.close();
  if(!soft&&!acrylic) {
    wallpaper.clear();grid=true;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(200);
    gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/L"optics-background.png");
    std::ofstream optics(out/L"optics.json");optics<<"{\"dpi\":[";
    for(int dpi:{96,120,144}) {
      _wputenv_s(L"FILO_TEST_DPI",std::to_wstring(dpi).c_str());
      light.glassStyle=0;light.glassReflections=.65f;light.glassRefraction=1;ds::Theme::Get().SetUserState(light);
      wheel::WheelWindow test;bool ok=test.Init();
      if(!ok){result=10;break;}
      wheel::WheelContent content;content.kind=wheel::WheelKind::Actions;content.centerPrimary=L"1 file";
      for(auto item:std::initializer_list<std::pair<const wchar_t*,const char*>>{{L"Share","share"},{L"Float","float"},{L"Copy","copy"},{L"Move","move"},{L"Copy Path","copypath"},{L"Terminal","terminal"}})content.items.push_back({item.first,item.first,item.second,true,L""});
      test.SetContent(std::move(content),false);test.SetRadiusOverridePx(265.f*dpi/96.f);test.ShowAt(POINT{610,503});EnableWindow(test.Hwnd(),FALSE);Pump(800);
      const auto prefix=std::wstring(L"dpi-")+std::to_wstring(dpi);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-liquid.png"));
      light.glassReflections=0;ds::Theme::Get().SetUserState(light);test.Invalidate();Pump(250);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-no-reflection.png"));
      light.glassReflections=.65f;light.glassRefraction=0;ds::Theme::Get().SetUserState(light);test.Invalidate();Pump(250);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-no-refraction.png"));
      const unsigned frames=test.RenderedFrames(),masks=test.BackdropMaskUpdates();
      grid=false;colour=RGB(225,90,35);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(350);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-warm.png"));
      colour=RGB(40,95,210);InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(350);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-cool.png"));
      bool idle=frames==test.RenderedFrames()&&masks==test.BackdropMaskUpdates();
      grid=true;InvalidateRect(bg,nullptr,FALSE);UpdateWindow(bg);Pump(200);
      light.glassStyle=1;ds::Theme::Get().SetUserState(light);test.Invalidate();Pump(250);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-soft.png"));
      bool softLive=test.HasLiveBackdrop();
      light.glassStyle=0;light.glassRefraction=1;ds::Theme::Get().SetUserState(light);test.Invalidate();Pump(250);
      gfx::CaptureScreenToPng(RECT{100,100,1210,955},out/(prefix+L"-restored.png"));
      if(dpi!=96)optics<<',';
      optics<<"{\"dpi\":"<<dpi<<",\"live\":"<<(test.HasLiveBackdrop()?"true":"false")<<",\"soft_live\":"<<(softLive?"true":"false")<<",\"idle\":"<<(idle?"true":"false")<<",\"frames\":"<<frames<<",\"mask_updates\":"<<masks<<"}";
      if(!idle||!softLive||!test.HasLiveBackdrop())result=11;
      test.Dismiss(false);test.Destroy();Pump(100);
    }
    optics<<"]}\n";
    if(!Calibrate(bg,out))result=12;
  }
  DestroyWindow(bg); gfx::Shutdown(); OleUninitialize();
  return result;
}
