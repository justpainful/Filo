// End-to-end harness: acts as a real OLE drag SOURCE (like Explorer) and drives the mouse/keyboard with SendInput so the
// running Filo.exe sees exactly what it would see from a user: left button held, drag image window present, Shift/Ctrl/Alt
// pressed during the drag, the cursor moving over the wheel, then the button released over a sector.
//
//   filo_e2e_drag.exe --file <path> --mod shift|ctrl|alt --sector <k> [--count n] [--expect-clipboard] [--expect-file <path>]
//
// It only ever touches its own window and the wheel. Press ESC at any time to abort (the harness polls the key).
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")

static std::wstring g_file, g_shot;
static std::atomic<bool> g_dragStarted{false}, g_dragDone{false}, g_abort{false};
static DWORD g_dragResult = DROPEFFECT_NONE;
static HWND g_wnd;
static IDataObject* g_data = nullptr;

struct DropSource : IDropSource {
  LONG r = 1;
  STDMETHODIMP QueryInterface(REFIID i, void** p) override { if (i == IID_IUnknown || i == IID_IDropSource) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return ++r; }
  STDMETHODIMP_(ULONG) Release() override { return --r; }
  STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override {
    if (esc || g_abort) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
  }
  STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

// Minimal drop target that only drives the shell drag-image helper (what Explorer/desktop do): this is what makes the
// "SysDragImage" window appear while the cursor is over the source window.
struct HelperTarget : IDropTarget {
  IDropTargetHelper* h = nullptr;
  HWND hwnd;
  explicit HelperTarget(HWND w) : hwnd(w) { CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&h)); }
  STDMETHODIMP QueryInterface(REFIID i, void** p) override { if (i == IID_IUnknown || i == IID_IDropTarget) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* e) override { *e = DROPEFFECT_NONE; POINT p{pt.x, pt.y}; if (h) h->DragEnter(hwnd, d, &p, *e); return S_OK; }
  STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* e) override { *e = DROPEFFECT_NONE; POINT p{pt.x, pt.y}; if (h) h->DragOver(&p, *e); return S_OK; }
  STDMETHODIMP DragLeave() override { if (h) h->DragLeave(); return S_OK; }
  STDMETHODIMP Drop(IDataObject* d, DWORD, POINTL pt, DWORD* e) override { *e = DROPEFFECT_NONE; POINT p{pt.x, pt.y}; if (h) h->Drop(d, &p, *e); return S_OK; }
};
static HelperTarget* g_helperTarget = nullptr;

static void StartDrag(POINT pt) {
  if (g_dragStarted.exchange(true)) return;
  PIDLIST_ABSOLUTE pidl = nullptr;
  SHParseDisplayName(g_file.c_str(), nullptr, &pidl, 0, nullptr);
  IShellItem* item = nullptr; SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
  item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&g_data));
  IDragSourceHelper* helper = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
    POINT p = pt;  // client coordinates of the grab point
    helper->InitializeFromWindow(g_wnd, &p, g_data);
  }
  DropSource ds;
  DWORD eff = 0;
  HRESULT hr = DoDragDrop(g_data, &ds, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
  g_dragResult = eff;
  std::printf("DoDragDrop returned 0x%08lx effect=%lu\n", (unsigned long)hr, (unsigned long)eff);
  g_dragDone = true;
  if (helper) helper->Release();
  if (item) item->Release();
  ILFree(pidl);
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_MOUSEMOVE: if (w & MK_LBUTTON) StartDrag({(short)LOWORD(l), (short)HIWORD(l)}); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r); FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); DrawTextW(dc, L"Filo E2E drag source", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE); EndPaint(h, &ps); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

static void MouseMove(int x, int y) {
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  INPUT in{}; in.type = INPUT_MOUSE;
  in.mi.dx = (LONG)(((x - vx) * 65535.0) / (vw - 1)); in.mi.dy = (LONG)(((y - vy) * 65535.0) / (vh - 1));
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  SendInput(1, &in, sizeof in);
}
static void MouseButton(bool down) { INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; SendInput(1, &in, sizeof in); }
// Screen capture of a rectangle to a 24-bit BMP (for visual checks of the real composited wheel).
static void ShotBmp(const wchar_t* path, RECT r) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
  BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 24, BI_RGB};
  void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, scr, r.left, r.top, SRCCOPY | CAPTUREBLT);
  SelectObject(mem, old);
  const int stride = ((w * 3 + 3) / 4) * 4;
  FILE* f = _wfopen(path, L"wb");
  if (f) {
    BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + stride * h), 0, 0, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER))};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f); fwrite(bits, stride * h, 1, f); fclose(f);
  }
  DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, scr);
}
static void Key(WORD vk, bool down) { INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP; SendInput(1, &in, sizeof in); }
static void Sleep_(int ms) { for (int i = 0; i < ms / 10; ++i) { if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_abort = true; if (g_abort) return; Sleep(10); } }

