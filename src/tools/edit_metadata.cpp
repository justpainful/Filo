// Edit Metadata - grouped, searchable field list with a view mode and an edit mode, per-field removal and one-click clean-up.
#include "tools/tools.h"
#include "core/output.h"
#include "engines/meta/metadata.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_META_READY = WM_APP + 0x63;

class MetadataDialog : public ToolDialog {
 public:
  explicit MetadataDialog(const Selection& sel) {
    selection_ = sel;
    path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
    state_ = std::make_shared<State>();
  }
  ~MetadataDialog() override {}

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Edit Metadata"; o.widthDip = 620; o.heightDip = 720; o.resizable = true; o.minWidthDip = 440; o.minHeightDip = 480;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "edit-metadata"; }
  void TestApply() override { Save(); }
  void LoadPreset(const Json& j) override {
    // test/automation preset: {"set":{"IFD0:Artist":"x"},"remove":["GPS:GPSLatitude"],"clean":"gps|personal|all"}
    if (j.Has("set")) for (auto& [k, v] : j.Get("set").Items()) setValues_[k] = Widen(v.AsString());
    if (j.Has("remove")) for (auto& v : j.Get("remove").Elements()) removed_.insert(v.AsString());
    if (j.Has("clean")) { const std::string c = j.Get("clean").AsString(); clean_ = c == "gps" ? 1 : (c == "personal" ? 2 : (c == "all" ? 3 : 0)); }
    if (j.Has("setFile") && j.Get("setFile").IsBool()) {}
  }
  void OnOpened() override {
    ToolDialog::OnOpened();
    auto st = state_; fs::path p = path_; HWND hw = Hwnd();
    std::thread([st, p, hw] {
      meta::MetadataSet ms = meta::ReadMetadata(p);
      { std::lock_guard lk(st->m); st->ms = std::move(ms); st->ready = true; }
      PostMessageW(hw, WM_META_READY, 0, 0);
    }).detach();
  }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_META_READY) { Invalidate(); SetHandled(true); return 0; }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = 20;
    meta::MetadataSet ms; bool ready;
    { std::lock_guard lk(state_->m); ready = state_->ready; if (ready && !loaded_) { ms = state_->ms; } }
    if (ready && !loaded_) { data_ = std::move(ms); loaded_ = true; for (auto& f : data_.fields) buf_[f.key] = f.value; }

    // header: file + mode + search
    Rect head{body.x + pad, body.y + 18, body.w - 2 * pad, 56};
    Rect fileIcon{head.x, head.y, 40, 42};
    p.FillRoundRect(fileIcon, 11, pal.fieldFill);
    p.Icon("document", fileIcon.cx(), fileIcon.cy(), 23, pal.textSecondary, 1.5f);
    TextStyle t; t.size = dialog::bodyFontSize; t.weight = 600;
    p.Text({head.x + 52, head.y, head.w - 52, 24}, NameOf(path_), pal.text, t);
    TextStyle s; s.size = dialog::smallFontSize;
    p.Text({head.x + 52, head.y + 24, head.w - 52, 20}, loaded_ ? (data_.ok ? std::to_wstring(data_.fields.size()) + L" fields" + (data_.hasGps ? L"   \x00B7   contains a location" : L"") : data_.error) : L"Reading metadata...", pal.textSecondary, s);
    Rect bar{body.x + pad, head.bottom() + 4, body.w - 2 * pad, 30};
    auto bc = ui.Cols(bar, {Fixed(150), Flex(1)}, 10);
    int mode = ui.Segmented(bc[0], {L"View", L"Edit"}, edit_ ? 1 : 0, loaded_ && data_.ok && data_.canWrite);
    edit_ = mode == 1;
    ui.TextInput(bc[1], search_, L"Search fields");

    Rect list{body.x + pad, bar.bottom() + 16, body.w - 2 * pad, body.bottom() - bar.bottom() - 16 - 62};
    p.FillRoundRect(list, 14, Theme::Get().IsDark() ? pal.darkCanvas.WithAlpha(0.12f) : pal.fieldFill.WithAlpha(0.28f));
    p.StrokeRoundRect(list, 14, pal.divider.WithAlpha(0.07f), 1);
    // quick clean-up
    Rect quick{body.x + pad, list.bottom() + 10, body.w - 2 * pad, 28};
    {
      auto qc = ui.Cols(quick, {Fixed(96), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(qc[0], L"Clean up", pal.text, ls);
      int nv = ui.Segmented(qc[1], {L"None", L"Location", L"Personal", L"All"}, clean_, loaded_ && data_.ok && data_.canWrite);
      clean_ = nv;
    }

    if (!loaded_) {
      ui.ProgressBar({list.x + 20, list.cy(), list.w - 40, 6}, 0, true);
    } else if (!data_.ok) {
      TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true;
      p.Text(list.Inset(30), data_.error, pal.textSecondary, es);
    } else {
      DrawList(ui, list);
    }

    FooterInfo fi;
    const int changes = (int)(Changed().size() + removed_.size()) + (clean_ ? 1 : 0);
    fi.action = L"Save a Copy"; fi.actionEnabled = loaded_ && data_.ok && data_.canWrite && changes > 0 && !Busy();
    fi.status = !loaded_ ? L"" : (!data_.canWrite ? L"This file type can be read but not edited." : (changes ? std::to_wstring(changes) + L" change(s) ready." : L"Nothing changed yet. Switch to Edit to change fields."));
    if (DrawFooter(ui, content, fi).action) Save();
    DrawJobPanel(ui, content);
  }

 private:
  struct State { std::mutex m; meta::MetadataSet ms; bool ready = false; };

  std::map<std::string, std::wstring> Changed() const {
    std::map<std::string, std::wstring> c = setValues_;
    for (auto& f : data_.fields) {
      auto it = buf_.find(f.key);
      if (f.editable && it != buf_.end() && it->second != f.value && !removed_.count(f.key)) c[f.key] = it->second;
    }
    return c;
  }

  void DrawList(Ui& ui, const Rect& vp) {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    // collect visible rows (group headers + fields) filtered by the search text
    struct Row { bool header; const meta::Field* f; meta::Group g; };
    std::vector<Row> rows;
    std::wstring q = ToLower(search_);
    for (int gi = 0; gi < 12; ++gi) {
      meta::Group g = (meta::Group)gi;
      std::vector<const meta::Field*> fs_;
      for (auto& f : data_.fields) if (f.group == g) {
        if (!q.empty() && ToLower(f.label).find(q) == std::wstring::npos && ToLower(f.value).find(q) == std::wstring::npos && ToLower(Widen(f.key)).find(q) == std::wstring::npos) continue;
        fs_.push_back(&f);
      }
      if (fs_.empty()) continue;
      rows.push_back({true, nullptr, g});
      for (auto* f : fs_) rows.push_back({false, f, g});
    }
    const float rowH = 38, headH = 42;
    float total = 8; for (auto& r : rows) total += r.header ? headH : rowH;
    const float off = ui.BeginScroll(vp, total, ui.Id("metalist"));
    float y = vp.y - off + 4;
    const float x0 = vp.x + 12, w = vp.w - 24 - (total > vp.h ? 8.f : 0.f);
    TextStyle ls; ls.size = dialog::bodyFontSize;
    TextStyle vs; vs.size = dialog::bodyFontSize; vs.align = Align::Start;
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      const float h = r.header ? headH : rowH;
      if (y + h < vp.y - 2 || y > vp.bottom() + 2) { y += h; continue; }
      if (r.header) {
        ui.SectionTitle({x0, y + 8, w, 22}, meta::GroupLabel(r.g));
      } else {
        const meta::Field& f = *r.f;
        if (i % 2 == 0) p.FillRoundRect({x0 - 4, y, w + 8, rowH - 3}, 6, pal.controlFill.WithAlpha(Theme::Get().IsDark() ? 0.025f : 0.15f));
        ui.PushId(ui.Id(Widen(f.key)));
        const bool gone = removed_.count(f.key) > 0;
        auto c = ui.Cols({x0, y, w, rowH - 4}, {Fixed(0.38f * w), Flex(1), Fixed(f.removable && edit_ ? 28.f : 0.f)}, 8);
        p.Text(c[0], f.label, gone ? pal.textDisabled : pal.textSecondary, ls);
        if (edit_ && f.editable && !gone) {
          std::wstring& b = buf_[f.key];
          ui.TextInput(c[1], b, L"");
        } else {
          p.Text(c[1], gone ? L"(will be removed)" : f.value, gone ? pal.textDisabled : pal.text, vs);
          const auto valueHover = ui.Behavior(c[1], ui.Id("value-tip"), true, false);
          if (valueHover.hot) ui.Tooltip(f.value);
        }
        if (edit_ && f.removable && c[2].w > 0) {
          if (ui.IconButton({c[2].x, c[2].cy() - 12, 24, 24}, gone ? "reset" : "trash", true, gone ? L"Keep this field" : L"Remove this field", false)) {
            if (gone) removed_.erase(f.key); else removed_.insert(f.key);
          }
        }
        ui.PopId();
      }
      y += h;
    }
    if (rows.empty()) {
      TextStyle es; es.size = 14; es.align = Align::Center;
      p.Text({vp.x, vp.y + 30, vp.w, 24}, L"No fields match.", pal.textTertiary, es);
    }
    ui.EndScroll();
  }

  void Save() {
    if (!loaded_ || !data_.ok || Busy()) return;
    meta::Edits ed;
    ed.set = Changed();
    for (auto& k : removed_) ed.remove.push_back(k);
    const int clean = clean_;
    fs::path in = path_;
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(in, ExtOf(in), st, L" (metadata)");
    RunJob(L"Saving metadata...", [ed, clean, in, desired, st](JobContext& ctx) -> JobResult {
      fs::path cur = in;
      fs::path stage1;
      std::vector<std::wstring> warnings;
      if (!ed.Empty()) {
        fs::path tmp = clean ? ctx.TempDir() / (L"stage1" + in.extension().wstring()) : desired;
        meta::WriteResult wr = meta::WriteMetadata(cur, ed, tmp, &ctx);
        if (!wr.ok) return JobResult::Fail(wr.error);
        for (auto& w : wr.warnings) warnings.push_back(w);
        cur = wr.path; stage1 = wr.path;
        if (!clean) { JobResult r = JobResult::Ok({wr.path}); r.warnings = warnings; if (wr.contentVerified) r.warnings.push_back(L"Content verified unchanged (" + wr.verifyNote + L")."); return r; }
      }
      if (clean) {
        meta::StripProfile pr = clean == 1 ? meta::StripProfile::GpsOnly : (clean == 2 ? meta::StripProfile::PersonalInfo : meta::StripProfile::All);
        meta::WriteResult wr = meta::StripMetadata(cur, desired, pr, &ctx);
        if (!wr.ok) return JobResult::Fail(wr.error);
        for (auto& w : wr.warnings) warnings.push_back(w);
        JobResult r = JobResult::Ok({wr.path}); r.warnings = warnings;
        if (wr.contentVerified) r.warnings.push_back(L"Content verified unchanged (" + wr.verifyNote + L").");
        return r;
      }
      return JobResult::Fail(L"Nothing to change.");
    });
  }

  fs::path path_;
  std::shared_ptr<State> state_;
  meta::MetadataSet data_;
  bool loaded_ = false, edit_ = false;
  std::wstring search_;
  std::map<std::string, std::wstring> buf_, setValues_;
  std::set<std::string> removed_;
  int clean_ = 0;
};

} // namespace

ToolDialog* MakeEditMetadata(const Selection& sel) { return new MetadataDialog(sel); }

} // namespace filo::tools
