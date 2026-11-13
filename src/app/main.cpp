// Filo entry point.
//   Filo.exe                          start the background runtime (tray + drag wheels)
//   Filo.exe --background             same, silent (autostart)
//   Filo.exe --wheel [--kind K] <paths...>   open the wheel for files (Explorer verb / Send To); K = convert|tools|actions
//   Filo.exe --settings | --quit
//   Filo.exe --render-wheel ...       hidden visual-test mode (see RenderWheelMode)
#include "app/runtime.h"
#include "core/util.h"
#include "core/log.h"
#include "core/crash.h"
#include "core/com.h"
#include "core/settings.h"
#include "design/theme.h"
#include "gfx/imageio.h"
#include "wheel/wheel_window.h"
#include "ui/gallery.h"
#include "ui/settings_window.h"
#include "ui/progress_hud.h"
#include "tools/tools.h"
#include "engines/web/web.h"
#include "ui/link_zone.h"
#include "shell/browser_url.h"
#include "registry/capability.h"
#include "core/job.h"
#include <shellapi.h>
#include <cstdio>

using namespace filo;

static wheel::WheelContent SampleContent(const std::wstring& kind) {
  using namespace wheel;
  WheelContent c;
  if (kind == L"convert") {
    c.kind = WheelKind::Convert;
    for (auto l : {L"JPG", L"WEBP", L"HEIC", L"TIFF", L"AVIF", L"BMP", L"PDF", L"DOCX"}) c.items.push_back({std::wstring(L"convert:") + l, l, (l[0] == L'P' || l[0] == L'D') ? "document" : "image", true, L""});   // the real wheel takes these from selection.cpp (FormatIcon)
    c.centerPrimary = L"2.2 MB";
  } else if (kind == L"tools" || kind == L"tools9") {
    c.kind = WheelKind::Tools;
    struct T { const wchar_t* l; const char* i; } t[] = {{L"Compress", "compress"}, {L"Metadata", "metadata"}, {L"Edit", "edit"}, {L"Annotate", "annotate"},
                                                       {L"Add BG", "addbg"}, {L"Crop", "crop"}, {L"Redact", "redact"}};
    for (auto& x : t) c.items.push_back({std::wstring(L"tool:") + x.l, x.l, x.i, true, L""});
    if (kind == L"tools9") { c.items.push_back({L"tool:PDF", L"PDF", "document", true, L""}); c.items.push_back({L"tool:QR", L"QR", "qr", true, L""}); }
  } else {
    c.kind = WheelKind::Actions;
    struct T { const wchar_t* l; const char* i; } t[] = {{L"Share", "share"}, {L"Float", "float"}, {L"Copy", "copy"}, {L"Move", "move"}, {L"Copy path", "copypath"}, {L"Terminal", "terminal"}};
    for (auto& x : t) c.items.push_back({std::wstring(L"action:") + x.l, x.l, x.i, true, L""});
  }
  return c;
}

