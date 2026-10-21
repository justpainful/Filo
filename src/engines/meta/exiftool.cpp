// ExifTool process wrapper (args file with UTF-8, cancel, timeout) + small shared helpers (SHA-256).
#include "engines/meta/internal.h"
#include <bcrypt.h>
#include <fstream>
#include <mutex>
#include <rpc.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace filo::meta {

namespace {

fs::path FindDevCopy() {
  // walk up from the exe looking for third_party\exiftool\exiftool-*\exiftool(-k).exe (development builds only)
  std::error_code ec;
  fs::path p = ExeDir();
  for (int i = 0; i < 8 && !p.empty(); ++i, p = p.parent_path()) {
    fs::path tp = p / L"third_party" / L"exiftool";
    if (!fs::is_directory(tp, ec)) continue;
    for (auto& d : fs::directory_iterator(tp, ec)) {
      if (!d.is_directory()) continue;
      for (const wchar_t* n : {L"exiftool.exe", L"exiftool(-k).exe"}) {
        fs::path e = d.path() / n;
        if (fs::exists(e, ec) && fs::is_directory(d.path() / L"exiftool_files", ec)) return e;
      }
    }
  }
  return {};
}

} // namespace

fs::path ExifToolPath() {
  static fs::path cached;
  static std::once_flag once;
  std::call_once(once, [] {
    std::error_code ec;
    fs::path tools = ExeDir() / L"tools";
    const fs::path candidates[] = {tools / L"exiftool" / L"exiftool.exe", tools / L"exiftool.exe", tools / L"exiftool" / L"exiftool(-k).exe",
                                   ExeDir() / L"exiftool" / L"exiftool.exe", ExeDir() / L"exiftool.exe"};
    for (auto& c : candidates) if (fs::exists(c, ec)) { cached = c; return; }
    cached = FindDevCopy();
    if (cached.empty()) cached = FindTool(L"exiftool.exe");
  });
  return cached;
}
bool ExifToolAvailable() { return !ExifToolPath().empty(); }

namespace detail {

std::string EscapeValue(const std::wstring& v) {
  std::string out;
  std::string u = Narrow(v);
  for (char c : u) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

static std::wstring UniqueName() {
  UUID u; UuidCreate(&u);
  wchar_t b[40];
  swprintf_s(b, L"%08x%04x", u.Data1, u.Data2);
  return b;
}

EtRun RunExifTool(const std::vector<std::wstring>& args, JobContext* ctx, DWORD timeoutMs) {
  EtRun r;
  fs::path exe = ExifToolPath();
  if (exe.empty()) { r.err = "exiftool not found"; return r; }
  fs::path dir = ctx ? ctx->TempDir() : TempDir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path argFile = dir / (L"et-" + UniqueName() + L".args");
  {
    std::string text = "-charset\nutf8\n-charset\nfilename=utf8\n-api\nlargefilesupport=1\n";
    for (auto& a : args) {
      std::string u = Narrow(a);
      if (u.find('\n') != std::string::npos || u.find('\r') != std::string::npos) { r.err = "argument contains a line break"; return r; }
      text += u; text.push_back('\n');
    }
    HANDLE h = CreateFileW(LongPath(argFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) { r.err = "cannot write the argument file"; return r; }
    DWORD w = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
  }
  auto cleanup = OnExit([&] { std::error_code e2; fs::remove(argFile, e2); });
  ProcessOptions po;
  po.exe = exe;
  po.args = {L"-@", argFile.wstring()};
  po.cancel = ctx ? ctx->CancelFlag() : nullptr;
  po.timeoutMs = timeoutMs;
  po.lowPriority = false;
  po.captureLimit = 256u << 20;
  ProcessResult pr = RunProcess(po);
  r.started = pr.started; r.timedOut = pr.timedOut; r.cancelled = pr.cancelled;
  r.exit = (int)pr.exitCode;
  r.out = std::move(pr.out); r.err = std::move(pr.err);
  // the "-k" build prints "-- press ENTER --" on stderr before exiting
  size_t p = r.err.find("-- press ENTER --");
  while (p != std::string::npos) { r.err.erase(p, 17); p = r.err.find("-- press ENTER --"); }
  return r;
}

// ---- SHA-256 via CNG ---------------------------------------------------------------------------------------------------------

Sha256::Sha256() {
  BCRYPT_ALG_HANDLE a = nullptr;
  if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
  alg_ = a;
  DWORD objLen = 0, got = 0;
  BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof objLen, &got, 0);
  obj_.resize(objLen);
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptCreateHash(a, &h, obj_.data(), objLen, nullptr, 0, 0) == 0) h_ = h;
}
Sha256::~Sha256() {
  if (h_) BCryptDestroyHash((BCRYPT_HASH_HANDLE)h_);
  if (alg_) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)alg_, 0);
}
void Sha256::Update(const void* d, size_t n) {
  if (!h_) return;
  const uint8_t* p = (const uint8_t*)d;
  while (n) { ULONG c = (ULONG)std::min<size_t>(n, 1u << 30); BCryptHashData((BCRYPT_HASH_HANDLE)h_, (PUCHAR)p, c, 0); p += c; n -= c; }
}
std::string Sha256::Finish() {
  uint8_t out[32] = {};
  if (h_) BCryptFinishHash((BCRYPT_HASH_HANDLE)h_, out, 32, 0);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : out) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 15]); }
  return s;
}
std::string Sha256Hex(const void* data, size_t n) { Sha256 h; h.Update(data, n); return h.Finish(); }

} // namespace detail
} // namespace filo::meta
