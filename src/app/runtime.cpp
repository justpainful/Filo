#include "app/runtime.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/output.h"
#include "design/theme.h"
#include "registry/capability.h"
#include "registry/selection.h"
#include "shell/actions.h"
#include "shell/explorer.h"
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>

namespace filo::app {

using wheel::WheelKind;

static constexpr wchar_t kWndClass[] = L"Filo.Runtime";
static constexpr UINT WM_FILO_DISPATCH = WM_APP + 0x06;
static constexpr UINT_PTR kTimerWheelPaths = 100, kTimerTrayStatus = 101;
static constexpr int kHotkeyId = 1;
static constexpr int IDI_APP_ICON = 101;

enum MenuId { ID_TOGGLE = 1, ID_SETTINGS, ID_LOGS, ID_QUIT, ID_CONVERT_WHEEL, ID_TOOLS_WHEEL, ID_ACTIONS_WHEEL };

Runtime::Runtime() = default;
Runtime::~Runtime() = default;

// ------------------------------------------------------------------------------------------------ IDragServices
bool Runtime::DragImageWindowExists() { return FindWindowW(L"SysDragImage", nullptr) != nullptr; }
bool Runtime::IsShellViewAt(POINT pt) { return shell::IsShellViewWindow(WindowFromPoint(pt)); }
std::vector<std::filesystem::path> Runtime::PrefetchSelection(POINT pt) { return shell::QueryExplorerAtPoint(pt).selection; }
POINT Runtime::CursorPos() { POINT p{}; GetCursorPos(&p); return p; }
void Runtime::OpenWheel(WheelKind k, POINT at, const std::vector<std::filesystem::path>& files) { wheel_->Open(k, at, files, false); }
void Runtime::SwitchWheel(WheelKind k) { wheel_->Switch(k); }
void Runtime::CloseWheel(bool animated) { wheel_->Close(animated); }
void Runtime::ArmTimer(int id, unsigned ms) { ::SetTimer(hwnd_, (UINT_PTR)id, ms, nullptr); }
void Runtime::KillTimer(int id) { ::KillTimer(hwnd_, (UINT_PTR)id); }
void Runtime::SuppressMenuActivation(bool on) { shell::InputHooks::Get().SetSuppressMenuActivation(on); }

// ------------------------------------------------------------------------------------------------ window
bool Runtime::CreateMessageWindow() {
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = &Runtime::WndProcThunk;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWndClass;
  RegisterClassExW(&wc);
  // A real (hidden, top-level) window - not HWND_MESSAGE - so it receives TaskbarCreated / WM_SETTINGCHANGE broadcasts.
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, L"Filo", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  return hwnd_ != nullptr;
}

LRESULT CALLBACK Runtime::WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
  Runtime* self = nullptr;
  if (m == WM_NCCREATE) {
    self = static_cast<Runtime*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) self->hwnd_ = h;
  } else self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->WndProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void Runtime::ApplySettings() {
  Settings s = SettingsStore::Get().Snapshot();
  ds::ThemeState ts;
  ts.mode = s.theme == 1 ? ds::ThemeMode::Light : (s.theme == 2 ? ds::ThemeMode::Dark : ds::ThemeMode::System);
  ts.acrylicTransparency=s.acrylicTransparency; ts.acrylicBlur=s.acrylicBlur; ts.iconPack = s.iconPack; ts.glassStyle = s.glassStyle; ts.glassReflections = s.glassReflections; ts.glassRefraction = s.glassRefraction;
  ts.glassIntensity = s.glassIntensity; ts.reduceTransparency = s.reduceTransparency; ts.reduceMotion = s.reduceMotion; ts.wheelScale = s.wheelScale;
  ds::Theme::Get().SetUserState(ts);
  // Apply appearance changes to already-open tool windows and the live wheel.
  EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (wcscmp(name, L"Filo.Surface") == 0) PostMessageW(window, WM_THEMECHANGED, 0, 0);
    return TRUE;
  }, 0);
  session_->SetShortcuts(s.convertWheel, s.toolsWheel, s.actionsWheel, s.keepWheelOpenOnKeyRelease);
  session_->SetEnabled(s.backgroundIntegration && integrationEnabled_);
  shell::InputHooks::Get().SetDragThreshold(s.dragThresholdPx);
  JobManager::Get().SetMaxConcurrent(s.concurrentJobs);
  RegisterHotkey();
  // autostart
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
    if (s.startWithWindows) {
      std::wstring v = QuoteArg(ExePath().wstring()) + L" --background";
      RegSetValueExW(k, L"Filo", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"Filo");
    RegCloseKey(k);
  }
}

