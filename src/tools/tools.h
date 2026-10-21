// Tool dialog launchers + registration into the ToolRegistry.
//
// HOW TO ADD A TOOL
//   1. Write src/tools/<name>.cpp with a class deriving ToolDialog (see join_videos.cpp / edit_photo.cpp) and a factory
//      `ToolDialog* Make<Name>(const Selection&)`.
//   2. In your group file define `FILO_TOOL_GROUP(<Group>) { add(ToolInfo{...}, &Make<Name>); ... }` (Group = General, Image,
//      Video, Audio or Pdf) registering the tools in the order they should appear on the wheel.
//   3. Applicability rules (`ToolInfo::accepts`) decide whether the tool is offered for a selection - never list a tool that
//      cannot work on it.
#pragma once
#include "registry/selection.h"
#include "tools/tool_dialog.h"
#include <filesystem>
#include <windows.h>

namespace filo::tools {

using MakeToolFn = ToolDialog* (*)(const Selection&);
using AddToolFn = void (*)(ToolInfo, MakeToolFn);

// Registers every tool (id, wheel label, icon, applicability rule, launcher) with ToolRegistry. Call once at startup.
void RegisterAllTools(HWND owner);
void AddTool(ToolInfo t, MakeToolFn make);


// Rule helpers
bool SelAllGroup(const Selection& s, Group g);
bool SelAllIn(const Selection& s, std::initializer_list<Group> gs);
std::wstring SelReason(std::wstring* why, const wchar_t* reason);

// Opens the tool hidden, applies `presetJson`, runs its job to completion and fills `out` (outputs/error/warnings). For E2E tests.
bool RunToolForTest(const std::string& toolId, const Selection& sel, const std::string& presetJson, double timeoutSec, JobResult* out);

// Renders a tool dialog hidden into a PNG (visual review / regression). Returns false for unknown tools or render failure.
bool RenderToolToPng(const std::string& toolId, const Selection& sel, const std::filesystem::path& png, const POINT* anchor = nullptr);
// Standalone native tool window, also useful for visual and keyboard QA without global hooks.
ToolDialog* OpenTool(const std::string& toolId, const Selection& sel);

// Built-in factories
ToolDialog* MakeJoinVideos(const Selection& sel);
ToolDialog* MakeEditPhoto(const Selection& sel);

ui::DialogWindow* OpenFloatShelf(const std::vector<fs::path>& files, bool expanded = false, bool show = true);
bool FloatShelfShot(const std::vector<fs::path>& files, const fs::path& png, bool expanded = false);   // hidden render of the Float shelf

} // namespace filo::tools

// Group registrations: C linkage so an undefined group falls back to an empty stub (see register_tools.cpp).
#define FILO_TOOL_GROUP(Name) extern "C" void FiloRegister##Name##Tools(::filo::tools::AddToolFn add)
FILO_TOOL_GROUP(General);
FILO_TOOL_GROUP(Image);
FILO_TOOL_GROUP(Video);
FILO_TOOL_GROUP(Audio);
FILO_TOOL_GROUP(Pdf);
