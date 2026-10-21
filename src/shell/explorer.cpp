#include "shell/explorer.h"
#include "core/log.h"
#include <shellapi.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace filo::shell {

static std::vector<fs::path> ItemsToPaths(IShellItemArray* a) {
  std::vector<fs::path> out;
  DWORD n = 0;
  if (!a || FAILED(a->GetCount(&n))) return out;
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> it;
    if (FAILED(a->GetItemAt(i, &it))) continue;
    PWSTR p = nullptr;
    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) out.emplace_back(p);
    CoTaskMemFree(p);
  }
  return out;
}

static bool FillFromBrowser(IDispatch* disp, HWND hit, bool requireChild, ExplorerContext& ctx) {
  ComPtr<IServiceProvider> sp;
  if (FAILED(disp->QueryInterface(__uuidof(IServiceProvider), sp.AsVoid()))) return false;
  ComPtr<IShellBrowser> sb;
  if (FAILED(sp->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser), sb.AsVoid()))) return false;
  ComPtr<IShellView> sv;
  if (FAILED(sb->QueryActiveShellView(&sv))) return false;
  if (requireChild && hit) {
    HWND vw = nullptr; sv->GetWindow(&vw);
    if (!vw || !(hit == vw || IsChild(vw, hit))) return false;    // another tab / another view
  }
  ComPtr<IFolderView2> fv;
  if (FAILED(sv->QueryInterface(__uuidof(IFolderView2), fv.AsVoid()))) return false;
  ComPtr<IShellItemArray> arr;
  if (SUCCEEDED(fv->GetSelection(FALSE, &arr))) ctx.selection = ItemsToPaths(arr.Get());
  // current folder
  ComPtr<IShellItem> folder;
  if (SUCCEEDED(fv->GetFolder(__uuidof(IShellItem), folder.AsVoid()))) {
    PWSTR p = nullptr;
    if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) ctx.folder = p;
    CoTaskMemFree(p);
  }
  ctx.valid = true;
  return true;
}

bool IsShellViewWindow(HWND hit) {
  if (!hit) return false;
  HWND root = GetAncestor(hit, GA_ROOT);
  wchar_t cls[96] = {};
  GetClassNameW(root, cls, 96);
  if (!wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return true;
  // file dialogs also host SHELLDLL_DefView
  for (HWND w = hit; w; w = GetParent(w)) {
    GetClassNameW(w, cls, 96);
    if (!wcscmp(cls, L"SHELLDLL_DefView")) return true;
  }
  return false;
}

static ExplorerContext QueryForWindow(HWND hit, HWND root) {
  ExplorerContext ctx;
  ctx.root = root;
  wchar_t cls[96] = {};
  GetClassNameW(root, cls, 96);
  ComPtr<IShellWindows> sw;
  if (FAILED(CoCreate(CLSID_ShellWindows, sw))) { FILO_LOG(Warn, "explorer", "IShellWindows unavailable"); return ctx; }
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) {
    VARIANT e1, e2; VariantInit(&e1); VariantInit(&e2);
    long h = 0; ComPtr<IDispatch> d;
    if (SUCCEEDED(sw->FindWindowSW(&e1, &e2, SWC_DESKTOP, &h, SWFO_NEEDDISPATCH, &d)) && d) {
      ctx.isDesktop = FillFromBrowser(d.Get(), hit, false, ctx);
    }
    return ctx;
  }
  long count = 0; sw->get_Count(&count);
  for (long i = 0; i < count; ++i) {
    VARIANT idx; VariantInit(&idx); idx.vt = VT_I4; idx.lVal = i;
    ComPtr<IDispatch> d;
    if (FAILED(sw->Item(idx, &d)) || !d) continue;
    ComPtr<IWebBrowserApp> wb;
    if (FAILED(d->QueryInterface(__uuidof(IWebBrowserApp), wb.AsVoid()))) continue;
    SHANDLE_PTR hw = 0; wb->get_HWND(&hw);
    if ((HWND)hw != root) continue;
    if (FillFromBrowser(d.Get(), hit, hit != nullptr, ctx)) break;
  }
  return ctx;
}

ExplorerContext QueryExplorerAtPoint(POINT pt) {
  HWND hit = WindowFromPoint(pt);
  if (!hit) return {};
  HWND root = GetAncestor(hit, GA_ROOT);
  return QueryForWindow(hit, root);
}

ExplorerContext QueryForegroundExplorer() {
  HWND fg = GetForegroundWindow();
  if (!fg) return {};
  HWND root = GetAncestor(fg, GA_ROOT);
  wchar_t cls[96] = {};
  GetClassNameW(root, cls, 96);
  if (wcscmp(cls, L"CabinetWClass") && wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW")) return {};
  // For the desktop the focus child is the list view; for Explorer let the active tab decide (hit = nullptr).
  HWND focus = nullptr;
  GUITHREADINFO gi{sizeof gi};
  if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi)) focus = gi.hwndFocus;
  return QueryForWindow(focus ? focus : fg, root);
}

std::vector<fs::path> PathsFromDataObject(IDataObject* d) {
  std::vector<fs::path> out;
  if (!d) return out;
  ComPtr<IShellItemArray> arr;
  if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(d, __uuidof(IShellItemArray), arr.AsVoid()))) out = ItemsToPaths(arr.Get());
  if (!out.empty()) return out;
  FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM m{};
  if (SUCCEEDED(d->GetData(&f, &m))) {
    UINT n = DragQueryFileW((HDROP)m.hGlobal, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
      UINT len = DragQueryFileW((HDROP)m.hGlobal, i, nullptr, 0);
      std::wstring p(len, L'\0');
      DragQueryFileW((HDROP)m.hGlobal, i, p.data(), len + 1);
      out.emplace_back(p);
    }
    ReleaseStgMedium(&m);
  }
  return out;
}

} // namespace filo::shell
