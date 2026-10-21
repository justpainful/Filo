// Windows bsdtar (System32\tar.exe = libarchive) integration: availability probe, "archive -> temporary pax tar"
// conversion for RAR / 7z / tar.bz2 / tar.xz / zip variants, and independent verification of what our writers produce.
#include "core/process.h"
#include "engines/archive/internal.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <rpc.h>
#include <thread>
#include <unordered_map>

namespace filo::archive {

#include "probe_data.inc"

fs::path BsdtarPath() {
  static fs::path p = [] {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return fs::path();
    fs::path t = fs::path(sys) / L"tar.exe";
    std::error_code ec;
    return fs::exists(t, ec) ? t : fs::path();
  }();
  return p;
}

namespace detail {

static std::wstring UniqueName(const wchar_t* prefix) {
  UUID u; UuidCreate(&u);
  wchar_t b[48];
  swprintf_s(b, L"%s%08x%04x", prefix, u.Data1, u.Data2);
  return b;
}

static fs::path MakeScratch(JobContext* ctx) {
  fs::path base = ctx ? ctx->TempDir() : filo::TempDir();
  fs::path d = base / UniqueName(L"arc-");
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

TempFileGuard::~TempFileGuard() {
  if (dir.empty()) return;
  std::error_code ec;
  fs::remove_all(dir, ec);
}

namespace {
std::wstring MapBsdtarError(const std::string& err) {
  std::string l = err;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  auto has = [&](const char* s) { return l.find(s) != std::string::npos; };
  if (has("passphrase") || has("password") || has("encrypted")) return L"This archive is password-protected, which is not supported.";
  if (has("truncated") || has("premature end") || has("unexpected end")) return L"The archive is truncated (it ends before all of its data).";
  if (has("crc") || has("checksum") || has("corrupt") || has("damaged") || has("bad ")) return L"The archive is damaged (it failed its integrity checks).";
  if (has("unsupported") || has("not supported") || has("unknown") || has("unrecognized") || has("does not look like")) return L"This archive uses a format or feature that could not be read.";
  return L"The archive could not be read (it may be password-protected, damaged or use an unsupported feature).";
}
}

fs::path BsdtarToTar(const fs::path& archive, const ReaderConfig& cfg, JobContext* ctx) {
  fs::path tar = BsdtarPath();
  if (tar.empty()) Fail(L"Reading this kind of archive needs Windows' built-in tar.exe, which was not found.");
  fs::path scratch = MakeScratch(ctx);
  fs::path out = scratch / L"converted.tar";
  std::atomic<bool> stop{false}, tooBig{false}, userCancel{false}, done{false};
  std::thread mon([&] {
    uint64_t cap = cfg.maxTotalBytes + (64ull << 20);
    while (!done.load()) {
      if (ctx && ctx->Cancelled()) { userCancel = true; stop = true; break; }
      WIN32_FILE_ATTRIBUTE_DATA ad{};
      if (GetFileAttributesExW(LongPath(out).c_str(), GetFileExInfoStandard, &ad)) {
        uint64_t sz = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
        if (sz > cap || FreeSpace(scratch) < (32ull << 20)) { tooBig = true; stop = true; break; }
      }
      Sleep(100);
    }
  });
  ProcessOptions po;
  po.exe = tar;
  po.args = {L"-cf", out.wstring(), L"--format", L"pax", L"@" + archive.wstring()};
  po.cwd = scratch;
  po.cancel = &stop;
  po.captureLimit = 64 * 1024;
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  done = true;
  mon.join();
  auto cleanup = [&] { std::error_code ec; fs::remove_all(scratch, ec); };
  if (userCancel) { cleanup(); throw ArcCancelled{}; }
  if (tooBig) { cleanup(); Fail(L"The archive expands to more data than the safety limit allows (or the disk is full)."); }
  if (!r.started) { cleanup(); Fail(L"Windows' tar.exe could not be started."); }
  std::error_code ec;
  if (!r.Ok() || !fs::exists(out, ec)) { cleanup(); Fail(MapBsdtarError(r.err)); }
  return out;
}

} // namespace detail

// ---- availability ---------------------------------------------------------------------------------------------------------------

static bool ProbeArchive(const unsigned char* data, size_t n, const wchar_t* fileName) {
  fs::path tar = BsdtarPath();
  if (tar.empty()) return false;
  fs::path dir = detail::MakeScratch(nullptr);
  detail::TempFileGuard g{dir};
  fs::path f = dir / fileName;
  {
    HANDLE h = CreateFileW(LongPath(f).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, data, (DWORD)n, &w, nullptr);
    CloseHandle(h);
  }
  ProcessOptions po;
  po.exe = tar; po.args = {L"-tf", f.wstring()}; po.timeoutMs = 20000; po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  return r.Ok() && r.out.find("filo-probe.txt") != std::string::npos;
}

bool BsdtarCanRead7z() {
  static bool v = ProbeArchive(kProbe7z, sizeof kProbe7z, L"filo-probe.7z");
  return v;
}
bool BsdtarCanReadRar() {
  static bool v = ProbeArchive(kProbeRar, sizeof kProbeRar, L"filo-probe.rar");
  return v;
}

// ---- verification -----------------------------------------------------------------------------------------------------------------

// How the Windows bsdtar prints a name on a pipe: converted to the ANSI code page one UTF-16 unit at a time
// ('?' for anything unrepresentable, two '?' for an emoji). Used to compare against our own entry names.
static std::string AcpLossy(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) {
    if (c >= 0xD800 && c <= 0xDFFF) { s.push_back('?'); continue; }
    char buf[4];
    BOOL used = FALSE;
    int n = WideCharToMultiByte(detail::SystemAnsiCodePage(), 0, &c, 1, buf, 4, "?", &used);
    if (n <= 0) s.push_back('?'); else s.append(buf, (size_t)n);
  }
  return s;
}

bool VerifyWithBsdtar(const fs::path& produced, const std::vector<WrittenEntry>& expected, std::wstring* why, JobContext* ctx) {
  auto fail = [&](std::wstring m) { if (why) *why = std::move(m); return false; };
  fs::path tar = BsdtarPath();
  if (tar.empty()) return fail(L"Windows' tar.exe is not available to verify the result.");
  const std::atomic<bool>* cancel = ctx ? ctx->CancelFlag() : nullptr;

  // 1) names + sizes
  struct Seen { uint64_t size; bool dir; };
  std::unordered_map<std::string, Seen> seen;
  size_t lines = 0, dupes = 0;
  ProcessOptions po;
  po.exe = tar; po.args = {L"-tvf", produced.wstring()}; po.cancel = cancel; po.captureLimit = 1; po.lowPriority = false;
  po.onStdoutLine = [&](const std::string& ln) {
    size_t i = 0;
    std::string size;
    for (int tok = 0; tok < 8; ++tok) {
      while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t')) ++i;
      size_t j = i;
      while (j < ln.size() && ln[j] != ' ' && ln[j] != '\t') ++j;
      if (tok == 4) size = ln.substr(i, j - i);
      i = j;
    }
    while (i < ln.size() && ln[i] == ' ') ++i;
    std::string name = ln.substr(i);
    bool dir = !name.empty() && name.back() == '/';
    while (!name.empty() && name.back() == '/') name.pop_back();
    uint64_t sz = 0;
    try { sz = std::stoull(size); } catch (...) {}
    ++lines;
    if (!seen.emplace(name, Seen{sz, dir}).second) ++dupes;
  };
  ProcessResult r = RunProcess(po);
  if (r.cancelled) throw detail::ArcCancelled{};
  if (!r.Ok()) return fail(L"The produced archive could not be listed by an independent reader.");
  if (lines != expected.size() || dupes) return fail(L"The produced archive lists " + std::to_wstring(lines) + L" entries but " + std::to_wstring(expected.size()) + L" were written.");
  for (auto& e : expected) {
    std::string key = AcpLossy(e.name);
    auto it = seen.find(key);
    if (it == seen.end()) return fail(L"The produced archive is missing '" + e.name + L"'.");
    if (it->second.dir != e.isDir) return fail(L"'" + e.name + L"' has the wrong type in the produced archive.");
    if (!e.isDir && it->second.size != e.size) return fail(L"'" + e.name + L"' has the wrong size in the produced archive.");
  }
  // 2) read every byte (libarchive validates CRCs / structure while reading)
  ProcessOptions rd;
  rd.exe = tar; rd.args = {L"-xOf", produced.wstring()}; rd.cancel = cancel; rd.captureLimit = 1; rd.lowPriority = false;
  ProcessResult r2 = RunProcess(rd);
  if (r2.cancelled) throw detail::ArcCancelled{};
  if (!r2.Ok() || !r2.err.empty()) return fail(L"An independent reader reported a problem while reading the produced archive.");
  return true;
}

} // namespace filo::archive

namespace filo::archive {
bool VerifyReadable(const fs::path& produced, std::wstring* why, JobContext* ctx) {
  fs::path tar = BsdtarPath();
  if (tar.empty()) { if (why) *why = L"Windows' tar.exe is not available to verify the result."; return true; }   // cannot verify: do not block
  const std::atomic<bool>* cancel = ctx ? ctx->CancelFlag() : nullptr;
  for (const wchar_t* mode : {L"-tf", L"-xOf"}) {
    ProcessOptions po;
    po.exe = tar; po.args = {mode, produced.wstring()}; po.cancel = cancel; po.captureLimit = 1; po.lowPriority = false;
    ProcessResult r = RunProcess(po);
    if (r.cancelled) throw detail::ArcCancelled{};
    if (!r.Ok() || !r.err.empty()) { if (why) *why = L"An independent reader could not read the produced archive."; return false; }
  }
  return true;
}
}
