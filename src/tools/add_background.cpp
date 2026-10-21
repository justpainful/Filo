// Add a Background - layout measured from reference/add-background.png (573 dip wide): translucent preview well with the picture on
// a styled canvas, a "Background" label with two scrolling swatch rows (+ add-image button and custom-colour pill), slider rows
// Padding / Corners / Shadow, segmented Ratio, footer Reset | Save with Background.
#include "tools/tools.h"
#include "tools/image_compose.h"
#include "core/output.h"
#include "engines/image/bitmap.h"
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

struct Preset { Color top, bottom; const wchar_t* name; };
const Preset kPresets[] = {
  {Color::Hex(0x7FEAF6), Color::Hex(0x2BB8DB), L"Aqua"},     {Color::Hex(0xFFFFFF), Color::Hex(0xB4B9FF), L"Periwinkle"},
  {Color::Hex(0xFFFFFF), Color::Hex(0xFFE6F0), L"Blush"},    {Color::Hex(0xFFFFFF), Color::Hex(0x7FD2FF), L"Sky"},
  {Color::Hex(0x0C2B34), Color::Hex(0x0B7A68), L"Deep Sea"}, {Color::Hex(0xFFA070), Color::Hex(0xFF5A82), L"Sunset"},
  {Color::Hex(0x66B2FF), Color::Hex(0x4B6BE6), L"Ocean"},    {Color::Hex(0x45D1B8), Color::Hex(0x1FA08C), L"Mint"},
  {Color::Hex(0xB06BFF), Color::Hex(0x6F49D8), L"Violet"},   {Color::Hex(0x3F6FE8), Color::Hex(0x3F6FE8), L"Blue"},
  {Color::Hex(0x27395A), Color::Hex(0x27395A), L"Navy"},     {Color::Hex(0x7A4FC8), Color::Hex(0x7A4FC8), L"Purple"},
  {Color::Hex(0xA78FDB), Color::Hex(0xA78FDB), L"Lavender"}, {Color::Hex(0xDB5E8B), Color::Hex(0xDB5E8B), L"Pink"},
  {Color::Hex(0xD8383A), Color::Hex(0xD8383A), L"Red"},      {Color::Hex(0xF26B5C), Color::Hex(0xF26B5C), L"Coral"},
  {Color::Hex(0xF28E26), Color::Hex(0xF28E26), L"Orange"},   {Color::Hex(0x7A4A32), Color::Hex(0x7A4A32), L"Brown"},
  {Color::Hex(0x6E7B8C), Color::Hex(0x6E7B8C), L"Slate"},    {Color::Hex(0x1C1C1E), Color::Hex(0x1C1C1E), L"Black"},
};
constexpr int kPresetCount = (int)(sizeof kPresets / sizeof *kPresets);
const wchar_t* const kRatios[] = {L"Auto", L"1:1", L"16:9", L"4:5"};

Color HsvToRgb(float h, float s, float v) {
  h = std::fmod(std::max(h, 0.f), 360.f) / 60.f;
  const int i = (int)h; const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r = v, g = t, b = p;
  switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                   case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
  return Color(r, g, b, 1);
}

