// Shared helpers for the archive engine tests: scratch directories, independent readers (python -I, bsdtar, 7-Zip),
// manifests, job runner and the matrix-results.json collector.
#pragma once
#include "test.h"
#include "core/job.h"
#include "core/json.h"
#include "core/process.h"
#include "core/util.h"
#include "engines/archive/archive.h"
#include "engines/archive/internal.h"
#include "registry/capability.h"
#include "miniz.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace tu {

using namespace filo;

inline fs::path RepoRoot() {
  fs::path p = ExeDir();               // build\Debug_misc\bin
  for (int i = 0; i < 6; ++i) {
    if (fs::exists(p / L"docs" / L"ENGINE-CONTRACT.md")) return p;
    p = p.parent_path();
  }
  return ExeDir();
}
inline fs::path FixtureDir() { fs::path d = RepoRoot() / L"testdata" / L"archive" / L"fixtures"; std::error_code ec; fs::create_directories(d, ec); return d; }

// remove_all that also works for paths longer than MAX_PATH
inline void RemoveAll(const fs::path& p) {
  std::error_code ec;
  std::wstring s = fs::absolute(p).wstring();
  if (s.rfind(L"\\\\?\\", 0) != 0) s = L"\\\\?\\" + s;
  fs::remove_all(fs::path(s), ec);
}

// Big / many-file scratch lives under %LOCALAPPDATA% (not OneDrive).
inline fs::path GenRoot() {
  static fs::path r = [] {
    fs::path d = AppDataDir() / L"test-archive";
    RemoveAll(d);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
  }();
  return r;
}
inline fs::path Fresh(const std::wstring& name) {
  fs::path d = GenRoot() / name;
  RemoveAll(d);
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

inline void WriteBytes(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  size_t done = 0;
  while (done < data.size()) { DWORD w = 0; if (!WriteFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1u << 28), &w, nullptr)) break; done += w; }
  CloseHandle(h);
}
inline std::string ReadBytes(const fs::path& p) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
  std::string s((size_t)sz.QuadPart, '\0');
  size_t done = 0;
  while (done < s.size()) { DWORD g = 0; if (!ReadFile(h, s.data() + done, (DWORD)std::min<size_t>(s.size() - done, 1u << 28), &g, nullptr) || !g) break; done += g; }
  CloseHandle(h);
  s.resize(done);
  return s;
}
inline uint32_t Crc(const std::string& s) { return (uint32_t)mz_crc32(mz_crc32(0, nullptr, 0), (const unsigned char*)s.data(), s.size()); }
inline bool Exists(const fs::path& p) { return GetFileAttributesW(LongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
inline uint64_t FileSize(const fs::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA ad{};
  if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &ad)) return 0;
  return ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
}
inline uint64_t MTime(const fs::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA ad{};
  if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &ad)) return 0;
  return ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
}
inline std::string Pseudo(size_t n, uint32_t seed) {
  std::string s(n, '\0');
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s[i] = (char)(x >> 11); }
  return s;
}
inline std::string Text(size_t n, uint32_t seed) {
  std::string s;
  uint32_t x = seed + 7;
  while (s.size() < n) { x = x * 1664525u + 1013904223u; s += "line " + std::to_string(x % 1000) + " of some compressible text\n"; }
  s.resize(n);
  return s;
}

// ---- external tools ---------------------------------------------------------------------------------------------------------
inline fs::path PythonExe() {
  static fs::path p = [] { wchar_t b[MAX_PATH * 2]; return SearchPathW(nullptr, L"python.exe", nullptr, (DWORD)std::size(b), b, nullptr) ? fs::path(b) : fs::path(); }();
  return p;
}
inline fs::path SevenZipExe() {
  static fs::path p = [] {
    fs::path c = L"C:\\Users\\kuroi\\scoop\\shims\\7z.exe";
    if (fs::exists(c)) return c;
    wchar_t b[MAX_PATH * 2];
    return SearchPathW(nullptr, L"7z.exe", nullptr, (DWORD)std::size(b), b, nullptr) ? fs::path(b) : fs::path();
  }();
  return p;
}
struct Out { int code = -1; std::string out, err; };
inline Out Run(const fs::path& exe, std::vector<std::wstring> args, DWORD timeout = 600000, const fs::path& cwd = {}) {
  ProcessOptions po; po.exe = exe; po.args = std::move(args); po.timeoutMs = timeout; po.lowPriority = false; po.captureLimit = 64u << 20; po.cwd = cwd;
  ProcessResult r = RunProcess(po);
  Out o; o.code = r.started && !r.timedOut ? (int)r.exitCode : -1; o.out = r.out; o.err = r.err;
  return o;
}
// python -I -c <code> args...   (isolated mode: no cwd / script-dir imports)
inline Out Py(const std::string& code, std::vector<std::wstring> args = {}, DWORD timeout = 600000) {
  std::vector<std::wstring> a = {L"-I", L"-c", Widen(code)};
  for (auto& x : args) a.push_back(x);
  return Run(PythonExe(), a, timeout);
}
inline fs::path TarExe() { return archive::BsdtarPath(); }