void Runtime::RegisterHotkey() {
  UnregisterHotKey(hwnd_, kHotkeyId);
  Settings s = SettingsStore::Get().Snapshot();
  const Shortcut& sc = s.invokeKeyboard;
  if (sc.vk == 0) return;
  UINT mods = MOD_NOREPEAT;
  if (sc.mods & ModAlt) mods |= MOD_ALT;
  if (sc.mods & ModCtrl) mods |= MOD_CONTROL;
  if (sc.mods & ModShift) mods |= MOD_SHIFT;
  if (sc.mods & ModWin) mods |= MOD_WIN;
  if (!RegisterHotKey(hwnd_, kHotkeyId, mods, sc.vk))
    FILO_LOG(Warn, "runtime", "could not register the keyboard shortcut %ls (in use by another app)", sc.ToString().c_str());
}

// ------------------------------------------------------------------------------------------------ dispatch
struct DispatchPayload { std::wstring id; Selection sel; POINT at; };

void Runtime::Dispatch(const std::wstring& id, const Selection& sel, POINT at) {
  if (id.rfind(L"convert:", 0) == 0) { StartConversion(sel, id.substr(8)); return; }
  if (id.rfind(L"tool:", 0) == 0) {
    if (const ToolInfo* t = ToolRegistry::Get().FindTool(Narrow(id.substr(5)))) { if (t->launch) t->launch(sel); }
    return;
  }
  if (id.rfind(L"action:", 0) == 0) {
    if (const ActionInfo* a = ToolRegistry::Get().FindAction(Narrow(id.substr(7)))) { if (a->run) a->run(sel); }
    return;
  }
  FILO_LOG(Warn, "runtime", "unknown wheel entry id");
}

static std::wstring UpperFmt(std::wstring f) { CharUpperBuffW(f.data(), (DWORD)f.size()); return f; }

void Runtime::StartConversion(const Selection& sel, const std::wstring& outFmt) {
  const uint64_t batchId = nextBatch_++;
  Batch& b = batches_[batchId];
  b.label = UpperFmt(outFmt);
  for (const FileKind& item : sel.items) {
    const Capability* cap = CapabilityRegistry::Get().Find(item.format, outFmt);
    if (!cap || !cap->Runnable()) {
      ++b.total; ++b.failed;
      if (b.firstError.empty()) b.firstError = item.path.filename().wstring() + L" cannot be converted to " + b.label + L".";
      continue;
    }
    Capability capCopy = *cap;
    fs::path input = item.path;
    JobSpec spec;
    spec.operation = L"convert";
    spec.title = input.filename().wstring() + L" \x2192 " + b.label;
    spec.inputs = {input};
    spec.work = [capCopy, input](JobContext& ctx) { return RunConversion(capCopy, input, Json::MakeObject(), ctx); };
    ++b.total;
    uint64_t jid = JobManager::Get().Submit(std::move(spec));
    jobBatch_[jid] = batchId;
    activeJobs_.insert(jid);
  }
  if (b.total > b.failed) {
    POINT cur{}; GetCursorPos(&cur);
    if (auto* hud = ui::ProgressHud::Create(cur)) {
      huds_[batchId] = hud;
      const std::wstring name = sel.items.size() == 1 ? sel.items[0].path.filename().wstring() : std::to_wstring(sel.items.size()) + L" files";
      hud->SetTexts(L"Converting to " + b.label, name);
      hud->onCancel = [this, batchId] { for (auto& [jid, bid] : jobBatch_) if (bid == batchId) JobManager::Get().Cancel(jid); };
      hud->onGone = [this, batchId] { huds_.erase(batchId); };
    }
  }
  if (b.total == b.failed) {         // nothing could even start
    if (SettingsStore::Get().Snapshot().notifications) tray_.Balloon(L"Filo", b.firstError, true);
    batches_.erase(batchId);
  }
  UpdateTrayStatus();
}

