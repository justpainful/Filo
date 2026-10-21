// The "Actions" wheel: Share, Float, Copy, Move, Copy Path, Terminal. Implemented with documented shell APIs only.
#pragma once
#include "core/util.h"
#include <vector>

namespace filo::shell {

// Clipboard in the form Explorer understands (CF_HDROP + Preferred DropEffect = copy): Ctrl+V in Explorer copies the files.
bool CopyFilesToClipboard(const std::vector<fs::path>& files, std::wstring* err = nullptr);
// One full path per line; optionally wrapped in double quotes. CF_UNICODETEXT.
bool CopyPathsToClipboard(const std::vector<fs::path>& files, bool quoted);
// Opens the terminal in the file's parent folder (or the folder itself). `which`: "wt" | "powershell" | "cmd".
bool OpenTerminalAt(const fs::path& target, const std::wstring& which, std::wstring* err = nullptr);
// Prompts for a destination folder, then moves with IFileOperation (progress UI, conflict dialog, cross-volume safe, undo).
bool MoveFilesInteractive(const std::vector<fs::path>& files, HWND owner, std::wstring* err = nullptr);
// Windows Share UI (Nearby sharing, apps, Bluetooth, email... whatever the user has) for the files.
bool ShareFiles(const std::vector<fs::path>& files, HWND owner, std::wstring* err = nullptr);
// Reveals the file in Explorer with the item selected.
void RevealInExplorer(const fs::path& file);

// Registers the six actions in the ToolRegistry (Float is registered by the UI module because it owns the shelf window).
void RegisterShellActions(HWND ownerWindow);

} // namespace filo::shell
