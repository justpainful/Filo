#include "tools/tool_dialog.h"
#include "core/log.h"
#include "core/settings.h"
#include "shell/actions.h"
#include "shell/thumbnail.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

struct ThumbPayload { std::wstring key; shell::ThumbnailImage img; };

ToolDialog::~ToolDialog() {
  if (jobListener_) JobManager::Get().RemoveListener(jobListener_);
  shell::ThumbnailService::Get().CancelOwner((uint64_t)(uintptr_t)this);
}

void ToolDialog::OnOpened() {
  Settings st = SettingsStore::Get().Snapshot();
  if (st.applyLastUsed) {
    const Json& p = st.toolPresets.Get(ToolId());
    if (p.IsObject()) LoadPreset(p);
  }
}

void ToolDialog::OnCloseRequested() {
  if (Busy()) return;                       // the job panel offers an explicit Cancel; closing mid-job would orphan it
  Json preset = SavePreset();
  if (preset.IsObject() && preset.Size() > 0) {
    const std::string id = ToolId();
    SettingsStore::Get().Update([&](Settings& s) { s.toolPresets[id] = preset; });
  }
  CloseWindow();
}

bool ToolDialog::RunForTest(const Json& preset, double timeoutSec, JobResult* result) {
  LoadPreset(preset);
  TestApply();
  if (phase_ == JobPhase::Idle) return false;
  const double until = NowSeconds() + timeoutSec;
  MSG m;
  while (phase_ == JobPhase::Running && NowSeconds() < until) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Sleep(15);
  }
  if (result) *result = result_;
  return phase_ == JobPhase::Done;
}

// ------------------------------------------------------------------------------------------------ jobs
void ToolDialog::RunJob(const std::wstring& title, std::function<JobResult(JobContext&)> work) {
  if (Busy()) return;
  ResetJob();
  phase_ = JobPhase::Running; progress_ = 0; stage_.clear(); jobTitle_ = title;
  JobSpec spec;
  spec.operation = L"tool:" + Widen(ToolId());
  spec.title = title;
  for (auto& i : selection_.items) spec.inputs.push_back(i.path);
  spec.work = std::move(work);
  jobId_ = JobManager::Get().Submit(std::move(spec));
  if (jobListener_) JobManager::Get().RemoveListener(jobListener_);
  HWND hw = Hwnd(); const uint64_t id = jobId_;
  jobListener_ = JobManager::Get().AddListener([hw, id](const JobInfo& info) {
    if (info.id == id) PostMessageW(hw, WM_FILO_TOOL_JOB, (WPARAM)id, 0);
  });
  Invalidate();
}

LRESULT ToolDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_FILO_TOOL_JOB) {
    if ((uint64_t)wp == jobId_) {
      if (auto info = JobManager::Get().Info(jobId_)) {
        progress_ = info->progress; stage_ = info->stage;
        if (info->Finished()) {
          result_ = info->result;
          if (info->state == JobState::Completed) { phase_ = JobPhase::Done; doneAt_ = NowSeconds(); }
          else if (info->state == JobState::Cancelled) phase_ = JobPhase::Idle;
          else phase_ = JobPhase::Failed;
        }
      }
    }
    Invalidate();
    SetHandled(true); return 0;
  }
  if (msg == WM_FILO_TOOL_THUMB) {
    std::unique_ptr<ThumbPayload> p(reinterpret_cast<ThumbPayload*>(lp));
    if (p) {
      auto& e = thumbs_[p->key];
      e.px = std::move(p->img.bgraPremul); e.w = p->img.w; e.h = p->img.h; e.isIcon = p->img.isIcon; e.bmp.Reset();
    }
    Invalidate(); SetHandled(true); return 0;
  }
  return DialogWindow::OnMessage(msg, wp, lp);
}

void ToolDialog::OnDeviceReset(bool rebuilt) {
  DialogWindow::OnDeviceReset(rebuilt);
  if (!rebuilt) for (auto& [k, e] : thumbs_) e.bmp.Reset();
}