void Runtime::UpdateHud(uint64_t batchId) {
  auto h = huds_.find(batchId);
  auto b = batches_.find(batchId);
  if (h == huds_.end() || b == batches_.end() || b->second.total <= 0) return;
  double sum = b->second.ok + b->second.failed;
  for (auto& [jid, bid] : jobBatch_) if (bid == batchId) if (auto i = JobManager::Get().Info(jid)) sum += i->progress;
  h->second->SetProgress((float)(sum / b->second.total));
}

void Runtime::OnJobUpdate(const JobInfo& info) {
  if (!info.Finished()) {
    auto bi = jobBatch_.find(info.id);
    if (bi != jobBatch_.end()) UpdateHud(bi->second);
    UpdateTrayStatus();
    return;
  }
  activeJobs_.erase(info.id);
  auto bi = jobBatch_.find(info.id);
  if (bi != jobBatch_.end()) {
    Batch& b = batches_[bi->second];
    if (info.state == JobState::Completed) {
      ++b.ok;
      for (auto& o : info.result.outputs) b.outputs.push_back(o);
      if (!info.result.warnings.empty() && b.firstError.empty() && b.total == 1) b.firstError = L"";   // warnings shown below
    } else if (info.state == JobState::Failed) {
      ++b.failed;
      if (b.firstError.empty()) b.firstError = info.inputs.empty() ? info.result.error : info.inputs[0].filename().wstring() + L": " + info.result.error;
    } else { ++b.failed; }
    const uint64_t batchId = bi->second;
    jobBatch_.erase(bi);
    if (b.ok + b.failed >= b.total) {
      Settings st = SettingsStore::Get().Snapshot();
      auto hud = huds_.find(batchId);
      if (hud != huds_.end()) {
        fs::path first = b.outputs.empty() ? fs::path() : b.outputs[0];
        if (b.failed == 0) hud->second->Finish(ui::ProgressHud::State::Done, b.total == 1 ? L"Converted to " + b.label : L"Converted " + std::to_wstring(b.ok) + L" files",
                                              b.total == 1 && !first.empty() ? first.filename().wstring() : L"Click to show in Explorer", first);
        else hud->second->Finish(ui::ProgressHud::State::Failed, b.ok ? L"Some files failed" : L"Conversion failed", b.firstError.substr(0, 160));
      } else if (st.notifications) {
        if (b.failed == 0) {
          std::wstring text = b.total == 1 && !b.outputs.empty() ? b.outputs[0].filename().wstring() : std::to_wstring(b.ok) + L" files converted";
          if (b.total == 1 && !info.result.warnings.empty()) text += L"\n" + info.result.warnings[0];
          fs::path first = b.outputs.empty() ? fs::path() : b.outputs[0];
          tray_.Balloon(b.total == 1 ? L"Converted to " + b.label : L"Conversion finished", text, false,
                        [first] { if (!first.empty()) shell::RevealInExplorer(first); });
        } else if (!b.firstError.empty()) {          // (a cancelled batch has no error: nothing to announce)
          std::wstring text = b.firstError.substr(0, 180);
          if (b.ok) text += L"\n" + std::to_wstring(b.ok) + L" file(s) succeeded.";
          tray_.Balloon(L"Conversion failed", text, true);
        }
      }
      batches_.erase(batchId);
    }
  }
  UpdateTrayStatus();
  JobManager::Get().PurgeFinished(40);
}

void Runtime::UpdateTrayStatus() {
  if (activeJobs_.empty()) { tray_.SetTooltip(integrationEnabled_ ? L"Filo" : L"Filo (paused)"); return; }
  double sum = 0; int n = 0;
  for (auto id : activeJobs_) if (auto i = JobManager::Get().Info(id)) { sum += i->progress; ++n; }
  int pct = n ? (int)(sum / n * 100) : 0;
  tray_.SetTooltip(L"Filo - working on " + std::to_wstring(activeJobs_.size()) + L" file(s) - " + std::to_wstring(pct) + L"%");
}

// ------------------------------------------------------------------------------------------------ invocation paths
void Runtime::OpenWheelForPaths(const std::vector<fs::path>& paths, WheelKind kind) {
  POINT cur{}; GetCursorPos(&cur);
  wheel_->Open(kind, cur, paths, true);
}

void Runtime::InvokeFromKeyboard() {
  auto ctx = shell::QueryForegroundExplorer();
  if (!ctx.valid || ctx.selection.empty()) {
    tray_.Balloon(L"Filo", L"Select a file in File Explorer first, then press the shortcut.", false);
    return;
  }
  OpenWheelForPaths(ctx.selection, WheelKind::Convert);
}

