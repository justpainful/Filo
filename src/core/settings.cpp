#include "core/settings.h"
#include "core/log.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <cmath>

namespace filo {

std::wstring Shortcut::ToString() const {
  std::wstring s;
  if (mods & ModCtrl) s += L"Ctrl+";
  if (mods & ModAlt) s += L"Alt+";
  if (mods & ModShift) s += L"Shift+";
  if (mods & ModWin) s += L"Win+";
  if (vk) { wchar_t c = (wchar_t)MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR); if (c) s.push_back((wchar_t)towupper(c)); else s += L"VK" + std::to_wstring(vk); }
  else if (!s.empty()) s.pop_back();
  return s;
}

static Json ShortcutToJson(const Shortcut& s) { Json j = Json::MakeObject(); j["mods"] = (int)s.mods; j["vk"] = (int)s.vk; return j; }
static Shortcut ShortcutFromJson(const Json& j, Shortcut def) {
  if (!j.IsObject()) return def;
  return Shortcut{(uint32_t)j.Get("mods").AsInt(def.mods), (uint32_t)j.Get("vk").AsInt(def.vk)};
}

Json Settings::ToJson() const {
  Json j = Json::MakeObject();
  j["version"] = 1;
  j["startWithWindows"] = startWithWindows; j["startMinimized"] = startMinimized; j["backgroundIntegration"] = backgroundIntegration;
  j["notifications"] = notifications; j["outputLocation"] = (int)outputLocation; j["fixedOutputFolder"] = Narrow(fixedOutputFolder);
  j["filenameSuffix"] = Narrow(filenameSuffix); j["overwritePolicy"] = (int)overwritePolicy; j["preserveOriginals"] = preserveOriginals;
  j["convertWheel"] = ShortcutToJson(convertWheel); j["toolsWheel"] = ShortcutToJson(toolsWheel); j["actionsWheel"] = ShortcutToJson(actionsWheel);
  j["invokeKeyboard"] = ShortcutToJson(invokeKeyboard); j["dragThresholdPx"] = dragThresholdPx; j["keepWheelOpenOnKeyRelease"] = keepWheelOpenOnKeyRelease;
  j["iconPack"] = iconPack;
  j["acrylicTransparency"]=acrylicTransparency; j["acrylicBlur"]=acrylicBlur;
  j["glassStyle"] = glassStyle; j["glassReflections"] = glassReflections; j["glassRefraction"] = glassRefraction;
  j["theme"] = theme; j["glassIntensity"] = glassIntensity; j["reduceTransparency"] = reduceTransparency; j["reduceMotion"] = reduceMotion;
  j["wheelScale"] = wheelScale; j["language"] = Narrow(language);
  j["imageQuality"] = imageQuality; j["videoCrf"] = videoCrf; j["audioBitrateKbps"] = audioBitrateKbps; j["preserveMetadata"] = preserveMetadata;
  j["hardwareAcceleration"] = hardwareAcceleration; j["concurrentJobs"] = concurrentJobs; j["terminal"] = Narrow(terminal);
  j["toolPresets"] = toolPresets; j["applyLastUsed"] = applyLastUsed;
  return j;
}

