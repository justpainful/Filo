// Glue between the wheel window, the rule engine, thumbnails and the OLE drop target.
// Lifetime: created once by the runtime on the UI (STA) thread.
#pragma once
#include "wheel/wheel_window.h"
#include "registry/selection.h"
#include "shell/thumbnail.h"
#include "core/com.h"
#include <oleidl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <functional>
#include <memory>

namespace filo::shell {

class WheelController;

class WheelDropTarget : public IDropTarget {
 public:
  explicit WheelDropTarget(WheelController* c, HWND hwnd) : c_(c), hwnd_(hwnd) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IDropTargetHelper), helper_.AsVoid());   // keeps the drag image alive over the wheel
  }
  STDMETHODIMP QueryInterface(REFIID r, void** p) override;
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
  STDMETHODIMP DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override;
  STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* eff) override;
  STDMETHODIMP DragLeave() override;
  STDMETHODIMP Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override;
  void Detach() { c_ = nullptr; }
 private:
  ~WheelDropTarget() = default;
  LONG ref_ = 1;
  WheelController* c_;
  HWND hwnd_;
  ComPtr<IDropTargetHelper> helper_;
};

class WheelController {
 public:
  using ChooseFn = std::function<void(const std::wstring& entryId, const Selection& sel, POINT at)>;

  WheelController();
  ~WheelController();
  bool Init(HWND messageWnd, UINT thumbMessage);   // thumbnails are posted back to `messageWnd`
  void Shutdown();

  // `files` may be empty (drag source unknown): the wheel then waits for DragEnter's data object.
  void Open(wheel::WheelKind kind, POINT cursor, const std::vector<fs::path>& files, bool keyboardMode);
  void Switch(wheel::WheelKind kind);
  void Close(bool animated);
  bool IsOpen() const { return window_.Shown(); }
  wheel::WheelWindow& Window() { return window_; }

  ChooseFn onChoose;                       // a sector was chosen (drop or click/Enter)
  std::function<void()> onDropHandled;     // OLE Drop() delivered (accepted or not): drag session can finish
  std::function<void()> onClosed;          // wheel fully hidden

  // thumbnail posted back to the UI thread by the runtime's message loop
  void OnThumbnailReady(uint64_t ticket, std::shared_ptr<ThumbnailImage> img);

  // --- drop target entry points
  DWORD DropEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD offered);
  DWORD DropOver(DWORD keys, POINTL pt, DWORD offered);
  void DropLeave();
  DWORD DropDrop(IDataObject* d, DWORD keys, POINTL pt, DWORD offered);

  const Selection& CurrentSelection() const { return selection_; }
  wheel::WheelKind Kind() const { return kind_; }

 private:
  void SetSelection(const std::vector<fs::path>& files);
  void Rebuild(bool animate);
  wheel::WheelContent BuildContent() const;
  void Choose(int idx, POINT at);
  void RequestThumbnail();
  int HoveredEnabledIndex(POINTL pt) const;

  wheel::WheelWindow window_;
  WheelDropTarget* dropTarget_ = nullptr;
  wheel::WheelKind kind_ = wheel::WheelKind::Convert;
  Selection selection_;
  Availability avail_;
  std::vector<fs::path> dataPaths_;
  int page_ = 0;
  uint64_t ticket_ = 0;
  double hoverSince_ = 0;
  int lastHover_ = -1;
  HWND msgWnd_ = nullptr; UINT thumbMsg_ = 0;
  bool keyboardMode_ = false;
  bool chosen_ = false;

  struct Entry { std::wstring id; std::wstring label; std::string icon; bool enabled; std::wstring why; };
  std::vector<Entry> entries_;     // flattened entries of the current kind (all pages)
  std::vector<int> pageMap_;       // wheel index -> entries_ index (or -1 for the "More" item)
};

} // namespace filo::shell