class AddBackgroundDialog : public ToolDialog {
 public:
  explicit AddBackgroundDialog(const Selection& sel) {
    selection_ = sel;
    path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
    img::LoadOptions lo; lo.maxSide = 1400;
    if (!img::LoadBitmap(path_, src_, lo, &loadError_)) { src_ = {}; if (loadError_.empty()) loadError_ = L"This image cannot be opened."; }
    if (!src_.Empty()) {
      const int big = std::max(src_.info.fullWidth, src_.info.fullHeight);
      params_.padding = std::round(big * 0.10f);
      maxPad_ = std::max(400.f, big * 0.6f);
      ApplySwatch();
    }
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Add BG"; o.widthDip = 573; o.heightDip = 870; o.resizable = true; o.minWidthDip = 480; o.minHeightDip = 600;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "add-background"; }
  Json SavePreset() const override {
    Json j = Json::MakeObject();
    j["swatch"] = swatch_; j["corners"] = params_.corners; j["shadow"] = params_.shadow; j["ratio"] = params_.ratio;
    return j;
  }
  void LoadPreset(const Json& j) override {
    if (j.Has("swatch")) { swatch_ = std::clamp((int)j.Get("swatch").AsInt(), -2, kPresetCount - 1); ApplySwatch(); }
    if (j.Has("corners")) params_.corners = (float)j.Get("corners").AsNumber();
    if (j.Has("shadow")) params_.shadow = (float)j.Get("shadow").AsNumber();
    if (j.Has("padding")) params_.padding = (float)j.Get("padding").AsNumber();
    if (j.Has("ratio")) params_.ratio = std::clamp((int)j.Get("ratio").AsInt(), 0, 3);
    dirty_ = true;
  }
  void TestApply() override { Save(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = 16.f, inset = 24.f;
    const float controlsH = 79 + 18 + 3 * 35 + 12 + 30 + 18;
    Rect well{body.x + pad, body.y + 14, body.w - 2 * pad, std::max(160.f, body.h - 14 - controlsH - 8)};
    DrawWell(ui, well);

    float y = well.bottom() + 22;
    const Rect inner{body.x + inset, y, body.w - 2 * inset, 0};
    bool changed = false;
    if (pickerOpen_) {
      DrawPicker(ui, Rect{inner.x, y, inner.w, controlsH - 12}, changed);
    } else {
      // ---- Background label + swatch strip + actions
      Rect rowA{inner.x, y, inner.w, 34}, rowB{inner.x, y + 45, inner.w, 34};
      const float labelW = 92, actionW = 66;
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(ui.Mirror({inner.x, y, labelW, 34}, Rect{inner.x, y, inner.w, 34}), L"Background", pal.text, ls);
      Rect strip{inner.x + labelW + 8, y - 4, inner.w - labelW - 8 - actionW - 10, 79 + 8};
      strip = ui.Rtl() ? Rect{inner.x + actionW + 10, strip.y, strip.w, strip.h} : strip;
      DrawStrip(ui, strip, changed);
      const float ax = ui.Rtl() ? inner.x : inner.right() - actionW;
      Rect addImg{ax + (actionW - 34) / 2, y, 34, 34};
      ui.PushId(0xADD1);
      if (ui.IconButton(addImg, "image", true, L"Use an image as the background", true)) ChooseBackgroundImage(changed);
      ui.PopId();
      Rect pill{ax + 2, y + 45 + 3, actionW - 4, 28};
      ui.PushId(0xC0C0);
      Interaction it = ui.Behavior(pill, ui.Id("pill"), true, true);
      if (it.hot) ui.SetCursor((int)(intptr_t)IDC_HAND);
      p.FillRoundRect(pill, 14, Mix(pal.controlFill, accent::base.WithAlpha(0.20f), ui.Anim(ui.Id("ph"), it.hot ? 1.f : 0.f)));
      p.FillRoundRect(pill.Inset(3), 11, custom_);
      if (swatch_ == -1) p.StrokeRoundRect(pill.Inset(-3), 17, accent::base, 2.5f);
      ui.Tooltip(L"Custom colour");
      if (it.clicked) { pickerOpen_ = true; }
      ui.PopId();
      y += 79 + 18;

      auto fmtPx = [](float v) { return std::to_wstring((int)std::lround(v)) + L" px"; };
      const float defPad = std::round(std::max(src_.info.fullWidth, src_.info.fullHeight) * 0.10f);
      float v = params_.padding;
      if (ui.SliderRow({inner.x, y, inner.w, 30}, L"Padding", v, 0, maxPad_, 1, defPad, fmtPx, !src_.Empty(), 101.f, 70.f)) { params_.padding = v; changed = true; }
      y += 35;
      v = params_.corners;
      if (ui.SliderRow({inner.x, y, inner.w, 30}, L"Corners", v, 0, 120, 1, 24, fmtPx, !src_.Empty(), 101.f, 70.f)) { params_.corners = v; changed = true; }
      y += 35;
      v = params_.shadow;
      if (ui.SliderRow({inner.x, y, inner.w, 30}, L"Shadow", v, 0, 100, 1, 30, fmtPx, !src_.Empty(), 101.f, 70.f)) { params_.shadow = v; changed = true; }
      y += 35 + 12;
      {
        Rect row{inner.x, y, inner.w, 30};
        auto c = ui.Cols(row, {Fixed(101), Flex(1), Fixed(236)}, 8);
        p.Text(c[0], L"Ratio", pal.text, ls);
        int r = ui.Segmented(c[2], std::vector<std::wstring>(kRatios, kRatios + 4), params_.ratio, !src_.Empty());
        if (r != params_.ratio) { params_.ratio = r; changed = true; }
      }
    }
    if (changed) dirty_ = true;

    FooterInfo fi;
    fi.action = L"Save with Background"; fi.actionEnabled = !src_.Empty() && !Busy();
    fi.secondary = L"Reset";
    fi.status = !loadError_.empty() ? loadError_ : L"";
    fi.statusIsError = !loadError_.empty();
    FooterResult fr = DrawFooter(ui, content, fi);
    if (fr.secondary) { Reset(); }
    if (fr.action) Save();
    DrawJobPanel(ui, content);
  }

 private:
  void ApplySwatch() {
    params_.bg.image = nullptr;
    if (swatch_ >= 0 && swatch_ < kPresetCount) {
      params_.bg.kind = kPresets[swatch_].top.r == kPresets[swatch_].bottom.r && kPresets[swatch_].top.g == kPresets[swatch_].bottom.g && kPresets[swatch_].top.b == kPresets[swatch_].bottom.b ? BgSpec::Kind::Solid : BgSpec::Kind::Gradient;
      params_.bg.top = kPresets[swatch_].top; params_.bg.bottom = kPresets[swatch_].bottom;
    } else if (swatch_ == -1) { params_.bg.kind = BgSpec::Kind::Solid; params_.bg.top = custom_; params_.bg.bottom = custom_; }
    else if (swatch_ == -2 && !bgImage_.Empty()) { params_.bg.kind = BgSpec::Kind::Image; params_.bg.image = &bgImage_; }
    dirty_ = true;
  }

  void Reset() {
    swatch_ = 3; custom_ = Color::Hex(0x3068D4);
    const int big = std::max(src_.info.fullWidth, src_.info.fullHeight);
    params_.padding = std::round(big * 0.10f); params_.corners = 24; params_.shadow = 30; params_.ratio = 0;
    ApplySwatch();
  }

  void ChooseBackgroundImage(bool& changed) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreate(CLSID_FileOpenDialog, dlg))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.avif"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(L"Choose a background image");
    if (FAILED(dlg->Show(Hwnd()))) return;
    ComPtr<IShellItem> it;
    if (FAILED(dlg->GetResult(&it))) return;
    PWSTR p = nullptr;
    if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return;
    fs::path bp = p; CoTaskMemFree(p);
    img::LoadOptions lo; lo.maxSide = 1600;
    std::wstring err;
    if (img::LoadBitmap(bp, bgImage_, lo, &err)) { swatch_ = -2; ApplySwatch(); changed = true; bgImagePath_ = bp; }
  }

