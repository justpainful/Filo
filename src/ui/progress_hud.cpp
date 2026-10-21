#include "ui/progress_hud.h"
#include "core/settings.h"
#include "shell/actions.h"
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

namespace filo::ui {

using namespace filo::ds;

namespace {
constexpr float kW = 440.f, kH = 124.f;
constexpr UINT_PTR kTimerDismiss = 0xB0D1;
}

ProgressHud* ProgressHud::Create(POINT monitorPoint, bool show) {
  auto* h = new ProgressHud();
  DialogOptions o;
  o.title = L"Filo"; o.widthDip = kW; o.heightDip = kH; o.showTitleBar = false; o.topmost = true; o.activate = false; o.cornerRadius = 24.f;
  o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
  // top centre of the monitor's work area (Open() centres the body on the anchor and clamps it)
  MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromPoint(monitorPoint, MONITOR_DEFAULTTONEAREST), &mi);
  const float sc = [&] { if (const wchar_t* e = _wgetenv(L"FILO_TEST_DPI")) return (float)_wtof(e) / 96.f; UINT dx = 96, dy = 96; GetDpiForMonitor(MonitorFromPoint(monitorPoint, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy); return dx / 96.f; }();
  POINT anchor{(mi.rcWork.left + mi.rcWork.right) / 2, mi.rcWork.top + (int)std::lround((26 + kH / 2) * sc)};
  h->onClosed = [h] { if (h->onGone) h->onGone(); delete h; };
  if (!h->Open(o, anchor, show)) { delete h; return nullptr; }
  return h;
}

void ProgressHud::OnOpened() {}

void ProgressHud::Finish(State s, const std::wstring& title, const std::wstring& subtitle, const std::filesystem::path& reveal) {
  state_ = s; titleText_ = title; sub_ = subtitle; reveal_ = reveal;
  if (s == State::Done) progress_ = 1.f;
  if (Valid()) SetTimer(Hwnd(), kTimerDismiss, s == State::Done ? 2600 : 7000, nullptr);
  Invalidate();
}

void ProgressHud::OnCloseRequested() {
  if (state_ == State::Running && onCancel) onCancel();
  CloseWindow();
}

LRESULT ProgressHud::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TIMER && wp == kTimerDismiss) { KillTimer(Hwnd(), kTimerDismiss); CloseWindow(); return 0; }
  return DialogWindow::OnMessage(msg, wp, lp);
}

void ProgressHud::Draw(Ui& ui, const Rect&, const Rect& body) {
  const Palette& pal = ui.Pal(); Painter& p = ui.P();
  const bool rtl = ui.Rtl();
  auto X = [&](float x, float w) { return rtl ? body.right() - (x - body.x) - w : x; };
  ui.PushId(0x4D0D);
  // close / cancel button: the same 28 dip glass circle as every other window, always at the top-left (also in RTL layouts)
  Rect cb{body.x + dialog::closeInset, body.y + 16, 28, 28};
  if (ui.CloseButton(cb, true, true)) { ui.PopId(); OnCloseRequested(); return; }
  TextStyle ts; ts.size = 19.f; ts.weight = 700; ts.align = rtl ? Align::End : Align::Start;
  const float tl = body.x + dialog::closeInset + 28 + 12, tr = body.right() - 18;
  p.Text({tl, body.y + 14, tr - tl, 32}, titleText_, state_ == State::Failed ? Color::Hex(0xB3261E) : pal.text, ts);
  TextStyle ss; ss.size = 14.5f; ss.align = rtl ? Align::End : Align::Start; ss.wrap = state_ == State::Failed; ss.vcenter = false;
  p.Text({X(body.x + 20, body.w - 40), body.y + 58, body.w - 40, state_ == State::Failed ? 44.f : 24.f}, sub_, pal.textSecondary, ss);
  // progress bar: grey track, accent fill that eases toward the real value; indeterminate shimmer before the first update
  if (state_ != State::Failed) {
    Rect track{body.x + 20, body.bottom() - 30, body.w - 40, 6};
    p.FillRoundRect(track, 3, pal.trackInactive);
    if (progress_ > 0.001f || state_ == State::Done) {
      shown_ += (progress_ - shown_) * 0.25f;
      if (std::fabs(progress_ - shown_) > 0.002f) ui.RequestFrame(); else shown_ = progress_;
      Rect fill = track; fill.w = std::max(6.f, track.w * shown_);
      p.FillRoundRect(X(fill.x, fill.w) == fill.x ? fill : Rect{track.right() - fill.w, fill.y, fill.w, fill.h}, 3, state_ == State::Done ? Color::Hex(0x2FA84F) : accent::base);
    } else {
      const float t = (float)std::fmod(ui.Now() * 0.9, 1.0);
      Rect seg{track.x + (track.w + 80) * t - 80, track.y, 80, track.h};
      p.PushClip(track); p.FillRoundRect(seg, 3, accent::base); p.PopClip();
    }
  }
  // clicking a finished HUD shows the result in Explorer
  if (state_ == State::Done && !reveal_.empty()) {
    Interaction it = ui.Behavior({body.x + 56, body.y, body.w - 56, body.h}, ui.Id("reveal"), true, false);
    if (it.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
    if (it.clicked) { shell::RevealInExplorer(reveal_); ui.PopId(); CloseWindow(); return; }
  }
  ui.PopId();
}

}  // namespace filo::ui
