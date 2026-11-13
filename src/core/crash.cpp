#include "core/crash.h"
#include "core/util.h"
#include "core/log.h"
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace filo::crash {

static bool g_restart = false;
static int g_previousRestarts = 0;
static ULONGLONG g_startTick = GetTickCount64();

// Writes <prefix>-<stamp>.dmp and .txt (symbolised stack of the faulting thread) into the logs folder.
static void WriteReport(EXCEPTION_POINTERS* ep, const wchar_t* prefix) {
  SYSTEMTIME st; GetLocalTime(&st);
  wchar_t stamp[80]; swprintf_s(stamp, L"%s-%04d%02d%02d-%02d%02d%02d", prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  fs::path dir = LogDir();
  fs::path dmp = dir / (std::wstring(stamp) + L".dmp");
  fs::path txt = dir / (std::wstring(stamp) + L".txt");

  HANDLE f = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr, nullptr);
    CloseHandle(f);
  }
  FILE* t = _wfopen(txt.c_str(), L"w");
  if (t) {
    fprintf(t, "Filo %s %ls: exception 0x%08lX at %p\n", FILO_VERSION, prefix, ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrFrame.Offset = ctx.Rbp; sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
      if (!sf.AddrPC.Offset) break;
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
      auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
      DWORD64 disp = 0; DWORD ld = 0;
      IMAGEHLP_LINE64 line{sizeof line};
      bool hasSym = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym);
      bool hasLine = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line);
      fprintf(t, "#%02d %p %s", i, (void*)sf.AddrPC.Offset, hasSym ? sym->Name : "?");
      if (hasLine) fprintf(t, "  (%s:%lu)", line.FileName, line.LineNumber);
      fprintf(t, "\n");
    }
    fclose(t);
    SymCleanup(proc);
  }
}

static void RelaunchSelf() {
  // A crash right after a start-up is not retried forever: three quick restarts in a row are the limit.
  const bool quick = GetTickCount64() - g_startTick < 120000;
  const int n = quick ? g_previousRestarts + 1 : 1;
  if (n > 3) return;
  const std::wstring cmd = QuoteArg(ExePath().wstring()) + L" --background --wait-pid " + std::to_wstring(GetCurrentProcessId()) +
                           L" --crash-restart " + std::to_wstring(n);
  std::wstring mutableCmd = cmd;
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  }
}

static LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  WriteReport(ep, L"crash");
  log::Write(log::Level::Error, "crash", "unhandled exception; dump written to the logs folder");
  if (g_restart) {
    RelaunchSelf();
    // Windows Error Reporting keeps a crashed process alive for a while; end it now so the single-instance lock is free for the restart.
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG ReportFaultFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG reports = 0;
  if (!_wgetenv(L"FILO_TEST_NO_FAULT_REPORTS") && InterlockedIncrement(&reports) <= 5) {            // a fault that repeats every frame must not fill the disk
    WriteReport(ep, L"fault");
    log::Write(log::Level::Error, "crash", "a window failed while drawing; that frame was dropped (fault report written to the logs folder)");
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

void Install() { g_startTick = GetTickCount64(); SetUnhandledExceptionFilter(&Filter); }

void EnableRestartOnCrash(int previousRestarts) { g_restart = true; g_previousRestarts = previousRestarts; }

} // namespace filo::crash
