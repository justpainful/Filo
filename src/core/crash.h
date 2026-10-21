// Crash diagnostics: an unhandled-exception filter that writes a minidump + a symbolised stack trace into %LOCALAPPDATA%\Filo\logs.
// Dumps contain process memory; they stay local and are never uploaded (see PRIVACY in docs).
#pragma once
namespace filo::crash {
void Install();
}
