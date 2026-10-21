#include "ui/gallery.h"
#include <cstdio>

namespace filo::ui {

using namespace filo::ds;

GalleryDialog* GalleryDialog::Show(POINT at, bool rtl, bool show) {
  auto* d = new GalleryDialog();
  DialogOptions o;
  o.title = rtl ? L"معرض العناصر" : L"Widget Gallery";
  o.widthDip = 600; o.heightDip = 760; o.rtl = rtl; o.resizable = true; o.topmost = true;
  d->onClosed = [d] { delete d; };
  if (!d->Open(o, at, show)) { delete d; return nullptr; }
  return d;
}

static std::wstring Fmt(float v) { wchar_t b[32]; swprintf_s(b, L"%+d", (int)std::lround(v)); return b; }

void GalleryDialog::Draw(Ui& ui, const Rect& content, const Rect& body) {
  const float pad = dialog::contentInset;
  Rect area = content.Inset(pad, 14);
  ui.BeginColumn(area, 8);
  // preview well (dark, like Edit Photo)
  Rect well = ui.Take(180);
  ui.P().FillRoundRect(well, 14, ui.Pal().darkCanvas);
  ui.P().FillRoundRect(well.Inset(46, 14), 4, Color::Hex(0x4C8FD8));
  TextStyle zs; zs.size = 12;
  ui.P().Text({well.x + 14, well.y + 8, 120, 22}, L"100%", Color(1, 1, 1, 0.35f), zs);
  ui.Space(4);

  ui.SectionTitle(ui.Take(20), L"Light");
  const wchar_t* names[] = {L"Exposure", L"Brightness", L"Contrast"};
  for (int i = 0; i < 3; ++i) {
    ui.PushId(i);
    if (i == 0) ui.SliderRow(ui.Take(27), names[i], v_[i], -5, 5, 0.01f, 0, [](float v) { wchar_t b[32]; swprintf_s(b, L"%.2f EV", v); return std::wstring(b); });
    else ui.SliderRow(ui.Take(27), names[i], v_[i], -100, 100, 1, 0, Fmt);
    ui.PopId();
  }
  ui.Space(6);
  ui.Divider(ui.Take(1));

  // Add BG style block
  {
    Rect row = ui.Take(32 * 2 + 10);
    auto c = ui.Cols(row, {Fixed(92), Flex(1)}, 8);
    ui.Label({c[0].x, c[0].y, c[0].w, 32}, L"Background");
    const Color tops[] = {Color::Hex(0x7FD6F7), Color::Hex(0xE6E0FF), Color::Hex(0xFFFFFF), Color::Hex(0xE8F4FF), Color::Hex(0x0E2C38), Color::Hex(0xFF9A8B), Color::Hex(0x5DA9FF), Color::Hex(0x3AC3B0)};
    const Color bots[] = {Color::Hex(0x38C6E8), Color::Hex(0xB9B7FF), Color::Hex(0xFFF0F5), Color::Hex(0x69C6FF), Color::Hex(0x0B6C63), Color::Hex(0xFF5E7E), Color::Hex(0x4B6FE0), Color::Hex(0x22A08F)};
    ui.PushId(77);
    for (int i = 0; i < 8; ++i) {
      ui.PushId(i);
      Rect r{c[1].x + i * 40.f, c[1].y, control::swatch, control::swatch};
      if (ui.Swatch(ui.Mirror(r, c[1]), tops[i], bots[i], swatch_ == i)) swatch_ = i;
      ui.PopId();
    }
    ui.PopId();
  }
  ui.Space(4);
  ui.SliderRow(ui.Take(27), L"Padding", v_[3], 0, 400, 1, 0, [](float v) { return std::to_wstring((int)v) + L" px"; });
  ui.SliderRow(ui.Take(27), L"Corners", v_[4], 0, 100, 1, 24, [](float v) { return std::to_wstring((int)v) + L" px"; });
  {
    Rect row = ui.Take(30);
    auto c = ui.Cols(row, {Fixed(92), Flex(1)}, 8);
    ui.Label(c[0], L"Ratio");
    ratio_ = ui.Segmented(c[1], {L"Auto", L"1:1", L"16:9", L"4:5"}, ratio_);
  }
  {
    Rect row = ui.Take(30);
    auto c = ui.Cols(row, {Fixed(92), Flex(1), Fixed(130)}, 8);
    ui.Label(c[0], L"File name");
    ui.TextInput(c[1], text_, L"Name");
    ui.Dropdown(c[2], {L"PNG", L"JPEG", L"WebP"}, drop_);
  }
  {
    Rect row = ui.Take(30);
    auto c = ui.Cols(row, {Fixed(92), Fixed(130), Flex(1)}, 8);
    ui.Label(c[0], L"Size");
    ui.Stepper(c[1], step_, 1, 400, 1, 0, L"px");
    ui.Checkbox(c[2], L"Trim silent ends automatically", chk_);
  }
  {
    Rect row = ui.Take(26);
    auto c = ui.Cols(row, {Fixed(92), Fixed(60), Flex(1)}, 8);
    ui.Label(c[0], L"Option");
    ui.Toggle(c[1], tog_);
    ui.Segmented(c[2], {L"Mono", L"Stereo"}, seg_);
  }
  ui.ProgressBar(ui.Take(8), progress_);
  ui.Space(6);
  {
    Rect row = ui.Take(30);
    auto c = ui.Cols(row, {Fixed(90), Fixed(90), Fixed(100), Fixed(100), Flex(1)}, 8);
    ui.Button(c[0], L"Reset");
    ui.Button(c[1], L"Disabled", ButtonStyle::Secondary, false);
    ui.Button(c[2], L"Play", ButtonStyle::Secondary, true, "play");
    if (ui.Button(c[3], L"Danger", ButtonStyle::Danger)) {}
    ui.Button(c[4], L"Save with Background", ButtonStyle::Primary);
  }
  ui.EndColumn();
}

} // namespace filo::ui
