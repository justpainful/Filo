#include "test.h"
#include <windows.h>
#include <objbase.h>
#include <crtdbg.h>
#include <cstdlib>

int main(int argc, char** argv) {
  // Automated runs must never block on a modal "Debug Assertion Failed" / crash dialog: report to stderr and abort so the
  // runner (and the person at the keyboard) see a normal failure with a non-zero exit code.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) { _CrtSetReportMode(t, _CRTDBG_MODE_FILE); _CrtSetReportFile(t, _CRTDBG_FILE_STDERR); }
#endif
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int r = ftest::RunAll(argc, argv);
  CoUninitialize();
  return r;
}
