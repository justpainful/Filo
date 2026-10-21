#include "ui/settings_window.h"
#include "core/com.h"
#include "core/log.h"
#include "gfx/icon_packs.h"
#include "gfx/icons.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

namespace filo::ui {

using namespace filo::ds;

namespace {
SettingsWindow* g_win = nullptr;
std::wstring g_note;                       // last shortcut-capture problem, shown under the row

bool PickFolder(HWND owner, std::wstring& out) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreate(CLSID_FileOpenDialog, dlg))) return false;
  DWORD opts = 0; dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Choose the output folder");
  if (FAILED(dlg->Show(owner))) return false;
  ComPtr<IShellItem> it;
  if (FAILED(dlg->GetResult(&it))) return false;
  PWSTR p = nullptr;
  if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
  out = p; CoTaskMemFree(p);
  return true;
}

bool IsModifierVk(int vk) {
  return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
         vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}
std::wstring ShortcutText(const Shortcut& s) { return s.IsEmpty() ? L"None" : s.ToString(); }
}  // namespace

SettingsWindow* SettingsWindow::Show(bool show, int tab) {
  if (g_win) { if (IsWindow(g_win->Hwnd())) { ShowWindow(g_win->Hwnd(), SW_SHOWNORMAL); SetForegroundWindow(g_win->Hwnd()); } return g_win; }
  auto* d = new SettingsWindow();
  d->tab_ = tab;
  DialogOptions o;
  o.title = L"Settings"; o.widthDip = 560; o.heightDip = 640; o.resizable = true; o.minWidthDip = 480; o.minHeightDip = 460;
  o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
  POINT at{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
  d->onClosed = [d] { if (g_win == d) g_win = nullptr; delete d; };
  if (!d->Open(o, at, show)) { delete d; return nullptr; }
  g_win = d;
  return d;
}

// ---------------------------------------------------------------------------------------------- shortcut recording
void SettingsWindow::PollCapture(Shortcut* target, bool needsKey) {
  uint32_t mods = 0, vk = 0;
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= ModShift;
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= ModCtrl;
  if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= ModAlt;
  if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= ModWin;
  for (int k = 0x08; k < 0xFF && !vk; ++k) if (!IsModifierVk(k) && k != VK_ESCAPE && (GetAsyncKeyState(k) & 0x8000)) vk = (uint32_t)k;
  if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { capturing_ = 0; capMods_ = capVk_ = 0; g_note.clear(); return; }
  if (mods || vk) { capMods_ |= mods; if (vk) capVk_ = vk; return; }
  if (!capMods_ && !capVk_) return;                                    // nothing pressed yet
  // everything released: commit
  Shortcut n{capMods_, capVk_};
  capMods_ = capVk_ = 0; capturing_ = 0;
  if (needsKey ? (n.vk == 0 || n.mods == 0) : n.mods == 0) {
    g_note = needsKey ? L"Use at least one modifier plus a key (for example Ctrl+Alt+F)." : L"The wheel shortcut must include a modifier key (Shift, Ctrl, Alt or Win).";
    return;
  }
  *target = n; g_note.clear();
}

bool SettingsWindow::ShortcutRow(Ui& ui, const Rect& vp, float& y, const wchar_t* label, int slot, Shortcut& sc, bool needsKey) {
  const Palette& pal = ui.Pal(); Painter& p = ui.P();
  Rect row{vp.x, y, vp.w, 34};
  auto c = ui.Cols(row, {Flex(1), Fixed(176), Fixed(26)}, 8);
  TextStyle ls; ls.size = dialog::bodyFontSize;
  p.Text(c[0], label, pal.text, ls);
  const bool rec = capturing_ == slot;
  bool changed = false;
  ui.PushId(slot);
  Interaction it = ui.Behavior(c[1], ui.Id("cap"), true, true);
  const float a = ui.Anim(ui.Id("capa"), rec ? 1.f : (it.hot ? 0.5f : 0.f));
  p.FillRoundRect(c[1], control::buttonRadius, Color(pal.text.r, pal.text.g, pal.text.b, 0.07f + 0.05f * a));
  if (rec) p.StrokeRoundRect(c[1], control::buttonRadius, accent::base, 1.5f);
  TextStyle ts; ts.size = dialog::bodyFontSize; ts.align = Align::Center; ts.weight = 500;
  std::wstring shown = rec ? (capMods_ || capVk_ ? Shortcut{capMods_, capVk_}.ToString() + L" ..." : std::wstring(L"Press the keys...")) : ShortcutText(sc);
  p.Text(c[1], shown, rec ? accent::base : pal.text, ts);
  if (it.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
  if (it.clicked && !rec) { capturing_ = slot; lastNoteSlot_ = slot; capMods_ = capVk_ = 0; g_note.clear(); ui.RequestFrame(); }
  if (rec) {
    Shortcut before = sc;
    PollCapture(&sc, needsKey);
    changed = !(before == sc);
    ui.RequestFrame();
  }
  ui.PopId();
  y += 34;
  if (!g_note.empty() && (capturing_ == 0) && slot == lastNoteSlot_) {
    TextStyle ns; ns.size = dialog::smallFontSize; ns.wrap = true;
    p.Text({vp.x, y, vp.w, 18}, g_note, Color::Hex(0xB26A00), ns); y += 20;
  }
  return changed;
}

// ---------------------------------------------------------------------------------------------- tabs
namespace {
struct RowCtx {
  Ui& ui; const Rect& vp; float& y;
  Rect Label(const wchar_t* text, float h = 34) {
    Rect row{vp.x, y, vp.w, h}; y += h;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    ui.P().Text({row.x, row.y, row.w * 0.5f, row.h}, text, ui.Pal().text, ls);
    return row;
  }
  Rect Right(const Rect& row, float w) { return ui.Mirror({row.right() - w, row.y + (row.h - 26) / 2, w, 26}, row); }
  void Hint(const wchar_t* text) {
    TextStyle hs; hs.size = dialog::smallFontSize; hs.wrap = true;
    ui.P().Text({vp.x, y - 6, vp.w * 0.82f, 34}, text, ui.Pal().textTertiary, hs); y += 28;
  }
  void Heading(const wchar_t* text) { y += 10; ui.SectionTitle({vp.x, y, vp.w, 22}, text); y += 30; }
  bool Toggle(const wchar_t* text, bool& v, const wchar_t* hint = nullptr) {
    Rect row = Label(text);
    ui.PushId(ui.Id(std::wstring(text)));
    bool c = ui.Toggle(ui.Mirror({row.right() - 46, row.y + (row.h - 26) / 2, 46, 26}, row), v);
    ui.PopId();
    if (hint) Hint(hint);
    return c;
  }
};
}  // namespace

void SettingsWindow::General(Ui& ui, const Rect& vp, Settings& s, float& y) {
  RowCtx r{ui, vp, y};
  r.Heading(L"Startup");
  r.Toggle(L"Start Filo when I sign in", s.startWithWindows);
  r.Toggle(L"Drag-and-drop wheels", s.backgroundIntegration, L"Turn this off to keep Filo in the tray without watching drags.");
  r.Toggle(L"Show notifications", s.notifications);
  r.Heading(L"Keyboard");
  if (ShortcutRow(ui, vp, y, L"Open the wheel for the selected file", 4, s.invokeKeyboard, true)) {}
  r.Hint(L"Select a file in File Explorer, then press this shortcut.");
  r.Heading(L"Terminal");
  {
    Rect row = r.Label(L"Open Terminal with");
    static const std::vector<std::wstring> items{L"Windows Terminal", L"PowerShell", L"Command Prompt"};
    int sel = s.terminal == L"powershell" ? 1 : (s.terminal == L"cmd" ? 2 : 0);
    ui.PushId(0x7E87);
    if (ui.Dropdown(r.Right(row, 190), items, sel)) s.terminal = sel == 1 ? L"powershell" : (sel == 2 ? L"cmd" : L"wt");
    ui.PopId();
  }
  r.Heading(L"Language");
  {
    Rect row = r.Label(L"Interface direction");
    int sel = s.language == L"ar" ? 1 : 0;
    ui.PushId(0x1A6);
    int n = ui.Segmented(r.Right(row, 220), {L"Left to right", L"Right to left"}, sel);
    ui.PopId();
    s.language = n == 1 ? L"ar" : L"en";
    r.Hint(L"Mirrors window layouts the next time a window opens.");
  }
}

void SettingsWindow::Wheels(Ui& ui, const Rect& vp, Settings& s, float& y) {
  RowCtx r{ui, vp, y};
  r.Heading(L"Hold while dragging a file");
  Shortcut a = s.convertWheel, b = s.toolsWheel, c = s.actionsWheel;
  const bool ca = ShortcutRow(ui, vp, y, L"Convert wheel", 1, a, false);
  const bool cb = ShortcutRow(ui, vp, y, L"Tools wheel", 2, b, false);
  const bool cc = ShortcutRow(ui, vp, y, L"Actions wheel", 3, c, false);
  auto clash = [&](const Shortcut& x, const Shortcut& y2, const Shortcut& z) { return x == y2 || x == z; };
  if (ca) { if (clash(a, b, c)) { g_note = L"That combination already opens another wheel."; lastNoteSlot_ = 1; } else s.convertWheel = a; }
  if (cb) { if (clash(b, a, c)) { g_note = L"That combination already opens another wheel."; lastNoteSlot_ = 2; } else s.toolsWheel = b; }
  if (cc) { if (clash(c, a, b)) { g_note = L"That combination already opens another wheel."; lastNoteSlot_ = 3; } else s.actionsWheel = c; }
  r.Hint(L"Start dragging a file, then press the keys. Switching wheels while dragging works too.");
  r.Heading(L"Behaviour");
  r.Toggle(L"Keep the wheel open when I release the key", s.keepWheelOpenOnKeyRelease);
  {
    Rect row = r.Label(L"Drag distance before the wheel can open");
    double v = s.dragThresholdPx;
    ui.PushId(0xD7A);
    if (ui.Stepper(r.Right(row, 120), v, 0, 60, 1, 0, L"px")) s.dragThresholdPx = (int)v;
    ui.PopId();
    r.Hint(L"0 uses the Windows drag distance.");
  }
  {
    Rect row = r.Label(L"Wheel size");
    float v = s.wheelScale;
    ui.PushId(0x5CA1E);
    if (ui.Slider(ui.Mirror({row.right() - 190, row.y + 6, 190, 22}, row), v, 0.8f, 1.4f, 0.05f, 1.f)) s.wheelScale = v;
    ui.PopId();
  }
}

void SettingsWindow::Appearance(Ui& ui, const Rect& vp, Settings& s, float& y) {
  RowCtx r{ui, vp, y};
  r.Heading(L"Theme");
  {
    Rect row = r.Label(L"Colour scheme");
    ui.PushId(0x7E3E);
    s.theme = ui.Segmented(r.Right(row, 240), {L"System", L"Light", L"Dark"}, std::clamp(s.theme, 0, 2));
    ui.PopId();
  }
  r.Heading(L"Glass");
  {
    Rect row = r.Label(L"Glass style"); ui.PushId(0x61A56);
    s.glassStyle = ui.Segmented(r.Right(row, 300), {L"Liquid Glass", L"Soft Glass", L"Acrylic"}, std::clamp(s.glassStyle, 0, 2));
    ui.PopId();
  }
  if (s.glassStyle == 2) {
    Rect row{vp.x,y,vp.w,34};y+=36;ui.PushId(0xAC01);
    ui.SliderRow(row,L"Transparency",s.acrylicTransparency,0.f,1.f,.01f,.85f,
      [](float value){return std::to_wstring((int)std::lround(value*100))+L"%";},true,120,50);ui.PopId();
    row={vp.x,y,vp.w,34};y+=36;ui.PushId(0xAC02);
    ui.SliderRow(row,L"Background blur",s.acrylicBlur,0.f,48.f,1.f,30.f,
      [](float value){return std::to_wstring((int)std::lround(value))+L" px";},true,120,50);ui.PopId();
    r.Hint(L"Higher transparency reveals more desktop colour. Blur softens details without fading text.");
  }
  if (s.glassStyle == 0) {
    Rect row = r.Label(L"Reflections"); ui.PushId(0x61A57);
    ui.Slider(r.Right(row, 190), s.glassReflections, 0.f, 1.f, .05f, 1.f); ui.PopId();
    row = r.Label(L"Edge refraction"); ui.PushId(0x61A58);
    ui.Slider(r.Right(row, 190), s.glassRefraction, 0.f, 1.f, .05f, 1.f); ui.PopId();
  }
  if (s.glassStyle != 2) {
    Rect row = r.Label(L"Glass intensity");
    float v = s.glassIntensity;
    ui.PushId(0x61A55);
    if (ui.Slider(ui.Mirror({row.right() - 190, row.y + 6, 190, 22}, row), v, 0.f, 1.f, 0.05f, 1.f)) s.glassIntensity = v;
    ui.PopId();
  }
  r.Toggle(L"Reduce transparency", s.reduceTransparency, L"Uses solid surfaces instead of glass. Also follows the Windows setting.");
  r.Toggle(L"Reduce motion", s.reduceMotion);
  r.Heading(L"Icon packs");
  {
    const auto& packs=gfx::IconPacks::Get().All();
    auto& p=ui.P();const auto& pal=ui.Pal();
    const float gap=8, cardW=(vp.w-gap)/2;
    const char* previews[]={"image","edit","copy","terminal","share"};
    for(size_t i=0;i<packs.size();++i) {
      const auto& pack=packs[i];
      Rect card=ui.Mirror({vp.x+(i%2)*(cardW+gap),y+(i/2)*84.f,cardW,76},vp);
      ui.PushId(0x1C0);const auto id=ui.Id(pack.id);
      auto it=ui.Behavior(card,id,true,true);
      if(it.clicked) {s.iconPack=pack.id;ui.RequestFrame();}
      const bool active=gfx::IconPacks::Get().Resolve(s.iconPack).id==pack.id;
      const float hover=ui.Anim(id,it.hot?1.f:0.f);
      p.FillRoundRect(card,10,active?accent::base.WithAlpha(.10f):pal.text.WithAlpha(.035f+.035f*hover));
      p.StrokeRoundRect(card,10,active?accent::base:it.focused?pal.text:pal.divider.WithAlpha(.20f),active||it.focused?1.5f:1.f);
      TextStyle title;title.size=13;title.weight=active?600:500;
      p.Text({card.x+12,card.y+5,card.w-40,24},pack.name,pal.text,title);
      if(active)gfx::IconCache::Get().DrawPack(p.DC(),"classic","check",card.right()-18,card.y+17,14,p.Brush(accent::base).Get());
      for(size_t k=0;k<5;++k)gfx::IconCache::Get().DrawPack(p.DC(),pack.id,previews[k],card.x+23+k*(card.w-46)/4,card.y+51,24,p.Brush(pal.text).Get());
      if(it.hot)ui.SetCursor((int)(intptr_t)IDC_HAND);
      ui.PopId();
    }
    y+=((packs.size()+1)/2)*84.f;
    TextStyle note;note.size=11;note.wrap=true;
    p.Text({vp.x,y,vp.w,36},gfx::IconPacks::Get().Resolve(s.iconPack).description,pal.textSecondary,note);y+=38;
    if(ui.Button({vp.x,y,136,27},L"Open packs folder",ButtonStyle::Quiet)) {
      auto dir=AppDataDir()/L"icon-packs";std::error_code ec;fs::create_directories(dir,ec);
      if(!ec)ShellExecuteW(Hwnd(),L"open",dir.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    const bool reload=ui.Button({vp.x+144,y,110,27},L"Reload packs",ButtonStyle::Quiet);
    y+=32;
    // All references to registry entries have left scope before an explicit reload.
    if(reload) {gfx::IconPacks::Get().Reload();ui.RequestFrame();}
  }

}

void SettingsWindow::Files(Ui& ui, const Rect& vp, Settings& s, float& y) {
  RowCtx r{ui, vp, y};
  r.Heading(L"Where results go");
  {
    Rect row = r.Label(L"Save results");
    ui.PushId(0x10C);
    int n = ui.Segmented(r.Right(row, 300), {L"Next to the original", L"Fixed folder"}, s.outputLocation == OutputLocation::FixedFolder ? 1 : 0);
    ui.PopId();
    s.outputLocation = n == 1 ? OutputLocation::FixedFolder : OutputLocation::NextToOriginal;
  }
  if (s.outputLocation == OutputLocation::FixedFolder) {
    Rect row{vp.x, y, vp.w, 34}; y += 38;
    auto c = ui.Cols(row, {Flex(1), Fixed(96)}, 8);
    ui.PushId(0xF01D);
    ui.TextInput(c[0], s.fixedOutputFolder, L"Folder path");
    if (ui.Button(c[1], L"Browse...", ButtonStyle::Secondary)) { std::wstring f; if (PickFolder(Hwnd(), f)) s.fixedOutputFolder = f; }
    ui.PopId();
  }
  {
    Rect row = r.Label(L"Add to the file name");
    ui.PushId(0x5FF1);
    ui.TextInput(r.Right(row, 190), s.filenameSuffix, L"nothing");
    ui.PopId();
  }
  {
    Rect row = r.Label(L"If the file already exists");
    ui.PushId(0x0FE2);
    int n = ui.Segmented(r.Right(row, 220), {L"Keep both", L"Replace"}, s.overwritePolicy == OverwritePolicy::Overwrite ? 1 : 0);
    ui.PopId();
    s.overwritePolicy = n == 1 ? OverwritePolicy::Overwrite : OverwritePolicy::UniqueName;
    r.Hint(L"Originals are never modified. Results are written to a temporary file first and moved into place when finished.");
  }
  r.Heading(L"Default quality");
  {
    Rect row = r.Label(L"Image quality");
    double v = s.imageQuality;
    ui.PushId(0x1A9);
    if (ui.Stepper(r.Right(row, 120), v, 1, 100, 1, 0, L"%")) s.imageQuality = (int)v;
    ui.PopId();
  }
  {
    Rect row = r.Label(L"Video quality (lower is better)");
    double v = s.videoCrf;
    ui.PushId(0x1C2F);
    if (ui.Stepper(r.Right(row, 120), v, 14, 36, 1, 0, L"CRF")) s.videoCrf = (int)v;
    ui.PopId();
  }
  {
    Rect row = r.Label(L"Audio bitrate");
    static const int kRates[] = {96, 128, 160, 192, 256, 320};
    std::vector<std::wstring> items; int sel = 3;
    for (int i = 0; i < 6; ++i) { items.push_back(std::to_wstring(kRates[i]) + L" kbps"); if (kRates[i] == s.audioBitrateKbps) sel = i; }
    ui.PushId(0xA0D1);
    if (ui.Dropdown(r.Right(row, 140), items, sel)) s.audioBitrateKbps = kRates[sel];
    ui.PopId();
  }
  r.Toggle(L"Keep metadata (EXIF, tags) in results", s.preserveMetadata);
  r.Toggle(L"Use GPU video encoding when available", s.hardwareAcceleration);
  {
    Rect row = r.Label(L"Files processed at once");
    double v = s.concurrentJobs;
    ui.PushId(0xC0C);
    if (ui.Stepper(r.Right(row, 120), v, 1, 8, 1, 0)) s.concurrentJobs = (int)v;
    ui.PopId();
  }
  r.Heading(L"Tools");
  r.Toggle(L"Remember each tool's last settings", s.applyLastUsed);
  {
    Rect row = r.Label(L"Forget remembered tool settings");
    ui.PushId(0xF026E7);
    if (ui.Button(r.Right(row, 110), L"Forget", ButtonStyle::Secondary)) resetPresets_ = true;
    ui.PopId();
  }
}

void SettingsWindow::Draw(Ui& ui, const Rect& content, const Rect& body) {
  Settings s = SettingsStore::Get().Snapshot();
  const std::string before = s.ToJson().Dump();
  const float pad = dialog::contentInset + 8;
  Rect tabs{body.x + pad, content.y + 12, body.w - 2 * pad, 30};
  ui.PushId(0x5E77);
  const int nt = ui.Segmented(tabs, {L"General", L"Wheels", L"Appearance", L"Files"}, tab_);
  ui.PopId();
  if (nt != tab_) { tab_ = nt; capturing_ = 0; g_note.clear(); }
  Rect vp{body.x + pad, tabs.bottom() + 6, body.w - 2 * pad, std::max(80.f, body.bottom() - tabs.bottom() - 6 - 10)};
  const float off = ui.BeginScroll(vp, contentH_, ui.Id("settings-scroll"));
  float y = vp.y - off;
  Rect inner{vp.x, vp.y, vp.w - (contentH_ > vp.h ? 12.f : 0.f), vp.h};
  switch (tab_) {
    case 0: General(ui, inner, s, y); break;
    case 1: Wheels(ui, inner, s, y); break;
    case 2: Appearance(ui, inner, s, y); break;
    default: Files(ui, inner, s, y); break;
  }
  contentH_ = y - (vp.y - off) + 16;
  ui.EndScroll();
  const bool reset = resetPresets_; resetPresets_ = false;
  if (reset || s.ToJson().Dump() != before) {
    SettingsStore::Get().Update([&](Settings& x) {
      Json keep = x.toolPresets;
      x = s;
      x.toolPresets = reset ? Json::MakeObject() : keep;
    });
  }
}

}  // namespace filo::ui
