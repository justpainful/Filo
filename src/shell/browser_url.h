// Reads the address of the page open in the foreground browser (Chrome, Edge, Firefox, Brave, Opera, ...) through UI Automation:
// the same documented interface screen readers use. Read-only: nothing is typed, clicked or changed, and the clipboard is untouched.
#pragma once
#include <windows.h>
#include <string>

namespace filo::shell {

// `window`: the foreground top-level window. Returns "https://..." or an empty string when it is not a browser, has no address bar
// Filo can read, or the bar holds a search instead of an address. Must run on a COM-initialised (STA) thread.
std::wstring BrowserPageUrl(HWND window);

// Pure helper (unit-tested): turns what an address bar shows ("www.youtube.com/watch?v=1", "https://x.com/a/status/1") into an
// absolute address, or "" when the text is not an address (spaces, no dot, ...).
std::wstring NormalizeAddressBarText(const std::wstring& text);

}  // namespace filo::shell
