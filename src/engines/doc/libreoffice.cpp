#include "engines/doc/libreoffice.h"
#include "core/process.h"
#include "core/log.h"
#include "engines/doc/docutil.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <mutex>
#include <chrono>

namespace filo::doc {

namespace {

std::mutex g_cacheMu;
fs::path g_cached;
double g_cachedAt = -100;
std::mutex g_runMu;           // one LibreOffice run at a time inside this process

bool RegString(HKEY root, REGSAM view, const wchar_t* sub, const wchar_t* value, std::wstring& out) {
  HKEY k;
  if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return false;
  wchar_t buf[1024]; DWORD sz = sizeof buf, type = 0;
  LONG r = RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)buf, &sz);
  RegCloseKey(k);
  if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  out.assign(buf, wcsnlen(buf, sizeof buf / sizeof(wchar_t)));
  return !out.empty();
}

fs::path Probe() {
  std::error_code ec;
  wchar_t env[1024];
  if (DWORD n = GetEnvironmentVariableW(L"FILO_SOFFICE", env, 1024); n > 0 && n < 1024) { fs::path p = env; if (fs::exists(p, ec)) return p; }
  fs::path portable = ExeDir() / L"LibreOffice" / L"program" / L"soffice.exe";
  if (fs::exists(portable, ec)) return portable;
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
    for (REGSAM view : {(REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY}) {
      std::wstring dir;
      if (RegString(root, view, L"SOFTWARE\\LibreOffice\\UNO\\InstallPath", L"", dir)) {
        fs::path p = fs::path(dir) / L"soffice.exe";
        if (fs::exists(p, ec)) return p;
      }
    }
  for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
    wchar_t pf[1024];
    DWORD n = GetEnvironmentVariableW(var, pf, 1024);
    if (!n || n >= 1024) continue;
    for (auto& e : fs::directory_iterator(pf, fs::directory_options::skip_permission_denied, ec)) {
      std::wstring nm = e.path().filename().wstring();
      if (StartsWithI(nm, L"LibreOffice")) {
        fs::path p = e.path() / L"program" / L"soffice.exe";
        if (fs::exists(p, ec)) return p;
      }
    }
  }
  fs::path onPath = FindTool(L"soffice.exe");
  if (!onPath.empty() && fs::exists(onPath, ec)) return onPath;
  return {};
}

std::wstring UrlEncodePath(const std::wstring& path) {
  std::string u = Narrow(path);
  std::string o;
  for (unsigned char c : u) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') o.push_back((char)c);
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
  }
  return Widen(o);
}

// Terminates soffice processes whose command line mentions `needle` (our private profile folder).
void KillProfileProcesses(const std::wstring& needle) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  using NtQueryFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  static NtQueryFn ntq = (NtQueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
  PROCESSENTRY32W pe{sizeof pe};
  std::wstring needleLower = ToLower(needle);
  if (Process32FirstW(snap, &pe)) do {
    std::wstring name = ToLower(pe.szExeFile);
    if (name != L"soffice.bin" && name != L"soffice.exe") continue;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
    if (!h) continue;
    bool match = false;
    if (ntq) {
      std::vector<uint8_t> buf(8192);
      ULONG ret = 0;
      LONG st = ntq(h, 60 /*ProcessCommandLineInformation*/, buf.data(), (ULONG)buf.size(), &ret);
      if (st == (LONG)0xC0000004 && ret > buf.size()) { buf.resize(ret); st = ntq(h, 60, buf.data(), (ULONG)buf.size(), &ret); }
      if (st >= 0) {
        auto* us = reinterpret_cast<UNICODE_STRING*>(buf.data());
        std::wstring cl(us->Buffer, us->Length / sizeof(wchar_t));
        match = ToLower(cl).find(needleLower) != std::wstring::npos;
      }
    }
    if (match) TerminateProcess(h, 1);
    CloseHandle(h);
  } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
}

struct NamedMutex {
  HANDLE h = nullptr;
  NamedMutex() { h = CreateMutexW(nullptr, FALSE, L"Local\\Filo.LibreOffice.Profile"); if (h) WaitForSingleObject(h, 600000); }
  ~NamedMutex() { if (h) { ReleaseMutex(h); CloseHandle(h); } }
};

} // namespace

static std::atomic<bool> g_disabledForTests{false};
void SetLibreOfficeDisabledForTests(bool disabled) { g_disabledForTests = disabled; }

