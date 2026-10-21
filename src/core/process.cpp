#include "core/process.h"
#include "core/log.h"
#include <thread>
#include <mutex>

namespace filo {

namespace {

HANDLE SharedJob() {
  static HANDLE job = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    return j;
  }();
  return job;
}

struct LineSplitter {
  std::string pending;
  void Feed(const char* d, size_t n, const std::function<void(const std::string&)>& cb) {
    for (size_t i = 0; i < n; ++i) {
      char c = d[i];
      if (c == '\n' || c == '\r') { if (!pending.empty()) { if (cb) cb(pending); pending.clear(); } }
      else pending.push_back(c);
    }
  }
  void Flush(const std::function<void(const std::string&)>& cb) { if (!pending.empty() && cb) cb(pending); pending.clear(); }
};

} // namespace

ProcessResult RunProcess(const ProcessOptions& o) {
  ProcessResult res;
  if (o.exe.empty()) { res.startError = L"no executable"; return res; }

  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, inR = nullptr, inW = nullptr;
  if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0) || !CreatePipe(&inR, &inW, &sa, 0)) {
    res.startError = L"CreatePipe failed"; return res;
  }
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

  std::wstring cmd = QuoteArg(o.exe.wstring());
  for (auto& a : o.args) { cmd.push_back(L' '); cmd += QuoteArg(a); }

  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = o.mergeStderr ? outW : errW;
  PROCESS_INFORMATION pi{};
  DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (o.lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
  std::wstring cwd = o.cwd.wstring();
  BOOL ok = CreateProcessW(o.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(outW); CloseHandle(errW); CloseHandle(inR);
  if (!ok) {
    res.startError = LastErrorMessage();
    CloseHandle(outR); CloseHandle(errR); CloseHandle(inW);
    return res;
  }
  res.started = true;
  if (HANDLE job = SharedJob()) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  if (!o.stdinData.empty()) { DWORD w; WriteFile(inW, o.stdinData.data(), (DWORD)o.stdinData.size(), &w, nullptr); }
  CloseHandle(inW);

  std::mutex mu;
  auto reader = [&](HANDLE h, std::string& sink, const std::function<void(const std::string&)>& cb) {
    LineSplitter ls; char buf[8192]; DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) {
      { std::lock_guard lk(mu); if (sink.size() < o.captureLimit) sink.append(buf, std::min<size_t>(n, o.captureLimit - sink.size())); }
      ls.Feed(buf, n, cb);
    }
    ls.Flush(cb);
  };
  std::thread tOut([&] { reader(outR, res.out, o.onStdoutLine); });
  std::thread tErr([&] { if (!o.mergeStderr) reader(errR, res.err, o.onStderrLine); });

  const ULONGLONG t0 = GetTickCount64();
  for (;;) {
    DWORD w = WaitForSingleObject(pi.hProcess, 50);
    if (w == WAIT_OBJECT_0) break;
    if (o.cancel && o.cancel->load()) { TerminateProcess(pi.hProcess, 1); res.cancelled = true; WaitForSingleObject(pi.hProcess, 5000); break; }
    if (o.timeoutMs != INFINITE && GetTickCount64() - t0 > o.timeoutMs) { TerminateProcess(pi.hProcess, 1); res.timedOut = true; WaitForSingleObject(pi.hProcess, 5000); break; }
  }
  DWORD code = (DWORD)-1; GetExitCodeProcess(pi.hProcess, &code); res.exitCode = code;
  CloseHandle(pi.hProcess);
  tOut.join(); tErr.join();
  CloseHandle(outR); CloseHandle(errR);
  return res;
}

bool SpawnPiped(const fs::path& exe, const std::vector<std::wstring>& args, PipedProcess& out, bool lowPriority) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE outR = nullptr, outW = nullptr;
  if (!CreatePipe(&outR, &outW, &sa, 1 << 20)) return false;
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  std::wstring cmd = QuoteArg(exe.wstring());
  for (auto& a : args) { cmd.push_back(L' '); cmd += QuoteArg(a); }
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  si.hStdInput = nul; si.hStdOutput = outW; si.hStdError = nul;
  PROCESS_INFORMATION pi{};
  DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
  CloseHandle(outW); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) { CloseHandle(outR); return false; }
  if (HANDLE job = SharedJob()) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread); CloseHandle(pi.hThread);
  out.process = pi.hProcess; out.stdoutRead = outR;
  return true;
}

std::string RunCapture(const fs::path& exe, const std::vector<std::wstring>& args, DWORD timeoutMs, int* exitCode) {
  ProcessOptions o; o.exe = exe; o.args = args; o.timeoutMs = timeoutMs; o.lowPriority = false;
  ProcessResult r = RunProcess(o);
  if (exitCode) *exitCode = (int)r.exitCode;
  return r.Ok() ? r.out : std::string();
}

} // namespace filo