bool ToolDialog::DrawJobPanel(Ui& ui, const Rect& area) {
  if (phase_ == JobPhase::Idle) return false;
  const Palette& pal = ui.Pal();
  Painter& p = ui.P();
  // soft scrim over the content so the dialog state stays visible but clearly inactive
  p.FillRect(area, Theme::Get().IsDark() ? Color(0, 0, 0, 0.28f) : Color(1, 1, 1, 0.45f));
  const float cw = std::min(area.w - 48, 380.f);
  const int extraLines = phase_ == JobPhase::Done ? (int)std::min<size_t>(result_.warnings.size(), 4) : 0;
  const float cardH = 220.f + (extraLines > 1 ? (extraLines - 1) * 18.f : 0.f);
  Rect card{area.cx() - cw / 2, area.cy() - cardH / 2, cw, cardH};
  p.Shadow(card, 18, 24, Color(0, 0, 0, 0.25f), 8);
  p.FillRoundRect(card, 18, Theme::Get().IsDark() ? Color::Hex(0x2A2B2F, 0.96f) : Color::Hex(0xFFFFFF, 0.92f));
  p.StrokeRoundRect(card, 18, pal.divider, 1.f);
  TextStyle title; title.size = 16; title.weight = 600; title.align = Align::Center;
  TextStyle sm; sm.size = 12; sm.align = Align::Center; sm.wrap = true; sm.ellipsis = false; sm.vcenter = false;
  ui.PushId(0x10B);
  if (phase_ == JobPhase::Running) {
    p.Text({card.x + 16, card.y + 22, card.w - 32, 26}, jobTitle_.empty() ? L"Working..." : jobTitle_, pal.text, title);
    ui.ProgressBar({card.x + 28, card.y + 66, card.w - 56, 8}, (float)progress_, progress_ <= 0.001);
    TextStyle pc; pc.size = 13; pc.align = Align::Center;
    wchar_t buf[32]; swprintf_s(buf, L"%d%%", (int)std::lround(progress_ * 100));
    p.Text({card.x + 16, card.y + 84, card.w - 32, 22}, progress_ > 0.001 ? std::wstring(buf) : L"Preparing...", pal.textSecondary, pc);
    if (!stage_.empty()) p.Text({card.x + 24, card.y + 110, card.w - 48, 36}, stage_, pal.textTertiary, sm);
    if (ui.Button({card.cx() - 55, card.bottom() - 54, 110, 30}, L"Cancel")) JobManager::Get().Cancel(jobId_);
  } else if (phase_ == JobPhase::Done) {
    p.FillCircle(card.cx(), card.y + 44, 22, accent::base.WithAlpha(0.18f));
    p.Icon("check", card.cx(), card.y + 44, 26, accent::base, 2.4f);
    p.Text({card.x + 16, card.y + 76, card.w - 32, 24}, L"Done", pal.text, title);
    std::wstring name = result_.outputs.empty() ? L"" : NameOf(result_.outputs[0]);
    if (result_.outputs.size() > 1) name = std::to_wstring(result_.outputs.size()) + L" items saved";
    TextStyle nm; nm.size = 13; nm.align = Align::Center;
    p.Text({card.x + 20, card.y + 102, card.w - 40, 22}, name, pal.textSecondary, nm);
    if (!result_.warnings.empty()) {
      std::wstring all;
      for (int i = 0; i < extraLines; ++i) { if (i) all += L"\n"; all += result_.warnings[i]; }
      if (result_.warnings.size() > 4) all += L"\n+" + std::to_wstring(result_.warnings.size() - 4);
      p.Text({card.x + 24, card.y + 124, card.w - 48, 18.f * (extraLines + 1)}, all, Color::Hex(0xB26A00), sm);
    }
    auto c = ui.Cols({card.x + 20, card.bottom() - 54, card.w - 40, 30}, {Flex(1), Flex(1), Flex(1)}, 8);
    if (ui.Button(c[0], L"Show in Folder") && !result_.outputs.empty()) Reveal(result_.outputs[0]);
    if (ui.Button(c[1], L"Edit Again")) ResetJob();
    if (ui.Button(c[2], L"Close", ButtonStyle::Primary)) { ResetJob(); OnCloseRequested(); }
  } else {
    p.FillCircle(card.cx(), card.y + 44, 22, Color::Hex(0xE5484D, 0.16f));
    p.Icon("warning", card.cx(), card.y + 44, 26, Color::Hex(0xE5484D), 2.f);
    p.Text({card.x + 16, card.y + 76, card.w - 32, 24}, L"Couldn't finish", pal.text, title);
    p.Text({card.x + 24, card.y + 104, card.w - 48, 60}, result_.error.empty() ? L"Something went wrong." : result_.error, pal.textSecondary, sm);
    auto c = ui.Cols({card.x + 20, card.bottom() - 54, card.w - 40, 30}, {Flex(1), Flex(1)}, 8);
    if (ui.Button(c[0], L"Back")) ResetJob();
    if (ui.Button(c[1], L"Close", ButtonStyle::Primary)) { ResetJob(); OnCloseRequested(); }
  }
  ui.PopId();
  return true;
}

