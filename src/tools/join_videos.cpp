// Join Videos - modelled on the owner's reference window: light rounded window, circular close button at the top left,
// centred title, "Playback order" heading, rounded rows (number, thumbnail, file name, move up/down), footer divider and the
// "Join Videos" button at the bottom right.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/output.h"
#include "engines/media/media_info.h"
#include "engines/media/media_ops.h"
#include <algorithm>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

class JoinVideosDialog : public ToolDialog {
 public:
  explicit JoinVideosDialog(const Selection& sel) {
    selection_ = sel;
    for (auto& i : sel.items) AddClip(i.path);
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Join Videos"; o.widthDip = 480; o.heightDip = 560; o.rtl = Settings_RTL();
    return o;
  }
  std::string ToolId() const override { return "join-videos"; }
  bool AcceptsFileDrop() const override { return !Busy(); }
  void OnFilesDropped(const std::vector<fs::path>& files) override { for (auto& f : files) AddClip(f); }
  bool OnEnter() override { return false; }
  void TestApply() override { if (clips_.size() >= 2) StartJoin(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    // ---- heading
    Rect head{body.x + pad, body.y + 16, body.w - 2 * pad, 22};
    auto hc = ui.Cols(head, {Flex(1), Fixed(120)}, 8);
    ui.SectionTitle(hc[0], L"Playback order");
    TextStyle cnt; cnt.size = 12; cnt.align = Align::End;
    p.Text(hc[1], std::to_wstring(clips_.size()) + (clips_.size() == 1 ? L" clip" : L" clips"), pal.textTertiary, cnt);

    // ---- list
    Rect vp{body.x + pad - 4, head.bottom() + 10, body.w - 2 * pad + 8, body.bottom() - head.bottom() - 20};
    const float rowH = control::listRowHeight, gap = 8;
    const float contentH = clips_.size() * (rowH + gap);
    const float off = ui.BeginScroll(vp, contentH, ui.Id("list"));
    int swapA = -1, swapB = -1;
    for (int i = 0; i < (int)clips_.size(); ++i) {
      Rect row{vp.x + 4, vp.y + i * (rowH + gap) - off, vp.w - 8 - (contentH > vp.h ? 8.f : 0.f), rowH};
      if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
      ui.PushId(i);
      DrawRow(ui, row, i, swapA, swapB);
      ui.PopId();
    }
    if (clips_.empty()) {
      TextStyle es; es.size = 13; es.align = Align::Center; es.wrap = true;
      p.Text({vp.x + 20, vp.cy() - 30, vp.w - 40, 60}, L"Drop videos here to add them", pal.textTertiary, es);
    }
    ui.EndScroll();
    if (swapA >= 0) std::swap(clips_[swapA], clips_[swapB]);

    // ---- footer
    FooterInfo fi;
    fi.action = L"Join Videos";
    fi.actionEnabled = clips_.size() >= 2 && !Busy();
    fi.status = clips_.size() < 2 ? L"Add at least two videos." : OutputSummary();
    fi.statusIsError = clips_.size() < 2;
    if (DrawFooter(ui, content, fi).action) StartJoin();
    DrawJobPanel(ui, content);
  }

 private:
  struct Clip { fs::path path; media::MediaSummary info; };
  std::vector<Clip> clips_;

  static bool Settings_RTL() { return SettingsStore::Get().Snapshot().language == L"ar"; }

  void AddClip(const fs::path& path) {
    FileKind k = DetectFile(path);
    if (k.group != Group::Video) return;
    for (auto& c : clips_) if (c.path == path) return;
    Clip c; c.path = path; c.info = media::SummarizeMedia(path);
    clips_.push_back(std::move(c));
  }

  std::wstring OutputSummary() const {
    double total = 0; for (auto& c : clips_) total += c.info.duration;
    const auto& f = clips_.front().info;
    wchar_t b[160];
    if (f.ok && f.width > 0) swprintf_s(b, L"MP4 \x00B7 %dx%d \x00B7 %.0f fps (from the first clip) \x00B7 %s", f.width, f.height, f.fps > 0 ? f.fps : 30.0, FormatTime(total, false).c_str());
    else swprintf_s(b, L"MP4 \x00B7 %s", FormatTime(total, false).c_str());
    return b;
  }

  void DrawRow(Ui& ui, const Rect& row, int i, int& swapA, int& swapB) {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Clip& c = clips_[i];
    Interaction it = ui.Behavior(row, ui.Id("row"), true, false);
    const float hov = ui.Anim(ui.Id("rowhov"), it.hot ? 1.f : 0.f);
    p.FillRoundRect(row, control::listRowRadius, Mix(pal.fieldFill, accent::base.WithAlpha(0.12f), hov));
    p.StrokeRoundRect(row, control::listRowRadius, Mix(pal.divider, accent::base.WithAlpha(0.35f), hov), 1.f);
    const bool rtl = ui.Rtl();
    auto X = [&](float x, float w) { return rtl ? row.right() - (x - row.x) - w : x; };
    TextStyle ns; ns.size = 15; ns.weight = 600; ns.align = Align::Center;
    p.Text({X(row.x + 10, 28), row.y, 28, row.h}, std::to_wstring(i + 1), pal.textSecondary, ns);
    Rect th{X(row.x + 44, 76), row.y + 10, 76, row.h - 20};
    p.FillRoundRect(th, 8, Color(0, 0, 0, 0.14f));
    if (ID2D1Bitmap* bmp = Thumb(c.path, 160)) {
      // cover-fit the thumbnail inside the rounded rect
      auto sz = bmp->GetSize();
      const float s = std::max(th.w / sz.width, th.h / sz.height);
      p.PushClipRoundRect(th, 8);
      p.Bitmap(bmp, {th.cx() - sz.width * s / 2, th.cy() - sz.height * s / 2, sz.width * s, sz.height * s});
      p.PopClip();
    } else p.Icon("video", th.cx(), th.cy(), 22, pal.textTertiary, 1.5f);
    const float tx = row.x + 130, tw = row.w - 130 - 54;
    TextStyle nm; nm.size = 13; nm.weight = 500;
    p.Text({X(tx, tw), row.y + 11, tw, 22}, NameOf(c.path), pal.text, nm);
    TextStyle sub; sub.size = 11.5f;
    std::wstring info = c.info.ok ? (c.info.duration > 0 ? FormatTime(c.info.duration, false) : L"") : L"Can't read this file";
    if (c.info.ok && c.info.width > 0) info += L"  \x00B7  " + std::to_wstring(c.info.width) + L"\x00D7" + std::to_wstring(c.info.height);
    p.Text({X(tx, tw), row.y + 33, tw, 20}, info, c.info.ok ? pal.textTertiary : Color::Hex(0xC2410C), sub);
    // move up / down (stacked, right side like the reference)
    const float bx = X(row.right() - 14 - 22, 22);
    if (ui.IconButton({bx, row.y + 8, 22, 22}, "chevronup", i > 0 && !Busy(), L"Move up", true)) { swapA = i - 1; swapB = i; }
    if (ui.IconButton({bx, row.y + row.h - 30, 22, 22}, "chevrondown", i + 1 < (int)clips_.size() && !Busy(), L"Move down", true)) { swapA = i; swapB = i + 1; }
  }

  void StartJoin() {
    media::JoinRequest req;
    for (auto& c : clips_) req.inputs.push_back(c.path);
    Settings st = SettingsStore::Get().Snapshot();
    req.crf = st.videoCrf; req.hardware = st.hardwareAcceleration; req.audioBitrateKbps = st.audioBitrateKbps;
    req.outputPath = DesiredOutputPath(req.inputs[0], L"mp4", st, L" (joined)");
    selection_.items.clear();
    for (auto& c : clips_) selection_.items.push_back(DetectFile(c.path));
    RunJob(L"Joining videos...", [req](JobContext& ctx) -> JobResult { return media::JoinVideos(req, ctx); });
  }
};

} // namespace

ToolDialog* MakeJoinVideos(const Selection& sel) { return new JoinVideosDialog(sel); }

} // namespace filo::tools
