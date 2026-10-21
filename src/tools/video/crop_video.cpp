// Crop Video: preview with a draggable / resizable crop rectangle (video pixels), aspect presets, exact size fields.
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

constexpr media::AspectPreset kAspects[] = {media::AspectPreset::Free, media::AspectPreset::Square1x1, media::AspectPreset::R4x3, media::AspectPreset::R3x4,
                                            media::AspectPreset::R16x9, media::AspectPreset::R9x16, media::AspectPreset::R4x5};
constexpr double kRatios[] = {0, 1.0, 4.0 / 3.0, 3.0 / 4.0, 16.0 / 9.0, 9.0 / 16.0, 4.0 / 5.0};
constexpr int kCrfs[] = {18, 0, 28};          // Best / Balanced (Settings default) / Smaller
constexpr float kMinCrop = 16.f;

class CropVideoDialog : public VideoToolDialog {
 public:
  explicit CropVideoDialog(const Selection& sel) : VideoToolDialog(sel) { ResetRect(); result_ = std::make_shared<media::CropResult>(); }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Crop Video"; o.widthDip = 648; o.heightDip = 760; o.resizable = true; o.minWidthDip = 560; o.minHeightDip = 640;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "crop-video"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["quality"] = quality_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("quality")) quality_ = std::clamp((int)j.Get("quality").AsInt(1), 0, 2);
    if (j.Has("aspect")) aspect_ = std::clamp((int)j.Get("aspect").AsInt(0), 0, 6);
    if (j.Has("w") && j.Has("h")) {
      FRect r{(float)j.Get("x").AsNumber(0), (float)j.Get("y").AsNumber(0), (float)j.Get("w").AsNumber(), (float)j.Get("h").AsNumber()};
      crop_ = Quantize(r, SrcW(), SrcH());
    }
  }
  void TestApply() override { StartCrop(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float below = 10 + 34 + 2 + 34 + 10 + 28 + 10 + 30 + 10 + 28 + 8;
    const float wellH = std::max(170.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    EditCrop(ui, g);
    SinkOptions so; so.clickToggles = false; so.arrowsStep = false;
    DrawWellSink(ui, g, so);
    if (Usable()) { Nudge(ui); DrawOverlay(ui, g); }

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, -1, 0);
    y += 34 + 2;
    DrawScrubber(ui, {well.x + 10, y, well.w - 20, 34});
    y += 34 + 10;

    // ---- aspect presets
    {
      Rect row{well.x, y, well.w, 28};
      auto c = ui.Cols(row, {Fixed(64), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Aspect", pal.text, ls);
      const int ns = ui.Segmented(c[1], {L"Free", L"1:1", L"4:3", L"3:4", L"16:9", L"9:16", L"4:5"}, aspect_, Usable() && !Busy());
      if (ns != aspect_) { aspect_ = ns; ApplyAspect(); }
    }
    y += 28 + 10;

    // ---- exact size / position
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(64), Fixed(98), Fixed(14), Fixed(98), Flex(1), Fixed(16), Fixed(92), Fixed(16), Fixed(92)}, 6);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      TextStyle mid; mid.size = 13; mid.align = Align::Center;
      TextStyle sm; sm.size = 13; sm.align = Align::End;
      const int W = SrcW(), H = SrcH();
      p.Text(c[0], L"Size", pal.text, ls);
      p.Text(c[2], L"\x00D7", pal.textSecondary, mid);
      p.Text(c[5], L"X", pal.textSecondary, sm);
      p.Text(c[7], L"Y", pal.textSecondary, sm);
      const bool en = Usable() && !Busy();
      ui.PushId(0x7C01);
      if (Field(ui, c[1], 0, dw_, en, 0, W)) {
        int w = std::clamp((int)std::lround(dw_) & ~1, 2, W & ~1);
        if (dw_ >= kMinCrop) { crop_.w = w; if (kRatios[aspect_] > 0) crop_.h = std::max(2, (int)std::lround(w / kRatios[aspect_]) & ~1); FitInside(); }
      }
      if (Field(ui, c[3], 1, dh_, en, 0, H)) {
        int h = std::clamp((int)std::lround(dh_) & ~1, 2, H & ~1);
        if (dh_ >= kMinCrop) { crop_.h = h; if (kRatios[aspect_] > 0) crop_.w = std::max(2, (int)std::lround(h * kRatios[aspect_]) & ~1); FitInside(); }
      }
      if (Field(ui, c[6], 2, dx_, en, 0, W)) { crop_.x = std::clamp((int)std::lround(dx_) & ~1, 0, std::max(0, (W - crop_.w) & ~1)); }
      if (Field(ui, c[8], 3, dy_, en, 0, H)) { crop_.y = std::clamp((int)std::lround(dy_) & ~1, 0, std::max(0, (H - crop_.h) & ~1)); }
      ui.PopId();
    }
    y += 30 + 10;

    // ---- quality + audio note
    {
      Rect row{well.x, y, well.w, 28};
      auto c = ui.Cols(row, {Fixed(64), Fixed(270), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Quality", pal.text, ls);
      quality_ = ui.Segmented(c[1], {L"Best", L"Balanced", L"Smaller file"}, quality_, !Busy());
      TextStyle hs; hs.size = 12.5f; hs.wrap = true;
      p.Text(c[2], info_.hasAudio ? L"Audio is kept. Picture keeps its native pixels." : L"No audio track. Picture keeps its native pixels.", pal.textTertiary, hs);
    }

    // ---- footer
    FooterInfo fi;
    fi.action = L"Crop"; fi.secondary = L"Reset";
    fi.actionEnabled = Usable() && !Busy();
    fi.secondaryEnabled = Usable() && !Busy();
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else fi.status = L"Output " + std::to_wstring(crop_.w) + L" \x00D7 " + std::to_wstring(crop_.h) + L" px, from " + std::to_wstring(SrcW()) + L" \x00D7 " + std::to_wstring(SrcH());
    const bool enter = ConsumeEnter();
    FooterResult fr = DrawFooter(ui, content, fi);
    if (fr.secondary) { ResetRect(); aspect_ = 0; }
    if ((fr.action || enter) && fi.actionEnabled) StartCrop();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && result_->ok)
      DrawDoneNote(ui, content, {L"Cropped to " + std::to_wstring(result_->outWidth) + L" \x00D7 " + std::to_wstring(result_->outHeight) + L" px",
                                 L"from " + std::to_wstring(result_->applied.x) + L", " + std::to_wstring(result_->applied.y)});
  }

 private:
  double Aspect() const { return kRatios[std::clamp(aspect_, 0, 6)]; }

  void ResetRect() {
    const int W = std::max(2, SrcW()), H = std::max(2, SrcH());
    crop_ = Quantize(FRect{W * 0.1f, H * 0.1f, W * 0.8f, H * 0.8f}, W, H);
    lw_ = lh_ = lx_ = ly_ = -1;
  }

  void FitInside() {
    const int W = SrcW(), H = SrcH();
    crop_.w = std::min(crop_.w, W & ~1); crop_.h = std::min(crop_.h, H & ~1);
    if (crop_.x + crop_.w > W) crop_.x = std::max(0, (W - crop_.w) & ~1);
    if (crop_.y + crop_.h > H) crop_.y = std::max(0, (H - crop_.h) & ~1);
  }

  void ApplyAspect() {
    const int W = SrcW(), H = SrcH();
    if (aspect_ <= 0 || W <= 0) return;
    const double cx = (crop_.x + crop_.w / 2.0) / W, cy = (crop_.y + crop_.h / 2.0) / H;
    media::CropRect full = media::AspectCropRect(W, H, kAspects[aspect_], 0.5, 0.5, 1.0);
    const double zoom = std::max(1.0, (double)full.w / std::max(2, crop_.w));
    media::CropRect r = media::AspectCropRect(W, H, kAspects[aspect_], cx, cy, zoom);
    crop_ = {r.x, r.y, r.w, r.h};
  }

  // numeric field that keeps what the user types (a plain Stepper would re-format it on every key stroke)
  bool Field(Ui& ui, const Rect& r, int idx, double& draft, bool enabled, double lo, double hi) {
    int* last = idx == 0 ? &lw_ : idx == 1 ? &lh_ : idx == 2 ? &lx_ : &ly_;
    const int cur = idx == 0 ? crop_.w : idx == 1 ? crop_.h : idx == 2 ? crop_.x : crop_.y;
    if (*last != cur) { draft = cur; *last = cur; }
    ui.PushId(idx);
    const bool ch = ui.Stepper(r, draft, lo, hi, 2, 0, L"", enabled);
    ui.PopId();
    return ch;
  }

  void EditCrop(Ui& ui, const WellGeom& g) {
    const int W = SrcW(), H = SrcH();
    if (!Usable() || W <= 0) return;
    const Rect img = g.image;
    const InputState& in = ui.In();
    Interaction it = ui.Behavior(img.Inset(-12), ui.Id("cropedit"), !Busy(), false);
    const Rect cr = VideoToScreen(g, (float)crop_.x, (float)crop_.y, (float)crop_.w, (float)crop_.h);
    bool inside = false;
    const int flags = HitHandle(cr, in.mx, in.my, 10.f, &inside);
    auto toV = [&](float sx, float sy, float& vx, float& vy) { vx = (sx - img.x) / g.scale; vy = (sy - img.y) / g.scale; };
    float vx, vy; toV(in.mx, in.my, vx, vy);
    if (it.pressed) {
      const FRect fr{(float)crop_.x, (float)crop_.y, (float)crop_.w, (float)crop_.h};
      if (flags) drag_.Begin(2, flags, vx, vy, fr);
      else if (inside) drag_.Begin(1, 0, vx, vy, fr);
      else drag_.Begin(3, 0, std::clamp(vx, 0.f, (float)W), std::clamp(vy, 0.f, (float)H), fr);
    }
    if (drag_.Active()) {
      if (in.down) {
        FRect nr = drag_.Update(vx, vy, W, H, Aspect(), kMinCrop);
        if (drag_.mode != 3 || (nr.w >= kMinCrop && nr.h >= kMinCrop)) {
          IRect q = Quantize(nr, W, H);
          if (Aspect() > 0 && drag_.mode != 1) { q.h = std::max(2, (int)std::lround(q.w / Aspect()) & ~1); if (q.y + q.h > H) q.y = std::max(0, (H - q.h) & ~1); }
          crop_ = q;
        }
        ui.RequestFrame();
        if (drag_.mode == 1) ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
        else if (drag_.mode == 2) ui.SetCursor(CursorForFlags(drag_.flags));
        else ui.SetCursor((int)(intptr_t)IDC_CROSS);
      } else drag_.End();
    } else if (it.hot) {
      if (flags) ui.SetCursor(CursorForFlags(flags));
      else if (inside) ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
      else ui.SetCursor((int)(intptr_t)IDC_CROSS);
    }
  }

  void Nudge(Ui& ui) {
    if (!SinkFocused(ui)) return;
    const int W = SrcW(), H = SrcH();
    for (const auto& k : ui.In().keys) {
      const int n = k.shift ? 20 : 2;
      if (k.vk == VK_LEFT) crop_.x = std::max(0, crop_.x - n);
      else if (k.vk == VK_RIGHT) crop_.x = std::min((W - crop_.w) & ~1, crop_.x + n);
      else if (k.vk == VK_UP) crop_.y = std::max(0, crop_.y - n);
      else if (k.vk == VK_DOWN) crop_.y = std::min((H - crop_.h) & ~1, crop_.y + n);
    }
  }

  void DrawOverlay(Ui& ui, const WellGeom& g) {
    Painter& p = ui.P();
    const Rect img = g.image;
    const Rect cr = VideoToScreen(g, (float)crop_.x, (float)crop_.y, (float)crop_.w, (float)crop_.h);
    const Color dim(0, 0, 0, 0.58f);
    p.FillRect({img.x, img.y, img.w, cr.y - img.y}, dim);
    p.FillRect({img.x, cr.bottom(), img.w, img.bottom() - cr.bottom()}, dim);
    p.FillRect({img.x, cr.y, cr.x - img.x, cr.h}, dim);
    p.FillRect({cr.right(), cr.y, img.right() - cr.right(), cr.h}, dim);
    const Color grid(1, 1, 1, 0.28f), white(1, 1, 1, 0.95f);
    for (int i = 1; i <= 2; ++i) {
      p.Line(cr.x + cr.w * i / 3, cr.y, cr.x + cr.w * i / 3, cr.bottom(), grid, 1.f);
      p.Line(cr.x, cr.y + cr.h * i / 3, cr.right(), cr.y + cr.h * i / 3, grid, 1.f);
    }
    p.StrokeRoundRect(cr, 0, Color(1, 1, 1, 0.85f), 1.2f);
    const float L = std::min(18.f, std::min(cr.w, cr.h) / 3), t = 3.5f;
    auto corner = [&](float x, float y, float sx, float sy) {
      p.Line(x - sx * t / 2, y, x + sx * L, y, white, t, true);
      p.Line(x, y - sy * t / 2, x, y + sy * L, white, t, true);
    };
    corner(cr.x, cr.y, 1, 1); corner(cr.right(), cr.y, -1, 1); corner(cr.x, cr.bottom(), 1, -1); corner(cr.right(), cr.bottom(), -1, -1);
    // edge grips
    const float gl = std::min(22.f, cr.w / 4), gv = std::min(22.f, cr.h / 4);
    p.Line(cr.cx() - gl / 2, cr.y, cr.cx() + gl / 2, cr.y, white, t, true);
    p.Line(cr.cx() - gl / 2, cr.bottom(), cr.cx() + gl / 2, cr.bottom(), white, t, true);
    p.Line(cr.x, cr.cy() - gv / 2, cr.x, cr.cy() + gv / 2, white, t, true);
    p.Line(cr.right(), cr.cy() - gv / 2, cr.right(), cr.cy() + gv / 2, white, t, true);
    // size chip
    const std::wstring txt = std::to_wstring(crop_.w) + L" \x00D7 " + std::to_wstring(crop_.h);
    TextStyle cs; cs.size = 12; cs.weight = 600; cs.align = Align::Center;
    const float cw = p.TextWidth(txt, cs) + 16;
    Rect chip{cr.x + 8, cr.y + 8, cw, 22};
    if (cr.w < cw + 20 || cr.h < 44) chip = {cr.x, cr.y - 28, cw, 22};
    if (chip.y < img.y) chip.y = cr.bottom() + 6;
    p.FillRoundRect(chip, 8, Color(0, 0, 0, 0.62f));
    p.Text(chip, txt, Color(1, 1, 1, 0.95f), cs);
  }

  void StartCrop() {
    media::CropRequest req;
    req.rect = {crop_.x, crop_.y, crop_.w, crop_.h};
    Settings st = SettingsStore::Get().Snapshot();
    req.hardware = st.hardwareAcceleration; req.preserveMetadata = st.preserveMetadata; req.crf = kCrfs[std::clamp(quality_, 0, 2)];
    auto res = result_ = std::make_shared<media::CropResult>();
    const fs::path in = path_;
    RunJob(L"Cropping video...", [in, req, res](JobContext& ctx) -> JobResult {
      *res = media::CropVideo(in, req, ctx);
      return static_cast<JobResult>(*res);
    });
  }

  IRect crop_;
  int aspect_ = 0, quality_ = 1;
  RectDrag drag_;
  double dw_ = 0, dh_ = 0, dx_ = 0, dy_ = 0;
  int lw_ = -1, lh_ = -1, lx_ = -1, ly_ = -1;
  std::shared_ptr<media::CropResult> result_;
};

} // namespace

ToolDialog* MakeCropVideo(const Selection& sel) { return new CropVideoDialog(sel); }

} // namespace filo::tools
