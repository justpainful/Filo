#include "core/util.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <chrono>

namespace filo {

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string Narrow(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring ToLower(std::wstring_view s) {
  std::wstring r(s);
  if (!r.empty()) CharLowerBuffW(r.data(), (DWORD)r.size());
  return r;
}
std::wstring Trim(std::wstring_view s) {
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) ++a;
  while (b > a && iswspace(s[b - 1])) --b;
  return std::wstring(s.substr(a, b - a));
}
bool IEquals(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}
bool StartsWithI(std::wstring_view s, std::wstring_view p) {
  return s.size() >= p.size() && IEquals(s.substr(0, p.size()), p);
}
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(sep, i);
    if (j == std::wstring_view::npos) j = s.size();
    out.emplace_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}
std::wstring ExtOf(const fs::path& p) {
  std::wstring e = p.extension().wstring();
  if (!e.empty() && e[0] == L'.') e.erase(0, 1);
  return ToLower(e);
}

std::wstring FormatBytes(uint64_t b) {
  wchar_t buf[48];
  if (b < 1000) swprintf_s(buf, L"%llu B", b);
  else if (b < 1000ull * 1000) swprintf_s(buf, L"%.0f KB", b / 1e3);
  else if (b < 1000ull * 1000 * 1000) swprintf_s(buf, L"%.1f MB", b / 1e6);
  else swprintf_s(buf, L"%.2f GB", b / 1e9);
  return buf;
}
std::wstring FormatTime(double sec, bool centis) {
  if (sec < 0) sec = 0;
  int total = (int)sec;
  int cs = (int)((sec - total) * 100.0 + 0.5);
  if (cs >= 100) { cs = 0; ++total; }
  int h = total / 3600, m = (total / 60) % 60, s = total % 60;
  wchar_t buf[48];
  if (h > 0) {
    if (centis) swprintf_s(buf, L"%d:%02d:%02d.%02d", h, m, s, cs); else swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
  } else {
    if (centis) swprintf_s(buf, L"%d:%02d.%02d", m, s, cs); else swprintf_s(buf, L"%d:%02d", m, s);
  }
  return buf;
}

std::wstring LongPath(const fs::path& p) {
  std::wstring s = fs::absolute(p).lexically_normal().wstring();
  std::replace(s.begin(), s.end(), L'/', L'\\');
  if (s.size() >= 240 && !StartsWithI(s, L"\\\\?\\")) {
    if (StartsWithI(s, L"\\\\")) s = L"\\\\?\\UNC\\" + s.substr(2);
    else s = L"\\\\?\\" + s;
  }
  return s;
}

std::wstring QuoteArg(std::wstring_view a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(a);
  std::wstring r = L"\"";
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
    if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
    if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(L'"'); }
    else { r.append(bs, L'\\'); r.push_back(a[i]); }
  }
  r.push_back(L'"');
  return r;
}

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}
fs::path AppDataDir() {
  static fs::path d = [] { auto p = KnownFolder(FOLDERID_LocalAppData) / L"Filo"; std::error_code ec; fs::create_directories(p, ec); return p; }();
  return d;
}
fs::path LogDir()  { auto p = AppDataDir() / L"logs";  std::error_code ec; fs::create_directories(p, ec); return p; }
fs::path TempDir() { auto p = AppDataDir() / L"temp";  std::error_code ec; fs::create_directories(p, ec); return p; }
fs::path ExePath() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n < buf.size()) { buf.resize(n); break; }
    buf.resize(buf.size() * 2);
  }
  return buf;
}
fs::path ExeDir() { return ExePath().parent_path(); }

fs::path FindTool(std::wstring_view exeName) {
  std::error_code ec;
  fs::path a = ExeDir() / L"tools" / exeName;
  if (fs::exists(a, ec)) return a;
  a = ExeDir() / exeName;
  if (fs::exists(a, ec)) return a;
  wchar_t buf[MAX_PATH * 2];
  std::wstring name(exeName);
  if (SearchPathW(nullptr, name.c_str(), nullptr, (DWORD)std::size(buf), buf, nullptr)) return buf;
  return {};
}

fs::path UniquePath(const fs::path& desired) {
  std::error_code ec;
  if (!fs::exists(desired, ec)) return desired;
  fs::path dir = desired.parent_path(), stem = desired.stem(), ext = desired.extension();
  for (int i = 1; i < 100000; ++i) {
    fs::path c = dir / (stem.wstring() + L" (" + std::to_wstring(i) + L")" + ext.wstring());
    if (!fs::exists(c, ec)) return c;
  }
  return desired;
}

std::wstring HResultMessage(HRESULT hr) {
  wchar_t* msg = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr);
  std::wstring s = n ? std::wstring(msg, n) : L"";
  if (msg) LocalFree(msg);
  while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
  wchar_t code[24]; swprintf_s(code, L" (0x%08X)", (unsigned)hr);
  return s + code;
}
std::wstring LastErrorMessage(DWORD err) { return HResultMessage(HRESULT_FROM_WIN32(err)); }

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace filo
