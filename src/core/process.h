// Safe child-process runner: never goes through a shell, quotes every argument, captures output, supports cancellation
// and timeouts, and ties children to a Job Object so they die with Filo (no orphaned ffmpeg after a crash).
#pragma once
#include "core/util.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace filo {

struct ProcessOptions {
  fs::path exe;
  std::vector<std::wstring> args;                 // raw arguments; quoting is done here
  fs::path cwd;
  std::function<void(const std::string& line)> onStdoutLine;   // called on the runner's thread
  std::function<void(const std::string& line)> onStderrLine;
  const std::atomic<bool>* cancel = nullptr;      // polled ~every 50 ms
  DWORD timeoutMs = INFINITE;
  bool lowPriority = true;                        // BELOW_NORMAL: keeps the desktop responsive during long encodes
  size_t captureLimit = 4u << 20;                 // bytes kept in ProcessResult::out/err
  bool mergeStderr = false;
  std::string stdinData;                          // optional, written then closed
};

struct ProcessResult {
  bool started = false;
  bool cancelled = false;
  bool timedOut = false;
  DWORD exitCode = (DWORD)-1;
  std::string out, err;
  std::wstring startError;
  bool Ok() const { return started && !cancelled && !timedOut && exitCode == 0; }
};

ProcessResult RunProcess(const ProcessOptions& o);

// Starts a child with a piped stdout (binary-safe) for streaming consumers such as the media player. The process is placed in
// the kill-on-close job. `stdoutRead` must be closed by the caller; terminate with TerminateProcess + CloseHandle.
struct PipedProcess { HANDLE process = nullptr; HANDLE stdoutRead = nullptr; };
bool SpawnPiped(const fs::path& exe, const std::vector<std::wstring>& args, PipedProcess& out, bool lowPriority = false);

// Convenience: run and return stdout (empty on failure).
std::string RunCapture(const fs::path& exe, const std::vector<std::wstring>& args, DWORD timeoutMs = 60000, int* exitCode = nullptr);

} // namespace filo
