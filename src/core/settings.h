// Persistent user settings (JSON in %LOCALAPPDATA%\Filo\settings.json). Thread-safe snapshot/update API.
#pragma once
#include "core/json.h"
#include "core/util.h"
#include <mutex>
#include <functional>

namespace filo {

enum Mod : uint32_t { ModNone = 0, ModShift = 1, ModCtrl = 2, ModAlt = 4, ModWin = 8 };

struct Shortcut {
  uint32_t mods = 0;      // Mod flags that must be down
  uint32_t vk = 0;        // optional extra virtual key (0 = modifiers only)
  std::wstring ToString() const;
  bool IsEmpty() const { return mods == 0 && vk == 0; }
  bool operator==(const Shortcut& o) const { return mods == o.mods && vk == o.vk; }
};

enum class OverwritePolicy { UniqueName, Overwrite };
enum class OutputLocation { NextToOriginal, FixedFolder };

struct Settings {
  // General
  bool startWithWindows = false;
  bool startMinimized = true;
  bool backgroundIntegration = true;
  bool notifications = true;
  OutputLocation outputLocation = OutputLocation::NextToOriginal;
  std::wstring fixedOutputFolder;
  std::wstring filenameSuffix;                 // appended to the stem, e.g. " (converted)"; empty by default
  OverwritePolicy overwritePolicy = OverwritePolicy::UniqueName;
  bool preserveOriginals = true;               // never changes; kept visible for clarity
  // Keyboard & gestures
  Shortcut convertWheel{ModShift, 0};
  Shortcut toolsWheel{ModAlt, 0};
  Shortcut actionsWheel{ModCtrl, 0};
  Shortcut invokeKeyboard{ModCtrl | ModAlt, 'F'};   // opens the wheel for the selected Explorer items
  int dragThresholdPx = 0;                          // 0 = system default
  bool keepWheelOpenOnKeyRelease = true;
  // Appearance
  int theme = 1;                 // 0 system, 1 light (default: matches the reference), 2 dark
  std::string iconPack = "classic"; // Stable plugin ID; existing users retain the original icons.
  int glassStyle = 0;             // 0 Liquid Glass, 1 Soft Glass (0.1.2 material), 2 Windows Acrylic, 3 Glassy UI
  float glassReflections = 0.65f;
  float glassRefraction = 1.0f;
  float acrylicTransparency = .85f; // 0 opaque, 1 maximum colour transfer
  float acrylicBlur = 30.f;          // DIP, independently adjustable
  float glassIntensity = 1.0f;
  bool reduceTransparency = false;
  bool reduceMotion = false;
  float wheelScale = 1.0f;
  std::wstring language = L"en"; // en | ar
  // File processing
  int imageQuality = 90;         // 1..100 lossy default
  int videoCrf = 22;
  int audioBitrateKbps = 192;
  bool preserveMetadata = true;
  bool hardwareAcceleration = true;
  int concurrentJobs = 2;
  std::wstring terminal = L"wt";  // wt | powershell | cmd
  // Tool presets: toolId -> JSON object of last used options
  Json toolPresets = Json::MakeObject();
  bool applyLastUsed = true;

  Json ToJson() const;
  static Settings FromJson(const Json& j);
};

class SettingsStore {
 public:
  static SettingsStore& Get();
  Settings Snapshot();                                    // copy
  void Update(const std::function<void(Settings&)>& fn);  // mutate + persist + notify
  void Load();
  void Save();
  int AddListener(std::function<void(const Settings&)> cb);
  fs::path FilePath() const;
 private:
  SettingsStore() = default;
  std::mutex mu_;
  Settings s_;
  std::vector<std::pair<int, std::function<void(const Settings&)>>> listeners_;
  int next_ = 1;
};

} // namespace filo