Settings Settings::FromJson(const Json& j) {
  Settings s;
  if (!j.IsObject()) return s;
  auto B = [&](const char* k, bool d) { return j.Has(k) ? j.Get(k).AsBool(d) : d; };
  auto I = [&](const char* k, int d) { return j.Has(k) ? (int)j.Get(k).AsInt(d) : d; };
  auto F = [&](const char* k, float d) { return j.Has(k) ? (float)j.Get(k).AsNumber(d) : d; };
  auto S = [&](const char* k, const std::wstring& d) { return j.Has(k) ? Widen(j.Get(k).AsString()) : d; };
  s.startWithWindows = B("startWithWindows", s.startWithWindows); s.startMinimized = B("startMinimized", s.startMinimized);
  s.backgroundIntegration = B("backgroundIntegration", s.backgroundIntegration); s.notifications = B("notifications", s.notifications);
  s.outputLocation = (OutputLocation)std::clamp(I("outputLocation", 0), 0, 1); s.fixedOutputFolder = S("fixedOutputFolder", s.fixedOutputFolder);
  s.filenameSuffix = S("filenameSuffix", s.filenameSuffix); s.overwritePolicy = (OverwritePolicy)std::clamp(I("overwritePolicy", 0), 0, 1);
  s.convertWheel = ShortcutFromJson(j.Get("convertWheel"), s.convertWheel); s.toolsWheel = ShortcutFromJson(j.Get("toolsWheel"), s.toolsWheel);
  s.actionsWheel = ShortcutFromJson(j.Get("actionsWheel"), s.actionsWheel); s.invokeKeyboard = ShortcutFromJson(j.Get("invokeKeyboard"), s.invokeKeyboard);
  s.dragThresholdPx = I("dragThresholdPx", 0); s.keepWheelOpenOnKeyRelease = B("keepWheelOpenOnKeyRelease", true);
  s.iconPack = j.Get("iconPack").StringOr("classic");
  if (s.iconPack.empty() || s.iconPack.size()>48) s.iconPack="classic";
  s.acrylicTransparency=F("acrylicTransparency",.85f);
  s.acrylicBlur=F("acrylicBlur",30.f);
  s.acrylicTransparency=std::isfinite(s.acrylicTransparency)?std::clamp(s.acrylicTransparency,0.f,1.f):.85f;
  s.acrylicBlur=std::isfinite(s.acrylicBlur)?std::clamp(s.acrylicBlur,0.f,48.f):30.f;
  s.glassStyle = std::clamp(I("glassStyle", 0), 0, 3);
  s.glassReflections = std::clamp(F("glassReflections", .65f), 0.f, 1.f);
  s.glassRefraction = std::clamp(F("glassRefraction", 1.f), 0.f, 1.f);
  s.theme = std::clamp(I("theme", 1), 0, 2); s.glassIntensity = std::clamp(F("glassIntensity", 1.f), 0.f, 1.f);
  s.reduceTransparency = B("reduceTransparency", false); s.reduceMotion = B("reduceMotion", false);
  s.wheelScale = std::clamp(F("wheelScale", 1.f), 0.6f, 2.0f); s.language = S("language", L"en");
  s.imageQuality = std::clamp(I("imageQuality", 90), 1, 100); s.videoCrf = std::clamp(I("videoCrf", 22), 0, 51);
  s.audioBitrateKbps = std::clamp(I("audioBitrateKbps", 192), 32, 512); s.preserveMetadata = B("preserveMetadata", true);
  s.hardwareAcceleration = B("hardwareAcceleration", true); s.concurrentJobs = std::clamp(I("concurrentJobs", 2), 1, 8);
  s.terminal = S("terminal", L"wt");
  if (j.Get("toolPresets").IsObject()) s.toolPresets = j.Get("toolPresets");
  s.applyLastUsed = B("applyLastUsed", true);
  return s;
}

SettingsStore& SettingsStore::Get() { static SettingsStore* s = new SettingsStore(); return *s; }
fs::path SettingsStore::FilePath() const { return AppDataDir() / L"settings.json"; }

Settings SettingsStore::Snapshot() { std::lock_guard lk(mu_); return s_; }

void SettingsStore::Load() {
  std::lock_guard lk(mu_);
  std::ifstream f(FilePath(), std::ios::binary);
  if (!f) return;
  std::stringstream ss; ss << f.rdbuf();
  std::string err;
  auto j = Json::Parse(ss.str(), &err);
  if (!j) { FILO_LOG(Warn, "settings", "settings.json is corrupt (%s); using defaults", err.c_str()); return; }
  s_ = Settings::FromJson(*j);
}

void SettingsStore::Save() {
  Json j;
  { std::lock_guard lk(mu_); j = s_.ToJson(); }
  // write atomically: temp + rename
  fs::path p = FilePath(), tmp = p; tmp += L".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << j.Dump(true); }
  std::error_code ec; fs::rename(tmp, p, ec);
  if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
}

void SettingsStore::Update(const std::function<void(Settings&)>& fn) {
  Settings snap;
  std::vector<std::function<void(const Settings&)>> cbs;
  { std::lock_guard lk(mu_); fn(s_); snap = s_; for (auto& l : listeners_) cbs.push_back(l.second); }
  Save();
  for (auto& cb : cbs) cb(snap);
}

int SettingsStore::AddListener(std::function<void(const Settings&)> cb) {
  std::lock_guard lk(mu_); listeners_.emplace_back(next_, std::move(cb)); return next_++;
}

} // namespace filo
