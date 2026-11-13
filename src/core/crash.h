// Crash diagnostics: an unhandled-exception filter that writes a minidump + a symbolised stack trace into %LOCALAPPDATA%\Filo\logs.
// Dumps contain process memory; they stay local and are never uploaded (see PRIVACY in docs).
//
// Two safety nets sit on top of that:
//  * a drawing fault (a bug while a window paints) is reported as a non-fatal "fault-*" report and only that frame is dropped
//    (Surface::RenderNow uses ReportFaultFilter);
//  * if the background app does crash anyway, it starts itself again (at most 3 times in a row), so the tray icon never just vanishes.
#pragma once
#include <windows.h>

namespace filo::crash {

void Install();

// Background runtime only: relaunch Filo (with --background) after an unhandled crash. `previousRestarts` comes from --crash-restart.
void EnableRestartOnCrash(int previousRestarts);

// Exception filter for __try/__except: writes a non-fatal report (rate limited) and returns EXCEPTION_EXECUTE_HANDLER.
LONG ReportFaultFilter(EXCEPTION_POINTERS* ep);

}  // namespace filo::crash