  void DrawStrip(Ui& ui, const Rect& strip, bool& changed) {
    Painter& p = ui.P();
    const float pitch = 43, sw = control::swatch + 2, rowGap = 45;
    const float contentW = std::ceil(kPresetCount / 2.f) * pitch + 4;
    const float maxOff = std::max(0.f, contentW - strip.w + 10);
    const InputState& in = ui.In();
    if (in.inWindow && strip.Contains(in.mx, in.my) && in.wheel != 0) { stripOff_ = std::clamp(stripOff_ - in.wheel * 60.f, 0.f, maxOff); ui.RequestFrame(); }
    // drag to scroll on the background of the strip
    p.PushClip(strip);
    ui.PushId(0x57210);
    for (int i = 0; i < kPresetCount; ++i) {
      const int col = i / 2, row = i % 2;
      Rect r{strip.x + 4 + col * pitch - stripOff_ + 2, strip.y + 4 + row * rowGap, sw, sw};
      if (r.right() < strip.x - 2 || r.x > strip.right() + 2) continue;
      ui.PushId(i);
      if (ui.Swatch(r, kPresets[i].top, kPresets[i].bottom, swatch_ == i, kPresets[i].name)) { swatch_ = i; ApplySwatch(); changed = true; }
      ui.PopId();
    }
    ui.PopId();
    p.PopClip();
    // slim scroll hint: arrows appear when more swatches exist off-screen
    if (stripOff_ > 1 && ui.IconButton({strip.x - 2, strip.cy() - 11, 22, 22}, "chevronleft", true, L"", true)) stripOff_ = std::max(0.f, stripOff_ - pitch * 3);
    if (stripOff_ < maxOff - 1 && ui.IconButton({strip.right() - 20, strip.cy() - 11, 22, 22}, "chevronright", true, L"", true)) stripOff_ = std::min(maxOff, stripOff_ + pitch * 3);
  }

