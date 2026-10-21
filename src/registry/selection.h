// A selection of files/folders plus the tool/action registries and the rule engine that decides what the wheels show.
#pragma once
#include "registry/capability.h"
#include <functional>
#include <set>

namespace filo {

struct Selection {
  std::vector<FileKind> items;
  bool empty() const { return items.empty(); }
  size_t count() const { return items.size(); }
  bool AllFiles() const;                        // no folders
  bool AnyFolder() const;
  bool SameGroup(Group* g = nullptr) const;     // every item belongs to one Group (folders never match)
  bool SameFormat(std::wstring* f = nullptr) const;
  uint64_t TotalSize() const;
  static Selection FromPaths(const std::vector<fs::path>& paths);
};

struct ToolInfo {
  std::string id;               // "compress", "edit-photo", "trim-video", ...
  std::wstring label;           // shown on the wheel ("Compress")
  std::wstring wheelLabel;      // short (<= 9 chars) variant; defaults to label
  std::string icon;
  std::string category;         // "general" | "image" | "video" | "audio" | "pdf"
  // true when the tool can work on the selection; `why` explains otherwise (shown as a tooltip when listed disabled).
  std::function<bool(const Selection&, std::wstring* why)> accepts;
  // Opens the tool UI / starts the job. Called on the UI thread. Null while the tool UI is not implemented => never offered.
  std::function<void(const Selection&)> launch;
};

struct ActionInfo {
  std::string id;               // "share", "float", "copy", "move", "copypath", "terminal"
  std::wstring label;
  std::string icon;
  std::function<bool(const Selection&, std::wstring* why)> accepts;
  std::function<void(const Selection&)> run;
};

class ToolRegistry {
 public:
  static ToolRegistry& Get();
  void AddTool(ToolInfo t);
  void AddAction(ActionInfo a);
  const std::vector<ToolInfo>& Tools() const { return tools_; }
  const std::vector<ActionInfo>& Actions() const { return actions_; }
  const ToolInfo* FindTool(const std::string& id) const;
  const ActionInfo* FindAction(const std::string& id) const;
 private:
  std::vector<ToolInfo> tools_;
  std::vector<ActionInfo> actions_;
};

struct WheelEntry {
  std::wstring id;       // "convert:webp" | "tool:compress" | "action:copy"
  std::wstring label;
  std::string icon;
  bool enabled = true;
  std::wstring why;      // reason when disabled
};

struct Availability {
  std::vector<WheelEntry> convert, tools, actions;
  std::wstring convertEmptyReason;   // "No conversions are available for mixed files" etc.
  std::wstring toolsEmptyReason;
};

// Rule engine. Never lists an operation that cannot run on this selection with the engines present on this machine.
Availability ComputeAvailability(const Selection& sel);

// Preferred clockwise ordering of output formats per input group (keeps the wheel stable between files).
const std::vector<std::wstring>& PreferredOutputOrder(Group g);

} // namespace filo
