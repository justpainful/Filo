// Read QR Codes - decodes every QR code in an image locally (ZXing-cpp), shows what each one contains and lets the user copy it.
// Links are only displayed: Filo never opens them on its own; the "Open Link" button asks Windows to open the address on request.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/settings.h"
#include "ui/canvas.h"
#include "engines/image/qr.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_QR_DONE = WM_APP + 0x76;

bool CopyText(HWND owner, const std::wstring& s) {
  if (!OpenClipboard(owner)) return false;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  if (h) { memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
  CloseClipboard();
  return h != nullptr;
}

std::wstring KindLabel(const std::wstring& k) {
  if (k == L"url") return L"Link";
  if (k == L"email") return L"Email";
  if (k == L"phone") return L"Phone number";
  if (k == L"sms") return L"Text message";
  if (k == L"geo") return L"Location";
  if (k == L"wifi") return L"Wi-Fi network";
  if (k == L"vcard") return L"Contact card";
  return L"Text";
}

class ReadQrDialog : public ToolDialog {
 public:
  explicit ReadQrDialog(const Selection& sel) {
    selection_ = sel;
    shared_ = std::make_shared<Shared>();
    if (sel.items.empty()) return;
    path_ = sel.items[0].path;
    img::LoadOptions lo; lo.maxSide = 1400;
    if (!img::LoadBitmap(path_, base_, lo, &err_)) { base_ = {}; if (err_.empty()) err_ = L"This image cannot be opened."; }
    scanning_ = !base_.Empty();
  }
  ~ReadQrDialog() override { if (shared_) shared_->alive = false; }

 protected:
  struct Shared { std::mutex m; std::atomic<bool> alive{true}; std::vector<img::QrResult> results; std::wstring err; };
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Read QR Code"; o.widthDip = 500; o.heightDip = 600; o.resizable = true; o.minWidthDip = 400; o.minHeightDip = 420;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "read-qr"; }
  void OnOpened() override { ToolDialog::OnOpened(); StartScan(); }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_QR_DONE) {
      std::lock_guard lk(shared_->m);
      results_ = shared_->results; scanErr_ = shared_->err; scanning_ = false;
      Invalidate(); SetHandled(true); return 0;
    }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float listH = std::clamp(results_.size() * 96.f + 8.f, 80.f, body.h * 0.5f);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, std::max(140.f, body.h - 15 - listH - 32)};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (!base_.Empty()) {
      CanvasMap m = MapCanvas(well, base_.width, base_.height, CanvasView{}, 22.f);
      EnsureBitmap();
      p.PushClipRoundRect(well, dialog::wellRadius);
      if (bmp_) p.Bitmap(bmp_.Get(), m.image);
      // outline every code found (coordinates are in pixels of the full-size image the engine analysed)
      const float sx = (float)base_.width / (float)std::max(1, base_.info.fullWidth), sy = (float)base_.height / (float)std::max(1, base_.info.fullHeight);
      for (size_t i = 0; i < results_.size(); ++i) {
        const auto& c = results_[i].corners;
        float px[4], py[4];
        for (int k = 0; k < 4; ++k) { px[k] = m.ToScreenX(c[k].x * sx); py[k] = m.ToScreenY(c[k].y * sy); }
        for (int k = 0; k < 4; ++k) p.Line(px[k], py[k], px[(k + 1) % 4], py[(k + 1) % 4], accent::base, 3.f, true);
        TextStyle ts; ts.size = 12; ts.weight = 700; ts.align = Align::Center;
        const Rect tag{px[0] - 2, py[0] - 22, 24, 20};
        p.FillRoundRect(tag, 10, accent::base); p.Text(tag, std::to_wstring(i + 1), Color(1, 1, 1, 1), ts);
      }
      p.PopClip();
      if (scanning_) { TextStyle ts; ts.size = 14; ts.align = Align::Center; p.Text({well.x, well.bottom() - 40, well.w, 28}, L"Looking for QR codes...", Color(1, 1, 1, 0.85f), ts); }
    } else { TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true; p.Text(well.Inset(30), err_, Color(1, 1, 1, 0.7f), es); }

    Rect vp{body.x + pad, well.bottom() + 10, body.w - 2 * pad, listH};
    if (!scanning_ && results_.empty() && !base_.Empty()) {
      TextStyle ts; ts.size = 14; ts.align = Align::Center; ts.wrap = true;
      p.Text(vp, scanErr_.empty() ? L"No QR code found in this image." : scanErr_, pal.textSecondary, ts);
    } else {
      const float rowH = 90, gap = 6, totalH = results_.size() * (rowH + gap);
      const float off = ui.BeginScroll(vp, totalH, ui.Id("qr"));
      for (int i = 0; i < (int)results_.size(); ++i) {
        const auto& r = results_[i];
        Rect row{vp.x, vp.y + i * (rowH + gap) - off, vp.w - (totalH > vp.h ? 10.f : 0.f), rowH};
        if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
        ui.PushId(i);
        p.FillRoundRect(row, control::listRowRadius, pal.fieldFill); p.StrokeRoundRect(row, control::listRowRadius, pal.divider, 1.f);
        TextStyle ks; ks.size = 12; ks.weight = 600;
        p.Text({row.x + 14, row.y + 8, row.w - 28, 18}, std::to_wstring(i + 1) + L"  \x00B7  " + KindLabel(r.kind), pal.textSecondary, ks);
        TextStyle ts; ts.size = 13.5f; ts.wrap = true; ts.ellipsis = true;
        p.Text({row.x + 14, row.y + 26, row.w - 28, 36}, r.text, pal.text, ts);
        float bx = row.right() - 14;
        if (ui.Button({bx - 76, row.bottom() - 32, 76, 24}, L"Copy", ButtonStyle::Secondary)) CopyText(Hwnd(), r.text);
        if (r.isUrl && ui.Button({bx - 76 - 8 - 100, row.bottom() - 32, 100, 24}, L"Open Link", ButtonStyle::Secondary)) ShellExecuteW(nullptr, L"open", r.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ui.PopId();
      }
      ui.EndScroll();
    }
    FooterInfo fi;
    fi.status = results_.empty() ? (scanning_ ? L"Reading..." : L"Nothing is sent anywhere; reading happens on this PC.") : L"Links are never opened automatically.";
    if (results_.size() > 1) { fi.action = L"Copy All"; fi.actionEnabled = true; }
    if (DrawFooter(ui, content, fi).action) {
      std::wstring all; for (auto& r : results_) { if (!all.empty()) all += L"\r\n"; all += r.text; }
      CopyText(Hwnd(), all);
    }
  }

 private:
  void EnsureBitmap() {
    if (bmp_ || !dc_ || base_.Empty()) return;
    std::vector<uint8_t> t = base_.px;
    for (size_t i = 0; i < (size_t)base_.width * base_.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(base_.width, base_.height), t.data(), base_.width * 4, &bp, &b))) bmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
  }
  void StartScan() {
    if (path_.empty() || base_.Empty()) return;
    auto sh = shared_; HWND hw = Hwnd(); fs::path in = path_;
    std::thread([sh, hw, in]() {
      std::wstring err;
      auto res = img::ReadQrCodes(in, &err);                // full-size read: small codes inside large photos still decode
      if (!sh->alive) return;
      { std::lock_guard lk(sh->m); sh->results = std::move(res); sh->err = err; }
      PostMessageW(hw, WM_QR_DONE, 0, 0);
    }).detach();
  }

  fs::path path_;
  img::Bitmap base_;
  std::wstring err_, scanErr_;
  std::vector<img::QrResult> results_;
  bool scanning_ = false;
  ComPtr<ID2D1Bitmap> bmp_;
  std::shared_ptr<Shared> shared_;
};

}  // namespace

ToolDialog* MakeReadQr(const Selection& s) { return new ReadQrDialog(s); }

}  // namespace filo::tools
