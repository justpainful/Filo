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
constexpr UINT WM_DL_LISTED = WM_APP + 0x7A;
constexpr UINT WM_DL_COUNT = WM_APP + 0x7B;

// An account being listed: the worker adds posts while the window shows the running count.
struct ListShared {
  std::atomic<bool> cancel{false};
  std::atomic<bool> keep{false};          // "Use what was found": stop listing but keep the posts
  std::atomic<int> found{0};
  std::mutex m;
  web::AccountInfo info;
};

std::wstring Count(size_t n) {
  std::wstring d = std::to_wstring(n), out;
  for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) out += L','; out += d[i]; }
  return out;
}

bool PickFileDialog(HWND owner, std::wstring& out) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return false;
  DWORD opts = 0; dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  dlg->SetTitle(L"Choose a cookies.txt file");
  COMDLG_FILTERSPEC spec[] = {{L"Cookies (*.txt)", L"*.txt"}, {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, spec);
  if (FAILED(dlg->Show(owner))) return false;
  ComPtr<IShellItem> it;
  if (FAILED(dlg->GetResult(&it))) return false;
  PWSTR p = nullptr;
  if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
  out = p; CoTaskMemFree(p);
  return true;
}

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
  DownloadDialog(const std::wstring& url, const web::LinkInfo* fixture, const web::AccountInfo* accFixture = nullptr) : urlText_(url) {
    Settings st = SettingsStore::Get().Snapshot();
    quality_ = st.downloadQuality == 1 ? web::Quality::Compatible : web::Quality::Best;
    threshold_ = st.downloadFolderThreshold;
    dest_ = st.downloadFolder.empty() ? web::DefaultDownloadsDir() : fs::path(st.downloadFolder);
    saveText_ = st.downloadSaveText;
    cookieMode_ = std::clamp(st.downloadCookies, 0, 4);
    cookieFile_ = st.downloadCookiesFile;
    parallel_ = std::clamp(st.downloadParallel, 1, 4);
    if (fixture) { fixture_ = *fixture; useFixture_ = true; }
    if (accFixture) { accFixture_ = *accFixture; useFixture_ = true; accountFixture_ = true; }
  }
  ~DownloadDialog() override {
    if (probe_) probe_->cancel = true;
    if (list_) list_->cancel = true;
    if (thumbs_) thumbs_->alive = false;
  }

 protected:
  enum class Stage { Input, Probing, Listing, Ready, Failed };
  enum class Mode { Post, Account };

  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Download from Link"; o.widthDip = 600; o.heightDip = 700; o.resizable = true; o.minWidthDip = 500; o.minHeightDip = 520;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "download-link"; }

  void OnOpened() override {
    ToolDialog::OnOpened();
    if (useFixture_) { if (accountFixture_) AdoptAccount(accFixture_); else Adopt(fixture_); return; }
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
    if (msg == WM_DL_THUMB || msg == WM_DL_COUNT) { Invalidate(); SetHandled(true); return 0; }
    if (msg == WM_DL_LISTED) {
      if (list_ && (uint64_t)wp == epoch_) {
        web::AccountInfo ai;
        { std::lock_guard lk(list_->m); ai = list_->info; }
        if (ai.ok) AdoptAccount(ai);
        else { stage_ = Stage::Failed; error_ = ai.error; needsLogin_ = ai.needsLogin; Invalidate(); }
      }
      SetHandled(true); return 0;
    }
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
      const bool busy = stage_ == Stage::Probing || stage_ == Stage::Listing;
      ui.TextInput(c[0], urlText_, L"Paste a link: a post, or a whole account or channel", false, !busy);
      if (ui.Button(c[1], L"Paste", ButtonStyle::Secondary, !busy)) {
        const std::wstring u = web::ExtractUrl(ClipboardText(Hwnd()));
        if (!u.empty()) { urlText_ = u; StartProbe(); }
      }
      if (ui.Button(c[2], stage_ == Stage::Ready ? L"Reload" : L"Read", ButtonStyle::Primary, !busy && !web::ExtractUrl(urlText_).empty())) StartProbe();
      ui.PopId();
      y += 42;
    }

    if (stage_ != Stage::Ready) { DrawStatusBody(ui, {body.x + pad, y, body.w - 2 * pad, body.bottom() - y - 8}); DrawFooterBar(ui, content); return; }
    if (mode_ == Mode::Account) { DrawAccount(ui, body, y); DrawFooterBar(ui, content); return; }

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
    } else if (stage_ == Stage::Listing) {
      const int n = list_ ? list_->found.load() : 0;
      p.Text({r.x, r.y + 60, r.w, 26}, L"Reading the account...", pal.text, ts);
      ui.ProgressBar({r.cx() - 120, r.y + 98, 240, 6}, 0, true);
      TextStyle big; big.size = 22; big.weight = 700; big.align = Align::Center;
      p.Text({r.x, r.y + 118, r.w, 34}, n > 0 ? Count((size_t)n) + (n == 1 ? L" post found" : L" posts found") : std::wstring(L"Looking for posts..."), accent::base, big);
      TextStyle sm; sm.size = 12.5f; sm.align = Align::Center; sm.wrap = true; sm.vcenter = false;
      p.Text({r.x + 30, r.y + 160, r.w - 60, 40}, L"Filo lists every post first. Nothing is saved until you press Download. Big accounts take a few minutes.", pal.textTertiary, sm);
      ui.PushId(0xD14);
      if (ui.Button({r.cx() - 100, r.y + 214, 200, 30}, L"Use what was found", ButtonStyle::Secondary, n > 0) && list_) { list_->keep = true; list_->cancel = true; }
      ui.PopId();
    } else if (stage_ == Stage::Failed) {
      p.FillCircle(r.cx(), r.y + 86, 26, Color::Hex(0xE5484D, 0.16f));
      p.Icon("warning", r.cx(), r.y + 86, 28, Color::Hex(0xE5484D), 2.f);
      TextStyle t; t.size = 16; t.weight = 600; t.align = Align::Center;
      p.Text({r.x, r.y + 126, r.w, 26}, accountUrl_ ? L"This account could not be read" : L"This link could not be read", pal.text, t);
      p.Text({r.x + 24, r.y + 158, r.w - 48, 70}, error_.empty() ? L"Something went wrong." : error_, pal.textSecondary, ts);
      if (needsLogin_) {
        ui.PushId(0xD15);
        TextStyle ls; ls.size = dialog::bodyFontSize;
        p.Text({r.x + 24, r.y + 236, 150, 30}, L"Sign in with", pal.text, ls);
        DrawCookiePicker(ui, {r.x + 170, r.y + 236, r.w - 194, 30});
        if (ui.Button({r.cx() - 70, r.y + 282, 140, 30}, L"Read again", ButtonStyle::Primary, !web::ExtractUrl(urlText_).empty())) StartProbe();
        ui.PopId();
      }
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
    if (info_.textOnly) s = L"Text only  \x00B7  no picture or video";
    return s;
  }

  float OptionsHeight() const {
    return 36.f * (2 + (info_.CountOf(web::ItemKind::Video) ? 2 : 0) + (info_.CountOf(web::ItemKind::Video) || info_.CountOf(web::ItemKind::Audio) ? 1 : 0) + (info_.HasText() ? 1 : 0)) + 18.f;
  }

  int SelectedFiles() const {
    if (info_.textOnly) return saveText_ && info_.HasText() ? 1 : 0;
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
    if (stage_ == Stage::Ready && mode_ == Mode::Account) {
      const int posts = SelectedPosts();
      fi.actionEnabled = posts > 0 && (accVideos_ || accPictures_ || saveText_);
      fi.status = fi.actionEnabled ? Count((size_t)posts) + (acc_.ref.isPlaylist ? (posts == 1 ? L" video" : L" videos") : (posts == 1 ? L" post" : L" posts")) + L"  \x00B7  into " + web::AccountFolderName(acc_.platform, acc_.handle)
                                   : std::wstring(L"Choose what to save.");
      if (DrawFooter(ui, content, fi).action) StartDownload();
      return;
    }
    const int n = SelectedFiles();
    fi.actionEnabled = stage_ == Stage::Ready && n > 0;
    if (stage_ == Stage::Ready) {
      if (n == 0) fi.status = L"Choose at least one item.";
      else {
        fi.status = std::to_wstring(n) + (n == 1 ? L" file" : L" files");
        if (const int64_t b = SelectedBytes()) fi.status += L"  \x00B7  about " + FormatBytes((uint64_t)b);
        if (threshold_ > 0 && n >= threshold_) fi.status += L"  \x00B7  saved in a folder";
      }
    } else fi.status = stage_ == Stage::Probing || stage_ == Stage::Listing ? L"Reading..." : L"Downloads go to " + dest_.wstring();
    if (DrawFooter(ui, content, fi).action) StartDownload();
  }

  void DrawOptions(Ui& ui, const Rect& r) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    p.FillRect({r.x, r.y, r.w, 1}, pal.divider);
    float y = r.y + 10;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    auto label = [&](float yy, const wchar_t* t) { p.Text({r.x, yy, r.w * 0.4f, 30}, t, pal.text, ls); };
    ui.PushId(0xD12);
    if (info_.HasText()) {
      ui.Checkbox(ui.Mirror({r.x, y + 2, r.w, 26}, r), info_.textOnly ? L"Save the post text (.txt)" : L"Also save the post text (.txt)", saveText_);
      y += 36;
    }
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

  // ------------------------------------------------------------------------------------------------ accounts
  int SelectedPosts() const {
    int n = 0;
    for (char c : accPicked_) n += c ? 1 : 0;
    return n;
  }
  void PickFirst(int count) {                              // count 0 = everything
    for (size_t i = 0; i < accPicked_.size(); ++i) accPicked_[i] = count <= 0 || (int)i < count;
  }

  void ApplyCookies() {
    web::SetCookies((web::CookieSource)std::clamp(cookieMode_, 0, 4), cookieFile_);
    SettingsStore::Get().Update([&](Settings& s) { s.downloadCookies = cookieMode_; s.downloadCookiesFile = cookieFile_; });
  }

  // "Sign in with": none, a browser's saved login, or a cookies.txt file. Sites like Instagram only show accounts to signed-in visitors.
  void DrawCookiePicker(Ui& ui, const Rect& r) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    static const std::vector<std::wstring> kinds = {L"Nobody", L"Firefox", L"Chrome", L"Edge", L"cookies.txt file"};
    Rect dd{r.right() - 170, r.y + 2, 170, 26};
    int sel = cookieMode_;
    if (ui.Dropdown(ui.Mirror(dd, r), kinds, sel) && sel != cookieMode_) {
      cookieMode_ = sel;
      if (sel == 4 && cookieFile_.empty()) { std::wstring f; if (PickFileDialog(Hwnd(), f)) cookieFile_ = f; else cookieMode_ = 0; }
      ApplyCookies();
    }
    if (cookieMode_ == 4) {
      Rect fb{dd.x - 8 - 150, r.y + 2, 150, 26};
      const std::wstring name = cookieFile_.empty() ? std::wstring(L"Choose file...") : fs::path(cookieFile_).filename().wstring();
      if (ui.Button(ui.Mirror(fb, r), name, ButtonStyle::Secondary)) { std::wstring f; if (PickFileDialog(Hwnd(), f)) { cookieFile_ = f; ApplyCookies(); } }
    }
    (void)pal; (void)p;
  }

  float AccountOptionsHeight() const { return 36.f * 6 + 18.f; }

  void DrawAccount(Ui& ui, const Rect& body, float y) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const float pad = dialog::contentInset;
    const bool rtl = ui.Rtl();
    // ---- header: a disc with the first letter, the name, the handle, how many posts
    {
      Rect th{body.x + pad, y, 72, 72};
      if (rtl) th.x = body.right() - pad - 72;
      p.FillCircle(th.cx(), th.cy(), 36, accent::base.WithAlpha(0.16f));
      TextStyle ti; ti.size = 28; ti.weight = 700; ti.align = Align::Center;
      p.Text(th, acc_.name.empty() ? std::wstring(L"?") : std::wstring(1, (wchar_t)towupper(acc_.name[0])), accent::base, ti);
      const float tx = rtl ? body.x + pad : th.right() + 14, tw = body.w - 2 * pad - 72 - 14;
      TextStyle t1; t1.size = 16; t1.weight = 700; t1.ellipsis = true; t1.align = rtl ? Align::End : Align::Start;
      TextStyle t2; t2.size = 13; t2.ellipsis = true; t2.align = rtl ? Align::End : Align::Start;
      p.Text({tx, y + 4, tw, 24}, acc_.name.empty() ? acc_.handle : acc_.name, pal.text, t1);
      p.Text({tx, y + 32, tw, 18}, acc_.platform.name + (acc_.ref.isPlaylist ? std::wstring(L"  \x00B7  Playlist") : acc_.handle.empty() ? std::wstring() : L"  \x00B7  @" + acc_.handle), pal.textSecondary, t2);
      std::wstring sum = Count(acc_.entries.size()) + (acc_.ref.isPlaylist ? (acc_.entries.size() == 1 ? L" video" : L" videos") : (acc_.entries.size() == 1 ? L" post" : L" posts"));
      if (acc_.capped) sum += L"  \x00B7  the first " + Count(acc_.entries.size()) + L" are listed";
      else if (acc_.interrupted) sum += L"  \x00B7  listing stopped early";
      if (acc_.platform.id == "x") sum += L"  \x00B7  reposts not included";
      p.Text({tx, y + 52, tw, 18}, sum, pal.textTertiary, t2);
      y += 84;
    }
    const float optH = AccountOptionsHeight();
    {
      TextStyle ts; ts.size = 13; ts.weight = 600; ts.align = rtl ? Align::End : Align::Start;
      Rect bar{body.x + pad, y, body.w - 2 * pad, 28};
      const int total = (int)acc_.entries.size(), sel = SelectedPosts();
      p.Text(ui.Mirror({bar.x, bar.y, bar.w - 200, 28}, bar), Count((size_t)sel) + L" of " + Count((size_t)total) + (acc_.ref.isPlaylist ? L" videos selected" : L" posts selected"), pal.textSecondary, ts);
      ui.PushId(0xD17);
      if (ui.Button(ui.Mirror({bar.right() - 190, bar.y + 1, 90, 26}, bar), L"All", ButtonStyle::Quiet, sel < total)) PickFirst(0);
      if (ui.Button(ui.Mirror({bar.right() - 96, bar.y + 1, 90, 26}, bar), L"None", ButtonStyle::Quiet, sel > 0)) std::fill(accPicked_.begin(), accPicked_.end(), 0);
      ui.PopId();
      y += 32;
    }
    Rect vp{body.x + pad, y, body.w - 2 * pad, std::max(60.f, body.bottom() - optH - y - 6)};
    DrawPostList(ui, vp);
    DrawAccountOptions(ui, {body.x + pad, body.bottom() - optH, body.w - 2 * pad, optH});
  }

  // the posts, newest first; the ones past "Download the latest N" are dimmed
  void DrawPostList(Ui& ui, const Rect& vp) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const float rowH = 44.f, gap = 4.f;
    const float total = acc_.entries.size() * (rowH + gap);
    const float off = ui.BeginScroll(vp, total, ui.Id("acc-scroll"));
    const float w = vp.w - (total > vp.h ? 12.f : 0.f);
    TextStyle t1; t1.size = 13.f; t1.weight = 600; t1.ellipsis = true; t1.align = ui.Rtl() ? Align::End : Align::Start;
    TextStyle t2; t2.size = 12.f; t2.ellipsis = true; t2.align = ui.Rtl() ? Align::End : Align::Start;
    const int first = std::max(0, (int)std::floor(off / (rowH + gap)));
    const int last = std::min((int)acc_.entries.size(), first + (int)std::ceil(vp.h / (rowH + gap)) + 2);
    for (int i = first; i < last; ++i) {
      const web::AccountEntry& e = acc_.entries[i];
      Rect row{vp.x, vp.y - off + i * (rowH + gap), w, rowH};
      const bool on = i < (int)accPicked_.size() && accPicked_[i];
      p.FillRoundRect(row, control::listRowRadius, pal.fieldFill);
      if (on) p.StrokeRoundRect(row, control::listRowRadius, accent::base, 1.5f);
      std::wstring title = e.title.empty() ? (e.text.empty() ? std::wstring(L"Post ") + Widen(e.id) : e.text) : e.title;
      std::replace(title.begin(), title.end(), L'\n', L' ');
      std::wstring sub = web::FormatUtcTime(e.timestamp);
      if (!sub.empty()) sub = sub.substr(0, 10);
      auto add = [&](const std::wstring& s) { if (s.empty()) return; if (!sub.empty()) sub += L"  \x00B7  "; sub += s; };
      if (e.duration > 0) { const int t = (int)(e.duration + 0.5); wchar_t b[24]; if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60); else swprintf_s(b, L"%d:%02d", t / 60, t % 60); add(b); }
      if (e.hasMedia && !e.media.empty()) add(std::to_wstring(e.media.size()) + (e.media.size() == 1 ? L" file" : L" files"));
      if (e.hasMedia && e.media.empty()) add(L"text only");
      Rect tr{row.x + 12, row.y + 5, row.w - 52, 18};
      Rect sr{row.x + 12, row.y + 24, row.w - 52, 16};
      p.Text(ui.Mirror(tr, row), title, on ? pal.text : pal.textTertiary, t1);
      p.Text(ui.Mirror(sr, row), sub, pal.textTertiary, t2);
      DrawMark(p, ui.Rtl() ? row.x + 22 : row.right() - 22, row.cy(), on, 10);
      ui.PushId(i);
      Interaction hit = ui.Behavior(row, ui.Id("accrow"), true, false);
      if (hit.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
      if (hit.clicked && i < (int)accPicked_.size()) accPicked_[i] = !accPicked_[i];
      ui.PopId();
    }
    ui.EndScroll();
  }

  void DrawAccountOptions(Ui& ui, const Rect& r) {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    p.FillRect({r.x, r.y, r.w, 1}, pal.divider);
    float y = r.y + 10;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    auto label = [&](float yy, const wchar_t* t) { p.Text(ui.Mirror({r.x, yy, r.w * 0.4f, 30}, r), t, pal.text, ls); };
    ui.PushId(0xD16);
    // how many
    {
      label(y, acc_.ref.isPlaylist ? L"Select the first" : L"Select the latest");
      std::vector<std::wstring> names;
      for (int v : limitVals_) names.push_back(v == 0 ? std::wstring(L"All") : Count((size_t)v));
      const int before = limitIdx_;
      limitIdx_ = std::clamp(ui.Segmented(ui.Mirror({r.right() - 290, y + 2, 290, 26}, r), names, limitIdx_), 0, (int)limitVals_.size() - 1);
      if (limitIdx_ != before) PickFirst(limitVals_[limitIdx_]);
      y += 36;
    }
    // what
    {
      label(y, L"Save");
      Rect box{r.right() - 290, y + 2, 290, 26};
      auto c = ui.Cols(ui.Mirror(box, r), {Flex(1), Flex(1), Flex(1.25f)}, 4);
      ui.Checkbox(c[0], L"Videos", accVideos_);
      ui.Checkbox(c[1], L"Pictures", accPictures_);
      ui.Checkbox(c[2], L"Post text", saveText_);
      y += 36;
    }
    label(y, L"Video quality");
    {
      int n = ui.Segmented(ui.Mirror({r.right() - 290, y + 2, 290, 26}, r), {L"Best available", L"Most compatible"}, quality_ == web::Quality::Best ? 0 : 1);
      quality_ = n == 1 ? web::Quality::Compatible : web::Quality::Best;
      y += 36;
    }
    label(y, L"Sign in with");
    DrawCookiePicker(ui, {r.x + r.w * 0.3f, y, r.w * 0.7f, 30});
    y += 36;
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
    {
      ui.Checkbox(ui.Mirror({r.x, y + 2, r.w * 0.52f, 26}, r), L"Skip posts saved before", skipDone_);
      p.Text(ui.Mirror({r.x + r.w * 0.56f, y, r.w * 0.2f, 30}, r), L"At once", pal.text, ls);
      double v = parallel_;
      if (ui.Stepper(ui.Mirror({r.right() - 110, y + 2, 110, 26}, r), v, 1, 4, 1, 0)) parallel_ = (int)v;
    }
    ui.PopId();
  }

  void StartList(const std::wstring& url) {
    if (probe_) probe_->cancel = true;
    if (list_) list_->cancel = true;
    if (thumbs_) thumbs_->alive = false;
    thumbs_.reset();
    list_ = std::make_shared<ListShared>();
    mode_ = Mode::Account; accountUrl_ = true;
    stage_ = Stage::Listing; error_.clear(); needsLogin_ = false;
    urlText_ = url;
    const uint64_t epoch = ++epoch_;
    auto sh = list_; HWND hw = Hwnd();
    std::thread([sh, url, hw, epoch]() {
      web::ListOptions lo;
      web::AccountInfo ai = web::ListAccount(url, lo, &sh->cancel, [&](const web::AccountEntry&) {
        const int n = ++sh->found;
        if (n < 20 || n % 10 == 0) PostMessageW(hw, WM_DL_COUNT, 0, 0);
      });
      if (sh->cancel && !sh->keep) return;                 // replaced by another link, or the window closed
      { std::lock_guard lk(sh->m); sh->info = std::move(ai); }
      PostMessageW(hw, WM_DL_LISTED, (WPARAM)epoch, 0);
    }).detach();
    Invalidate();
  }

  void AdoptAccount(const web::AccountInfo& ai) {
    acc_ = ai;
    mode_ = Mode::Account; accountUrl_ = true;
    stage_ = Stage::Ready;
    limitVals_.clear();
    for (int v : {25, 100, 500}) if ((int)ai.entries.size() > v) limitVals_.push_back(v);
    limitVals_.push_back(0);
    limitIdx_ = (int)limitVals_.size() - 1;               // all of it
    accPicked_.assign(ai.entries.size(), 1);
    Invalidate();
  }

  void StartAccountDownload() {
    const int n = SelectedPosts();
    if (n <= 0) return;
    web::AccountDownloadRequest req;
    req.account = acc_;
    for (size_t i = 0; i < accPicked_.size(); ++i) if (accPicked_[i]) req.entries.push_back((int)i);
    req.videos = accVideos_; req.pictures = accPictures_; req.text = saveText_;
    req.quality = quality_;
    req.destRoot = dest_;
    req.folderThreshold = threshold_;
    req.skipDone = skipDone_;
    req.parallel = parallel_;
    SettingsStore::Get().Update([&](Settings& s) {
      s.downloadQuality = quality_ == web::Quality::Compatible ? 1 : 0;
      s.downloadFolder = dest_ == web::DefaultDownloadsDir() ? std::wstring() : dest_.wstring();
      s.downloadSaveText = saveText_;
      s.downloadParallel = parallel_;
    });
    RunJob(L"Downloading from " + (acc_.name.empty() ? acc_.handle : acc_.name), [req](JobContext& ctx) { return web::DownloadAccount(req, ctx); });
  }

  // ------------------------------------------------------------------------------------------------ probe and download
  void StartProbe() {
    const std::wstring url = web::ExtractUrl(urlText_);
    if (url.empty()) { stage_ = Stage::Failed; error_ = L"That is not a web address."; Invalidate(); return; }
    ApplyCookies();
    needsLogin_ = false;
    if (web::IsAccountUrl(url)) { StartList(url); return; }
    mode_ = Mode::Post; accountUrl_ = false;
    if (list_) list_->cancel = true;
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
    if (stage_ == Stage::Ready && mode_ == Mode::Account) { StartAccountDownload(); return; }
    if (stage_ != Stage::Ready || SelectedFiles() == 0) return;
    web::DownloadRequest req;
    req.info = info_;
    req.saveText = saveText_ && info_.HasText();
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
      s.downloadSaveText = saveText_;
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
  // accounts
  Mode mode_ = Mode::Post;
  bool accountUrl_ = false, needsLogin_ = false, accountFixture_ = false;
  web::AccountInfo acc_, accFixture_;
  std::shared_ptr<ListShared> list_;
  std::vector<int> limitVals_;
  std::vector<char> accPicked_;
  int limitIdx_ = 0;
  bool accVideos_ = true, accPictures_ = true, saveText_ = true, skipDone_ = true;
  int parallel_ = 3, cookieMode_ = 0;
  std::wstring cookieFile_;
};

}  // namespace

ui::DialogWindow* OpenDownloadDialog(const std::wstring& url, const web::LinkInfo* fixture, bool show, const web::AccountInfo* accountFixture) {
  auto* d = new DownloadDialog(url, fixture, accountFixture);
  return ToolDialog::OpenDialog(d, show) ? d : nullptr;
}

}  // namespace filo::tools