// Filo.exe --render-wheel <convert|tools|actions> <out.png> [--backdrop img.png] [--cursor x,y] [--radius px] [--hover k] [--dark|--light]
static int RenderWheelMode(int argc, wchar_t** argv) {
  std::wstring kind = argc > 2 ? argv[2] : L"convert";
  fs::path out = argc > 3 ? argv[3] : L"wheel.png";
  fs::path backdrop; POINT cur{1280, 720}; float radius = 0; int hover = -1;
  for (int i = 4; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--backdrop" && i + 1 < argc) backdrop = argv[++i];
    else if (a == L"--cursor" && i + 1 < argc) { swscanf_s(argv[++i], L"%ld,%ld", &cur.x, &cur.y); }
    else if (a == L"--radius" && i + 1 < argc) radius = (float)_wtof(argv[++i]);
    else if (a == L"--hover" && i + 1 < argc) hover = _wtoi(argv[++i]);
    else if (a == L"--icon-pack" && i + 1 < argc) { auto state=ds::Theme::Get().State(); state.iconPack=Narrow(argv[++i]); ds::Theme::Get().SetUserState(state); }
    else if (a == L"--acrylic" || a == L"--soft-glass" || a == L"--liquid-glass" || a == L"--glassy" || a == L"--symbolic") { auto state=ds::Theme::Get().State(); state.glassStyle=a==L"--acrylic"?2:a==L"--soft-glass"?1:a==L"--glassy"?3:a==L"--symbolic"?4:0; ds::Theme::Get().SetUserState(state); }
    else if (a == L"--light") { ds::ThemeState s = ds::Theme::Get().State(); s.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(s); }
    else if (a == L"--dark") { ds::ThemeState s = ds::Theme::Get().State(); s.mode = ds::ThemeMode::Dark; ds::Theme::Get().SetUserState(s); }
  }
  wheel::WheelWindow w;
  if (!w.Init()) { std::fprintf(stderr, "wheel init failed\n"); return 2; }
  if (radius > 0) w.SetRadiusOverridePx(radius);
  w.SetContent(SampleContent(kind), false);
  w.PrepareOffscreen(cur, true);
  RECT r = w.ScreenRect();
  if (!backdrop.empty()) {
    gfx::BackdropImage bi;
    float sigma = ds::glass::blurSigmaDip * (radius > 0 ? radius / ds::wheel::radiusDip : 1.f);
    if (!gfx::BackdropFromImage(backdrop, r, sigma, bi)) { std::fprintf(stderr, "backdrop load failed\n"); return 3; }
    w.SetBackdropImage(std::move(bi));
  }
  if (hover >= 0) w.SetHoverImmediate(hover);
  std::vector<uint8_t> px; int pw = 0, ph = 0;
  if (!w.RenderNow(false, &px, &pw, &ph)) { std::fprintf(stderr, "render failed\n"); return 4; }
  if (!gfx::SavePng(out, px.data(), pw, ph, true)) { std::fprintf(stderr, "save failed\n"); return 5; }
  std::printf("rendered %dx%d to %ls (window origin %ld,%ld)\n", pw, ph, out.c_str(), r.left, r.top);
  return 0;
}