  void DrawPicker(Ui& ui, const Rect& area, bool& changed) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    TextStyle ls; ls.size = dialog::bodyFontSize;
    p.Text({area.x, area.y, area.w, 24}, L"Custom colour", pal.heading, [] { TextStyle t; t.size = dialog::bodyFontSize; t.weight = 700; return t; }());
    ui.PushId(0x91C4);
    float h = hue_, s = sat_, v = val_;
    Rect r1{area.x, area.y + 36, area.w, 30};
    bool ch = false;
    ch |= ui.SliderRow(r1, L"Hue", h, 0, 359, 1, 210, [](float x) { return std::to_wstring((int)x) + L"\x00B0"; }, true, 101.f, 56.f);
    ch |= ui.SliderRow({area.x, area.y + 71, area.w, 30}, L"Saturation", s, 0, 1, 0.01f, 0.7f, [](float x) { return std::to_wstring((int)std::lround(x * 100)) + L"%"; }, true, 101.f, 56.f);
    ch |= ui.SliderRow({area.x, area.y + 106, area.w, 30}, L"Brightness", v, 0, 1, 0.01f, 0.85f, [](float x) { return std::to_wstring((int)std::lround(x * 100)) + L"%"; }, true, 101.f, 56.f);
    if (ch) { hue_ = h; sat_ = s; val_ = v; custom_ = HsvToRgb(hue_, sat_, val_); swatch_ = -1; ApplySwatch(); changed = true; }
    Rect prev{area.x, area.y + 148, 64, 30};
    p.FillRoundRect(prev, 10, custom_);
    p.StrokeRoundRect(prev, 10, pal.divider, 1.f);
    if (ui.Button({area.right() - 96, area.y + 148, 96, 30}, L"Done", ButtonStyle::Primary)) pickerOpen_ = false;
    ui.PopId();
  }

  void RebuildPreview() {
    if (src_.Empty()) return;
    int cw, ch;
    ComputeCanvasSize(src_.info.fullWidth > 0 ? src_.info.fullWidth : src_.width, src_.info.fullHeight > 0 ? src_.info.fullHeight : src_.height, params_, cw, ch);
    const float k = std::min(1.f, 900.f / (float)std::max(cw, ch));
    img::Bitmap out;
    // the preview source is already downscaled (maxSide 1400): scale lengths relative to FULL size so shapes keep their proportions
    if (ComposeAddBackground(src_, params_, k, out)) {
      for (size_t i = 0; i < (size_t)out.width * out.height; ++i) { uint8_t* q = &out.px[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
      preview_ = std::move(out); bmp_.Reset();
    }
    dirty_ = false;
  }

  void DrawWell(Ui& ui, const Rect& well) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    p.FillRoundRect(well, 22, Color(pal.text.r, pal.text.g, pal.text.b, 0.075f));
    if (src_.Empty()) {
      TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true;
      p.Text(well.Inset(30), loadError_, pal.textSecondary, es);
      return;
    }
    if (dirty_) RebuildPreview();
    if (!bmp_ && dc_ && !preview_.Empty()) {
      D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
      ComPtr<ID2D1Bitmap1> b;
      if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(preview_.width, preview_.height), preview_.px.data(), preview_.width * 4, &bp, &b))) bmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
    }
    if (bmp_) {
      Rect avail = well.Inset(28);
      const float k = std::min(avail.w / preview_.width, avail.h / preview_.height);
      Rect dst{well.cx() - preview_.width * k / 2, well.cy() - preview_.height * k / 2, preview_.width * k, preview_.height * k};
      p.Shadow(dst, 16, 14, Color(0, 0, 0, 0.18f), 4);
      p.Bitmap(bmp_.Get(), dst, 1.f, 16.f * std::min(1.f, dst.w / std::max(1.f, (float)preview_.width)));
    }
  }

  void Save() {
    if (src_.Empty() || Busy()) return;
    AddBgParams prm = params_;
    fs::path in = path_, bgPath = bgImagePath_;
    const int bgKind = swatch_;
    std::wstring fmt = src_.info.format;
    if (fmt != L"jpg" && fmt != L"png" && fmt != L"webp" && fmt != L"tiff" && fmt != L"bmp") fmt = L"png";
    if (!img::CanEncode(fmt)) fmt = L"png";
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(in, fmt, st, L" (background)");
    RunJob(L"Adding the background...", [in, prm, bgPath, bgKind, fmt, desired, st](JobContext& ctx) mutable -> JobResult {
      img::Bitmap full, bgBmp; std::wstring err;
      img::LoadOptions lo; lo.ctx = &ctx;
      if (!img::LoadBitmap(in, full, lo, &err)) return JobResult::Fail(err.empty() ? L"The image could not be opened." : err);
      ctx.SetProgress(0.25);
      if (bgKind == -2 && !bgPath.empty()) {
        if (!img::LoadBitmap(bgPath, bgBmp, lo, &err)) return JobResult::Fail(L"The background image could not be opened.");
        prm.bg.image = &bgBmp;
      }
      img::Bitmap out;
      if (!ComposeAddBackground(full, prm, 1.f, out)) return JobResult::Fail(L"Could not compose the image.");
      if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
      ctx.SetProgress(0.7);
      out.info.meta = full.info.meta;
      OutputTransaction tx(desired, st.overwritePolicy);
      img::SaveOptions so; so.quality = st.imageQuality; so.preserveMetadata = false; so.ctx = &ctx;
      img::SaveResult sr = img::SaveBitmap(tx.TempPath(), out, fmt, so);
      if (!sr.ok) return JobResult::Fail(sr.error);
      std::wstring e;
      if (!tx.Commit(&e)) return JobResult::Fail(e);
      JobResult r = JobResult::Ok({tx.FinalPath()});
      r.warnings = sr.warnings;
      return r;
    });
  }

  fs::path path_, bgImagePath_;
  img::Bitmap src_, bgImage_, preview_;
  std::wstring loadError_;
  AddBgParams params_;
  float maxPad_ = 400;
  int swatch_ = 3;
  Color custom_ = Color::Hex(0x3068D4);
  float hue_ = 215, sat_ = 0.75f, val_ = 0.83f;
  bool pickerOpen_ = false, dirty_ = true;
  float stripOff_ = 0;
  ComPtr<ID2D1Bitmap> bmp_;
};

} // namespace

ToolDialog* MakeAddBackground(const Selection& sel) { return new AddBackgroundDialog(sel); }

} // namespace filo::tools
