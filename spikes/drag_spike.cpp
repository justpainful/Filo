// SPIKE: validates the supported Explorer drag integration strategy before the real implementation.
//   1. WH_MOUSE_LL / WH_KEYBOARD_LL detect "LMB held + Shift pressed" during a drag.
//   2. IShellWindows/IFolderView2 reads the Explorer/desktop selection (real file paths) - documented COM only.
//   3. A topmost no-activate window with a registered IDropTarget appears under the cursor; OLE routes the drag to it.
//   4. The drop target only ever accepts DROPEFFECT_COPY, so the source can never move/delete the original.
// Build: see scripts (cl drag_spike.cpp ole32 shell32 user32 gdi32 oleaut32 uuid shlwapi)
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>
#include "core/com.h"
using filo::ComPtr;
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <shellapi.h>

static FILE* g_log;
static void Log(const wchar_t* fmt, ...) {
  wchar_t buf[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, ap); va_end(ap);
  SYSTEMTIME st; GetLocalTime(&st);
  fwprintf(g_log, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
  fflush(g_log);
}

static HWND g_main, g_wheel;
static bool g_lbtn = false, g_wheelShown = false, g_dragMoved = false;
static POINT g_down{};
static std::vector<std::wstring> g_sel;
constexpr UINT WM_SHOWWHEEL = WM_APP + 1, WM_HIDEWHEEL = WM_APP + 2;
constexpr int WHEEL = 360; // window size px
constexpr int RADIUS = 150;

// ---------------------------------------------------------------- selection via documented shell COM
static std::vector<std::wstring> ItemsToPaths(IShellItemArray* a) {
  std::vector<std::wstring> out; DWORD n = 0;
  if (!a || FAILED(a->GetCount(&n))) return out;
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> it;
    if (SUCCEEDED(a->GetItemAt(i, &it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out.push_back(p); CoTaskMemFree(p); }
    }
  }
  return out;
}
static std::vector<std::wstring> SelectionFromBrowser(IDispatch* disp, HWND hit, bool requireChild) {
  std::vector<std::wstring> out;
  ComPtr<IServiceProvider> sp; if (FAILED(disp->QueryInterface(__uuidof(IServiceProvider), sp.AsVoid()))) return out;
  ComPtr<IShellBrowser> sb; if (FAILED(sp->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser), sb.AsVoid()))) return out;
  ComPtr<IShellView> sv; if (FAILED(sb->QueryActiveShellView(&sv))) return out;
  if (requireChild) {
    HWND vw = nullptr; sv->GetWindow(&vw);
    if (!vw || !(hit == vw || IsChild(vw, hit))) return out;
  }
  ComPtr<IFolderView2> fv; if (FAILED(sv->QueryInterface(__uuidof(IFolderView2), fv.AsVoid()))) return out;
  ComPtr<IShellItemArray> arr;
  if (SUCCEEDED(fv->GetSelection(FALSE, &arr))) out = ItemsToPaths(arr.Get());
  return out;
}
static std::vector<std::wstring> QuerySelection(POINT pt) {
  std::vector<std::wstring> out;
  HWND hit = WindowFromPoint(pt);
  HWND root = GetAncestor(hit, GA_ROOT);
  wchar_t cls[96] = {}; GetClassNameW(root, cls, 96);
  Log(L"hit=%p root=%p class=%s", hit, root, cls);
  ComPtr<IShellWindows> sw;
  if (FAILED(filo::CoCreate(CLSID_ShellWindows, sw))) { Log(L"no IShellWindows"); return out; }
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) {
    VARIANT e1{}, e2{}; VariantInit(&e1); VariantInit(&e2); long h = 0; ComPtr<IDispatch> d;
    if (SUCCEEDED(sw->FindWindowSW(&e1, &e2, SWC_DESKTOP, &h, SWFO_NEEDDISPATCH, &d)) && d) out = SelectionFromBrowser(d.Get(), hit, false);
    Log(L"desktop selection: %zu items", out.size());
    return out;
  }
  long count = 0; sw->get_Count(&count);
  for (long i = 0; i < count; ++i) {
    VARIANT idx; VariantInit(&idx); idx.vt = VT_I4; idx.lVal = i; ComPtr<IDispatch> d;
    if (FAILED(sw->Item(idx, &d)) || !d) continue;
    ComPtr<IWebBrowserApp> wb; if (FAILED(d->QueryInterface(__uuidof(IWebBrowserApp), wb.AsVoid()))) continue;
    SHANDLE_PTR hw = 0; wb->get_HWND(&hw);
    if ((HWND)hw != root) continue;
    out = SelectionFromBrowser(d.Get(), hit, true);
    if (!out.empty()) break;
  }
  Log(L"explorer selection: %zu items", out.size());
  return out;
}

