// Filo core utilities: strings, paths, small helpers. No dependencies beyond the Win32 SDK + STL.
#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <optional>
#include <functional>
#include <cstdint>

namespace filo {

namespace fs = std::filesystem;

std::wstring Widen(std::string_view utf8);
std::string  Narrow(std::wstring_view w);
std::wstring ToLower(std::wstring_view s);
std::wstring Trim(std::wstring_view s);
bool         IEquals(std::wstring_view a, std::wstring_view b);
bool         StartsWithI(std::wstring_view s, std::wstring_view prefix);
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep);

// Extension without the dot, lowercased ("photo.JPG" -> "jpg").
std::wstring ExtOf(const fs::path& p);

// "2.2 MB", "830 KB" - base 1000 like the reference UI (macOS Finder).
std::wstring FormatBytes(uint64_t bytes);
// "0:41.74" (m:ss.cc) or "1:02:03.45"
std::wstring FormatTime(double seconds, bool centis = true);

// Adds the \\?\ prefix for long paths (>= 240 chars) and normalises separators.
std::wstring LongPath(const fs::path& p);

// Quote one argument for CreateProcess command lines (CommandLineToArgvW rules). Prevents injection via file names.
std::wstring QuoteArg(std::wstring_view a);

// Per-user data directories: %LOCALAPPDATA%\Filo\{logs,cache,temp}
fs::path AppDataDir();
fs::path LogDir();
fs::path TempDir();           // job scratch space, cleaned at start-up
fs::path ExeDir();
fs::path ExePath();

// Finds "name" next to the exe (tools\ subfolder first), then in PATH. Empty when not found.
fs::path FindTool(std::wstring_view exeName);

// Picks a file name that does not exist yet: "photo.webp" -> "photo (1).webp" -> "photo (2).webp"...
fs::path UniquePath(const fs::path& desired);

// Scope guard
template <class F> struct ScopeExit {
  F f; bool armed = true;
  explicit ScopeExit(F fn) : f(std::move(fn)) {}
  ScopeExit(ScopeExit&& o) noexcept : f(std::move(o.f)), armed(o.armed) { o.armed = false; }
  ~ScopeExit() { if (armed) f(); }
  void dismiss() { armed = false; }
};
template <class F> ScopeExit<F> OnExit(F f) { return ScopeExit<F>(std::move(f)); }

std::wstring HResultMessage(HRESULT hr);
std::wstring LastErrorMessage(DWORD err = GetLastError());

// Monotonic seconds.
double NowSeconds();

} // namespace filo
