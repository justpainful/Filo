// Download from a link: the window that opens when a link is dropped on the link zone (drag a link from the browser, press Right Ctrl).
// Reads the link with yt-dlp, shows what it holds (videos, every picture of a slideshow, sound) and downloads the chosen items in the
// best quality the site serves into the Downloads folder under a unique Filo_<Platform>_<Author>_<Id> name.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/settings.h"
#include "engines/image/bitmap.h"
#include "engines/web/web.h"
#include "shell/actions.h"
#include "ui/link_zone.h"
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_DL_PROBED = WM_APP + 0x78;
constexpr UINT WM_DL_THUMB = WM_APP + 0x79;

struct ProbeShared {
  std::atomic<bool> cancel{false};
  std::mutex m;
  web::LinkInfo info;
};

struct ThumbShared {
  std::atomic<bool> alive{true};
  std::mutex m;
  std::vector<img::Bitmap> bmp;
  std::vector<char> ready;
};

bool PickFolderDialog(HWND owner, std::wstring& out) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return false;
  DWORD opts = 0; dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Choose where downloads are saved");
  if (FAILED(dlg->Show(owner))) return false;
  ComPtr<IShellItem> it;
  if (FAILED(dlg->GetResult(&it))) return false;
  PWSTR p = nullptr;
  if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
  out = p; CoTaskMemFree(p);
  return true;
}

std::wstring ClipboardText(HWND owner) {
  std::wstring s;
  if (!OpenClipboard(owner)) return s;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) { s = w; GlobalUnlock(h); } }
  CloseClipboard();
  return s;
}

class DownloadDialog : public ToolDialog {
 public:
  DownloadDialog(const std::wstring& url, const web::LinkInfo* fixture) : urlText_(url) {
    Settings st = SettingsStore::Get().Snapshot();
    quality_ = st.downloadQuality == 1 ? web::Quality::Compatible : web::Quality::Best;
    threshold_ = st.downloadFolderThreshold;
    dest_ = st.downloadFolder.empty() ? web::DefaultDownloadsDir() : fs::path(st.downloadFolder);
    if (fixture) { fixture_ = *fixture; useFixture_ = true; }
  }
  ~DownloadDialog() override {
    if (probe_) probe_->cancel = true;
    if (thumbs_) thumbs_->alive = false;
  }

 protected:
  enum class Stage { Input, Probing, Ready, Failed };

  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Download from Link"; o.widthDip = 600; o.heightDip = 700; o.resizable = true; o.minWidthDip = 500; o.minHeightDip = 520;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "download-link"; }