fs::path FindLibreOffice() {
  if (g_disabledForTests) return {};
  std::lock_guard lk(g_cacheMu);
  double now = NowSeconds();
  std::error_code ec;
  if (!g_cached.empty() && fs::exists(g_cached, ec) && now - g_cachedAt < 30) return g_cached;
  if (g_cached.empty() && now - g_cachedAt < 5) return g_cached;
  g_cached = Probe();
  g_cachedAt = now;
  return g_cached;
}

std::wstring LibreOfficeVersion() {
  fs::path p = FindLibreOffice();
  if (p.empty()) return {};
  DWORD h = 0, sz = GetFileVersionInfoSizeW(p.c_str(), &h);
  if (!sz) return {};
  std::vector<uint8_t> buf(sz);
  if (!GetFileVersionInfoW(p.c_str(), 0, sz, buf.data())) return {};
  VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return {};
  return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionLS));
}

LoResult LibreOfficeConvert(const LoConvertSpec& spec, JobContext* ctx) {
  LoResult res;
  fs::path soffice = FindLibreOffice();
  if (soffice.empty()) { res.error = L"LibreOffice is not installed, so this conversion is not available. Install LibreOffice (free) and try again."; return res; }
  if (ctx) ctx->SetStage(JobState::Preparing, L"Waiting for LibreOffice...");
  std::lock_guard lk(g_runMu);
  NamedMutex nm;
  if (ctx && ctx->Cancelled()) { res.error = L"Cancelled"; return res; }

  std::error_code ec;
  static std::atomic<unsigned> counter{0};
  fs::path work = TempDir() / (L"lo-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter++));
  fs::create_directories(work, ec);
  auto cleanup = OnExit([&] { std::error_code e2; fs::remove_all(work, e2); });

  std::wstring inExt = spec.inExt.empty() ? ExtOf(spec.input) : spec.inExt;
  if (inExt.empty() || inExt.size() > 8) inExt = L"bin";
  fs::path inCopy = work / (L"in." + inExt);
  fs::copy_file(spec.input, inCopy, fs::copy_options::overwrite_existing, ec);
  if (ec) { res.error = L"The file could not be read for conversion."; return res; }

  fs::path profile = AppDataDir() / L"lo-profile";
  fs::create_directories(profile, ec);
  std::wstring profilePath = profile.wstring();
  std::replace(profilePath.begin(), profilePath.end(), L'\\', L'/');
  std::wstring profileUrl = L"file:///" + UrlEncodePath(profilePath);

  DWORD timeout = spec.timeoutMs;
  if (!timeout) {
    uint64_t sz = fs::file_size(spec.input, ec);
    uint64_t mb = ec ? 0 : sz >> 20;
    timeout = (DWORD)std::min<uint64_t>(180000 + mb * 30000, 1200000);
  }

  ProcessOptions po;
  po.exe = soffice;
  po.args = {L"--headless", L"--norestore", L"--nolockcheck", L"--nodefault", L"--nologo", L"--nofirststartwizard",
             L"-env:UserInstallation=" + profileUrl, L"--convert-to", spec.convertTo, L"--outdir", work.wstring(), inCopy.wstring()};
  po.cwd = work;
  po.cancel = ctx ? ctx->CancelFlag() : nullptr;
  po.timeoutMs = timeout;
  po.lowPriority = false;
  if (ctx) ctx->SetStage(JobState::Running, L"Converting with LibreOffice...");
  ProcessResult pr = RunProcess(po);
  if (!pr.started) { res.error = L"LibreOffice could not be started."; return res; }
  if (pr.cancelled || pr.timedOut) {
    KillProfileProcesses(L"lo-profile");
    Sleep(300);
    if (pr.timedOut) {
      // a hung instance may have left a locked/broken profile behind; start clean next time
      std::error_code e3; fs::remove_all(profile, e3);
      res.error = L"LibreOffice took too long and was stopped.";
    } else res.error = L"Cancelled";
    return res;
  }
  fs::path produced = work / (L"in." + spec.outExt);
  if (!fs::exists(produced, ec) || fs::file_size(produced, ec) == 0) {
    FILO_LOG(Warn, "doc.lo", "LibreOffice produced no output (exit %lu)", (unsigned long)pr.exitCode);
    res.error = pr.exitCode == 0 ? L"LibreOffice could not open this document (it may be damaged, password-protected or in an unsupported format)."
                                 : L"LibreOffice failed to convert this document.";
    return res;
  }
  fs::create_directories(spec.output.parent_path(), ec);
  if (!MoveFileExW(produced.c_str(), spec.output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    fs::copy_file(produced, spec.output, fs::copy_options::overwrite_existing, ec);
    if (ec) { res.error = L"The converted file could not be saved."; return res; }
  }
  res.ok = true;
  return res;
}

} // namespace filo::doc
