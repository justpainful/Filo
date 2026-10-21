// Documented-COM access to File Explorer / Desktop state: selection, folder, window. No injection, no private interfaces:
// IShellWindows + IServiceProvider(SID_STopLevelBrowser) + IShellBrowser + IFolderView2 (the same path Raymond Chen's
// "get the selection of a shell window" articles and countless shell tools use).
#pragma once
#include "core/com.h"
#include "core/util.h"
#include <shobjidl.h>
#include <vector>

namespace filo::shell {

struct ExplorerContext {
  bool valid = false;
  bool isDesktop = false;
  HWND root = nullptr;                 // top-level Explorer window (or Progman/WorkerW)
  fs::path folder;                     // folder currently displayed (when it is a file-system folder)
  std::vector<fs::path> selection;     // selected file-system items
};

// Selection of the shell view under `screenPt` (the window the drag started in). Must run on an STA thread.
ExplorerContext QueryExplorerAtPoint(POINT screenPt);
// Selection of the foreground Explorer/Desktop window (keyboard invocation).
ExplorerContext QueryForegroundExplorer();

// Paths from an OLE data object (CF_HDROP / shell ID list via SHCreateShellItemArrayFromDataObject).
std::vector<fs::path> PathsFromDataObject(IDataObject* d);

// True when the window under the point belongs to an Explorer-style shell view (cheap class-name check).
bool IsShellViewWindow(HWND hit);

} // namespace filo::shell
