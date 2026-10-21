// filo-cli: headless harness for the conversion engines (integration tests and diagnostics). Never shows a window.
//   filo-cli list [--all]                              capabilities registered on this PC (--all includes non-runnable ones)
//   filo-cli convert <input> <outFormat> [k=v ...]     runs one conversion through the registry (temp -> verify -> atomic commit)
// Options are passed to the engine as JSON (numbers and true/false are recognised): e.g.  filo-cli convert a.pdf docx ocr=false
#include "core/util.h"
#include "core/job.h"
#include "core/json.h"
#include "core/settings.h"
#include "registry/capability.h"
#include "registry/filetype.h"
#include "engines/engines.h"
#include <objbase.h>
#include <cstdio>
#include <string>

using namespace filo;

static int Usage() {
  std::puts("usage: filo-cli list [--all]\n       filo-cli convert <input> <outFormat> [key=value ...]");
  return 2;
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SettingsStore::Get().Load();
  RegisterAllEngines();
  if (argc < 2) return Usage();
  const std::wstring cmd = argv[1];
  if (cmd == L"list") {
    const bool all = argc > 2 && std::wstring(argv[2]) == L"--all";
    int n = 0;
    for (const Capability& c : CapabilityRegistry::Get().All()) {
      if (!all && !c.Runnable()) continue;
      std::printf("%s\t%s\t%s\n", Narrow(c.in).c_str(), Narrow(c.out).c_str(), c.Runnable() ? "runnable" : "not runnable here");
      ++n;
    }
    std::printf("%d capabilities\n", n);
    return 0;
  }
  if (cmd == L"convert" && argc >= 4) {
    fs::path input = fs::absolute(argv[2]);
    const std::wstring out = argv[3];
    FileKind kind = DetectFile(input);
    const Capability* cap = CapabilityRegistry::Get().Find(kind.format, out);
    if (!cap) { std::printf("{\"ok\":false,\"error\":\"no conversion from %s to %s\"}\n", Narrow(kind.format).c_str(), Narrow(out).c_str()); return 1; }
    if (!cap->Runnable()) { std::printf("{\"ok\":false,\"error\":\"%s to %s is not available on this PC\"}\n", Narrow(kind.format).c_str(), Narrow(out).c_str()); return 1; }
    Json opts = Json::MakeObject();
    for (int i = 4; i < argc; ++i) {
      std::string kv = Narrow(argv[i]); const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      char* end = nullptr; const double d = strtod(v.c_str(), &end);
      if (v == "true" || v == "false") opts[k] = (v == "true");
      else if (end && *end == 0 && !v.empty()) opts[k] = d;
      else opts[k] = v;
    }
    Capability capCopy = *cap;
    JobSpec spec; spec.operation = L"convert"; spec.title = input.filename().wstring(); spec.inputs = {input};
    spec.work = [capCopy, input, opts](JobContext& ctx) { return RunConversion(capCopy, input, opts, ctx); };
    const uint64_t id = JobManager::Get().Submit(std::move(spec));
    JobManager::Get().WaitFor(id, 30 * 60 * 1000);
    auto info = JobManager::Get().Info(id);
    Json j = Json::MakeObject();
    const bool ok = info && info->state == JobState::Completed;
    j["ok"] = ok;
    j["error"] = Narrow(info ? info->result.error : std::wstring(L"timed out"));
    Json outs = Json::MakeArray(); if (info) for (auto& o : info->result.outputs) outs.Push(Narrow(o.wstring())); j["outputs"] = outs;
    Json ws = Json::MakeArray(); if (info) for (auto& w : info->result.warnings) ws.Push(Narrow(w)); j["warnings"] = ws;
    std::printf("%s\n", j.Dump().c_str());
    return ok ? 0 : 1;
  }
  return Usage();
}
