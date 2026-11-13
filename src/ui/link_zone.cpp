#include "ui/link_zone.h"
#include "core/settings.h"
#include "shell/explorer.h"
#include <shellscalingapi.h>
#include <cmath>

namespace filo::ui {

using namespace filo::ds;

namespace {
constexpr float kW = 330.f, kH = 116.f;
constexpr UINT_PTR kTimerGone = 0xB0D2;
}

LinkZone* LinkZone::Create(POINT at, bool show) {
  auto* z = new LinkZone();
  DialogOptions o;
  o.title = L"Filo"; o.widthDip = kW; o.heightDip = kH; o.showTitleBar = false; o.topmost = true; o.activate = false; o.cornerRadius = 26.f;
  o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
  z->onClosed = [z] { if (z->onGone) z->onGone(); delete z; };
  if (!z->Open(o, at, show)) { delete z; return nullptr; }
  return z;
}

void LinkZone::Dismiss() {
  if (dismissing_) return;
  dismissing_ = true;
  CloseWindow();
}

bool LinkZone::AcceptsRawDrop(IDataObject* d) {
  std::wstring url, platform;
  if (!classify) return false;
  if (!classify(shell::LinkTextsFromDataObject(d), &url, &platform)) return false;
  url_ = url; platform_ = platform;
  return true;
}

void LinkZone::OnRawDragState(bool over, bool acceptable) {
  state_ = !over ? 0 : (acceptable ? 1 : 2);
  Invalidate();
}

void LinkZone::OnRawDrop(IDataObject*) {
  const std::wstring url = url_;
  if (!url.empty() && onDrop) onDrop(url);
}

void LinkZone::Draw(Ui& ui, const Rect&, const Rect& body) {
  const Palette& pal = ui.Pal(); Painter& p = ui.P();
  const bool rtl = ui.Rtl();
  // drop target ring: fills with the accent while a link hovers
  const float hot = ui.Anim(ui.Id("zone-hot"), state_ == 1 ? 1.f : 0.f, 0.14f);
  const Rect ring = body.Inset(10);
  p.StrokeRoundRect(ring, 18, Color(accent::base.r, accent::base.g, accent::base.b, 0.35f + 0.5f * hot), 2.f);
  if (hot > 0.01f) p.FillRoundRect(ring, 18, Color(accent::base.r, accent::base.g, accent::base.b, 0.10f * hot));

  // icon bubble
  const float iconD = 56.f;
  const float ix = rtl ? body.right() - 26 - iconD : body.x + 26;
  const Rect bub{ix, body.y + (body.h - iconD) / 2, iconD, iconD};
  p.FillRoundRect(bub, iconD / 2, state_ == 2 ? Color::Hex(0xB3261E) : accent::base);
  DrawDownloadGlyph(p, bub.x + iconD / 2, bub.y + iconD / 2, 28.f, Color(1, 1, 1, 1), 2.4f);

  TextStyle ts; ts.size = 19.f; ts.weight = 700; ts.align = rtl ? Align::End : Align::Start;
  TextStyle ss; ss.size = 14.f; ss.align = rtl ? Align::End : Align::Start; ss.wrap = true;
  const float tx = rtl ? body.x + 22 : bub.right() + 16, tw = rtl ? bub.x - 16 - (body.x + 22) : body.right() - 22 - tx;
  std::wstring title = L"Drop to download", sub = L"Release the link here";
  if (state_ == 1) { title = platform_.empty() ? L"Download this link" : L"Download from " + platform_; sub = L"Release to choose what to save"; }
  else if (state_ == 2) { title = L"Not a link"; sub = L"Drag a link from your browser"; }
  p.Text({tx, body.y + 28, tw, 28}, title, state_ == 2 ? Color::Hex(0xB3261E) : pal.text, ts);
  p.Text({tx, body.y + 58, tw, 36}, sub, pal.textSecondary, ss);
}

}  // namespace filo::ui
