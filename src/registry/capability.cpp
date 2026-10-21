#include "registry/capability.h"
#include "core/output.h"
#include "core/log.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace filo {

CapabilityRegistry& CapabilityRegistry::Get() { static CapabilityRegistry* r = new CapabilityRegistry(); return *r; }

void CapabilityRegistry::Add(Capability c) {
  if (c.id.empty()) c.id = Narrow(c.in) + ">" + Narrow(c.out);
  for (auto& e : caps_) if (e.id == c.id) { e = std::move(c); return; }    // later registration wins (engine override)
  caps_.push_back(std::move(c));
}

const Capability* CapabilityRegistry::Find(const std::wstring& in, const std::wstring& out) const {
  std::wstring i = CanonicalExt(in), o = CanonicalExt(out);
  for (auto& c : caps_) if (c.in == i && c.out == o) return &c;
  return nullptr;
}

std::vector<const Capability*> CapabilityRegistry::OutputsFor(const std::wstring& inFormat) const {
  std::wstring i = CanonicalExt(inFormat);
  std::vector<const Capability*> r;
  for (auto& c : caps_) if (c.in == i && c.Runnable()) r.push_back(&c);
  return r;
}

void CapabilityRegistry::LoadResults(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  if (!in) return;
  std::stringstream ss; ss << in.rdbuf();
  auto j = Json::Parse(ss.str());
  if (!j) return;
  for (auto& [k, v] : j->Get("results").Items()) status_[k] = Widen(v.Get("status").AsString());
}
std::wstring CapabilityRegistry::StatusOf(const std::string& id) const {
  auto it = status_.find(id);
  return it == status_.end() ? L"untested" : it->second;
}

// Triggers OneDrive hydration of an online-only file (explicit user request) and waits for it.
static bool EnsureLocal(const fs::path& p, JobContext& ctx, std::wstring* err) {
  DWORD a = GetFileAttributesW(LongPath(p).c_str());
  if (a == INVALID_FILE_ATTRIBUTES) { *err = L"The file is no longer available."; return false; }
  if (!(a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE))) return true;
  ctx.SetStage(JobState::Preparing, L"Downloading from the cloud...");
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { *err = L"The file could not be downloaded: " + LastErrorMessage(); return false; }
  char b; DWORD n; ReadFile(h, &b, 1, &n, nullptr);
  CloseHandle(h);
  for (int i = 0; i < 6000; ++i) {
    if (ctx.Cancelled()) { *err = L"Cancelled"; return false; }
    a = GetFileAttributesW(LongPath(p).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE))) return true;
    Sleep(100);
  }
  *err = L"The cloud file did not finish downloading."; return false;
}

JobResult RunConversion(const Capability& cap, const fs::path& input, const Json& options, JobContext& ctx, const fs::path* forcedOutput) {
  Settings st = SettingsStore::Get().Snapshot();
  std::wstring err;
  if (!EnsureLocal(input, ctx, &err)) return JobResult::Fail(err);
  FileKind kind = DetectFile(input);
  if (!kind.exists) return JobResult::Fail(L"The file is no longer available.");
  if (!kind.readable) return JobResult::Fail(L"The file cannot be opened (access denied or in use).");
  if (!cap.run) return JobResult::Fail(L"This conversion is not supported.");
  if (cap.available && !cap.available()) return JobResult::Fail(L"A required component for this conversion is not installed.");

  ConvertTask t;
  t.input = input; t.outExt = cap.out; t.inKind = kind; t.options = options.IsObject() ? options : Json::MakeObject();
  auto def = [&](const char* k, Json v) { if (!t.options.Has(k)) t.options[k] = std::move(v); };
  def("quality", st.imageQuality); def("crf", st.videoCrf); def("audioBitrateKbps", st.audioBitrateKbps);
  def("preserveMetadata", st.preserveMetadata); def("hardwareAcceleration", st.hardwareAcceleration);

  JobResult jr;
  if (kind.extensionMismatch)
    ctx.AddWarning(L"The file extension does not match its content (detected: " + kind.format + L"); it was converted using its real format.");

  fs::path desired = forcedOutput ? *forcedOutput : DesiredOutputPath(input, cap.out, st);
  if (cap.producesFolder) {
    fs::path dir = desired; dir.replace_extension();
    OutputDirTransaction tx(dir);
    t.output = tx.TempDir();
    ctx.SetStage(JobState::Running, L"");
    EngineResult er = cap.run(t, ctx);
    if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
    for (auto& w : er.warnings) ctx.AddWarning(w);
    if (!er.ok) return JobResult::Fail(er.error.empty() ? L"The conversion failed." : er.error);
    ctx.SetStage(JobState::Finalizing, L"");
    if (!tx.Commit(&err)) return JobResult::Fail(err);
    return JobResult::Ok({tx.FinalDir()});
  }

  OutputTransaction tx(desired, st.overwritePolicy);
  t.output = tx.TempPath();
  ctx.SetStage(JobState::Running, L"");
  EngineResult er = cap.run(t, ctx);
  if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
  for (auto& w : er.warnings) ctx.AddWarning(w);
  if (!er.ok) return JobResult::Fail(er.error.empty() ? L"The conversion failed." : er.error);
  ctx.SetStage(JobState::Finalizing, L"");
  if (!tx.Commit(&err, cap.verify)) return JobResult::Fail(err);
  return JobResult::Ok({tx.FinalPath()});
}

} // namespace filo