// Filo.exe --gallery [--rtl] [--dark|--light] [--shot out.png]
static int GalleryMode(int argc, wchar_t** argv) {
  fs::path shot, offscreen; bool rtl = false;
  for (int i = 2; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == L"--offscreen" && i + 1 < argc) offscreen = argv[++i];
    else if (a == L"--rtl") rtl = true;
    else if (a == L"--light") { ds::ThemeState s = ds::Theme::Get().State(); s.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(s); }
    else if (a == L"--dark") { ds::ThemeState s = ds::Theme::Get().State(); s.mode = ds::ThemeMode::Dark; ds::Theme::Get().SetUserState(s); }
  }
  POINT at{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
  if (!offscreen.empty()) {
    auto* d = ui::GalleryDialog::Show(at, rtl, false);
    if (!d) return 1;
    bool ok = d->RenderToPng(offscreen);
    std::printf("offscreen render %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
  }
  auto* d = ui::GalleryDialog::Show(at, rtl);
  if (!d) { FILO_LOG(Error, "gallery", "dialog failed to open"); return 1; }
  HWND hw = d->Hwnd();
  FILO_LOG(Info, "gallery", "opened hwnd=%p visible=%d", hw, IsWindowVisible(hw));
  const double t0 = NowSeconds(); bool shotDone = false;
  MSG m;
  while (IsWindow(hw)) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (!shot.empty() && !shotDone && NowSeconds() - t0 > 1.2) {
      RECT r; GetWindowRect(hw, &r);
      shotDone = gfx::CaptureScreenToPng(r, shot);
      FILO_LOG(Info, "gallery", "screenshot %d rect=%ld,%ld,%ld,%ld", shotDone, r.left, r.top, r.right, r.bottom);
      PostMessageW(hw, WM_CLOSE, 0, 0);
    }
    if (!IsWindow(hw)) break;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
  }
  return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  ComInit com;
  OleInitialize(nullptr);
  log::Init();
  crash::Install();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int rc = 0;
  if (argc > 1 && std::wstring(argv[1]) == L"--render-wheel") {
    rc = RenderWheelMode(argc, argv);
  } else if (argc > 1 && std::wstring(argv[1]) == L"--gallery") {
    rc = GalleryMode(argc, argv);
  } else if (argc > 2 && std::wstring(argv[1]) == L"--settings-shot") {
    // Filo.exe --settings-shot <out.png> [tab 0..3] : render the settings window hidden for visual review
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    for (int i = 4; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--dark") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Dark; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--light") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(ts); }
    }
    auto* w = ui::SettingsWindow::Show(false, argc > 3 ? _wtoi(argv[3]) : 0);
    bool ok = w && w->TestShot(argv[2]);
    std::printf("settings shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else if (argc > 2 && std::wstring(argv[1]) == L"--wheel-dump") {
    // Filo.exe --wheel-dump <files...> : prints what each wheel would offer for this selection (diagnostics, no window)
    SettingsStore::Get().Load();
    RegisterAllEngines();
    tools::RegisterAllTools(nullptr);
    std::vector<fs::path> files; for (int i = 2; i < argc; ++i) files.emplace_back(argv[i]);
    Availability av = ComputeAvailability(Selection::FromPaths(files));
    auto dump = [](const char* name, const std::vector<WheelEntry>& v, const std::wstring& why) {
      std::printf("%s:", name);
      for (auto& e : v) std::printf(" [%s%s]", Narrow(e.label).c_str(), e.enabled ? "" : " (disabled)");
      if (v.empty()) std::printf(" (none) %s", Narrow(why).c_str());
      std::printf("\n");
    };
    dump("convert", av.convert, av.convertEmptyReason);
    dump("tools  ", av.tools, av.toolsEmptyReason);
    dump("actions", av.actions, L"");
    rc = 0;
  } else if (argc > 2 && std::wstring(argv[1]) == L"--hud-shot") {
    // Filo.exe --hud-shot <out.png> [running|done|failed] : render the progress HUD hidden for visual review
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    const std::wstring mode = argc > 3 ? argv[3] : L"running";
    auto* h = ui::ProgressHud::Create(POINT{1200, 300}, false);
    bool ok = h != nullptr;
    if (h) {
      h->SetTexts(L"Converting to MP4", L"geese.mov"); h->SetProgress(0.62f);
      if (mode == L"done") h->Finish(ui::ProgressHud::State::Done, L"Converted to MP4", L"geese.mp4", {});
      if (mode == L"failed") h->Finish(ui::ProgressHud::State::Failed, L"Conversion failed", L"geese.mov: the video could not be read (it may be damaged).");
      ok = h->TestShot(argv[2]);
    }
    std::printf("hud shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else if (argc > 1 && std::wstring(argv[1]) == L"--crash-test") {
    // Filo.exe --crash-test : crashes on purpose to check the report + automatic restart (starts a --background instance afterwards)
    crash::EnableRestartOnCrash(0);
    volatile int* boom = nullptr;
    *boom = 1;
  } else if (argc > 1 && std::wstring(argv[1]) == L"--browser-urls") {
    // Filo.exe --browser-urls : diagnostics for the download hotkey. For every open browser window prints the host of the page it shows.
    struct Ctx { int n = 0; };
    Ctx ctx;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
      if (!IsWindowVisible(h)) return TRUE;
      wchar_t cls[96] = {}; GetClassNameW(h, cls, 96);
      if (wcscmp(cls, L"Chrome_WidgetWin_1") && wcscmp(cls, L"MozillaWindowClass")) return TRUE;
      wchar_t title[120] = {}; GetWindowTextW(h, title, 120);
      if (!title[0]) return TRUE;
      const std::wstring url = shell::BrowserPageUrl(h);
      size_t s = url.find(L"://"); s = s == std::wstring::npos ? 0 : s + 3;
      const std::wstring host = url.empty() ? std::wstring(L"(no address found)") : url.substr(s, url.find(L'/', s) == std::wstring::npos ? std::wstring::npos : url.find(L'/', s) - s);
      std::printf("%ls  ->  %ls\n", cls, host.c_str());
      ++reinterpret_cast<Ctx*>(l)->n;
      return TRUE;
    }, (LPARAM)&ctx);
    std::printf("%d browser window(s)\n", ctx.n);
    rc = 0;
  } else if (argc > 3 && std::wstring(argv[1]) == L"--download-shot") {
    // Filo.exe --download-shot <out.png> <fixture.json> [--dark|--light] [--wait] : render the Download from Link window hidden from a
    // yt-dlp style JSON file (no probing), for visual review. --wait lets thumbnails of real picture URLs load first.
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    bool wait = false;
    for (int i = 4; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--dark") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Dark; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--light") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--wait") wait = true;
    }
    bool ok = false;
    std::string text, jerr;
    if (FILE* f = _wfopen(argv[3], L"rb")) { char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); fclose(f); }
    if (auto j = Json::Parse(text, &jerr)) {
      web::LinkInfo li = j->Has("_filo_account") ? web::LinkInfo() : web::ParseProbe(*j, Widen(j->Get("webpage_url").StringOr("https://example.com/post")));
      web::AccountInfo acct;
      if (j->Has("_filo_account")) acct = web::AccountFromJson(*j, Widen(j->Get("_filo_account").Get("url").StringOr("https://x.com/NASA")));
      if (auto* d = tools::OpenDownloadDialog(L"", &li, false, j->Has("_filo_account") ? &acct : nullptr)) {
        const double t0 = NowSeconds(); MSG m;
        while (NowSeconds() - t0 < (wait ? 6.0 : 0.3)) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(20); }
        ok = d->RenderToPng(argv[2]);
        d->CloseWindow();
      }
    }
    std::printf("download shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else if (argc > 2 && std::wstring(argv[1]) == L"--link-zone-shot") {
    // Filo.exe --link-zone-shot <out.png> [0|1|2] : the link drop zone (0 waiting, 1 a link is over it, 2 not a link)
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    auto* z = ui::LinkZone::Create(POINT{900, 500}, false);
    bool ok = z && z->TestShot(argv[2], argc > 3 ? _wtoi(argv[3]) : 0);
    std::printf("link zone shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else if (argc > 3 && std::wstring(argv[1]) == L"--float-shot") {
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    std::vector<fs::path> files; bool expanded = false;
    for (int i = 3; i < argc; ++i) { if (std::wstring(argv[i]) == L"--expanded") expanded = true; else files.emplace_back(argv[i]); }
    bool ok = tools::FloatShelfShot(files, argv[2], expanded);
    std::printf("float shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else if (argc > 3 && std::wstring(argv[1]) == L"--tool") {
    SettingsStore::Get().Load();
    const Settings st = SettingsStore::Get().Snapshot();
    ds::ThemeState ts;
    ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light);
    ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack = st.iconPack; ts.glassStyle = st.glassStyle; ts.glassReflections = st.glassReflections; ts.glassRefraction = st.glassRefraction;
    ts.glassIntensity = st.glassIntensity; ts.reduceTransparency = st.reduceTransparency; ts.reduceMotion = st.reduceMotion;
    ds::Theme::Get().SetUserState(ts);
    tools::RegisterAllTools(nullptr);
    std::vector<fs::path> files; for (int i = 3; i < argc; ++i) files.emplace_back(argv[i]);
    auto* d = tools::OpenTool(Narrow(argv[2]), Selection::FromPaths(files));
    if (!d) rc = 1;
    else {
      const HWND hw = d->Hwnd();
      MSG m;
      while (IsWindow(hw)) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (IsWindow(hw)) MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
      }
    }
  } else if (argc > 4 && std::wstring(argv[1]) == L"--tool-run") {
    // Filo.exe --tool-run <toolId> <presetJson|-> <files...> : headless end-to-end run of a tool dialog's job; prints a JSON result
    SettingsStore::Get().Load();
    RegisterAllEngines();
    tools::RegisterAllTools(nullptr);
    std::vector<fs::path> files; for (int i = 4; i < argc; ++i) files.emplace_back(argv[i]);
    JobResult jr;
    std::string preset = std::wstring(argv[3]) == L"-" ? "{}" : Narrow(argv[3]);
    bool ok = tools::RunToolForTest(Narrow(argv[2]), Selection::FromPaths(files), preset, 600.0, &jr);
    Json out = Json::MakeObject();
    out["ok"] = ok; out["error"] = Narrow(jr.error);
    Json outs = Json::MakeArray(); for (auto& o : jr.outputs) outs.Push(Narrow(o.wstring())); out["outputs"] = outs;
    Json ws = Json::MakeArray(); for (auto& w : jr.warnings) ws.Push(Narrow(w)); out["warnings"] = ws;
    std::printf("%s\n", out.Dump().c_str());
    rc = ok ? 0 : 1;
  } else if (argc > 4 && std::wstring(argv[1]) == L"--tool-shot") {
    // Filo.exe --tool-shot <toolId> <out.png> <files...> : render a tool dialog hidden for visual review
    SettingsStore::Get().Load();
    { Settings st = SettingsStore::Get().Snapshot(); ds::ThemeState ts; ts.acrylicTransparency=st.acrylicTransparency; ts.acrylicBlur=st.acrylicBlur; ts.iconPack=st.iconPack; ts.mode = st.theme == 2 ? ds::ThemeMode::Dark : (st.theme == 0 ? ds::ThemeMode::System : ds::ThemeMode::Light); ds::Theme::Get().SetUserState(ts); }
    tools::RegisterAllTools(nullptr);
    std::vector<fs::path> files; POINT toolAt{800, 500}; bool haveAt = false;
    for (int i = 4; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--dark") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Dark; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--light") { ds::ThemeState ts = ds::Theme::Get().State(); ts.mode = ds::ThemeMode::Light; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--glassy") { ds::ThemeState ts = ds::Theme::Get().State(); ts.glassStyle = 3; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--symbolic") { ds::ThemeState ts = ds::Theme::Get().State(); ts.glassStyle = 4; ds::Theme::Get().SetUserState(ts); }
      else if (a == L"--at" && i + 1 < argc) { swscanf_s(argv[++i], L"%ld,%ld", &toolAt.x, &toolAt.y); haveAt = true; }
      else files.emplace_back(a);
    }
    bool ok = tools::RenderToolToPng(Narrow(argv[2]), Selection::FromPaths(files), argv[3], haveAt ? &toolAt : nullptr);
    std::printf("tool shot %s\n", ok ? "ok" : "FAILED");
    rc = ok ? 0 : 1;
  } else {
    app::StartupArgs a;
    for (int i = 1; i < argc; ++i) {
      std::wstring s = argv[i];
      if (s == L"--quit") a.quit = true;
      else if (s == L"--settings") a.openSettings = true;
      else if (s == L"--background") a.background = true;
      else if (s == L"--kind" && i + 1 < argc) a.wheelKind = argv[++i];
      else if (s == L"--download" && i + 1 < argc) a.downloadUrl = argv[++i];
      else if (s == L"--crash-restart" && i + 1 < argc) a.crashRestarts = _wtoi(argv[++i]);
      else if (s == L"--wait-pid" && i + 1 < argc) {            // a restart after a crash waits for the crashed process to end (single-instance mutex)
        if (HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(argv[++i]))) { WaitForSingleObject(hp, 15000); CloseHandle(hp); }
      }
      else if (s == L"--wheel") {
        for (int j = i + 1; j < argc; ++j) {
          std::wstring p = argv[j];
          if (p == L"--kind" && j + 1 < argc) { a.wheelKind = argv[++j]; continue; }
          a.wheelPaths.push_back(p);
        }
        break;
      }
    }
    app::Runtime rt;
    rt.registerTools = [](HWND h) { tools::RegisterAllTools(h); };
    rt.onOpenSettings = [] { ui::SettingsWindow::Show(); };
    rc = rt.Run(a);
  }
  LocalFree(argv);
  gfx::Shutdown();
  OleUninitialize();
  return rc;
}