void Runtime::HandleCopyData(const std::wstring& payload) {
  // payload: "<kind>\n<path>\n<path>..."
  auto parts = Split(payload, L'\n');
  if (parts.empty()) return;
  pendingWheelKind_ = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) if (!parts[i].empty()) pendingWheelPaths_.emplace_back(parts[i]);
  ::SetTimer(hwnd_, kTimerWheelPaths, 180, nullptr);       // Explorer starts one process per selected file: collect them
}

void Runtime::ForwardArgsToRunning(const StartupArgs& a) {
  HWND other = FindWindowW(kWndClass, nullptr);
  if (!other) return;
  std::wstring payload = a.wheelKind.empty() ? L"convert" : a.wheelKind;
  for (auto& p : a.wheelPaths) payload += L"\n" + p;
  COPYDATASTRUCT cds{};
  cds.dwData = 0xF110;
  cds.cbData = (DWORD)((payload.size() + 1) * sizeof(wchar_t));
  cds.lpData = payload.data();
  AllowSetForegroundWindow(ASFW_ANY);
  SendMessageTimeoutW(other, WM_COPYDATA, (WPARAM)hwnd_, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, nullptr);
}

void Runtime::ShowTrayMenu(POINT pt) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (integrationEnabled_ ? MF_CHECKED : 0), ID_TOGGLE, L"Enable drag wheels");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings...");
  AppendMenuW(m, MF_STRING, ID_LOGS, L"Open logs folder");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit Filo");
  SetForegroundWindow(hwnd_);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case ID_TOGGLE:
      integrationEnabled_ = !integrationEnabled_;
      session_->SetEnabled(integrationEnabled_ && SettingsStore::Get().Snapshot().backgroundIntegration);
      UpdateTrayStatus();
      break;
    case ID_SETTINGS: PostMessageW(hwnd_, WM_FILO_SETTINGS, 0, 0); break;
    case ID_LOGS: ShellExecuteW(nullptr, L"open", LogDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_QUIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
  }
}

