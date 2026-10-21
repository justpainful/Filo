#include "tools/tools.h"
#include "registry/selection.h"
#include <map>

// Each tool group lives in its own file(s) and exposes one Register<Group>Tools function. Until a group is implemented the weak
// stub below keeps the build green (the group simply contributes no tools).
#define FILO_WEAK_GROUP(name) __pragma(comment(linker, "/alternatename:FiloRegister" #name "Tools=FiloRegister" #name "ToolsStub"))
FILO_WEAK_GROUP(General)
FILO_WEAK_GROUP(Image)
FILO_WEAK_GROUP(Video)
FILO_WEAK_GROUP(Audio)
FILO_WEAK_GROUP(Pdf)

extern "C" {
void FiloRegisterGeneralToolsStub(filo::tools::AddToolFn) {}
void FiloRegisterImageToolsStub(filo::tools::AddToolFn) {}
void FiloRegisterVideoToolsStub(filo::tools::AddToolFn) {}
void FiloRegisterAudioToolsStub(filo::tools::AddToolFn) {}
void FiloRegisterPdfToolsStub(filo::tools::AddToolFn) {}
}

namespace filo::tools {

static std::map<std::string, ToolDialog* (*)(const Selection&)>& Factories() { static std::map<std::string, ToolDialog* (*)(const Selection&)> m; return m; }

void AddTool(ToolInfo t, MakeToolFn make) {
  Factories()[t.id] = make;
  t.launch = [make](const Selection& s) { ToolDialog::OpenDialog(make(s)); };
  ToolRegistry::Get().AddTool(std::move(t));
}

bool RenderToolToPng(const std::string& id, const Selection& sel, const std::filesystem::path& png, const POINT* anchor) {
  auto it = Factories().find(id);
  if (it == Factories().end()) return false;
  ToolDialog* d = it->second(sel);
  POINT at = anchor ? *anchor : POINT{800, 500};
  bool ok = false;
  if (ToolDialog::OpenDialog(d, false, &at)) { ok = d->RenderToPng(png); d->CloseWindow(); }
  return ok;
}

ToolDialog* OpenTool(const std::string& id, const Selection& sel) {
  auto it = Factories().find(id);
  if (it == Factories().end()) return nullptr;
  const ToolInfo* tool = ToolRegistry::Get().FindTool(id);
  std::wstring reason;
  if (!tool || (tool->accepts && !tool->accepts(sel, &reason))) return nullptr;
  ToolDialog* d = it->second(sel);
  return ToolDialog::OpenDialog(d) ? d : nullptr;
}

bool RunToolForTest(const std::string& id, const Selection& sel, const std::string& presetJson, double timeoutSec, JobResult* out) {
  auto it = Factories().find(id);
  if (it == Factories().end()) return false;
  ToolDialog* d = it->second(sel);
  POINT at{800, 500};
  Json preset = Json::Parse(presetJson).value_or(Json::MakeObject());
  bool ok = false;
  if (ToolDialog::OpenDialog(d, false, &at)) { ok = d->RunForTest(preset, timeoutSec, out); d->CloseWindow(); }
  return ok;
}

// ---- rules shared by the registrations
bool SelAllGroup(const Selection& s, Group g) {
  if (s.empty()) return false;
  for (auto& i : s.items) if (i.group != g) return false;
  return true;
}
bool SelAllIn(const Selection& s, std::initializer_list<Group> gs) {
  if (s.empty()) return false;
  for (auto& i : s.items) { bool ok = false; for (auto g : gs) if (i.group == g) ok = true; if (!ok) return false; }
  return true;
}
std::wstring SelReason(std::wstring* why, const wchar_t* r) { if (why) *why = r; return r; }

void FloatFiles(const Selection& sel);   // float_shelf.cpp

void RegisterAllTools(HWND) {
  ToolRegistry::Get().AddAction({"float", L"Float", "float",
    [](const Selection& s, std::wstring* why) { if (s.empty()) { if (why) *why = L"Nothing selected."; return false; } return true; },
    [](const Selection& s) { FloatFiles(s); }});
  // Wheel order = registration order. Reference image wheel (clockwise from 12): Compress, Metadata, Edit, Annotate, Add BG, Crop, Redact.
  FiloRegisterGeneralTools(&AddTool);            // compress, metadata, (pdf) merge/organize/split ...

  ToolInfo edit;
  edit.id = "edit-photo"; edit.label = L"Edit Photo"; edit.wheelLabel = L"Edit"; edit.icon = "edit"; edit.category = "image";
  edit.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one image."); return false; }
    return true;
  };
  AddTool(edit, &MakeEditPhoto);
  FiloRegisterImageTools(&AddTool);              // annotate, add bg, crop, redact, collage, qr, create pdf

  ToolInfo join;
  join.id = "join-videos"; join.label = L"Join Videos"; join.wheelLabel = L"Join"; join.icon = "join"; join.category = "video";
  join.accepts = [](const Selection& s, std::wstring* why) {
    if (!SelAllGroup(s, Group::Video)) { SelReason(why, L"Select video files."); return false; }
    if (s.count() < 2) { SelReason(why, L"Select two or more videos."); return false; }
    return true;
  };
  AddTool(join, &MakeJoinVideos);
  FiloRegisterVideoTools(&AddTool);              // trim, crop, speed, split, frames, redact, remove audio
  FiloRegisterAudioTools(&AddTool);              // normalize, visualizer, trim, channels, bleep
  FiloRegisterPdfTools(&AddTool);                // (pdf tools may also come from RegisterGeneralTools)
}

} // namespace filo::tools
