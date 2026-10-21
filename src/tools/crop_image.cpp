// Crop Images - shaded crop frame with handles, aspect presets, custom ratio, exact pixel size, batch (same relative crop for all).
#include "tools/tools.h"
#include "tools/crop_frame.h"
#include "core/output.h"
#include "engines/image/bitmap.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

struct Ratio { const wchar_t* name; float r; };   // r: 0 = free, -1 = original, -2 = custom
const Ratio kRatios[] = {{L"Free", 0}, {L"Original", -1}, {L"1:1", 1.f}, {L"4:3", 4.f / 3}, {L"3:2", 1.5f}, {L"16:9", 16.f / 9}, {L"3:4", 3.f / 4}, {L"2:3", 2.f / 3}, {L"9:16", 9.f / 16}, {L"4:5", 0.8f}, {L"Custom", -2}};

class CropImageDialog : public ToolDialog {
 public:
  explicit CropImageDialog(const Selection& sel) {
    selection_ = sel;
    if (!sel.items.empty()) {
      img::LoadOptions lo; lo.maxSide = 1600;
      if (!img::LoadBitmap(sel.items[0].path, base_, lo, &err_)) { base_ = {}; if (err_.empty()) err_ = L"This image cannot be opened."; }
    }
    if (!base_.Empty()) { fullW_ = base_.info.fullWidth; fullH_ = base_.info.fullHeight; crop_ = {0, 0, (float)base_.width, (float)base_.height}; }
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Crop"; o.widthDip = 620; o.heightDip = 720; o.resizable = true; o.minWidthDip = 480; o.minHeightDip = 520;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "crop-image"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["ratio"] = ratioIdx_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("ratio")) { ratioIdx_ = std::clamp((int)j.Get("ratio").AsInt(), 0, (int)(sizeof kRatios / sizeof *kRatios) - 1); ApplyRatio(); }
    if (j.Has("rect") && j.Get("rect").Size() == 4) {      // normalised x,y,w,h
      const Json& r = j.Get("rect");
      crop_ = {(float)(r.At(0).AsNumber() * base_.width), (float)(r.At(1).AsNumber() * base_.height), (float)(r.At(2).AsNumber() * base_.width), (float)(r.At(3).AsNumber() * base_.height)};
    }
  }
  void TestApply() override { Start(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float controlsH = 96;
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, std::max(160.f, body.h - 15 - controlsH - 12)};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (!base_.Empty()) {
      ui.PushId(0xC20B);
      CanvasNavigate(ui, well, view_, base_.width, base_.height, ui.Id("cv"), 22.f);
      CanvasMap m = MapCanvas(well, base_.width, base_.height, view_, 22.f);
      EnsureBitmap();
      p.PushClipRoundRect(well, dialog::wellRadius);
      if (bmp_) p.Bitmap(bmp_.Get(), m.image);
      RectEdit ed = CropFrame(ui, m, crop_, EffectiveRatio(), ui.Id("frame"));
      if (ed.changed) { ratioIdx_ = (kRatios[ratioIdx_].r == 0 || ed.changed) ? ratioIdx_ : ratioIdx_; }
      p.PopClip();
      ui.PopId();
    } else {
      TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true;
      p.Text(well.Inset(30), err_, Color(1, 1, 1, 0.65f), es);
    }

    // ---- controls
    float y = well.bottom() + 14;
    const float inset = 24;
    Rect row1{body.x + inset, y, body.w - 2 * inset, 30};
    {
      auto c = ui.Cols(row1, {Fixed(70), Fixed(150), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Aspect", pal.text, ls);
      std::vector<std::wstring> names; for (auto& r : kRatios) names.push_back(r.name);
      int sel = ratioIdx_;
      if (ui.Dropdown(c[1], names, sel, !base_.Empty()) && sel != ratioIdx_) { ratioIdx_ = sel; ApplyRatio(); }
      if (kRatios[ratioIdx_].r == -2) {
        auto cc = ui.Cols(c[2], {Fixed(70), Fixed(14), Fixed(70), Flex(1)}, 4);
        ui.PushId(0xCC1);
        if (ui.Stepper(cc[0], cw_, 1, 99, 1, 0)) ApplyRatio();
        p.Text(cc[1], L":", pal.text, [] { TextStyle t; t.align = Align::Center; return t; }());
        if (ui.Stepper(cc[2], ch_, 1, 99, 1, 0)) ApplyRatio();
        ui.PopId();
      }
    }
    y += 38;
    if (!base_.Empty()) {
      Rect row2{body.x + inset, y, body.w - 2 * inset, 30};
      auto c = ui.Cols(row2, {Fixed(70), Fixed(110), Fixed(16), Fixed(110), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Size", pal.text, ls);
      const float sx = (float)fullW_ / base_.width, sy = (float)fullH_ / base_.height;
      double w = std::round(crop_.w * sx), h = std::round(crop_.h * sy);
      const bool single = selection_.count() == 1;
      ui.PushId(0x517E);
      if (ui.Stepper(c[1], w, 1, fullW_, 1, 0, L"px", single)) SetSize(w, h, true);
      p.Text(c[2], L"\x00D7", pal.textSecondary, [] { TextStyle t; t.align = Align::Center; return t; }());
      if (ui.Stepper(c[3], h, 1, fullH_, 1, 0, L"px", single)) SetSize(w, h, false);
      ui.PopId();
      if (!single) { TextStyle ss; ss.size = dialog::smallFontSize; p.Text(c[4], L"Same crop for every image", pal.textTertiary, ss); }
    }
    FooterInfo fi;
    fi.action = selection_.count() > 1 ? L"Crop All" : L"Crop"; fi.actionEnabled = !base_.Empty() && !Busy();
    fi.secondary = L"Reset"; fi.secondaryEnabled = !base_.Empty();
    fi.status = L"Saved as a new image; the original is never changed.";
    FooterResult fr = DrawFooter(ui, content, fi);
    if (fr.secondary) { ratioIdx_ = 0; crop_ = {0, 0, (float)base_.width, (float)base_.height}; view_.Reset(); }
    if (fr.action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  float EffectiveRatio() const {
    const float r = kRatios[ratioIdx_].r;
    if (r == -1) return base_.Empty() ? 0.f : (float)fullW_ / fullH_;
    if (r == -2) return (float)(cw_ / std::max(1.0, ch_));
    return r;
  }
  void ApplyRatio() {
    const float r = EffectiveRatio();
    if (r > 0 && !base_.Empty()) crop_ = FitAspect((float)base_.width, (float)base_.height, r);
  }
  void SetSize(double w, double h, bool widthDriven) {
    const float sx = (float)fullW_ / base_.width, sy = (float)fullH_ / base_.height;
    const float r = EffectiveRatio();
    if (r > 0) { if (widthDriven) h = w / r; else w = h * r; }
    crop_.w = std::min((float)(w / sx), (float)base_.width); crop_.h = std::min((float)(h / sy), (float)base_.height);
    crop_.x = std::clamp(crop_.x, 0.f, base_.width - crop_.w); crop_.y = std::clamp(crop_.y, 0.f, base_.height - crop_.h);
  }
  void EnsureBitmap() {
    if (bmp_ || !dc_ || base_.Empty()) return;
    std::vector<uint8_t> t = base_.px;
    for (size_t i = 0; i < (size_t)base_.width * base_.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(base_.width, base_.height), t.data(), base_.width * 4, &bp, &b))) bmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
  }
  void Start() {
    if (base_.Empty() || Busy()) return;
    const float nx = crop_.x / base_.width, ny = crop_.y / base_.height, nw = crop_.w / base_.width, nh = crop_.h / base_.height;
    const std::vector<FileKind> files = selection_.items;
    Settings st = SettingsStore::Get().Snapshot();
    RunJob(files.size() > 1 ? L"Cropping images..." : L"Cropping...", [files, nx, ny, nw, nh, st](JobContext& ctx) -> JobResult {
      JobResult jr; jr.ok = true; std::wstring firstErr; int ok = 0;
      for (size_t i = 0; i < files.size(); ++i) {
        if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
        ctx.SetProgress((double)i / files.size());
        img::Bitmap full; std::wstring err; img::LoadOptions lo; lo.ctx = &ctx;
        if (!img::LoadBitmap(files[i].path, full, lo, &err)) { if (firstErr.empty()) firstErr = files[i].path.filename().wstring() + L": " + err; continue; }
        int x = std::clamp((int)std::lround(nx * full.width), 0, full.width - 1), y = std::clamp((int)std::lround(ny * full.height), 0, full.height - 1);
        int w = std::clamp((int)std::lround(nw * full.width), 1, full.width - x), h = std::clamp((int)std::lround(nh * full.height), 1, full.height - y);
        img::Bitmap out; out.Allocate(w, h); out.info = full.info; out.info.fullWidth = w; out.info.fullHeight = h;
        for (int r = 0; r < h; ++r) memcpy(out.Row(r), full.Row(y + r) + x * 4, (size_t)w * 4);
        std::wstring fmt = full.info.format; if (!img::CanEncode(fmt)) fmt = full.info.hasAlpha ? L"png" : L"jpg";
        fs::path desired = DesiredOutputPath(files[i].path, fmt, st, L" (cropped)");
        OutputTransaction tx(desired, st.overwritePolicy);
        img::SaveOptions so; so.quality = st.imageQuality; so.preserveMetadata = st.preserveMetadata; so.ctx = &ctx;
        img::SaveResult sr = img::SaveBitmap(tx.TempPath(), out, fmt, so);
        std::wstring e;
        if (!sr.ok) { if (firstErr.empty()) firstErr = sr.error; continue; }
        if (!tx.Commit(&e)) { if (firstErr.empty()) firstErr = e; continue; }
        jr.outputs.push_back(tx.FinalPath()); ++ok;
        for (auto& w2 : sr.warnings) jr.warnings.push_back(w2);
      }
      if (ok == 0) return JobResult::Fail(firstErr.empty() ? L"Nothing was cropped." : firstErr);
      if (!firstErr.empty()) jr.warnings.push_back(firstErr);
      return jr;
    });
  }

  img::Bitmap base_;
  std::wstring err_;
  int fullW_ = 0, fullH_ = 0;
  RectF crop_;
  CanvasView view_;
  int ratioIdx_ = 0;
  double cw_ = 16, ch_ = 9;
  ComPtr<ID2D1Bitmap> bmp_;
};

} // namespace

ToolDialog* MakeCropImage(const Selection& sel) { return new CropImageDialog(sel); }

} // namespace filo::tools
