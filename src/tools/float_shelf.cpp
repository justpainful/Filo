// Float - keeps files within reach on the desktop. Collapsed it is a small fanned stack of the files' thumbnails with a paper clip;
// click it to open a tall glass list from which any file can be dragged into another window; click the "N items" header to
// collapse it again. Files are only referenced (never moved or copied here): dragging a row out starts a normal shell drag and the
// drop target decides copy / move / link. Files dropped onto Float are added; entries disappear when the file no longer exists.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/com.h"
#include "core/log.h"
#include "core/settings.h"
#include <shlobj.h>
#include <algorithm>
#include <memory>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_SHELF_DRAG = WM_APP + 0x72;
constexpr UINT WM_SHELF_ACTION = WM_APP + 0x73;
enum ShelfAction { Expand=1, Collapse, Remove, Clear, Close, Fit };
constexpr float kStackBody = 128.f;      // collapsed window body (square)
constexpr float kPanelW = 280.f;         // expanded panel width
constexpr float kHeader = 118.f;         // expanded panel: close button + stack + "N items"
constexpr float kRowH = 62.f;

class FloatShelf;
FloatShelf* g_shelf = nullptr;

class FloatShelf : public ToolDialog {
 public:
  void Add(const std::vector<fs::path>& files) {
    for (auto& f : files) {
      if (std::find_if(items_.begin(), items_.end(), [&](const Item& i) { return i.path == f; }) != items_.end()) continue;
      items_.push_back({f, false, nextItemId_++});
    }
    if (expanded_ && Valid()) PostMessageW(Hwnd(), WM_SHELF_ACTION, Fit, 0);
    Invalidate();
  }
  void SetExpanded(bool on) {
    expanded_ = on;
    if (on) { SetGlassBody(true); SetCornerRadius(32.f); FitPanel(); RecaptureGlass(); }
    else { SetGlassBody(false); SetBodySize(kStackBody, kStackBody); }
    Invalidate();
  }
  bool Expanded() const { return expanded_; }

