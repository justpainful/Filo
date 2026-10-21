// Conversion Capability Registry. Every conversion path Filo advertises is registered here with the engine that performs
// it, its options, its known losses and a verifier. Nothing is shown in the UI unless it is registered AND runnable.
#pragma once
#include "core/job.h"
#include "core/json.h"
#include "registry/filetype.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace filo {

// One user-tweakable option of a conversion (rendered by the options popup; also used by tests).
struct OptionSpec {
  enum class Kind { Slider, Toggle, Choice, Text };
  std::string id;
  std::wstring label;
  Kind kind = Kind::Slider;
  double min = 0, max = 100, step = 1, def = 0;
  std::vector<std::pair<std::string, std::wstring>> choices;   // Choice: (value, label)
  std::wstring unit;
};

// What the engine receives for one input file.
struct ConvertTask {
  fs::path input;
  fs::path output;            // TEMP path to write (OutputTransaction); extension == outExt
  std::wstring outExt;        // canonical, lowercase
  Json options = Json::MakeObject();
  FileKind inKind;            // detected kind of `input`
  // Defaults from Settings are merged into `options` by the registry before run() (quality, bitrate, crf, preserveMetadata).
};

struct EngineResult {
  bool ok = false;
  std::wstring error;                       // user-facing
  std::vector<std::wstring> warnings;       // e.g. "Animation was reduced to the first frame"
  std::vector<fs::path> extraOutputs;       // multi-file outputs written next to `output` inside the same temp dir (PDF->PNG pages)
  static EngineResult Ok() { EngineResult r; r.ok = true; return r; }
  static EngineResult Fail(std::wstring why) { EngineResult r; r.error = std::move(why); return r; }
};

using ConvertFn = std::function<EngineResult(const ConvertTask&, JobContext&)>;
using VerifyFn = std::function<bool(const fs::path& produced, std::wstring* why)>;

struct Capability {
  std::string id;                 // "png>webp"
  Group group = Group::Unknown;   // group of the input
  std::wstring in, out;           // canonical lowercase format ids
  std::string engine;             // "wic", "ffmpeg", "pdfium", "native", "libreoffice", ...
  bool lossy = false;             // output loses information relative to input (shown in matrix + UI hint)
  bool producesFolder = false;    // output is a folder of files (PDF -> PNG pages, slides -> images, archive extract)
  std::wstring limitation;        // human-readable caveat, shown in FORMAT-MATRIX.md and as a tooltip
  std::vector<OptionSpec> options;
  std::function<bool()> available;    // runtime prerequisite (e.g. LibreOffice installed); null = always
  ConvertFn run;                      // null => documented-but-unsupported path (never offered in the UI)
  VerifyFn verify;                    // opens the produced file; null => only the size check
  bool Runnable() const { return run && (!available || available()); }
};

class CapabilityRegistry {
 public:
  static CapabilityRegistry& Get();
  void Add(Capability c);
  const Capability* Find(const std::wstring& in, const std::wstring& out) const;
  // Runnable outputs for an input format, in registration order.
  std::vector<const Capability*> OutputsFor(const std::wstring& inFormat) const;
  const std::vector<Capability>& All() const { return caps_; }
  // Test status loaded from tests/matrix-results.json (written by `filo-cli matrix --run`). Id -> "pass"/"fail"/"untested".
  void LoadResults(const fs::path& jsonFile);
  std::wstring StatusOf(const std::string& id) const;
 private:
  std::vector<Capability> caps_;
  std::map<std::string, std::wstring> status_;
};

// Calls every engine's Register function (engines/register.cpp). Idempotent.
void RegisterAllEngines();

// Runs one conversion synchronously inside a job context: merges Settings defaults into the options, writes to a temp
// file, verifies and commits atomically. `final` receives the committed path (or the output dir for folder outputs).
JobResult RunConversion(const Capability& cap, const fs::path& input, const Json& options, JobContext& ctx, const fs::path* forcedOutput = nullptr);

} // namespace filo
