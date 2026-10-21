#include "shell/wheel_controller.h"
#include "shell/explorer.h"
#include "core/log.h"
#include <algorithm>
#include <set>

namespace filo::shell {

using wheel::WheelKind;

// ------------------------------------------------------------------------------------------------ drop target shim
STDMETHODIMP WheelDropTarget::QueryInterface(REFIID r, void** p) {
  if (r == IID_IUnknown || r == IID_IDropTarget) { *p = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
  *p = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP WheelDropTarget::DragEnter(IDataObject* d, DWORD k, POINTL pt, DWORD* eff) {
  *eff = c_ ? c_->DropEnter(d, k, pt, *eff) : DROPEFFECT_NONE;
  if (helper_) { POINT p{pt.x, pt.y}; helper_->DragEnter(hwnd_, d, &p, *eff); }
  return S_OK;
}
STDMETHODIMP WheelDropTarget::DragOver(DWORD k, POINTL pt, DWORD* eff) {
  *eff = c_ ? c_->DropOver(k, pt, *eff) : DROPEFFECT_NONE;
  if (helper_) { POINT p{pt.x, pt.y}; helper_->DragOver(&p, *eff); }
  return S_OK;
}
STDMETHODIMP WheelDropTarget::DragLeave() {
  if (helper_) helper_->DragLeave();
  if (c_) c_->DropLeave();
  return S_OK;
}
STDMETHODIMP WheelDropTarget::Drop(IDataObject* d, DWORD k, POINTL pt, DWORD* eff) {
  DWORD offered = *eff;
  *eff = c_ ? c_->DropDrop(d, k, pt, offered) : DROPEFFECT_NONE;
  if (helper_) { POINT p{pt.x, pt.y}; helper_->Drop(d, &p, *eff); }
  return S_OK;
}

// ------------------------------------------------------------------------------------------------ controller
WheelController::WheelController() = default;
WheelController::~WheelController() { Shutdown(); }

bool WheelController::Init(HWND messageWnd, UINT thumbMessage) {
  msgWnd_ = messageWnd; thumbMsg_ = thumbMessage;
  if (!window_.Init()) return false;
  dropTarget_ = new WheelDropTarget(this, window_.Hwnd());
  HRESULT hr = RegisterDragDrop(window_.Hwnd(), dropTarget_);
  if (FAILED(hr)) { FILO_LOG(Error, "wheel", "RegisterDragDrop failed: %ls", HResultMessage(hr).c_str()); return false; }
  window_.onChoose = [this](int idx) { Choose(idx, window_.CenterScreen()); };
  window_.onDismissed = [this] { if (onClosed) onClosed(); };
  return true;
}

void WheelController::Shutdown() {
  if (dropTarget_) {
    dropTarget_->Detach();
    if (window_.Valid()) RevokeDragDrop(window_.Hwnd());
    dropTarget_->Release();
    dropTarget_ = nullptr;
  }
  window_.Destroy();
}

void WheelController::SetSelection(const std::vector<fs::path>& files) {
  selection_ = Selection{};
  if (files.size() > 400) {   // quick extension-only classification for huge batches
    for (auto& p : files) {
      FileKind k; k.path = p; k.exists = true; k.readable = true; k.declaredExt = CanonicalExt(ExtOf(p));
      k.format = k.declaredExt; k.group = GroupForExt(k.format);
      selection_.items.push_back(k);
    }
  } else {
    selection_ = Selection::FromPaths(files);
  }
  avail_ = ComputeAvailability(selection_);
}

wheel::WheelContent WheelController::BuildContent() const {
  wheel::WheelContent c;
  c.kind = kind_;
  const int per = wheel::kMaxItems;
  if ((int)entries_.size() <= per) {
    for (const auto& e : entries_) c.items.push_back({e.id, e.label, e.icon, e.enabled, e.why});
  } else {
    const int perPage = per - 1;                     // the last slot is "More"
    const int pages = ((int)entries_.size() + perPage - 1) / perPage;
    const int pg = page_ % pages;
    for (int i = pg * perPage; i < std::min((int)entries_.size(), (pg + 1) * perPage); ++i) {
      const auto& e = entries_[i];
      c.items.push_back({e.id, e.label, e.icon, e.enabled, e.why});
    }
    c.items.push_back({L"page:next", L"More", "more", true, L""});
  }
  if (selection_.count() == 1) {
    c.centerPrimary = selection_.items[0].isDir ? L"Folder" : FormatBytes(selection_.items[0].size);
    c.centerSecondary = selection_.items[0].path.filename().wstring();
  } else if (selection_.count() > 1) {
    c.centerPrimary = std::to_wstring(selection_.count()) + L" files";
    c.centerSecondary = FormatBytes(selection_.TotalSize());
  }
  if (selection_.empty()) c.centerIcon = "document";
  if (c.items.empty()) {
    if (selection_.empty()) c.message = L"Drag files here";
    else if (kind_ == WheelKind::Convert) c.message = avail_.convertEmptyReason;
    else if (kind_ == WheelKind::Tools) c.message = avail_.toolsEmptyReason;
  }
  return c;
}

void WheelController::Rebuild(bool animate) {
  entries_.clear();
  const std::vector<WheelEntry>* src = kind_ == WheelKind::Convert ? &avail_.convert : (kind_ == WheelKind::Tools ? &avail_.tools : &avail_.actions);
  for (auto& e : *src) entries_.push_back({e.id, e.label, e.icon, e.enabled, e.why});
  window_.SetContent(BuildContent(), animate);
}

void WheelController::RequestThumbnail() {
  window_.ClearThumbnail();
  ThumbnailService::Get().CancelOwner(1);
  if (selection_.count() != 1 || selection_.items[0].isDir || !msgWnd_) return;
  const uint64_t ticket = ++ticket_;
  HWND hw = msgWnd_; UINT msg = thumbMsg_;
  ThumbnailService::Get().Request(selection_.items[0].path, 256, 1, [hw, msg, ticket](const ThumbnailImage& img) {
    if (img.w <= 0) return;
    auto* sp = new std::shared_ptr<ThumbnailImage>(std::make_shared<ThumbnailImage>(img));
    if (!PostMessageW(hw, msg, (WPARAM)ticket, (LPARAM)sp)) delete sp;
  });
}

void WheelController::OnThumbnailReady(uint64_t ticket, std::shared_ptr<ThumbnailImage> img) {
  if (ticket != ticket_ || !img || !window_.Shown()) return;
  window_.SetThumbnail(img->w, img->h, img->bgraPremul.data(), (size_t)img->w * 4);
}

void WheelController::Open(WheelKind kind, POINT cursor, const std::vector<fs::path>& files, bool keyboardMode) {
  kind_ = kind; page_ = 0; keyboardMode_ = keyboardMode; chosen_ = false; dataPaths_.clear(); lastHover_ = -1;
  SetSelection(files);
  Rebuild(false);
  RequestThumbnail();
  window_.SetActivatable(keyboardMode);
  window_.ShowAt(cursor, true);
  if (keyboardMode) {
    window_.FocusForKeyboard();
    window_.MoveSelection(+1);     // the first enabled item is pre-selected so Enter works immediately
  }
}

void WheelController::Switch(WheelKind kind) {
  if (kind == kind_) return;
  kind_ = kind; page_ = 0; lastHover_ = -1;
  Rebuild(true);
}

void WheelController::Close(bool animated) {
  ThumbnailService::Get().CancelOwner(1);
  window_.Dismiss(animated);
}

int WheelController::HoveredEnabledIndex(POINTL pt) const {
  wheel::Hit h = window_.HitTestScreen({pt.x, pt.y});
  if (h.zone != wheel::Zone::Sector) return -1;
  const auto& items = window_.Content().items;
  if (h.sector < 0 || h.sector >= (int)items.size() || !items[h.sector].enabled) return -1;
  return h.sector;
}

static DWORD EffectFor(bool valid, DWORD offered) {
  if (!valid) return DROPEFFECT_NONE;
  // Never MOVE: the source must not delete or relocate the original. COPY (or LINK as a last resort) only.
  if (offered & DROPEFFECT_COPY) return DROPEFFECT_COPY;
  if (offered & DROPEFFECT_LINK) return DROPEFFECT_LINK;
  return DROPEFFECT_NONE;
}

static std::set<std::wstring> NormSet(const std::vector<fs::path>& v) {
  std::set<std::wstring> s;
  for (auto& p : v) s.insert(ToLower(p.wstring()));
  return s;
}

DWORD WheelController::DropEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD offered) {
  std::vector<fs::path> paths = PathsFromDataObject(d);
  if (!paths.empty()) {
    std::vector<fs::path> cur;
    for (auto& i : selection_.items) cur.push_back(i.path);
    if (NormSet(paths) != NormSet(cur)) {
      FILO_LOG(Info, "wheel", "drag data differs from the prefetched selection (%zu files); rebuilding", paths.size());
      SetSelection(paths); Rebuild(true); RequestThumbnail();
    }
    dataPaths_ = std::move(paths);
  }
  return DropOver(keys, pt, offered);
}

DWORD WheelController::DropOver(DWORD, POINTL pt, DWORD offered) {
  int idx = HoveredEnabledIndex(pt);
  window_.SetHoverIndex(idx);
  const double now = NowSeconds();
  if (idx != lastHover_) { lastHover_ = idx; hoverSince_ = now; }
  bool valid = idx >= 0;
  if (valid && window_.Content().items[idx].id == L"page:next") {
    valid = false;
    if (now - hoverSince_ > 0.45) { ++page_; Rebuild(true); hoverSince_ = now; lastHover_ = -1; }
  }
  return EffectFor(valid, offered);
}

void WheelController::DropLeave() {
  window_.SetHoverIndex(-1);
  lastHover_ = -1;
}

DWORD WheelController::DropDrop(IDataObject* d, DWORD, POINTL pt, DWORD offered) {
  int idx = HoveredEnabledIndex(pt);
  std::wstring id = idx >= 0 ? window_.Content().items[idx].id : L"";
  std::vector<fs::path> paths = PathsFromDataObject(d);
  if (!paths.empty() && paths.size() != selection_.count()) SetSelection(paths);
  const bool valid = idx >= 0 && id != L"page:next";
  if (valid) {
    chosen_ = true;
    window_.ConfirmFlash(idx);
    Selection sel = selection_;
    POINT at{pt.x, pt.y};
    if (onChoose) onChoose(id, sel, at);
  }
  window_.Dismiss(true);
  if (onDropHandled) onDropHandled();
  return EffectFor(valid, offered);
}

void WheelController::Choose(int idx, POINT at) {
  const auto& items = window_.Content().items;
  if (idx < 0 || idx >= (int)items.size() || !items[idx].enabled) return;
  if (items[idx].id == L"page:next") { ++page_; Rebuild(true); return; }
  std::wstring id = items[idx].id;
  window_.ConfirmFlash(idx);
  Selection sel = selection_;
  window_.Dismiss(true);
  if (onChoose) onChoose(id, sel, at);
}

} // namespace filo::shell