  void OnOpened() override {
    ToolDialog::OnOpened();
    if (useFixture_) { Adopt(fixture_); return; }
    if (!web::ExtractUrl(urlText_).empty()) StartProbe();
  }

  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_DL_PROBED) {
      if (probe_ && (uint64_t)wp == epoch_) {
        web::LinkInfo li;
        { std::lock_guard lk(probe_->m); li = probe_->info; }
        Adopt(li);
      }
      SetHandled(true); return 0;
    }
    if (msg == WM_DL_THUMB) { Invalidate(); SetHandled(true); return 0; }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void OnDeviceReset(bool rebuilt) override {
    ToolDialog::OnDeviceReset(rebuilt);
    if (!rebuilt) for (auto& b : bmps_) b.Reset();
  }

  // ------------------------------------------------------------------------------------------------ draw
  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    if (DrawJobPanel(ui, content)) return;
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const bool rtl = ui.Rtl();
    float y = body.y + 10;

    // ---- link row
    {
      Rect row{body.x + pad, y, body.w - 2 * pad, 30};
      auto c = ui.Cols(row, {Flex(1), Fixed(70), Fixed(78)}, 8);
      ui.PushId(0xD11);
      ui.TextInput(c[0], urlText_, L"Paste a link from TikTok, YouTube, X, Instagram...", false, stage_ != Stage::Probing);
      if (ui.Button(c[1], L"Paste", ButtonStyle::Secondary, stage_ != Stage::Probing)) {
        const std::wstring u = web::ExtractUrl(ClipboardText(Hwnd()));
        if (!u.empty()) { urlText_ = u; StartProbe(); }
      }
      if (ui.Button(c[2], stage_ == Stage::Ready ? L"Reload" : L"Read", ButtonStyle::Primary, stage_ != Stage::Probing && !web::ExtractUrl(urlText_).empty())) StartProbe();
      ui.PopId();
      y += 42;
    }

    if (stage_ != Stage::Ready) { DrawStatusBody(ui, {body.x + pad, y, body.w - 2 * pad, body.bottom() - y - 8}); DrawFooterBar(ui, content); return; }

    // ---- header: thumbnail + title + summary
    {
      Rect th{body.x + pad, y, 84, 84};
      if (rtl) th.x = body.right() - pad - 84;
      p.FillRoundRect(th, 14, pal.darkCanvas);
      DrawThumb(p, headerThumbItem_, th, 14);
      const float tx = rtl ? body.x + pad : th.right() + 14, tw = body.w - 2 * pad - 84 - 14;
      TextStyle t1; t1.size = 16; t1.weight = 700; t1.wrap = true; t1.vcenter = false; t1.align = rtl ? Align::End : Align::Start;
      TextStyle t2; t2.size = 13; t2.align = rtl ? Align::End : Align::Start;
      std::wstring title = info_.title.empty() ? L"Untitled" : info_.title;
      p.Text({tx, y, tw, 42}, title, pal.text, t1);
      std::wstring who = info_.platform.name + (info_.uploader.empty() ? L"" : L"  \x00B7  @" + info_.uploader);
      p.Text({tx, y + 46, tw, 18}, who, pal.textSecondary, t2);
      p.Text({tx, y + 64, tw, 18}, Summary(), pal.textTertiary, t2);
      y += 96;
    }

    // ---- options block (bottom) and the scrolling list between
    const float optH = OptionsHeight();
    Rect vp{body.x + pad, y, body.w - 2 * pad, std::max(60.f, body.bottom() - optH - y - 6)};
    DrawChoices(ui, vp);
    DrawOptions(ui, {body.x + pad, body.bottom() - optH, body.w - 2 * pad, optH});
    DrawFooterBar(ui, content);
  }

 private:
  // ------------------------------------------------------------------------------------------------ pieces
  void DrawStatusBody(Ui& ui, const Rect& r) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    TextStyle ts; ts.size = 15; ts.align = Align::Center; ts.wrap = true; ts.vcenter = false;
    if (stage_ == Stage::Probing) {
      p.Text({r.x, r.y + 70, r.w, 26}, L"Reading the link...", pal.text, ts);
      ui.ProgressBar({r.cx() - 120, r.y + 108, 240, 6}, 0, true);
      TextStyle sm; sm.size = 12.5f; sm.align = Align::Center; sm.wrap = true; sm.vcenter = false;
      p.Text({r.x + 30, r.y + 130, r.w - 60, 40}, L"Filo asks the site what this post contains. Nothing is saved yet.", pal.textTertiary, sm);
    } else if (stage_ == Stage::Failed) {
      p.FillCircle(r.cx(), r.y + 86, 26, Color::Hex(0xE5484D, 0.16f));
      p.Icon("warning", r.cx(), r.y + 86, 28, Color::Hex(0xE5484D), 2.f);
      TextStyle t; t.size = 16; t.weight = 600; t.align = Align::Center;
      p.Text({r.x, r.y + 126, r.w, 26}, L"This link could not be read", pal.text, t);
      p.Text({r.x + 24, r.y + 158, r.w - 48, 70}, error_.empty() ? L"Something went wrong." : error_, pal.textSecondary, ts);
    } else {
      p.FillCircle(r.cx(), r.y + 86, 30, accent::base.WithAlpha(0.16f));
      DrawDownloadGlyph(p, r.cx(), r.y + 86, 30, accent::base, 2.6f);
      TextStyle t; t.size = 16; t.weight = 600; t.align = Align::Center;
      p.Text({r.x, r.y + 130, r.w, 26}, L"Paste or drop a link", pal.text, t);
      p.Text({r.x + 30, r.y + 160, r.w - 60, 60}, L"Or drag a link out of your browser and press Right Ctrl while you hold it.", pal.textSecondary, ts);
    }
  }

  std::wstring Summary() const {
    std::wstring s;
    auto add = [&](int n, const wchar_t* one, const wchar_t* many) { if (!n) return; if (!s.empty()) s += L"  \x00B7  "; s += std::to_wstring(n) + L" " + (n == 1 ? one : many); };
    add(info_.CountOf(web::ItemKind::Video), L"video", L"videos");
    add(info_.CountOf(web::ItemKind::Image), L"picture", L"pictures");
    add(info_.CountOf(web::ItemKind::Audio), L"sound", L"sounds");
    if (info_.isPlaylist && info_.items.size() >= 80) s += L"  \x00B7  First 80 items";
    return s;
  }

  float OptionsHeight() const { return 36.f * (2 + (info_.CountOf(web::ItemKind::Video) ? 2 : 0) + (info_.CountOf(web::ItemKind::Video) || info_.CountOf(web::ItemKind::Audio) ? 1 : 0)) + 18.f; }

  int SelectedFiles() const {
    int n = 0, videos = 0;
    for (size_t i = 0; i < picked_.size(); ++i) if (picked_[i]) { ++n; if (info_.items[i].kind == web::ItemKind::Video) ++videos; }
    if (wantAudio_) n += videos > 0 ? videos : info_.CountOf(web::ItemKind::Video);
    return n;
  }

  int64_t SelectedBytes() const {
    int64_t b = 0;
    for (size_t i = 0; i < picked_.size(); ++i) if (picked_[i]) b += info_.items[i].size;
    return b;
  }

  void DrawFooterBar(Ui& ui, const Rect& content) {
    FooterInfo fi;
    fi.action = L"Download";
    const int n = SelectedFiles();
    fi.actionEnabled = stage_ == Stage::Ready && n > 0;
    if (stage_ == Stage::Ready) {
      if (n == 0) fi.status = L"Choose at least one item.";
      else {
        fi.status = std::to_wstring(n) + (n == 1 ? L" file" : L" files");
        if (const int64_t b = SelectedBytes()) fi.status += L"  \x00B7  about " + FormatBytes((uint64_t)b);
        if (threshold_ > 0 && n >= threshold_) fi.status += L"  \x00B7  saved in a folder";
      }
    } else fi.status = stage_ == Stage::Probing ? L"Reading..." : L"Downloads go to " + dest_.wstring();
    if (DrawFooter(ui, content, fi).action) StartDownload();
  }

  void DrawOptions(Ui& ui, const Rect& r) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    p.FillRect({r.x, r.y, r.w, 1}, pal.divider);
    float y = r.y + 10;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    auto label = [&](float yy, const wchar_t* t) { p.Text({r.x, yy, r.w * 0.4f, 30}, t, pal.text, ls); };
    ui.PushId(0xD12);
    if (info_.CountOf(web::ItemKind::Video)) {
      label(y, L"Video quality");
      int n = ui.Segmented(ui.Mirror({r.right() - 290, y + 2, 290, 26}, r), {L"Best available", L"Most compatible"}, quality_ == web::Quality::Best ? 0 : 1);
      quality_ = n == 1 ? web::Quality::Compatible : web::Quality::Best;
      y += 36;
      label(y, L"Video format");
      int format = ui.Segmented(ui.Mirror({r.right() - 290, y + 2, 290, 26}, r), {L"Auto", L"MP4", L"MKV", L"WebM"}, (int)videoContainer_);
      videoContainer_ = (web::VideoContainer)std::clamp(format, 0, 3);
      y += 36;
    }
    if (info_.CountOf(web::ItemKind::Video) || info_.CountOf(web::ItemKind::Audio)) {
      label(y, L"Audio format");
      int n = ui.Segmented(ui.Mirror({r.right() - 290, y + 2, 290, 26}, r), {L"Original", L"MP3", L"M4A", L"WAV"}, (int)audioFormat_);
      audioFormat_ = (web::AudioFormat)std::clamp(n, 0, 3);
      y += 36;
    }
    label(y, L"Save to");
    {
      const float bw = 84;
      TextStyle ps; ps.size = 13; ps.align = Align::End; ps.ellipsis = true;
      Rect pathR = ui.Mirror({r.x + r.w * 0.22f, y, r.w * 0.78f - bw - 10, 30}, r);
      p.Text(pathR, dest_.wstring(), pal.textSecondary, ps);
      if (ui.Button(ui.Mirror({r.right() - bw, y + 2, bw, 26}, r), L"Change...", ButtonStyle::Secondary)) {
        std::wstring f;
        if (PickFolderDialog(Hwnd(), f)) dest_ = f;
      }
      y += 36;
    }
    label(y, L"Use a folder from");
    {
      double v = threshold_;
      if (ui.Stepper(ui.Mirror({r.right() - 150, y + 2, 150, 26}, r), v, 0, 100, 1, 0, L" files")) threshold_ = (int)v;
    }
    ui.PopId();
  }

  // pick marks: filled accent disc with a check, or an empty ring
  void DrawMark(Painter& p, float cx, float cy, bool on, float r = 11) {
    if (on) { p.FillCircle(cx, cy, r, accent::base); p.Icon("check", cx, cy, r * 1.25f, Color(1, 1, 1, 1), 2.4f); }
    else { p.FillCircle(cx, cy, r, Color(0, 0, 0, 0.30f)); p.StrokeCircle(cx, cy, r - 0.5f, Color(1, 1, 1, 0.9f), 1.8f); }
  }

  void DrawChoices(Ui& ui, const Rect& vp) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    std::vector<int> imgs, vids, auds;
    for (int i = 0; i < (int)info_.items.size(); ++i) {
      switch (info_.items[i].kind) { case web::ItemKind::Image: imgs.push_back(i); break; case web::ItemKind::Video: vids.push_back(i); break; default: auds.push_back(i); }
    }
    // layout pass (heights) so the scroll area knows its content
    const float gap = 8.f;
    const int cols = std::clamp((int)std::floor((vp.w - 12 + gap) / (104 + gap)), 2, 6);
    const float tileW = (vp.w - 12 - gap * (cols - 1)) / cols;
    bool portrait = true;
    if (!imgs.empty()) { const auto& it = info_.items[imgs[0]]; portrait = it.height >= it.width; }
    const float tileH = portrait ? tileW * 1.4f : tileW * 0.75f;
    const int rowsN = (int)((imgs.size() + cols - 1) / cols);
    const float rowH = 62.f;
    float total = 0;
    if (!imgs.empty()) total += 34 + rowsN * (tileH + gap);
    if (!vids.empty()) total += 34 + vids.size() * (rowH + 6);
    if (!auds.empty()) total += 34 + auds.size() * (rowH + 6);
    if (info_.HasAudioChoice()) total += 34 + rowH + 6;

    const float off = ui.BeginScroll(vp, total, ui.Id("dl-scroll"));
    float y = vp.y - off;
    const float x0 = vp.x, w = vp.w - (total > vp.h ? 12.f : 0.f);
    TextStyle hs; hs.size = 13; hs.weight = 700;
    auto heading = [&](const std::wstring& t, std::vector<int>* group) {
      p.Text({x0, y, w * 0.6f, 28}, t, pal.textSecondary, hs);
      if (group && group->size() > 1) {
        bool all = true; for (int i : *group) all = all && picked_[i];
        if (ui.Button(ui.Mirror({x0 + w - 190, y + 1, 90, 26}, {x0, y, w, 28}), L"All", ButtonStyle::Quiet, !all)) for (int i : *group) picked_[i] = true;
        if (ui.Button(ui.Mirror({x0 + w - 96, y + 1, 90, 26}, {x0, y, w, 28}), L"None", ButtonStyle::Quiet)) for (int i : *group) picked_[i] = false;
      }
      y += 34;
    };

    if (!imgs.empty()) {
      ui.PushId(0x1A);
      heading(L"Pictures (" + std::to_wstring(imgs.size()) + L")", &imgs);
      for (size_t k = 0; k < imgs.size(); ++k) {
        const int i = imgs[k];
        const int cx = (int)(k % cols), cy = (int)(k / cols);
        Rect t{x0 + cx * (tileW + gap), y + cy * (tileH + gap), tileW, tileH};
        if (ui.Rtl()) t.x = x0 + w - 12 - (cx + 1) * tileW - cx * gap + 12 - 12;
        if (t.bottom() < vp.y - 2 || t.y > vp.bottom() + 2) continue;
        ui.PushId(i);
        p.FillRoundRect(t, 12, pal.darkCanvas);
        DrawThumb(p, i, t, 12);
        if (!picked_[i]) p.FillRoundRect(t, 12, Color(0, 0, 0, 0.35f));
        if (picked_[i]) p.StrokeRoundRect(t, 12, accent::base, 2.5f);
        TextStyle ns; ns.size = 11.5f; ns.weight = 700; ns.align = Align::Center;
        Rect nb{t.x + 6, t.bottom() - 24, 26, 18};
        p.FillRoundRect(nb, 9, Color(0, 0, 0, 0.55f));
        p.Text(nb, std::to_wstring(k + 1), Color(1, 1, 1, 1), ns);
        DrawMark(p, t.right() - 17, t.y + 17, picked_[i]);
        Interaction it = ui.Behavior(t, ui.Id("tile"), true, false);
        if (it.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
        if (it.clicked) picked_[i] = !picked_[i];
        ui.PopId();
      }
      y += rowsN * (tileH + gap);
      ui.PopId();
    }

    auto itemRow = [&](int i, const wchar_t* iconName) {
      const web::Item& it = info_.items[i];
      Rect row{x0, y, w, rowH};
      ui.PushId(i);
      p.FillRoundRect(row, control::listRowRadius, pal.fieldFill);
      p.StrokeRoundRect(row, control::listRowRadius, picked_[i] ? accent::base : pal.divider, picked_[i] ? 2.f : 1.f);
      Rect th{row.x + 8, row.y + 7, 80, 48};
      p.FillRoundRect(th, 8, pal.darkCanvas);
      DrawThumb(p, i, th, 8);
      TextStyle t1; t1.size = 13.5f; t1.weight = 600;
      TextStyle t2; t2.size = 12.5f;
      const float tx = th.right() + 12, tw = row.right() - 46 - tx;
      std::wstring name = it.title.empty() ? (it.kind == web::ItemKind::Audio ? std::wstring(L"Sound") : L"Video") : it.title;
      if (it.index > 1 || (it.index == 1 && info_.CountOf(web::ItemKind::Video) > 1)) name = L"#" + std::to_wstring(it.index) + L"  " + name;
      p.Text({tx, row.y + 9, tw, 22}, name, pal.text, t1);
      p.Text({tx, row.y + 31, tw, 20}, it.detail, pal.textSecondary, t2);
      DrawMark(p, row.right() - 24, row.cy(), picked_[i], 11);
      (void)iconName;
      Interaction hit = ui.Behavior(row, ui.Id("row"), true, false);
      if (hit.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
      if (hit.clicked) picked_[i] = !picked_[i];
      ui.PopId();
      y += rowH + 6;
    };

    if (!vids.empty()) { ui.PushId(0x1B); heading(vids.size() > 1 ? L"Videos (" + std::to_wstring(vids.size()) + L")" : L"Video", &vids); for (int i : vids) itemRow(i, L"video"); ui.PopId(); }
    if (!auds.empty()) { ui.PushId(0x1C); heading(auds.size() > 1 ? L"Sounds (" + std::to_wstring(auds.size()) + L")" : L"Sound", &auds); for (int i : auds) itemRow(i, L"audio"); ui.PopId(); }
    if (info_.HasAudioChoice()) {
      ui.PushId(0x1D);
      heading(L"Audio", nullptr);
      Rect row{x0, y, w, rowH};
      p.FillRoundRect(row, control::listRowRadius, pal.fieldFill);
      p.StrokeRoundRect(row, control::listRowRadius, wantAudio_ ? accent::base : pal.divider, wantAudio_ ? 2.f : 1.f);
      p.FillCircle(row.x + 36, row.cy(), 20, accent::base.WithAlpha(0.16f));
      p.Icon("bleep", row.x + 36, row.cy(), 22, accent::base, 1.8f);
      TextStyle t1; t1.size = 13.5f; t1.weight = 600; TextStyle t2; t2.size = 12.5f;
      p.Text({row.x + 68, row.y + 9, row.w - 120, 22}, vids.size() > 1 ? L"Audio of the chosen videos" : L"Audio only", pal.text, t1);
      p.Text({row.x + 68, row.y + 31, row.w - 120, 20}, L"Tick it alone to skip the video. Choose an audio format below.", pal.textSecondary, t2);
      DrawMark(p, row.right() - 24, row.cy(), wantAudio_, 11);
      Interaction hit = ui.Behavior(row, ui.Id("audio"), true, false);
      if (hit.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
      if (hit.clicked) wantAudio_ = !wantAudio_;
      y += rowH + 6;
      ui.PopId();
    }
    ui.EndScroll();
  }

  // ------------------------------------------------------------------------------------------------ thumbnails
  void DrawThumb(Painter& p, int item, const Rect& box, float radius) {
    if (item < 0 || !thumbs_) return;
    ID2D1Bitmap* b = BitmapFor(item);
    if (!b) return;
    const D2D1_SIZE_F sz = b->GetSize();
    if (sz.width <= 0 || sz.height <= 0) return;
    // cover: crop through a clip so the picture fills the box without distortion
    const float s = std::max(box.w / sz.width, box.h / sz.height);
    const float dw = sz.width * s, dh = sz.height * s;
    Rect dst{box.cx() - dw / 2, box.cy() - dh / 2, dw, dh};
    p.PushClipRoundRect(box, radius);
    p.Bitmap(b, dst);
    p.PopClip();
  }

  ID2D1Bitmap* BitmapFor(int item) {
    if (item < 0 || item >= (int)bmps_.size()) return nullptr;
    if (bmps_[item]) return bmps_[item].Get();
    if (!dc_ || !thumbs_) return nullptr;
    std::lock_guard lk(thumbs_->m);
    if (item >= (int)thumbs_->ready.size() || !thumbs_->ready[item]) return nullptr;
    const img::Bitmap& src = thumbs_->bmp[item];
    if (src.Empty()) return nullptr;
    std::vector<uint8_t> t = src.px;
    for (size_t i = 0; i < (size_t)src.width * src.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(src.width, src.height), t.data(), src.width * 4, &bp, &b))) bmps_[item] = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
    return bmps_[item].Get();
  }

  void StartThumbs() {
    thumbs_ = std::make_shared<ThumbShared>();
    const size_t n = info_.items.size();
    thumbs_->bmp.resize(n); thumbs_->ready.assign(n, 0);
    bmps_.assign(n, ComPtr<ID2D1Bitmap>());
    headerThumbItem_ = n ? 0 : -1;
    for (size_t i = 0; i < n; ++i) if (info_.items[i].kind == web::ItemKind::Image || info_.items[i].kind == web::ItemKind::Video) { headerThumbItem_ = (int)i; break; }
    auto sh = thumbs_; HWND hw = Hwnd();
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> jobs;
    for (const auto& it : info_.items) jobs.push_back({it.thumbUrl.empty() ? it.url : it.thumbUrl, it.headers});
    // a few workers so a 30-picture slideshow fills in quickly
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto jobsP = std::make_shared<decltype(jobs)>(std::move(jobs));
    const int workers = (int)std::min<size_t>(4, std::max<size_t>(1, n));
    for (int w = 0; w < workers; ++w) {
      std::thread([sh, next, jobsP, hw]() {
        for (;;) {
          const size_t i = next->fetch_add(1);
          if (i >= jobsP->size() || !sh->alive) return;
          const auto& [url, headers] = (*jobsP)[i];
          if (url.empty()) continue;
          HttpOptions ho; ho.headers = headers; ho.timeoutMs = 20000; ho.maxBytes = 40ull << 20;
          std::string bytes;
          HttpResult hr = HttpGetMemory(Widen(url), bytes, ho);
          if (!hr.ok || !sh->alive) continue;
          img::Bytes data(bytes.begin(), bytes.end());
          img::Bitmap bm; img::LoadOptions lo; lo.maxSide = 260;
          std::wstring ext = L"";
          if (!img::LoadBitmapFromMemory(data, bm, lo, nullptr, ext) || bm.Empty()) continue;
          { std::lock_guard lk(sh->m); sh->bmp[i] = std::move(bm); sh->ready[i] = 1; }
          if (sh->alive) PostMessageW(hw, WM_DL_THUMB, (WPARAM)i, 0);
        }
      }).detach();
    }
  }

  // ------------------------------------------------------------------------------------------------ probe and download
  void StartProbe() {
    const std::wstring url = web::ExtractUrl(urlText_);
    if (url.empty()) { stage_ = Stage::Failed; error_ = L"That is not a web address."; Invalidate(); return; }
    if (probe_) probe_->cancel = true;
    if (thumbs_) thumbs_->alive = false;
    thumbs_.reset();
    probe_ = std::make_shared<ProbeShared>();
    stage_ = Stage::Probing; error_.clear();
    urlText_ = url;
    const uint64_t epoch = ++epoch_;
    auto sh = probe_; HWND hw = Hwnd();
    std::thread([sh, url, hw, epoch]() {
      web::LinkInfo li = web::Probe(url, &sh->cancel);
      if (sh->cancel) return;
      { std::lock_guard lk(sh->m); sh->info = std::move(li); }
      PostMessageW(hw, WM_DL_PROBED, (WPARAM)epoch, 0);
    }).detach();
    Invalidate();
  }

  void Adopt(const web::LinkInfo& li) {
    info_ = li;
    if (!li.ok) { stage_ = Stage::Failed; error_ = li.error; Invalidate(); return; }
    stage_ = Stage::Ready;
    picked_.assign(info_.items.size(), false);
    wantAudio_ = false;
    bool anyVisual = false;
    for (size_t i = 0; i < info_.items.size(); ++i) {
      const auto k = info_.items[i].kind;
      if (k != web::ItemKind::Audio) { picked_[i] = true; anyVisual = true; }
    }
    if (!anyVisual) for (size_t i = 0; i < picked_.size(); ++i) picked_[i] = true;      // sound-only post: take it
    StartThumbs();
    Invalidate();
  }

  void StartDownload() {
    if (stage_ != Stage::Ready || SelectedFiles() == 0) return;
    web::DownloadRequest req;
    req.info = info_;
    for (size_t i = 0; i < picked_.size(); ++i) if (picked_[i]) req.items.push_back((int)i);
    req.wantAudio = wantAudio_;
    req.quality = quality_;
    req.videoContainer = videoContainer_;
    req.audioFormat = audioFormat_;
    req.destRoot = dest_;
    req.folderThreshold = threshold_;
    SettingsStore::Get().Update([&](Settings& s) {
      s.downloadQuality = quality_ == web::Quality::Compatible ? 1 : 0;
      s.downloadFolderThreshold = threshold_;
      s.downloadFolder = dest_ == web::DefaultDownloadsDir() ? std::wstring() : dest_.wstring();
    });
    RunJob(L"Downloading from " + info_.platform.name, [req](JobContext& ctx) { return web::Download(req, ctx); });
  }

  std::wstring urlText_;
  Stage stage_ = Stage::Input;
  std::wstring error_;
  web::LinkInfo info_, fixture_;
  bool useFixture_ = false;
  std::vector<char> picked_;
  bool wantAudio_ = false;
  web::Quality quality_ = web::Quality::Best;
  web::VideoContainer videoContainer_ = web::VideoContainer::Auto;
  web::AudioFormat audioFormat_ = web::AudioFormat::Original;
  int threshold_ = 4;
  fs::path dest_;
  std::shared_ptr<ProbeShared> probe_;
  std::shared_ptr<ThumbShared> thumbs_;
  std::vector<ComPtr<ID2D1Bitmap>> bmps_;
  int headerThumbItem_ = -1;
  uint64_t epoch_ = 0;
};

}  // namespace

ui::DialogWindow* OpenDownloadDialog(const std::wstring& url, const web::LinkInfo* fixture, bool show) {
  auto* d = new DownloadDialog(url, fixture);
  return ToolDialog::OpenDialog(d, show) ? d : nullptr;
}

}  // namespace filo::tools
