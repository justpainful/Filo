#include "core/log.h"
#include "core/util.h"
#include <mutex>
#include <cstdio>
#include <atomic>

namespace filo::log {
static std::mutex g_mu;
static FILE* g_file = nullptr;
static std::atomic<int> g_min{(int)Level::Info};
static std::wstring g_path;

void Init(const wchar_t* name) {
  std::lock_guard lk(g_mu);
  if (g_file) return;
  fs::path p = LogDir() / name;
  std::error_code ec;
  // rotate at 2 MB
  if (fs::exists(p, ec) && fs::file_size(p, ec) > 2u * 1024 * 1024) {
    fs::path old = p; old += L".1";
    fs::remove(old, ec); fs::rename(p, old, ec);
  }
  g_file = _wfsopen(p.c_str(), L"ab", _SH_DENYWR);
  g_path = p.wstring();
#ifdef _DEBUG
  g_min = (int)Level::Debug;
#endif
}
void SetMinLevel(Level l) { g_min = (int)l; }
std::wstring CurrentLogPath() { return g_path; }

void Write(Level l, std::string_view comp, std::string_view msg) {
  if ((int)l < g_min.load()) return;
  static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
  SYSTEMTIME st; GetLocalTime(&st);
  char head[96];
  int n = snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [t%lu] %.*s: ", st.wYear, st.wMonth, st.wDay, st.wHour,
                   st.wMinute, st.wSecond, st.wMilliseconds, names[(int)l], GetCurrentThreadId(), (int)comp.size(), comp.data());
  std::lock_guard lk(g_mu);
  if (g_file) {
    fwrite(head, 1, n, g_file);
    fwrite(msg.data(), 1, msg.size(), g_file);
    fputc('\n', g_file);
    fflush(g_file);
  }
  std::string dbg(head, n); dbg.append(msg); dbg.push_back('\n');
  OutputDebugStringA(dbg.c_str());
}
void Writef(Level l, std::string_view comp, const char* fmt, ...) {
  if ((int)l < g_min.load()) return;
  char buf[2048];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Write(l, comp, buf);
}
}