// ---------------------------------------------------------------- drop target
struct DropTarget : IDropTarget {
  LONG ref = 1;
  STDMETHODIMP QueryInterface(REFIID r, void** p) override {
    if (r == IID_IUnknown || r == IID_IDropTarget) { *p = this; AddRef(); return S_OK; } *p = nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&ref); }
  static bool Inside(POINTL pt) {
    RECT r; GetWindowRect(g_wheel, &r);
    double dx = pt.x - (r.left + r.right) / 2.0, dy = pt.y - (r.top + r.bottom) / 2.0;
    return std::sqrt(dx * dx + dy * dy) <= RADIUS;
  }
  STDMETHODIMP DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
    FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM m{};
    Log(L"DragEnter keys=0x%x eff=0x%x pt=(%ld,%ld)", keys, *eff, pt.x, pt.y);
    if (SUCCEEDED(d->GetData(&f, &m))) {
      UINT n = DragQueryFileW((HDROP)m.hGlobal, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; ++i) { wchar_t p[MAX_PATH * 2]; DragQueryFileW((HDROP)m.hGlobal, i, p, (UINT)std::size(p)); Log(L"  DataObject file[%u]=%s", i, p); }
      ReleaseStgMedium(&m);
    } else Log(L"  no CF_HDROP");
    *eff = Inside(pt) ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
  STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* eff) override {
    static ULONGLONG last = 0; if (GetTickCount64() - last > 250) { last = GetTickCount64(); Log(L"DragOver keys=0x%x pt=(%ld,%ld) inside=%d", keys, pt.x, pt.y, Inside(pt)); }
    *eff = Inside(pt) ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
  STDMETHODIMP DragLeave() override { Log(L"DragLeave"); return S_OK; }
  STDMETHODIMP Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
    Log(L"DROP at (%ld,%ld) inside=%d keys=0x%x offered eff=0x%x", pt.x, pt.y, Inside(pt), keys, *eff);
    *eff = Inside(pt) ? DROPEFFECT_COPY : DROPEFFECT_NONE; PostMessage(g_main, WM_HIDEWHEEL, 0, 0); return S_OK; }
};
static DropTarget g_dt;

// ---------------------------------------------------------------- layered wheel window (GDI placeholder art)
static void PaintWheel() {
  HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), WHEEL, -WHEEL, 1, 32, BI_RGB};
  uint32_t* px; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  for (int y = 0; y < WHEEL; ++y) for (int x = 0; x < WHEEL; ++x) {
    double dx = x + 0.5 - WHEEL / 2.0, dy = y + 0.5 - WHEEL / 2.0, r = std::sqrt(dx * dx + dy * dy);
    double a = std::clamp(RADIUS + 0.5 - r, 0.0, 1.0);
    double hole = std::clamp(r - 55.0, 0.0, 1.0);
    a *= hole;
    BYTE al = (BYTE)(a * 200), v = (BYTE)(255 * al / 255.0 * 0.95);
    px[y * WHEEL + x] = (al << 24) | (v << 16) | (v << 8) | v;
  }
  POINT src{0, 0}; SIZE sz{WHEEL, WHEEL}; BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  RECT r; GetWindowRect(g_wheel, &r); POINT pos{r.left, r.top};
  UpdateLayeredWindow(g_wheel, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
static LRESULT CALLBACK WheelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- hooks (kept tiny: post to the UI thread)
static LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION) {
    auto* ms = (MSLLHOOKSTRUCT*)l;
    if (w == WM_LBUTTONDOWN) { g_lbtn = true; g_dragMoved = false; g_down = ms->pt; }
    else if (w == WM_LBUTTONUP) { g_lbtn = false; PostMessage(g_main, WM_HIDEWHEEL, 0, 0); }
    else if (w == WM_MOUSEMOVE && g_lbtn && !g_dragMoved) {
      if (abs(ms->pt.x - g_down.x) > GetSystemMetrics(SM_CXDRAG) || abs(ms->pt.y - g_down.y) > GetSystemMetrics(SM_CYDRAG)) g_dragMoved = true;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}
static LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
    auto* k = (KBDLLHOOKSTRUCT*)l;
    if ((k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT) && g_lbtn && g_dragMoved) { POINT pt; GetCursorPos(&pt); PostMessage(g_main, WM_SHOWWHEEL, pt.x, pt.y); }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_SHOWWHEEL: {
    if (g_wheelShown) return 0;
    POINT pt{(LONG)w, (LONG)l};
    double t0 = (double)GetTickCount64();
    g_sel = QuerySelection(pt);
    for (auto& s : g_sel) Log(L"  selection: %s", s.c_str());
    SetWindowPos(g_wheel, HWND_TOPMOST, pt.x - WHEEL / 2, pt.y - WHEEL / 2, WHEEL, WHEEL, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PaintWheel(); g_wheelShown = true;
    Log(L"wheel shown at (%ld,%ld) selection+show took %.0f ms", pt.x, pt.y, (double)GetTickCount64() - t0);
    return 0; }
  case WM_HIDEWHEEL: if (g_wheelShown) { ShowWindow(g_wheel, SW_HIDE); g_wheelShown = false; Log(L"wheel hidden"); } return 0;
  case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
  filo::ComInit ci0; (void)ci0;
  g_log = _wfopen(L"drag_spike.log", L"w, ccs=UTF-8");
  OleInitialize(nullptr);
  WNDCLASSW wc{}; wc.hInstance = hi; wc.lpfnWndProc = MainProc; wc.lpszClassName = L"SpikeMain"; RegisterClassW(&wc);
  wc.lpfnWndProc = WheelProc; wc.lpszClassName = L"SpikeWheel"; RegisterClassW(&wc);
  g_main = CreateWindowExW(0, L"SpikeMain", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
  g_wheel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SpikeWheel", L"", WS_POPUP, 0, 0, WHEEL, WHEEL, nullptr, nullptr, hi, nullptr);
  HRESULT hr = RegisterDragDrop(g_wheel, &g_dt);
  Log(L"RegisterDragDrop hr=0x%08X", hr);
  HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, hi, 0), hk = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, hi, 0);
  Log(L"hooks installed %p %p; ready", hm, hk);
  MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  UnhookWindowsHookEx(hm); UnhookWindowsHookEx(hk); RevokeDragDrop(g_wheel); OleUninitialize();
  return 0;
}
