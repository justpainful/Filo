// Mute (Remove Audio): small confirmation window. Lists the audio tracks that will be dropped; the video stream is copied bit-exact.
#include "tools/video/vid_common.h"
#include <algorithm>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

class RemoveAudioDialog : public ToolDialog {
 public:
  explicit RemoveAudioDialog(const Selection& sel) {
    selection_ = sel;
    path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
    info_ = media::SummarizeMedia(path_);
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Remove Audio"; o.widthDip = 480; o.heightDip = 440;
    o.rtl = UiRtl();
    return o;
  }
  std::string ToolId() const override { return "remove-audio"; }
  void TestApply() override { Start(); }
  bool OnEnter() override { enter_ = true; Invalidate(); return true; }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const bool ok = info_.ok && info_.hasVideo;
    const bool hasAudio = ok && info_.hasAudio;

    // ---- the video
    Rect card{body.x + pad, body.y + 16, body.w - 2 * pad, 84};
    p.FillRoundRect(card, control::listRowRadius, pal.fieldFill);
    p.StrokeRoundRect(card, control::listRowRadius, pal.divider, 1.f);
    Rect th{card.x + 10, card.y + 10, 92, card.h - 20};
    if (ui.Rtl()) th.x = card.right() - 10 - th.w;
    p.FillRoundRect(th, 8, Color(0, 0, 0, 0.14f));
    if (ID2D1Bitmap* bmp = Thumb(path_, 200)) {
      auto sz = bmp->GetSize();
      const float s = std::max(th.w / sz.width, th.h / sz.height);
      p.PushClipRoundRect(th, 8);
      p.Bitmap(bmp, {th.cx() - sz.width * s / 2, th.cy() - sz.height * s / 2, sz.width * s, sz.height * s});
      p.PopClip();
    } else p.Icon("video", th.cx(), th.cy(), 22, pal.textTertiary, 1.5f);
    const float tx = ui.Rtl() ? card.x + 14 : th.right() + 14, tw = card.w - th.w - 34;
    TextStyle nm; nm.size = 14; nm.weight = 600;
    p.Text({tx, card.y + 16, tw, 24}, NameOf(path_), pal.text, nm);
    TextStyle sub; sub.size = 12.5f;
    std::wstring line = !info_.ok ? (info_.error.empty() ? L"This file cannot be read." : info_.error) : info_.videoLine;
    if (info_.ok && info_.duration > 0) line = FormatTime(info_.duration, false) + L"  \x00B7  " + line;
    p.Text({tx, card.y + 42, tw, 22}, line, info_.ok ? pal.textSecondary : Color::Hex(0xC2410C), sub);

    // ---- tracks
    float y = card.bottom() + 20;
    ui.SectionTitle({body.x + pad, y, body.w - 2 * pad, 22}, hasAudio && info_.audioTracks.size() > 1 ? L"Audio tracks to remove" : L"Audio to remove");
    y += 30;
    if (hasAudio) {
      std::vector<std::wstring> tracks;
      for (auto& t : info_.audioTracks) tracks.push_back(t.description.empty() ? t.codecName : t.description);
      if (tracks.empty()) tracks.push_back(info_.audioLine.empty() ? L"Audio track" : info_.audioLine);
      for (size_t i = 0; i < tracks.size() && i < 4; ++i) {
        Rect row{body.x + pad, y, body.w - 2 * pad, 38};
        p.FillRoundRect(row, 12, pal.fieldFill);
        p.StrokeRoundRect(row, 12, pal.divider, 1.f);
        p.Icon("audio", ui.Rtl() ? row.right() - 24 : row.x + 24, row.cy(), 18, pal.textSecondary, 1.6f);
        TextStyle ts; ts.size = 13.5f;
        p.Text({row.x + 46, row.y, row.w - 92, row.h}, tracks[i], pal.text, ts);
        TextStyle ss; ss.size = 12; ss.align = Align::End;
        p.Text({row.right() - 100, row.y, 86, row.h}, L"Removed", pal.textTertiary, ss);
        y += 44;
      }
      if (tracks.size() > 4) { TextStyle ms; ms.size = 12.5f; p.Text({body.x + pad, y, body.w - 2 * pad, 20}, L"+ " + std::to_wstring(tracks.size() - 4) + L" more", pal.textTertiary, ms); y += 24; }
    } else {
      Rect row{body.x + pad, y, body.w - 2 * pad, 44};
      p.FillRoundRect(row, 12, pal.fieldFill);
      TextStyle ts; ts.size = 13.5f; ts.align = Align::Center;
      p.Text(row, ok ? L"This video has no audio track." : L"Nothing to remove.", pal.textSecondary, ts);
      y += 52;
    }

    // ---- result
    y += 8;
    ui.SectionTitle({body.x + pad, y, body.w - 2 * pad, 22}, L"Result");
    y += 28;
    if (ok) {
      TextStyle os; os.size = 13.5f; os.weight = 500;
      p.Text({body.x + pad, y, body.w - 2 * pad, 22}, OutputName(), pal.text, os);
      y += 26;
    }
    TextStyle hs; hs.size = 12.5f; hs.wrap = true; hs.ellipsis = false; hs.vcenter = false;
    p.Text({body.x + pad, y, body.w - 2 * pad, 40}, L"The picture is copied as it is, without re-encoding, so there is no quality loss and it only takes a moment. The original is never changed.", pal.textSecondary, hs);

    FooterInfo fi;
    fi.action = L"Remove Audio";
    fi.actionEnabled = hasAudio && !Busy();
    fi.status = !ok ? (info_.error.empty() ? L"This file cannot be read." : info_.error) : (!hasAudio ? L"This video has no audio track." : L"");
    fi.statusIsError = !ok || !hasAudio;
    const bool enter = enter_; enter_ = false;
    FooterResult fr = DrawFooter(ui, content, fi);
    if ((fr.action || enter) && fi.actionEnabled) Start();
    DrawJobPanel(ui, content);
  }

 private:
  std::wstring OutputName() const {
    std::wstring ext = ExtOf(path_);
    static const wchar_t* ok[] = {L"mp4", L"mov", L"mkv", L"webm", L"avi", L"m4v", L"wmv"};
    bool known = false; for (auto e : ok) if (ext == e) known = true;
    if (!known) ext = L"mp4";
    Settings st = SettingsStore::Get().Snapshot();
    return DesiredOutputPath(path_, ext, st, L" (no audio)").filename().wstring();
  }
  void Start() {
    media::RemoveAudioRequest req;
    const fs::path in = path_;
    RunJob(L"Removing the audio...", [in, req](JobContext& ctx) -> JobResult { return media::RemoveAudio(in, req, ctx); });
  }

  fs::path path_;
  media::MediaSummary info_;
  bool enter_ = false;
};

} // namespace

ToolDialog* MakeRemoveAudio(const Selection& sel) { return new RemoveAudioDialog(sel); }

} // namespace filo::tools