 protected:
  struct Item { fs::path path; bool selected = false; uint64_t id=0; };

  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Float"; o.widthDip = kStackBody; o.heightDip = kStackBody; o.resizable = false; o.showTitleBar = false;
    o.topmost = true; o.activate = false; o.glassBody = false; o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "float-shelf"; }
  bool AcceptsFileDrop() const override { return true; }
  void OnFilesDropped(const std::vector<fs::path>& files) override { Add(files); }

  bool CanClose() const override { return !dragging_; }
  void OnCloseRequested() override {
    if(dragging_) { closeAfterDrag_=true;return; }
    CloseWindow();
  }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if(msg==WM_SHELF_DRAG) {StartDrag();SetHandled(true);return 0;}
    if(msg==WM_SHELF_ACTION) {
      switch(wp) {
        case Expand:SetExpanded(true);break;
        case Collapse:SetExpanded(false);break;
        case Remove:
          items_.erase(std::remove_if(items_.begin(),items_.end(),[&](const Item& i){return i.id==(uint64_t)lp;}),items_.end());
          if(items_.empty())OnCloseRequested();else {FitPanel();Invalidate();}break;
        case Clear:items_.clear();OnCloseRequested();break;
        case Close:OnCloseRequested();break;
        case Fit:if(expanded_)FitPanel();break;
      }
      SetHandled(true);return 0;
    }
    if(msg==WM_KEYDOWN && wp==VK_DELETE) {
      items_.erase(std::remove_if(items_.begin(),items_.end(),[](const Item& i){return i.selected;}),items_.end());
      if(items_.empty())OnCloseRequested();else {if(expanded_)FitPanel();Invalidate();}
      SetHandled(true);return 0;
    }
    if (msg == WM_TIMER && wp == 0x5F1) {
      const size_t n=items_.size();
      items_.erase(std::remove_if(items_.begin(),items_.end(),[](const Item& i){std::error_code ec;return !fs::exists(i.path,ec);}),items_.end());
      if(items_.size()!=n) {if(items_.empty())OnCloseRequested();else {if(expanded_)FitPanel();Invalidate();}}
      SetHandled(true);return 0;
    }
    return ToolDialog::OnMessage(msg,wp,lp);
  }
  void OnOpened() override { ToolDialog::OnOpened(); SetTimer(Hwnd(), 0x5F1, 1500, nullptr); }

  void Draw(Ui& ui, const Rect&, const Rect& body) override {
    if (expanded_) DrawPanel(ui, body); else DrawStack(ui, body);
  }

 private:
  // ---- the fanned stack of thumbnails with a paper clip (used collapsed and as the panel header)
  void DrawCards(Ui& ui, float cx, float cy, float size) {
    Painter& p = ui.P();
    const int n = (int)items_.size();
    const int shown = std::min(n, 3);
    static const float rot[3] = {-9.f, 8.f, 0.f};            // back, middle, front
    static const float dx[3] = {-6.f, 7.f, 0.f};
    for (int k = 0; k < shown; ++k) {
      const int idx = n - shown + k;                          // newest on top
      const int slot = 3 - shown + k;
      const float r = rot[slot];
      const Rect card{cx - size / 2 + dx[slot] * size / 84.f, cy - size / 2, size, size};
      p.PushRotate(cx, cy, r);
      p.Shadow(card, 14 * size / 84.f, 10, Color(0, 0, 0, 0.28f), 3);
      p.FillRoundRect(card, 14 * size / 84.f, Color(1, 1, 1, 0.97f));
      const Rect img = card.Inset(5 * size / 84.f);
      if (ID2D1Bitmap* bmp = Thumb(items_[idx].path, 160)) {
        auto sz = bmp->GetSize(); const float s = std::min(img.w / sz.width, img.h / sz.height);
        p.Bitmap(bmp, {img.cx() - sz.width * s / 2, img.cy() - sz.height * s / 2, sz.width * s, sz.height * s}, 1.f, 9.f * size / 84.f);
      } else p.Icon("document", img.cx(), img.cy(), img.w * 0.4f, Color(0, 0, 0, 0.35f), 1.5f);
      p.PopTransform();
    }
    p.Icon("paperclip", cx - size * 0.30f, cy - size * 0.42f, size * 0.52f, Color(0.62f, 0.64f, 0.68f, 1.f), 1.9f);
  }

  void BeginMove(const Interaction& it, Ui& ui) {
    if (it.pressed) { GetCursorPos(&curStart_); RECT r; GetWindowRect(Hwnd(), &r); winStart_ = {r.left, r.top}; moved_ = false; }
    if (it.active && it.dragging) {
      POINT c; GetCursorPos(&c);
      const int dx = c.x - curStart_.x, dy = c.y - curStart_.y;
      if (!moved_ && (std::abs(dx) > 4 || std::abs(dy) > 4)) moved_ = true;
      if (moved_) { SetWindowPos(Hwnd(), nullptr, winStart_.x + dx, winStart_.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); ui.RequestFrame(); }
    }
  }
  bool EndMove(const Interaction& it) {                    // true when the press was a click (no movement)
    if (!it.released) return false;
    if (moved_) { moved_ = false; if (expanded_) RecaptureGlass(); return false; }
    return it.clicked;
  }

  void Request(ShelfAction action,LPARAM index=0) {PostMessageW(Hwnd(),WM_SHELF_ACTION,action,index);}
  void DrawStack(Ui& ui,const Rect& body) {
    ui.PushId(0x57AC);
    DrawCards(ui,body.cx(),body.cy()+4,84.f);
    // Always visible and handled before the stack's larger drag target.
    if(ui.CloseButton({body.x+4,body.y+4,28,28},true,CanClose())) {Request(Close);ui.PopId();return;}
    ui.Tooltip(L"Close Float — your files stay where they are");
    bool overClose=Rect{body.x+4,body.y+4,28,28}.Contains(ui.In().mx,ui.In().my);
    auto it=ui.Behavior(body.Inset(14),ui.Id("stack"),!overClose,true);
    BeginMove(it,ui);
    if(it.hot) {ui.SetCursor((int)(intptr_t)IDC_HAND);ui.Tooltip(L"Click to expand. Drag to move Float.");}
    if(items_.size()>1) {
      Rect badge{body.cx()+18,body.cy()+38,34,22};ui.P().FillRoundRect(badge,11,accent::base);
      TextStyle label;label.size=12.5f;label.weight=700;label.align=Align::Center;
      ui.P().Text(badge,std::to_wstring(items_.size()),Color(1,1,1),label);
    }
    if(EndMove(it)||(it.clicked&&!it.released))Request(Expand);
    ui.PopId();
  }
  void DrawPanel(Ui& ui,const Rect& body) {
    const auto& pal=ui.Pal();auto& p=ui.P();const auto& in=ui.In();
    ui.PushId(0x9A4E);
    DrawCards(ui,body.cx(),body.y+50,52.f);
    if(ui.CloseButton({body.x+12,body.y+12,28,28},true,CanClose())) {Request(Close);ui.PopId();return;}
    ui.Tooltip(L"Close Float — your files stay where they are");
    if(ui.IconButton({body.right()-40,body.y+12,28,28},"chevronup",true,L"Collapse Float"))Request(Collapse);
    auto head=ui.Behavior({body.x+50,body.y+8,body.w-100,70},ui.Id("head"),true,false);
    BeginMove(head,ui);EndMove(head);
    if(head.hot)ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
    Rect toggle{body.x+20,body.y+kHeader-36,body.w-40,26};
    if(ui.Button(toggle,std::to_wstring(items_.size())+(items_.size()==1?L" file":L" files"),ButtonStyle::Quiet,true,"chevronup"))Request(Collapse);
    // The footer owns its hit area before partially clipped rows are considered.
    if(ui.Button({body.x+20,body.bottom()-35,body.w-40,27},L"Clear shelf",ButtonStyle::Quiet,!dragging_)) {Request(Clear);ui.PopId();return;}
    ui.Tooltip(L"Remove all references from Float. Original files are kept.");
    Rect vp{body.x+8,body.y+kHeader,body.w-16,body.h-kHeader-44};
    float totalH=(float)items_.size()*kRowH;
    float off=ui.BeginScroll(vp,totalH,ui.Id("rows"));
    for(int i=0;i<(int)items_.size();++i) {
      auto& item=items_[i];
      Rect row{vp.x,vp.y+i*kRowH-off,vp.w-(totalH>vp.h?8.f:0.f),kRowH-4};
      if(row.bottom()<vp.y||row.y>vp.bottom())continue;
      ui.PushId(ui.Id(item.path.wstring()));
      Rect remove{row.right()-34,row.cy()-14,28,28};
      if(ui.IconButton(remove,"remove",!dragging_,L"Remove from Float (keeps the original file)"))Request(Remove,(LPARAM)item.id);
      auto it=ui.Behavior({row.x,row.y,row.w-36,row.h},ui.Id("row"),true,true);
      float hover=ui.Anim(ui.Id("rh"),it.hot||item.selected?1.f:0.f);
      // Paint only behind the row's content; the independent remove target remains unobscured.
      Rect main{row.x,row.y,row.w-36,row.h};
      p.FillRoundRect(main,12,pal.text.WithAlpha(.035f+.06f*hover));
      if(item.selected)p.StrokeRoundRect(main,12,accent::base,1.5f);
      Rect thumb{row.x+8,row.y+7,44,row.h-14};
      if(auto* bmp=Thumb(item.path,160)) {auto sz=bmp->GetSize();float sc=std::min(thumb.w/sz.width,thumb.h/sz.height);p.Bitmap(bmp,{thumb.cx()-sz.width*sc/2,thumb.cy()-sz.height*sc/2,sz.width*sc,sz.height*sc},1,6);}
      else p.Icon("document",thumb.cx(),thumb.cy(),22,pal.textSecondary);
      TextStyle name;name.size=12;name.weight=500;
      p.Text({thumb.right()+8,row.y+8,main.right()-thumb.right()-16,20},item.path.filename().wstring(),pal.text,name);
      TextStyle detail;detail.size=11;std::error_code ec;auto bytes=fs::file_size(item.path,ec);
      p.Text({thumb.right()+8,row.y+28,main.right()-thumb.right()-16,18},ec?L"":FormatBytes(bytes),pal.textSecondary,detail);
      if(it.clicked) {if(!in.ctrl)for(auto& other:items_)other.selected=false;item.selected=in.ctrl?!item.selected:true;}
      if(it.active&&it.dragging&&!dragPosted_&&!dragging_) {
        if(!item.selected) {for(auto& other:items_)other.selected=false;item.selected=true;}
        dragPosted_=true;PostMessageW(Hwnd(),WM_SHELF_DRAG,0,0);
      }
      ui.PopId();
    }
    ui.EndScroll();
    ui.PopId();
  }

  void FitPanel() {
    const float h = std::clamp(kHeader + items_.size() * kRowH + 44, 230.f, 560.f);
    SetBodySize(kPanelW, h);
    // keep the panel on its monitor
    RECT r; GetWindowRect(Hwnd(), &r);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromWindow(Hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
    const int m = (int)std::lround(DialogWindow::kMargin * (Dpi() / 96.f));
    int x = r.left, y = r.top;
    if (r.bottom - m > mi.rcWork.bottom) y -= (r.bottom - m) - mi.rcWork.bottom;
    if (r.right - m > mi.rcWork.right) x -= (r.right - m) - mi.rcWork.right;
    if (y + m < mi.rcWork.top) y = mi.rcWork.top - m;
    if (x + m < mi.rcWork.left) x = mi.rcWork.left - m;
    if (x != r.left || y != r.top) SetWindowPos(Hwnd(), nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void StartDrag() {
    dragPosted_ = false;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& i : items_) if (i.selected) { PIDLIST_ABSOLUTE pid = nullptr; if (SUCCEEDED(SHParseDisplayName(i.path.c_str(), nullptr, &pid, 0, nullptr)) && pid) pidls.push_back(pid); }
    if (pidls.empty()) return;
    ComPtr<IDataObject> data;
    HRESULT hr = SHCreateDataObject(nullptr, (UINT)pidls.size(), (PCUITEMID_CHILD_ARRAY)pidls.data(), nullptr, IID_IDataObject, data.AsVoid());
    for (auto p : pidls) CoTaskMemFree(p);
    if (FAILED(hr) || !data) { FILO_LOG(Warn, "float", "could not build the drag data object (0x%08X)", (unsigned)hr); return; }
    DWORD eff = 0;
    dragging_=true;CancelPointerInput();
    SHDoDragDrop(Hwnd(), data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    dragging_=false;CancelPointerInput();
    if(closeAfterDrag_){OnCloseRequested();return;}
    Invalidate();
  }

  uint64_t nextItemId_=1;
  std::vector<Item> items_;
  bool dragging_=false,closeAfterDrag_=false;
  bool expanded_ = false, dragPosted_ = false, moved_ = false;
  POINT curStart_{}, winStart_{};
};

}  // namespace

ui::DialogWindow* OpenFloatShelf(const std::vector<fs::path>& files,bool expanded,bool show) {
  auto* d=new FloatShelf();d->Add(files);POINT p{};GetCursorPos(&p);
  if(!ToolDialog::OpenDialog(d,show,&p))return nullptr;
  if(expanded)d->SetExpanded(true);
  return d;
}
void FloatFiles(const Selection& sel) {
  std::vector<fs::path> files;for(const auto& i:sel.items)files.push_back(i.path);
  if(g_shelf&&IsWindow(g_shelf->Hwnd())) {g_shelf->Add(files);ShowWindow(g_shelf->Hwnd(),SW_SHOWNOACTIVATE);return;}
  auto* d=static_cast<FloatShelf*>(OpenFloatShelf(files,false,true));if(!d)return;
  d->onClosed=[d]{if(g_shelf==d)g_shelf=nullptr;delete d;};g_shelf=d;
}
bool FloatShelfShot(const std::vector<fs::path>& files,const fs::path& png,bool expanded) {
  auto* d=OpenFloatShelf(files,expanded,false);if(!d)return false;
  bool ok=d->RenderToPng(png);d->CloseWindow();return ok;
}
} // namespace filo::tools
