// The background runtime: hidden message window, tray icon, global input observation, wheel controller, job feedback.
#pragma once
#include "shell/drag_session.h"
#include "shell/input_hooks.h"
#include "shell/tray.h"
#include "shell/wheel_controller.h"
#include "core/job.h"
#include "ui/progress_hud.h"
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace filo::app {

constexpr UINT WM_FILO_HOOK = WM_APP + 0x01;
constexpr UINT WM_FILO_THUMB = WM_APP + 0x02;
constexpr UINT WM_FILO_JOB = WM_APP + 0x03;
constexpr UINT WM_FILO_SETTINGS = WM_APP + 0x04;
constexpr UINT WM_FILO_OPENWHEEL = WM_APP + 0x05;

struct StartupArgs {
  bool quit = false;                       // --quit : ask the running instance to exit
  bool openSettings = false;               // --settings
  bool background = false;                 // --background (autostart): never show anything
  std::vector<std::wstring> wheelPaths;    // --wheel <paths...> (shell verbs / Send To)
  std::wstring wheelKind;                  // convert | tools | actions
};

class Runtime : public shell::IDragServices {
 public:
  Runtime();
  ~Runtime();
  int Run(const StartupArgs& args);
  std::function<void(HWND)> registerTools;      // set by main(): registers the tool dialogs (UI module)
  std::function<void()> onOpenSettings;         // set by main(): opens the settings window

  // IDragServices
  bool DragImageWindowExists() override;
  bool IsShellViewAt(POINT pt) override;
  std::vector<std::filesystem::path> PrefetchSelection(POINT pt) override;
  POINT CursorPos() override;
  void OpenWheel(wheel::WheelKind kind, POINT at, const std::vector<std::filesystem::path>& files) override;
  void SwitchWheel(wheel::WheelKind kind) override;
  void CloseWheel(bool animated) override;
  void ArmTimer(int id, unsigned ms) override;
  void KillTimer(int id) override;
  void SuppressMenuActivation(bool on) override;

 private:
  static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
  LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
  bool CreateMessageWindow();
  void ApplySettings();
  void RegisterHotkey();
  void Dispatch(const std::wstring& id, const Selection& sel, POINT at);
  void StartConversion(const Selection& sel, const std::wstring& outFmt);
  void OnJobUpdate(const JobInfo& info);
  void ShowTrayMenu(POINT pt);
  void InvokeFromKeyboard();
  void OpenWheelForPaths(const std::vector<fs::path>& paths, wheel::WheelKind kind);
  void ForwardArgsToRunning(const StartupArgs& a);
  void HandleCopyData(const std::wstring& payload);
  void UpdateTrayStatus();

  HWND hwnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  std::unique_ptr<shell::DragSession> session_;
  std::unique_ptr<shell::WheelController> wheel_;
  shell::TrayIcon tray_;
  int jobListener_ = 0, settingsListener_ = 0;
  bool integrationEnabled_ = true;
  std::vector<fs::path> pendingWheelPaths_;
  std::wstring pendingWheelKind_;
  struct Batch { int total = 0, ok = 0, failed = 0; std::vector<fs::path> outputs; std::wstring firstError; std::wstring label; };
  std::map<uint64_t, uint64_t> jobBatch_;       // job id -> batch id
  std::map<uint64_t, Batch> batches_;
  uint64_t nextBatch_ = 1;
  std::map<uint64_t, ui::ProgressHud*> huds_;   // batch id -> its HUD (nullptr entries are never stored)
  void UpdateHud(uint64_t batchId);
  std::set<uint64_t> activeJobs_;
  double lastProgress_ = 0;
};

} // namespace filo::app
