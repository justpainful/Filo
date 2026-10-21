// Shared helpers for the document engine tests.
#pragma once
#include "test.h"
#include "core/job.h"
#include "core/json.h"
#include "core/util.h"
#include "engines/engines.h"
#include "engines/doc/docmodel.h"
#include "engines/doc/docutil.h"
#include "engines/doc/imgenc.h"
#include "engines/doc/libreoffice.h"
#include "registry/capability.h"
#include <fstream>
#include <mutex>
#include <sstream>

namespace tu {
using namespace filo;
using namespace filo::doc;

inline fs::path Root() {
  static fs::path r = [] {
    fs::path p = ExeDir();
    for (int i = 0; i < 8; ++i) {
      if (fs::exists(p / "testdata") && fs::exists(p / "src")) return p;
      if (!p.has_parent_path() || p.parent_path() == p) break;
      p = p.parent_path();
    }
    return fs::current_path();
  }();
  return r;
}
inline fs::path TestData() { fs::path p = Root() / "testdata" / "doc"; std::error_code ec; fs::create_directories(p, ec); return p; }
// Fresh empty work folder per test.
inline fs::path Work(const std::string& name) {
  fs::path p = TestData() / "work" / Widen(name);
  std::error_code ec; fs::remove_all(p, ec); fs::create_directories(p, ec);
  return p;
}
inline std::string ReadAll(const fs::path& p) {
  Bytes b; ReadFileBytes(p, b, 1ull << 31);
  return std::string(b.begin(), b.end());
}
inline void WriteAll(const fs::path& p, const std::string& s) { std::error_code ec; fs::create_directories(p.parent_path(), ec); WriteFileBytes(p, s.data(), s.size()); }
inline uint64_t Hash(const fs::path& p) {
  Bytes b; if (!ReadFileBytes(p, b, 1ull << 31)) return 0;
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
  return h ^ b.size();
}
inline uint64_t HashDir(const fs::path& d) {
  uint64_t h = 7; std::error_code ec;
  std::vector<fs::path> files;
  for (auto& e : fs::recursive_directory_iterator(d, ec)) if (e.is_regular_file()) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (auto& f : files) h = h * 31 + Hash(f);
  return h;
}

inline bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---- model signature: one line per block, for semantic equality -------------------------------------------------------------
inline std::string RunsSig(const Runs& runs, bool fmt) {
  std::string s;
  for (auto& r : runs) {
    if (r.br) { s += "\\n"; continue; }
    std::string t = r.text;
    if (fmt) {
      if (r.bold) t = "**" + t + "**";
      if (r.italic) t = "_" + t + "_";
      if (r.code) t = "`" + t + "`";
      if (r.strike) t = "~~" + t + "~~";
      if (r.underline) t = "<u>" + t + "</u>";
      if (!r.link.empty()) t = "[" + t + "](" + r.link + ")";
    }
    s += t;
  }
  return s;
}
inline std::vector<std::string> Sig(const Document& d, bool fmt = false) {
  std::vector<std::string> v;
  for (auto& b : d.blocks) {
    switch (b.kind) {
      case Block::Kind::Heading: v.push_back("H" + std::to_string(b.level) + ":" + RunsSig(b.runs, fmt)); break;
      case Block::Kind::Paragraph: v.push_back("P:" + RunsSig(b.runs, fmt)); break;
      case Block::Kind::ListItem: v.push_back("L" + std::to_string(b.level) + (b.ordered ? "o" : "u") + ":" + RunsSig(b.runs, fmt)); break;
      case Block::Kind::Quote: v.push_back("Q:" + RunsSig(b.runs, fmt)); break;
      case Block::Kind::Code: v.push_back("C:" + PlainText(b.runs)); break;
      case Block::Kind::Rule: v.push_back("R"); break;
      case Block::Kind::PageBreak: v.push_back("PB"); break;
      case Block::Kind::Image: v.push_back("I"); break;
      case Block::Kind::Footnote: v.push_back("F" + b.label + ":" + RunsSig(b.runs, fmt)); break;
      case Block::Kind::Table: {
        std::string s = "T:";
        for (auto& row : b.table.rows) { for (auto& c : row) s += PlainText(c) + "|"; s += ";"; }
        v.push_back(s);
        break;
      }
    }
  }
  return v;
}
inline std::string SigStr(const Document& d, bool fmt = false) { std::string s; for (auto& l : Sig(d, fmt)) s += l + "\n"; return s; }
inline std::string AllText(const Document& d) {
  std::string s;
  for (auto& b : d.blocks) {
    if (b.kind == Block::Kind::Table) { for (auto& r : b.table.rows) for (auto& c : r) s += PlainText(c) + " "; }
    else s += PlainText(b.runs) + "\n";
  }
  return s;
}

inline void PrintDiff(const std::string& a, const std::string& b) {
  size_t i = 0; while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  size_t s0 = i > 60 ? i - 60 : 0;
  std::printf("  first difference at byte %zu\n  A: ...%s\n  B: ...%s\n", i, a.substr(s0, 140).c_str(), b.substr(s0, 140).c_str());
}
#define CHECK_SIG(a, b) do { std::string _sa = (a), _sb = (b); if (_sa != _sb) { tu::PrintDiff(_sa, _sb); std::printf("  %s:%d: CHECK_SIG failed\n", __FILE__, __LINE__); ++ftest::Failures(); } } while (0)

// A PNG made through WIC (independent encoder) with a gradient.
inline Bytes MakePng(int w, int h, bool jpeg = false) {
  std::vector<uint8_t> px((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { uint8_t* p = &px[((size_t)y * w + x) * 4]; p[0] = (uint8_t)(x * 255 / std::max(1, w - 1)); p[1] = (uint8_t)(y * 255 / std::max(1, h - 1)); p[2] = 128; p[3] = 255; }
  Bytes out; EncodeBgra(px.data(), w, h, w * 4, jpeg ? ImgFormat::Jpeg : ImgFormat::Png, 90, out);
  return out;
}

inline Document SampleDoc() {
  Document d;
  d.meta.title = "Filo sample";
  d.blocks.push_back(MakeHeading(1, "Quarterly report"));
  { Block p = MakeParagraph("Plain intro paragraph with "); Run b = MakeRun("bold"); b.bold = true; Run i = MakeRun(" and italic"); i.italic = true; Run l = MakeRun(" link"); l.link = "https://example.com/a?b=1&c=2";
    p.runs.push_back(b); p.runs.push_back(i); p.runs.push_back(l); p.runs.push_back(MakeRun(" end.")); d.blocks.push_back(p); }
  d.blocks.push_back(MakeHeading(2, "Lists"));
  for (auto [lvl, ord, txt] : {std::tuple{0, false, "first bullet"}, std::tuple{1, false, "nested bullet"}, std::tuple{0, false, "second bullet"}}) { Block b; b.kind = Block::Kind::ListItem; b.level = lvl; b.ordered = ord; b.runs.push_back(MakeRun(txt)); d.blocks.push_back(b); }
  { int n = 1; for (auto txt : {"step one", "step two", "step three"}) { Block b; b.kind = Block::Kind::ListItem; b.ordered = true; b.number = n++; b.runs.push_back(MakeRun(txt)); d.blocks.push_back(b); } }
  d.blocks.push_back(MakeHeading(2, "Table"));
  { Block t; t.kind = Block::Kind::Table; t.table.headerRow = true;
    for (int r = 0; r < 3; ++r) { std::vector<Cell> row; for (int c = 0; c < 3; ++c) { Cell cell; cell.header = r == 0; cell.runs.push_back(MakeRun(r == 0 ? "H" + std::to_string(c) : "r" + std::to_string(r) + "c" + std::to_string(c))); row.push_back(cell); } t.table.rows.push_back(row); }
    d.blocks.push_back(t); }
  d.blocks.push_back(MakeHeading(1, "Arabic and mixed"));
  d.blocks.push_back(MakeParagraph("مرحبا بالعالم! هذا مستند اختبار."));
  d.blocks.push_back(MakeParagraph("Mixed: النسخة 2024 of Filo – version 1.0 مع emoji \U0001F600\U0001F680."));
  { Block c; c.kind = Block::Kind::Code; Run r = MakeRun("int main() {\n  return 0;\n}"); r.code = true; c.runs.push_back(r); d.blocks.push_back(c); }
  { Block q; q.kind = Block::Kind::Quote; q.runs.push_back(MakeRun("A wise quote.")); d.blocks.push_back(q); }
  { Block img; img.kind = Block::Kind::Image; img.image = std::make_shared<ImageData>(); img.image->bytes = MakePng(120, 60); img.image->mime = "image/png"; img.image->w = 120; img.image->h = 60; img.image->alt = "gradient"; d.blocks.push_back(img); }
  d.blocks.push_back(MakeParagraph("Final paragraph."));
  FinalizeDocument(d);
  return d;
}

// ---- run a registered capability inside a JobManager job -----------------------------------------------------------------------
struct CapRun {
  bool found = false, ok = false;
  std::wstring error;
  std::vector<fs::path> outputs;
  std::vector<std::wstring> warnings;
  JobState state = JobState::Pending;
};
inline void EnsureRegistered() {
  static std::once_flag f;
  std::call_once(f, [] { filo::RegisterDocEngine(); });
}
inline const Capability* FindCap(const std::string& id) {
  EnsureRegistered();
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == id) return &c;
  return nullptr;
}
// `cancelAfterMs` > 0: cancels the job after that delay.
inline CapRun RunCap(const std::string& id, const fs::path& input, const fs::path& outPath, const Json& opts = Json::MakeObject(), int cancelAfterMs = 0) {
  CapRun r;
  const Capability* cap = FindCap(id);
  if (!cap) return r;
  r.found = true;
  fs::path out = outPath;
  JobSpec spec;
  spec.operation = L"convert"; spec.title = Widen(id); spec.inputs = {input};
  spec.work = [cap, input, opts, out](JobContext& ctx) { return RunConversion(*cap, input, opts, ctx, &out); };
  uint64_t jid = JobManager::Get().Submit(std::move(spec));
  if (cancelAfterMs > 0) { Sleep(cancelAfterMs); JobManager::Get().Cancel(jid); }
  JobManager::Get().WaitFor(jid, 600000);
  auto info = JobManager::Get().Info(jid);
  if (!info) return r;
  r.state = info->state;
  r.ok = info->result.ok;
  r.error = info->result.error;
  r.outputs = info->result.outputs;
  r.warnings = info->result.warnings;
  return r;
}
inline bool HaveLo() { return LibreOfficeAvailable(); }

} // namespace tu