int wmain(int argc, wchar_t** argv) {
  std::wstring mod = L"shift", expectFile; int sector = 0, count = 8; bool expectClip = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--file" && i + 1 < argc) g_file = argv[++i];
    else if (a == L"--mod" && i + 1 < argc) mod = argv[++i];
    else if (a == L"--sector" && i + 1 < argc) sector = _wtoi(argv[++i]);
    else if (a == L"--count" && i + 1 < argc) count = _wtoi(argv[++i]);
    else if (a == L"--expect-file" && i + 1 < argc) expectFile = argv[++i];
    else if (a == L"--expect-clipboard") expectClip = true;
    else if (a == L"--shot" && i + 1 < argc) g_shot = argv[++i];
  }
  if (g_file.empty()) { std::puts("usage: --file <path> --mod shift|ctrl|alt --sector k [--count n] [--expect-file p] [--expect-clipboard]"); return 2; }
  OleInitialize(nullptr);
  WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FiloE2ESource"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  g_wnd = CreateWindowExW(WS_EX_TOPMOST, L"FiloE2ESource", L"Filo E2E source", WS_POPUP | WS_VISIBLE | WS_BORDER, 200, 200, 360, 220, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(g_wnd, SW_SHOW);
  g_helperTarget = new HelperTarget(g_wnd);
  RegisterDragDrop(g_wnd, g_helperTarget);

  std::atomic<int> rc{1};
  std::thread driver([&] {
    Sleep_(600);
    RECT r; GetWindowRect(g_wnd, &r);
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const WORD vk = mod == L"ctrl" ? VK_CONTROL : (mod == L"alt" ? VK_MENU : VK_SHIFT);
    MouseMove(cx, cy); Sleep_(150);
    MouseButton(true); Sleep_(120);
    for (int i = 1; i <= 8 && !g_abort; ++i) { MouseMove(cx + i * 6, cy + i * 3); Sleep_(35); }
    for (int i = 0; i < 100 && !g_dragStarted && !g_abort; ++i) Sleep_(20);
    if (!g_dragStarted) { std::puts("FAIL: drag never started"); MouseButton(false); rc = 3; PostMessageW(g_wnd, WM_CLOSE, 0, 0); return; }
    Sleep_(350);
    std::printf("SysDragImage present: %d\n", FindWindowW(L"SysDragImage", nullptr) != nullptr);
    Key(vk, true); Sleep_(120);
    HWND wheel = nullptr; RECT wr{};
    for (int i = 0; i < 80 && !g_abort; ++i) {                      // up to ~1.6 s
      wheel = FindWindowW(L"Filo.Surface", L"Filo Wheel");
      if (wheel && IsWindowVisible(wheel)) break;
      wheel = nullptr; Sleep_(20);
    }
    if (!wheel) { std::puts("FAIL: wheel did not appear"); Key(vk, false); MouseButton(false); rc = 4; Sleep_(300); PostMessageW(g_wnd, WM_CLOSE, 0, 0); return; }
    Sleep_(350);                                                   // let the appear animation finish
    GetWindowRect(wheel, &wr);
    const double W = wr.right - wr.left, Rpx = W / (2.0 * 1.22);
    const double wx = (wr.left + wr.right) / 2.0, wy = (wr.top + wr.bottom) / 2.0;
    std::printf("wheel visible: rect=(%ld,%ld,%ld,%ld) R=%.1f\n", wr.left, wr.top, wr.right, wr.bottom, Rpx);
    const double ang = (360.0 / count) * sector * 3.14159265358979 / 180.0;
    const int tx = (int)std::lround(wx + std::sin(ang) * 0.672 * Rpx), ty = (int)std::lround(wy - std::cos(ang) * 0.672 * Rpx);
    POINT cur; GetCursorPos(&cur);
    for (int i = 1; i <= 14 && !g_abort; ++i) { MouseMove((int)(cur.x + (tx - cur.x) * i / 14.0), (int)(cur.y + (ty - cur.y) * i / 14.0)); Sleep_(25); }
    Sleep_(420);                                                    // hover animation settles
    if (!g_shot.empty()) { RECT sr{wr.left - 40, wr.top - 40, wr.right + 200, wr.bottom + 40}; ShotBmp(g_shot.c_str(), sr); std::wprintf(L"screenshot saved: %s\n", g_shot.c_str()); }
    Key(vk, false); Sleep_(60);
    MouseButton(false);
    for (int i = 0; i < 150 && !g_dragDone && !g_abort; ++i) Sleep_(20);
    Sleep_(700);
    rc = 0;
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  });
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  driver.join();
  if (g_abort) { std::puts("ABORTED by user (Esc)"); return 9; }
  int result = rc;
  if (result == 0 && g_dragResult != DROPEFFECT_COPY && g_dragResult != DROPEFFECT_LINK && !(g_dragResult == DROPEFFECT_NONE)) { std::puts("FAIL: source saw an unexpected effect (MOVE would delete the original!)"); result = 5; }
  if (result == 0 && (g_dragResult & DROPEFFECT_MOVE)) { std::puts("FAIL: MOVE effect returned to the source"); result = 5; }
  if (result == 0 && expectClip) {
    std::wstring clip;
    if (OpenClipboard(nullptr)) { HANDLE h = GetClipboardData(CF_UNICODETEXT); if (h) clip = (const wchar_t*)GlobalLock(h); if (h) GlobalUnlock(h); CloseClipboard(); }
    std::wprintf(L"clipboard: %s\n", clip.c_str());
    if (clip != g_file) { std::puts("FAIL: clipboard does not contain the path"); result = 6; }
  }
  if (result == 0 && !expectFile.empty()) {
    bool ok = false;
    for (int i = 0; i < 200; ++i) { if (PathFileExistsW(expectFile.c_str())) { ok = true; break; } Sleep(100); }
    std::wprintf(L"expected output %s: %s\n", expectFile.c_str(), ok ? L"present" : L"MISSING");
    if (!ok) result = 7;
  }
  if (GetFileAttributesW(g_file.c_str()) == INVALID_FILE_ATTRIBUTES) { std::puts("FAIL: the dragged ORIGINAL FILE disappeared"); result = 8; }
  std::puts(result == 0 ? "PASS" : "FAILED");
  return result;
}
