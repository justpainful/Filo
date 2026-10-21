// Edit Photo - layout measured from the reference screenshot: large dark preview well with zoom controls on top, then the
// section headings (Light / Color / Details / Additional Effects) with slider rows: label | slider | value | reset.
#include "tools/tools.h"
#include "tools/photo_adjust.h"
#include "ui/canvas.h"
#include "core/output.h"
#include "engines/image/bitmap.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_EDIT_PREVIEW = WM_APP + 0x61;

struct SliderDef { const wchar_t* label; float PhotoAdjust::*field; float lo, hi, step; bool ev; };

const SliderDef kLight[] = {
  {L"Exposure", &PhotoAdjust::exposure, -5, 5, 0.01f, true}, {L"Brightness", &PhotoAdjust::brightness, -100, 100, 1, false},
  {L"Contrast", &PhotoAdjust::contrast, -100, 100, 1, false}, {L"Highlights", &PhotoAdjust::highlights, -100, 100, 1, false},
  {L"Shadows", &PhotoAdjust::shadows, -100, 100, 1, false}, {L"Whites", &PhotoAdjust::whites, -100, 100, 1, false},
  {L"Blacks", &PhotoAdjust::blacks, -100, 100, 1, false}, {L"Gamma", &PhotoAdjust::gamma, -100, 100, 1, false}};
const SliderDef kColor[] = {
  {L"Temperature", &PhotoAdjust::temperature, -100, 100, 1, false}, {L"Tint", &PhotoAdjust::tint, -100, 100, 1, false},
  {L"Saturation", &PhotoAdjust::saturation, -100, 100, 1, false}, {L"Vibrance", &PhotoAdjust::vibrance, -100, 100, 1, false}};
const SliderDef kDetails[] = {
  {L"Sharpness", &PhotoAdjust::sharpness, 0, 100, 1, false}, {L"Clarity", &PhotoAdjust::clarity, -100, 100, 1, false},
  {L"Dehaze", &PhotoAdjust::dehaze, -100, 100, 1, false}, {L"Noise Reduction", &PhotoAdjust::noiseReduction, 0, 100, 1, false},
  {L"Grain", &PhotoAdjust::grain, 0, 100, 1, false}};

std::wstring FmtVal(const SliderDef& d, float v) {
  wchar_t b[32];
  if (d.ev) swprintf_s(b, L"%.2f EV", v);
  else if (d.lo < 0) swprintf_s(b, L"%+d", (int)std::lround(v));
  else swprintf_s(b, L"%d", (int)std::lround(v));
  return b;
}