// ---- manifests ---------------------------------------------------------------------------------------------------------------
struct Item { char kind; uint64_t size; uint32_t crc; bool operator==(const Item& o) const { return kind == o.kind && size == o.size && crc == o.crc; } };
using Manifest = std::map<std::wstring, Item>;   // name ('/' separators, dirs end with '/') -> item

inline Manifest ParseManifest(const std::string& js) {
  Manifest m;
  auto j = Json::Parse(js);
  if (!j) return m;
  for (auto& [k, v] : j->Items()) {
    std::string kind = v.At(0).AsString();
    m[Widen(k)] = Item{kind.empty() ? '?' : kind[0], (uint64_t)v.At(1).AsNumber(), (uint32_t)v.At(2).AsNumber()};
  }
  return m;
}

inline const char* kManifestPy = R"PY(
import sys, json, zipfile, tarfile, gzip, zlib, os
sys.stdout.reconfigure(encoding='utf-8')
def crc(b): return zlib.crc32(b) & 0xffffffff
kind, path = sys.argv[1], sys.argv[2]
out = {}
def norm(n):
    while n.startswith('./'): n = n[2:]
    return n
if kind == 'zip':
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        for i in z.infolist():
            n = i.filename
            if i.is_dir(): out[n.rstrip('/') + '/'] = ['d', 0, 0]
            else:
                d = z.read(i); out[n] = ['f', len(d), crc(d)]
        if bad is not None: out['__BAD__'] = ['x', 1, 1]
elif kind == 'tar':
    with tarfile.open(path) as t:
        for m in t:
            n = norm(m.name)
            if n in ('', '.'): continue
            if m.isdir(): out[n.rstrip('/') + '/'] = ['d', 0, 0]
            elif m.isfile():
                d = t.extractfile(m).read(); out[n] = ['f', len(d), crc(d)]
            else: out[n] = ['o', 0, 0]
elif kind == 'gz':
    d = gzip.open(path).read(); out['content'] = ['f', len(d), crc(d)]
elif kind == 'dir':
    base = os.path.abspath(path)
    pre = '\\\\?\\' + base
    for root, dirs, files in os.walk(pre):
        rel = os.path.relpath(root, pre).replace(os.sep, '/')
        if rel == '.': rel = ''
        if rel != '': out[rel + '/'] = ['d', 0, 0]
        for f in files:
            with open(os.path.join(root, f), 'rb') as fh: d = fh.read()
            out[(rel + '/' if rel else '') + f] = ['f', len(d), crc(d)]
print(json.dumps(out, ensure_ascii=False))
)PY";

inline Manifest PyManifest(const std::string& kind, const fs::path& p) {
  Out o = Py(kManifestPy, {Widen(kind), p.wstring()});
  if (o.code != 0) { std::printf("  manifest(%s) failed: %s\n", kind.c_str(), o.err.substr(0, 300).c_str()); return {}; }
  return ParseManifest(o.out);
}
// Manifest of an archive using the most fitting python reader.
inline Manifest ArchiveManifest(const fs::path& p) {
  std::wstring e = ExtOf(p);
  if (e == L"zip") return PyManifest("zip", p);
  return PyManifest("tar", p);       // tar, tar.gz, tgz, and the gz-wrapped tars we produce
}
inline std::string Diff(const Manifest& a, const Manifest& b) {
  std::string d;
  for (auto& [k, v] : a) { auto it = b.find(k); if (it == b.end()) d += "  only in A: " + Narrow(k) + "\n"; else if (!(it->second == v)) d += "  differs: " + Narrow(k) + "\n"; }
  for (auto& [k, v] : b) if (!a.count(k)) d += "  only in B: " + Narrow(k) + "\n";
  return d;
}
inline bool SameManifest(const Manifest& a, const Manifest& b, const char* what) {
  if (a.empty() && b.empty()) { std::printf("  %s: both manifests empty\n", what); return false; }
  std::string d = Diff(a, b);
  if (!d.empty()) std::printf("  manifest mismatch (%s):\n%s", what, d.substr(0, 1500).c_str());
  return d.empty();
}
inline Manifest DirManifest(const fs::path& d) { return PyManifest("dir", d); }
inline Manifest FilesOnly(Manifest m) { for (auto it = m.begin(); it != m.end();) { if (it->second.kind == 'd') it = m.erase(it); else ++it; } return m; }
// Prefixes every key with `prefix/` (folder archives keep the folder name).
inline Manifest WithRoot(const Manifest& m, const std::wstring& root) {
  Manifest r;
  r[root + L"/"] = Item{'d', 0, 0};
  for (auto& [k, v] : m) r[root + L"/" + k] = v;
  return r;
}