void ToolDialog::Reveal(const fs::path& p) { shell::RevealInExplorer(p); }

// ------------------------------------------------------------------------------------------------ footer
Rect ToolDialog::FooterRect(const Rect& c) { return {c.x, c.bottom() - dialog::footerHeight, c.w, dialog::footerHeight}; }
Rect ToolDialog::BodyAbove(const Rect& c) { return {c.x, c.y, c.w, c.h - dialog::footerHeight}; }

ToolDialog::FooterResult ToolDialog::DrawFooter(Ui& ui, const Rect& content, const FooterInfo& f) {
  FooterResult r;
  Rect fr = FooterRect(content);
  ui.P().FillRect({fr.x, fr.y, fr.w, 1.f}, ui.Pal().divider);
  const float pad = dialog::contentInset;
  const float btnH = control::buttonHeight + 4;
  TextStyle ts; ts.size = 13; ts.weight = 500;
  const float aw = std::max(104.f, ui.P().TextWidth(f.action, ts) + 36);
  Rect action{fr.right() - pad - aw, fr.y + (fr.h - btnH) / 2, aw, btnH};
  Rect sec{};
  float rightEdge = action.x - 8;
  if (!f.secondary.empty()) {
    const float sw = std::max(84.f, ui.P().TextWidth(f.secondary, ts) + 32);
    sec = {action.x - 8 - sw, action.y, sw, btnH};
    rightEdge = sec.x - 12;
  }
  Rect statusRect{fr.x + pad, fr.y, std::max(0.f, rightEdge - fr.x - pad), fr.h};   // computed in LTR space, mirrored below
  if (ui.Rtl()) {   // mirror the whole footer
    action = ui.Mirror(action, fr); if (!f.secondary.empty()) sec = ui.Mirror(sec, fr);
    statusRect = ui.Mirror(statusRect, fr);
  }
  ui.PushId(0xF007);
  if (!f.status.empty()) {
    TextStyle st; st.size = 13; st.wrap = true; st.ellipsis = true;
    ui.P().Text(statusRect, f.status, f.statusIsError ? Color::Hex(0xC2410C) : ui.Pal().textSecondary, st);
  }
  if (!f.secondary.empty() && ui.Button(sec, f.secondary, ButtonStyle::Secondary, f.secondaryEnabled)) r.secondary = true;
  if (ui.Button(action, f.action, ButtonStyle::Primary, f.actionEnabled)) r.action = true;
  ui.PopId();
  return r;
}

// ------------------------------------------------------------------------------------------------ thumbnails
ID2D1Bitmap* ToolDialog::Thumb(const fs::path& p, int side) {
  const std::wstring key = p.wstring();
  auto& e = thumbs_[key];
  if (!e.requested) {
    e.requested = true;
    HWND hw = Hwnd();
    shell::ThumbnailService::Get().Request(p, side, (uint64_t)(uintptr_t)this, [hw, key](const shell::ThumbnailImage& img) {
      if (img.w <= 0) return;
      auto* pl = new ThumbPayload{key, img};
      if (!PostMessageW(hw, WM_FILO_TOOL_THUMB, 0, (LPARAM)pl)) delete pl;
    });
  }
  if (!e.bmp && !e.px.empty() && dc_) {
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(e.w, e.h), e.px.data(), e.w * 4, &bp, &b))) e.bmp = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
  }
  return e.bmp.Get();
}

void ToolDialog::SetThumbOverride(const fs::path& p, int w, int h, const uint8_t* px) {
  auto& e = thumbs_[p.wstring()];
  e.requested = true; e.w = w; e.h = h; e.px.assign(px, px + (size_t)w * h * 4); e.bmp.Reset();
  Invalidate();
}

} // namespace filo::tools