class EditPhotoDialog : public ToolDialog {
 public:
  explicit EditPhotoDialog(const Selection& sel) {
    selection_ = sel;
    path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
    shared_ = std::make_shared<Shared>();
    img::LoadOptions lo; lo.maxSide = 1600;
    if (!img::LoadBitmap(path_, base_, lo, &loadError_)) { base_ = {}; if (loadError_.empty()) loadError_ = L"This image cannot be opened."; }
    if (!base_.Empty()) previewScale_ = (float)std::max(base_.width, base_.height) / (float)std::max(base_.info.fullWidth, base_.info.fullHeight);
    RenderAsync();
  }
  ~EditPhotoDialog() override { shared_->cancel = true; }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Edit Photo"; o.widthDip = 648; o.heightDip = 900; o.resizable = true; o.minWidthDip = 520; o.minHeightDip = 520;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "edit-photo"; }
  // Presets store only what the user changed; the sliders themselves are the values.
  static const std::vector<std::pair<const char*, float PhotoAdjust::*>>& Fields() {
    static const std::vector<std::pair<const char*, float PhotoAdjust::*>> f = {
      {"exposure", &PhotoAdjust::exposure}, {"brightness", &PhotoAdjust::brightness}, {"contrast", &PhotoAdjust::contrast}, {"highlights", &PhotoAdjust::highlights},
      {"shadows", &PhotoAdjust::shadows}, {"whites", &PhotoAdjust::whites}, {"blacks", &PhotoAdjust::blacks}, {"gamma", &PhotoAdjust::gamma},
      {"temperature", &PhotoAdjust::temperature}, {"tint", &PhotoAdjust::tint}, {"saturation", &PhotoAdjust::saturation}, {"vibrance", &PhotoAdjust::vibrance},
      {"sharpness", &PhotoAdjust::sharpness}, {"clarity", &PhotoAdjust::clarity}, {"dehaze", &PhotoAdjust::dehaze}, {"noiseReduction", &PhotoAdjust::noiseReduction},
      {"grain", &PhotoAdjust::grain}, {"vignette", &PhotoAdjust::vignette}, {"filterAmount", &PhotoAdjust::filterAmount}};
    return f;
  }
  Json SavePreset() const override { return Json::MakeObject(); }          // adjustments are per photo: never carried to the next one
  void LoadPreset(const Json& j) override {
    for (auto& [k, m] : Fields()) if (j.Has(k)) adj_.*m = (float)j.Get(k).AsNumber();
    if (j.Has("filter")) adj_.filter = (int)j.Get("filter").AsInt();
    RenderAsync();
  }
  void TestApply() override { Save(); }

  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_EDIT_PREVIEW) {
      std::lock_guard lk(shared_->m);
      if (shared_->readySeq == shared_->wantSeq && !shared_->out.empty()) { bmpDirty_ = true; haveOut_ = true; }
      Invalidate(); SetHandled(true); return 0;
    }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float wellH = std::clamp(body.h * 0.58f, 200.f, 391.f);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    DrawWell(ui, well);

    // ---- sections (scrollable)
    Rect vp{body.x + pad, well.bottom() + 12, body.w - 2 * pad, body.bottom() - well.bottom() - 16};
    const float rowH = dialog::rowHeight, headH = 34.f, gap = 0;
    const float totalH = (8 + 4 + 5 + 3) * rowH + 4 * headH + 24;
    const float off = ui.BeginScroll(vp, totalH, ui.Id("sections"));
    float y = vp.y - off;
    bool changed = false;
    auto heading = [&](const wchar_t* t) { ui.SectionTitle({vp.x, y + 6, vp.w, 22}, t); y += headH; };
    auto sliders = [&](const SliderDef* defs, int n) {
      for (int i = 0; i < n; ++i) {
        ui.PushId(ui.Id(std::wstring(defs[i].label)));
        float v = adj_.*(defs[i].field);
        const SliderDef d = defs[i];
        if (ui.SliderRow({vp.x, y, vp.w - (totalH > vp.h ? 10.f : 0.f), rowH}, d.label, v, d.lo, d.hi, d.step, 0, [d](float x) { return FmtVal(d, x); }, !loadError_.size())) { adj_.*(defs[i].field) = v; changed = true; }
        ui.PopId();
        y += rowH + gap;
      }
    };
    heading(L"Light"); sliders(kLight, 8);
    heading(L"Color"); sliders(kColor, 4);
    heading(L"Details"); sliders(kDetails, 5);
    heading(L"Additional Effects");
    {
      ui.PushId(0xF1F);
      float v = adj_.vignette;
      SliderDef vd{L"Vignette", nullptr, -100, 100, 1, false};
      if (ui.SliderRow({vp.x, y, vp.w - (totalH > vp.h ? 10.f : 0.f), rowH}, L"Vignette", v, -100, 100, 1, 0, [vd](float x) { return FmtVal(vd, x); })) { adj_.vignette = v; changed = true; }
      y += rowH;
      int nf = 0; const wchar_t* const* names = FilterNames(nf);
      std::vector<std::wstring> items(names, names + nf);
      Rect r1{vp.x, y, vp.w - (totalH > vp.h ? 10.f : 0.f), rowH};
      auto c = ui.Cols(r1, {Fixed(100), Fixed(150), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Filter", pal.text, ls);
      int sel = adj_.filter;
      Rect dd{c[1].x, c[1].y + 2, c[1].w, c[1].h - 4};
      if (ui.Dropdown(dd, items, sel)) { adj_.filter = sel; changed = true; }
      if (adj_.filter) {
        float amt = adj_.filterAmount;
        if (ui.Slider(c[2], amt, 0, 100, 1)) { adj_.filterAmount = amt; changed = true; }
      }
      y += rowH;
      ui.PopId();
    }
    ui.EndScroll();
    if (changed) RenderAsync();

    FooterInfo fi;
    fi.action = L"Save a Copy"; fi.actionEnabled = !base_.Empty() && !Busy();
    fi.secondary = L"Reset"; fi.secondaryEnabled = !(adj_ == PhotoAdjust{});
    fi.status = !loadError_.empty() ? loadError_ : (adj_ == PhotoAdjust{} ? L"Adjust the sliders; the original file is never changed." : L"");
    fi.statusIsError = !loadError_.empty();
    FooterResult fr = DrawFooter(ui, content, fi);
    if (fr.secondary) { adj_ = PhotoAdjust{}; RenderAsync(); }
    if (fr.action) Save();
    DrawJobPanel(ui, content);
  }

 private:
  struct Shared {
    std::mutex m;
    std::vector<uint8_t> out;              // adjusted preview, premultiplied BGRA
    uint64_t wantSeq = 0, readySeq = 0;
    std::atomic<bool> cancel{false};
  };

  void RenderAsync() {
    if (base_.Empty()) return;
    uint64_t seq;
    { std::lock_guard lk(shared_->m); seq = ++shared_->wantSeq; }
    auto sh = shared_; PhotoAdjust adj = adj_; std::vector<uint8_t> px = base_.px; int w = base_.width, h = base_.height; float ps = previewScale_;
    HWND hw = Hwnd();
    std::thread([sh, adj, px = std::move(px), w, h, ps, seq, hw]() mutable {
      ApplyPhotoAdjust(px, w, h, adj, ps, &sh->cancel);
      for (size_t i = 0; i < (size_t)w * h; ++i) {            // premultiply for Direct2D
        uint8_t* q = &px[i * 4]; const uint32_t a = q[3];
        if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); }
      }
      { std::lock_guard lk(sh->m); if (seq == sh->wantSeq) { sh->out = std::move(px); sh->readySeq = seq; } }
      if (hw) PostMessageW(hw, WM_EDIT_PREVIEW, 0, 0);
    }).detach();
  }

  ComPtr<ID2D1Bitmap> EnsureBitmap() {
    if (!dc_) return {};
    if (bmpDirty_ || !bmp_) {
      std::lock_guard lk(shared_->m);
      const std::vector<uint8_t>* src = haveOut_ && !shared_->out.empty() ? &shared_->out : nullptr;
      std::vector<uint8_t> tmp;
      if (!src && !base_.Empty()) { tmp = base_.px; for (size_t i = 0; i < (size_t)base_.width * base_.height; ++i) { uint8_t* q = &tmp[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } } src = &tmp; }
      if (src) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        ComPtr<ID2D1Bitmap1> b;
        if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(base_.width, base_.height), src->data(), base_.width * 4, &bp, &b))) bmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
      }
      bmpDirty_ = false;
    }
    return bmp_;
  }

  void DrawWell(Ui& ui, const Rect& well) {
    Painter& p = ui.P();
    p.FillRoundRect(well, dialog::wellRadius, ui.Pal().darkCanvas);
    ui.PushId(0x3E11);
    if (!base_.Empty()) {
      ComPtr<ID2D1Bitmap> bmp = EnsureBitmap();
      Rect inner = well.Inset(0, 0);
      CanvasNavigate(ui, inner, view_, base_.width, base_.height, ui.Id("canvas"), 10.5f);
      // press and hold on the picture to compare with the original
      Interaction held = ui.Behavior({inner.x, inner.y + 46, inner.w, inner.h - 46}, ui.Id("compare"), true, false);
      const bool showOrig = held.active && !held.dragging && view_.IsFit() ? true : false;
      CanvasMap m = MapCanvas(inner, base_.width, base_.height, view_, 10.5f);
      p.PushClipRoundRect(well, dialog::wellRadius);
      if (m.scale > 0) {
        if (base_.info.hasAlpha) DrawChecker(p, m.image.Inset(0));
        ID2D1Bitmap* use = bmp.Get();
        ComPtr<ID2D1Bitmap> origBmp;
        if (showOrig && dc_) {
          D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
          std::vector<uint8_t> t = base_.px;
          for (size_t i = 0; i < (size_t)base_.width * base_.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
          ComPtr<ID2D1Bitmap1> b; if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(base_.width, base_.height), t.data(), base_.width * 4, &bp, &b))) { origBmp = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get())); use = origBmp.Get(); }
        }
        p.Bitmap(use, m.image, 1.f, 0.f, true);
      }
      p.PopClip();
      // top bar: zoom readout + fit, right: compare hint + reset view (dark chips like the reference)
      const float dpr = Dpi() / 96.f;
      wchar_t zb[24]; swprintf_s(zb, L"%d%%", (int)std::lround(m.ZoomPercent(dpr) * (float)base_.info.fullWidth / (float)std::max(1, base_.width)));
      Rect chip{well.x + 20, well.y + 16, 34, 26};
      p.FillRoundRect(chip, 8, Color(1, 1, 1, 0.07f));
      p.Icon("zoom", chip.cx(), chip.cy(), 15, Color(1, 1, 1, 0.80f), 1.7f);
      TextStyle zs; zs.size = dialog::bodyFontSize; zs.weight = 500;
      p.Text({chip.right() + 10, chip.y, 60, chip.h}, zb, Color(1, 1, 1, 0.55f), zs);
      if (ui.Button({well.right() - 20 - 78, well.y + 16, 78, 26}, L"Reset", ButtonStyle::OnCanvas)) view_.Reset();
      if (held.active && !held.dragging) { TextStyle hs; hs.size = 12.5f; hs.align = Align::Center; p.Text({well.cx() - 90, well.bottom() - 34, 180, 22}, L"Original", Color(1, 1, 1, 0.75f), hs); }
    } else {
      TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true;
      p.Text(well.Inset(30), loadError_, Color(1, 1, 1, 0.65f), es);
    }
    ui.PopId();
  }

  void Save() {
    PhotoAdjust adj = adj_;
    fs::path in = path_;
    std::wstring fmt = base_.info.format.empty() ? L"png" : base_.info.format;
    if (!img::CanEncode(fmt)) fmt = base_.info.hasAlpha ? L"png" : L"jpg";
    Settings st = SettingsStore::Get().Snapshot();
    fs::path desired = DesiredOutputPath(in, fmt, st, L" (edited)");
    RunJob(L"Saving the edited photo...", [in, adj, fmt, desired, st](JobContext& ctx) -> JobResult {
      img::Bitmap full; std::wstring err;
      img::LoadOptions lo; lo.ctx = &ctx;
      if (!img::LoadBitmap(in, full, lo, &err)) return JobResult::Fail(err.empty() ? L"The image could not be opened." : err);
      ctx.SetProgress(0.3);
      ApplyPhotoAdjust(full.px, full.width, full.height, adj, 1.f, ctx.CancelFlag());
      if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
      ctx.SetProgress(0.7);
      OutputTransaction tx(desired, st.overwritePolicy);
      img::SaveOptions so; so.quality = st.imageQuality; so.preserveMetadata = st.preserveMetadata; so.ctx = &ctx;
      img::SaveResult sr = img::SaveBitmap(tx.TempPath(), full, fmt, so);
      if (!sr.ok) return JobResult::Fail(sr.error);
      std::wstring e;
      if (!tx.Commit(&e)) return JobResult::Fail(e);
      JobResult r = JobResult::Ok({tx.FinalPath()});
      r.warnings = sr.warnings;
      return r;
    });
  }

  fs::path path_;
  img::Bitmap base_;
  std::wstring loadError_;
  float previewScale_ = 1.f;
  PhotoAdjust adj_;
  CanvasView view_;
  std::shared_ptr<Shared> shared_;
  ComPtr<ID2D1Bitmap> bmp_;
  bool bmpDirty_ = true, haveOut_ = false;
};

} // namespace

ToolDialog* MakeEditPhoto(const Selection& sel) { return new EditPhotoDialog(sel); }

} // namespace filo::tools