// ---- jobs -------------------------------------------------------------------------------------------------------------------
inline const Capability* Cap(const std::string& id) {
  filo::RegisterArchiveEngine();
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == id) return &c;
  return nullptr;
}
struct JobOut { JobResult r; JobState state = JobState::Pending; std::vector<double> progress; };
inline JobOut RunCap(const std::string& id, const fs::path& in, Json opts = Json::MakeObject(), std::function<void(uint64_t)> midway = {}, DWORD waitMs = 900000) {
  JobOut jo;
  const Capability* cap = Cap(id);
  if (!cap) { jo.r = JobResult::Fail(L"capability not registered"); return jo; }
  JobSpec spec;
  spec.operation = L"convert"; spec.title = Widen(id); spec.inputs = {in};
  spec.work = [cap, in, opts](JobContext& ctx) { return RunConversion(*cap, in, opts, ctx); };
  auto& jm = JobManager::Get();
  std::mutex mu;
  std::vector<double> prog;
  int lid = jm.AddListener([&](const JobInfo& i) { std::lock_guard lk(mu); if (i.operation == L"convert" && i.title == Widen(id)) prog.push_back(i.progress); });
  uint64_t jid = jm.Submit(spec);
  if (midway) midway(jid);
  jm.WaitFor(jid, waitMs);
  auto info = jm.Info(jid);
  jm.RemoveListener(lid);
  if (info) { jo.r = info->result; jo.state = info->state; }
  { std::lock_guard lk(mu); jo.progress = prog; }
  return jo;
}

// ---- matrix results ----------------------------------------------------------------------------------------------------------
struct Matrix {
  std::map<std::string, std::pair<std::string, std::string>> results;
  ~Matrix() {
    if (results.empty()) return;
    Json root = Json::MakeObject();
    Json r = Json::MakeObject();
    for (auto& [id, st] : results) { Json e = Json::MakeObject(); e["status"] = st.first; e["note"] = st.second; r[id] = e; }
    root["results"] = r;
    WriteBytes(RepoRoot() / L"testdata" / L"archive" / L"matrix-results.json", root.Dump(true));
  }
};
inline Matrix& Mx() { static Matrix m; return m; }
inline void Record(const std::string& id, bool pass, const std::string& note) {
  auto& cur = Mx().results[id];
  if (cur.first == "fail") return;            // a failure sticks
  cur = {pass ? "pass" : "fail", note};
}

// A Windows-created tree used by many tests.
inline void MakeSourceTree(const fs::path& root) {
  WriteBytes(root / L"readme.txt", "hello\r\nworld\r\n");
  WriteBytes(root / L"empty.bin", "");
  WriteBytes(root / L"data" / L"numbers.csv", Text(20000, 1));
  WriteBytes(root / L"data" / L"random.bin", Pseudo(70000, 2));
  WriteBytes(root / L"\u0645\u0631\u062d\u0628\u0627" / L"\u0645\u0644\u0641 \u0639\u0631\u0628\u064a.txt", "arabic file\n");
  WriteBytes(root / L"\u65e5\u672c\u8a9e" / L"\u30d5\u30a1\u30a4\u30eb \U0001F600.txt", "japanese + emoji\n");
  WriteBytes(root / L"with space" / L"a b c.txt", Text(3000, 3));
  std::error_code ec;
  fs::create_directories(root / L"empty dir" / L"nested empty", ec);
  fs::create_directories(root / L"\u0641\u0627\u0631\u063a", ec);
}

} // namespace tu
