#include "core/crash.h"
#include "core/util.h"
#include "core/log.h"
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace filo::crash {

static LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  SYSTEMTIME st; GetLocalTime(&st);
  wchar_t stamp[64]; swprintf_s(stamp, L"crash-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
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
    fprintf(t, "Filo %s crash: exception 0x%08lX at %p\n", FILO_VERSION, ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
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
  }
  log::Write(log::Level::Error, "crash", "unhandled exception; dump written to the logs folder");
  return EXCEPTION_CONTINUE_SEARCH;
}

void Install() { SetUnhandledExceptionFilter(&Filter); }

} // namespace filo::crash
