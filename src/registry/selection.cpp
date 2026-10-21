#include "registry/selection.h"
#include <algorithm>

namespace filo {

bool Selection::AllFiles() const { for (auto& i : items) if (i.isDir) return false; return !items.empty(); }
bool Selection::AnyFolder() const { for (auto& i : items) if (i.isDir && i.format != L"rtfd") return true; return false; }
bool Selection::SameGroup(Group* g) const {
  if (items.empty()) return false;
  Group first = items[0].group;
  if (first == Group::Unknown || first == Group::Folder) return false;
  for (auto& i : items) if (i.group != first) return false;
  if (g) *g = first;
  return true;
}
bool Selection::SameFormat(std::wstring* f) const {
  if (items.empty() || items[0].group == Group::Unknown) return false;
  for (auto& i : items) if (i.format != items[0].format) return false;
  if (f) *f = items[0].format;
  return true;
}
uint64_t Selection::TotalSize() const { uint64_t s = 0; for (auto& i : items) s += i.size; return s; }
Selection Selection::FromPaths(const std::vector<fs::path>& paths) {
  Selection s;
  for (auto& p : paths) s.items.push_back(DetectFile(p));
  return s;
}

ToolRegistry& ToolRegistry::Get() { static ToolRegistry* r = new ToolRegistry(); return *r; }
void ToolRegistry::AddTool(ToolInfo t) {
  if (t.wheelLabel.empty()) t.wheelLabel = t.label;
  for (auto& e : tools_) if (e.id == t.id) { e = std::move(t); return; }
  tools_.push_back(std::move(t));
}
void ToolRegistry::AddAction(ActionInfo a) {
  for (auto& e : actions_) if (e.id == a.id) { e = std::move(a); return; }
  actions_.push_back(std::move(a));
}
const ToolInfo* ToolRegistry::FindTool(const std::string& id) const { for (auto& t : tools_) if (t.id == id) return &t; return nullptr; }
const ActionInfo* ToolRegistry::FindAction(const std::string& id) const { for (auto& a : actions_) if (a.id == id) return &a; return nullptr; }

const std::vector<std::wstring>& PreferredOutputOrder(Group g) {
  // Clockwise from 12 o'clock. Matches the reference image wheel: JPG WEBP HEIC TIFF AVIF BMP PDF DOCX (PNG when not the input).
  static const std::vector<std::wstring> image = {L"jpg", L"webp", L"heic", L"tiff", L"avif", L"bmp", L"pdf", L"docx", L"png", L"gif"};
  static const std::vector<std::wstring> video = {L"mp4", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"gif", L"mp3"};
  static const std::vector<std::wstring> audio = {L"mp3", L"m4a", L"wav", L"flac", L"ogg", L"opus", L"aiff", L"wma"};
  static const std::vector<std::wstring> pdf = {L"docx", L"pages", L"pptx", L"xlsx", L"jpg", L"png", L"epub", L"txt", L"md"};
  static const std::vector<std::wstring> doc = {L"docx", L"pdf", L"pages", L"md", L"txt", L"epub", L"html"};
  static const std::vector<std::wstring> pres = {L"pdf", L"jpg", L"png", L"pptx"};
  static const std::vector<std::wstring> sheet = {L"xlsx", L"ods", L"csv", L"tsv", L"pdf"};
  static const std::vector<std::wstring> sub = {L"srt", L"vtt", L"txt"};
  static const std::vector<std::wstring> arch = {L"zip", L"tar", L"gz", L"extract"};
  static const std::vector<std::wstring> none;
  switch (g) {
    case Group::Image: return image; case Group::Video: return video; case Group::Audio: return audio; case Group::Pdf: return pdf;
    case Group::Document: return doc; case Group::Presentation: return pres; case Group::Spreadsheet: return sheet;
    case Group::Subtitle: return sub; case Group::Archive: return arch; default: return none;
  }
}

static std::wstring UpperFmt(std::wstring f) { CharUpperBuffW(f.data(), (DWORD)f.size()); return f; }

Availability ComputeAvailability(const Selection& sel) {
  Availability av;
  const auto& reg = CapabilityRegistry::Get();
  const auto& tools = ToolRegistry::Get();

  // ---- conversions
  if (sel.empty()) { av.convertEmptyReason = L"Nothing selected."; }
  else if (sel.AnyFolder()) { av.convertEmptyReason = L"Folders have no conversions. Use the actions wheel."; }
  else {
    Group g{};
    if (!sel.SameGroup(&g)) {
      // Mixed or unsupported content: offer nothing rather than nonsense combinations.
      bool unsupported = false;
      for (auto& i : sel.items) if (!i.Supported()) unsupported = true;
      av.convertEmptyReason = unsupported ? L"This file type is not supported for conversion." : L"Mixed file types have no common conversions.";
    } else {
      // intersection of runnable output formats over all selected files (excluding each file's own format)
      std::vector<std::wstring> common;
      bool first = true;
      for (auto& it : sel.items) {
        std::vector<std::wstring> outs;
        for (const Capability* c : reg.OutputsFor(it.format)) if (c->out != it.format) outs.push_back(c->out);
        if (first) { common = outs; first = false; }
        else { std::vector<std::wstring> keep; for (auto& o : common) if (std::find(outs.begin(), outs.end(), o) != outs.end()) keep.push_back(o); common = keep; }
      }
      const auto& order = PreferredOutputOrder(g);
      std::stable_sort(common.begin(), common.end(), [&](const std::wstring& a, const std::wstring& b) {
        auto ia = std::find(order.begin(), order.end(), a), ib = std::find(order.begin(), order.end(), b);
        return (ia - order.begin()) < (ib - order.begin());
      });
      for (auto& o : common) {
        WheelEntry e; e.id = L"convert:" + o; e.label = o == L"extract" ? L"Extract" : UpperFmt(o); e.enabled = true;
        av.convert.push_back(e);
      }
      if (av.convert.empty()) av.convertEmptyReason = L"No conversions are available for this selection on this PC.";
    }
  }

  // ---- tools (only those whose launch is implemented and whose rules accept the selection)
  if (!sel.empty()) {
    for (auto& t : tools.Tools()) {
      if (!t.launch) continue;
      std::wstring why;
      bool ok = t.accepts ? t.accepts(sel, &why) : true;
      if (!ok) continue;                    // not applicable: hidden, not greyed out (the reference never lists inapplicable tools)
      WheelEntry e; e.id = L"tool:" + Widen(t.id); e.label = t.wheelLabel; e.icon = t.icon;
      av.tools.push_back(e);
    }
    if (av.tools.empty()) av.toolsEmptyReason = L"No tools apply to this selection.";
  }

  // ---- actions (always available when something is selected; individual actions may be disabled with a reason)
  if (!sel.empty()) {
    for (auto& a : tools.Actions()) {
      if (!a.run) continue;
      std::wstring why;
      bool ok = a.accepts ? a.accepts(sel, &why) : true;
      WheelEntry e; e.id = L"action:" + Widen(a.id); e.label = a.label; e.icon = a.icon; e.enabled = ok; e.why = why;
      av.actions.push_back(e);
    }
  }
  // Actions wheel order is fixed (reference): Share, Float, Copy, Move, Copy Path, Terminal.
  static const wchar_t* kOrder[] = {L"action:share", L"action:float", L"action:copy", L"action:move", L"action:copypath", L"action:terminal"};
  auto rank = [](const std::wstring& id) { for (int i = 0; i < 6; ++i) if (id == kOrder[i]) return i; return 6; };
  std::stable_sort(av.actions.begin(), av.actions.end(), [&](const WheelEntry& a, const WheelEntry& b) { return rank(a.id) < rank(b.id); });
  return av;
}

} // namespace filo