LRESULT Runtime::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  using shell::InputEvent;
  if (m == taskbarCreated_ && taskbarCreated_) { tray_.Recreate(); return 0; }
  switch (m) {
    case WM_FILO_HOOK: {
      switch (shell::EventOf(w)) {
        case InputEvent::LeftDown: session_->OnLeftDown(shell::PointOf(l)); break;
        case InputEvent::DragMoved: session_->OnDragMoved(); break;
        case InputEvent::LeftUp: session_->OnLeftUp(); break;
        case InputEvent::ModsChanged: session_->OnModsChanged(shell::ModsOf(w)); break;
        case InputEvent::KeyDown: session_->OnKeyDown(shell::VkOf(w)); break;
      }
      return 0; }
    case WM_FILO_THUMB: {
      auto* sp = reinterpret_cast<std::shared_ptr<shell::ThumbnailImage>*>(l);
      if (sp) { wheel_->OnThumbnailReady((uint64_t)w, *sp); delete sp; }
      return 0; }
    case WM_FILO_JOB: {
      auto* info = reinterpret_cast<JobInfo*>(l);
      if (info) { OnJobUpdate(*info); delete info; }
      return 0; }
    case WM_FILO_DISPATCH: {
      std::unique_ptr<DispatchPayload> p(reinterpret_cast<DispatchPayload*>(l));
      Dispatch(p->id, p->sel, p->at);
      return 0; }
    case WM_FILO_SETTINGS:
      // The settings window lives in the UI module; until it is linked in, fall back to the settings file.
      if (onOpenSettings) onOpenSettings(); else ShellExecuteW(nullptr, L"open", SettingsStore::Get().FilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return 0;
    case WM_TIMER:
      if (w == kTimerWheelPaths) {
        ::KillTimer(h, kTimerWheelPaths);
        WheelKind k = pendingWheelKind_ == L"tools" ? WheelKind::Tools : (pendingWheelKind_ == L"actions" ? WheelKind::Actions : WheelKind::Convert);
        auto paths = std::move(pendingWheelPaths_); pendingWheelPaths_.clear();
        if (!paths.empty()) OpenWheelForPaths(paths, k);
      } else if (w == (UINT_PTR)shell::DragSession::kSettle || w == (UINT_PTR)shell::DragSession::kGrace || w == (UINT_PTR)shell::DragSession::kProbe) {
        session_->OnTimer((int)w);
      }
      return 0;
    case WM_HOTKEY: if (w == (WPARAM)kHotkeyId) InvokeFromKeyboard(); return 0;
    case WM_COPYDATA: {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
      if (cds && cds->dwData == 0xF110 && cds->lpData) HandleCopyData(std::wstring((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t) - 1));
      return TRUE; }
    case shell::TrayIcon::kCallbackMessage:
      tray_.HandleMessage(w, l, [this](POINT pt) { ShowTrayMenu(pt); }, [this] { PostMessageW(hwnd_, WM_FILO_SETTINGS, 0, 0); });
      return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_DISPLAYCHANGE:
      ds::Theme::Get().RefreshFromSystem();
      return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------------------------------------ run
int Runtime::Run(const StartupArgs& args) {
  // single instance: a second launch forwards its request and exits
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Filo.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (args.quit) { HWND o = FindWindowW(kWndClass, nullptr); if (o) PostMessageW(o, WM_CLOSE, 0, 0); }
    else if (!args.wheelPaths.empty()) ForwardArgsToRunning(args);
    else if (args.openSettings) { HWND o = FindWindowW(kWndClass, nullptr); if (o) PostMessageW(o, WM_FILO_SETTINGS, 0, 0); }
    if (mutex) CloseHandle(mutex);
    return 0;
  }
  if (args.quit) { if (mutex) CloseHandle(mutex); return 0; }

  SettingsStore::Get().Load();
  RegisterAllEngines();
  if (!CreateMessageWindow()) { FILO_LOG(Error, "runtime", "message window failed"); return 1; }
  shell::RegisterShellActions(hwnd_);
  if (registerTools) registerTools(hwnd_);

  session_ = std::make_unique<shell::DragSession>(this);
  wheel_ = std::make_unique<shell::WheelController>();
  if (!wheel_->Init(hwnd_, WM_FILO_THUMB)) FILO_LOG(Error, "runtime", "wheel failed to initialise (no GPU?)");
  wheel_->onChoose = [this](const std::wstring& id, const Selection& sel, POINT at) {
    // Never run work inside the OLE Drop() callback: Explorer's drag loop is blocked until we return.
    PostMessageW(hwnd_, WM_FILO_DISPATCH, 0, (LPARAM)new DispatchPayload{id, sel, at});
  };
  wheel_->onDropHandled = [this] { session_->OnDropHandled(); };
  wheel_->onClosed = [this] { session_->OnWheelClosedExternally(); };

  HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  tray_.Create(hwnd_, icon, L"Filo");

  jobListener_ = JobManager::Get().AddListener([this](const JobInfo& info) { PostMessageW(hwnd_, WM_FILO_JOB, 0, (LPARAM)new JobInfo(info)); });
  settingsListener_ = SettingsStore::Get().AddListener([this](const Settings&) { PostMessageW(hwnd_, WM_FILO_SETTINGS + 100, 0, 0); });
  ApplySettings();

  if (!shell::InputHooks::Get().Start(hwnd_, WM_FILO_HOOK))
    tray_.Balloon(L"Filo", L"Drag wheels are unavailable: global input observation could not start.", true);

  if (!args.wheelPaths.empty()) { pendingWheelPaths_ = [&] { std::vector<fs::path> v; for (auto& p : args.wheelPaths) v.emplace_back(p); return v; }(); pendingWheelKind_ = args.wheelKind; ::SetTimer(hwnd_, kTimerWheelPaths, 50, nullptr); }
  if (args.openSettings) PostMessageW(hwnd_, WM_FILO_SETTINGS, 0, 0);

  FILO_LOG(Info, "runtime", "Filo %s running (hooks=%d)", FILO_VERSION, shell::InputHooks::Get().Running() ? 1 : 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_FILO_SETTINGS + 100) { ApplySettings(); continue; }
    TranslateMessage(&msg); DispatchMessageW(&msg);
  }

  shell::InputHooks::Get().Stop();
  JobManager::Get().RemoveListener(jobListener_);
  JobManager::Get().Shutdown();
  UnregisterHotKey(hwnd_, kHotkeyId);
  tray_.Destroy();
  wheel_->Shutdown();
  wheel_.reset();
  shell::ThumbnailService::Get().Shutdown();
  if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
  return 0;
}

} // namespace filo::app
