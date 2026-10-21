// Redact Photos - independent regions (draw, move, resize, delete), Solid cover or Blur, local face and text detection.
// The redaction is burned into the exported pixels (never an overlay), and blurred areas are blurred hard enough to be unreadable.
#include "tools/tools.h"
#include "tools/crop_frame.h"
#include "tools/vision.h"
#include "core/output.h"
#include "engines/image/bitmap.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

constexpr UINT WM_REDACT_DETECT = WM_APP + 0x64;

struct Region { RectF r; };                       // preview-pixel rectangle

const Color kCovers[] = {Color::Hex(0x000000), Color::Hex(0xFFFFFF), Color::Hex(0x6E7B8C), Color::Hex(0xD8383A), Color::Hex(0x3B8BFF)};

// ---- pixel work (also used for the export at full resolution)
void BoxBlurRegion(img::Bitmap& bm, int x0, int y0, int w, int h, int radius) {
  if (radius < 1 || w < 2 || h < 2) return;
  std::vector<uint8_t> tmp((size_t)w * h * 4);
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y) {                                      // horizontal into tmp
      const uint8_t* row = bm.Row(y0 + y) + x0 * 4; uint8_t* out = &tmp[(size_t)y * w * 4];
      long sum[3] = {0, 0, 0};
      auto at = [&](int i, int c) { return (int)row[std::clamp(i, 0, w - 1) * 4 + c]; };
      for (int i = -radius; i <= radius; ++i) for (int c = 0; c < 3; ++c) sum[c] += at(i, c);
      const int win = 2 * radius + 1;
      for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) { out[x * 4 + c] = (uint8_t)(sum[c] / win); sum[c] += at(x + radius + 1, c) - at(x - radius, c); }
        out[x * 4 + 3] = 255;
      }
    }
    for (int x = 0; x < w; ++x) {                                      // vertical back into the bitmap
      long sum[3] = {0, 0, 0};
      auto at = [&](int i, int c) { return (int)tmp[((size_t)std::clamp(i, 0, h - 1) * w + x) * 4 + c]; };
      for (int i = -radius; i <= radius; ++i) for (int c = 0; c < 3; ++c) sum[c] += at(i, c);
      const int win = 2 * radius + 1;
      for (int y = 0; y < h; ++y) {
        uint8_t* q = bm.Row(y0 + y) + (x0 + x) * 4;
        for (int c = 0; c < 3; ++c) { q[c] = (uint8_t)(sum[c] / win); sum[c] += at(y + radius + 1, c) - at(y - radius, c); }
        q[3] = 255;
      }
    }
  }
}

void ApplyRedaction(img::Bitmap& bm, const std::vector<Region>& regions, float sx, float sy, bool solid, const Color& cover, float strength) {
  for (const Region& g : regions) {
    int x = std::clamp((int)std::floor(g.r.x * sx), 0, bm.width - 1), y = std::clamp((int)std::floor(g.r.y * sy), 0, bm.height - 1);
    int w = std::clamp((int)std::ceil(g.r.w * sx) + 1, 1, bm.width - x), h = std::clamp((int)std::ceil(g.r.h * sy) + 1, 1, bm.height - y);
    if (solid) {
      const uint8_t b = (uint8_t)std::lround(cover.b * 255), gg = (uint8_t)std::lround(cover.g * 255), r = (uint8_t)std::lround(cover.r * 255);
      for (int yy = 0; yy < h; ++yy) { uint8_t* q = bm.Row(y + yy) + x * 4; for (int xx = 0; xx < w; ++xx) { q[xx * 4] = b; q[xx * 4 + 1] = gg; q[xx * 4 + 2] = r; q[xx * 4 + 3] = 255; } }
    } else {
      // mosaic first (destroys detail irreversibly), then a strong blur to make it smooth
      const int cell = std::max(4, (int)std::lround(std::min(w, h) / (6.f + (1.f - strength) * 10.f)));
      for (int by = 0; by < h; by += cell) for (int bx = 0; bx < w; bx += cell) {
        const int cw = std::min(cell, w - bx), ch = std::min(cell, h - by);
        long s[3] = {0, 0, 0};
        for (int yy = 0; yy < ch; ++yy) { const uint8_t* q = bm.Row(y + by + yy) + (x + bx) * 4; for (int xx = 0; xx < cw; ++xx) for (int c = 0; c < 3; ++c) s[c] += q[xx * 4 + c]; }
        const long n = (long)cw * ch;
        for (int yy = 0; yy < ch; ++yy) { uint8_t* q = bm.Row(y + by + yy) + (x + bx) * 4; for (int xx = 0; xx < cw; ++xx) { for (int c = 0; c < 3; ++c) q[xx * 4 + c] = (uint8_t)(s[c] / n); q[xx * 4 + 3] = 255; } }
      }
      BoxBlurRegion(bm, x, y, w, h, std::max(2, cell / 2));
    }
  }
}

class RedactDialog : public ToolDialog {
 public:
  explicit RedactDialog(const Selection& sel) {
    selection_ = sel;
    path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
    img::LoadOptions lo; lo.maxSide = 1600;
    if (!img::LoadBitmap(path_, base_, lo, &err_)) { base_ = {}; if (err_.empty()) err_ = L"This image cannot be opened."; }
    shared_ = std::make_shared<Shared>();
  }
  ~RedactDialog() override {}

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Redact"; o.widthDip = 640; o.heightDip = 740; o.resizable = true; o.minWidthDip = 520; o.minHeightDip = 540;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "redact-photo"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["solid"] = solid_; j["cover"] = cover_; j["strength"] = strength_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("solid")) solid_ = j.Get("solid").AsBool(true); if (j.Has("cover")) cover_ = std::clamp((int)j.Get("cover").AsInt(), 0, 4);
    if (j.Has("strength")) strength_ = (float)j.Get("strength").AsNumber(0.6);
    if (j.Has("regions")) for (auto& r : j.Get("regions").Elements()) if (r.Size() == 4)       // normalised x,y,w,h
      regions_.push_back({{(float)(r.At(0).AsNumber() * base_.width), (float)(r.At(1).AsNumber() * base_.height), (float)(r.At(2).AsNumber() * base_.width), (float)(r.At(3).AsNumber() * base_.height)}});
    if (j.Has("detect")) {    // automation: "faces" | "text"  (synchronous in tests)
      std::vector<VisionBox> boxes; std::wstring e;
      const std::string d = j.Get("detect").AsString();
      std::thread t([&] { if (d == "faces") DetectFaces(base_, boxes, &e); else DetectTextLines(base_, boxes, &e); });
      t.join();
      for (auto& b : boxes) regions_.push_back({{b.x, b.y, b.w, b.h}});
    }
  }
  void TestApply() override { Start(); }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override {
    if (msg == WM_REDACT_DETECT) {
      std::lock_guard lk(shared_->m);
      for (auto& b : shared_->boxes) regions_.push_back({{b.x, b.y, b.w, b.h}});
      shared_->boxes.clear(); notice_ = shared_->notice; detecting_ = false;
      if (!regions_.empty()) selected_ = (int)regions_.size() - 1;
      Invalidate(); SetHandled(true); return 0;
    }
    return ToolDialog::OnMessage(msg, wp, lp);
  }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset, controlsH = 118;
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, std::max(180.f, body.h - 15 - controlsH - 12)};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (!base_.Empty()) DrawCanvas(ui, well);
    else { TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true; p.Text(well.Inset(30), err_, Color(1, 1, 1, 0.65f), es); }

    float y = well.bottom() + 12;
    const float inset = 24;
    {   // detection + region buttons
      Rect row{body.x + inset, y, body.w - 2 * inset, 30};
      auto c = ui.Cols(row, {Fixed(124), Fixed(116), Flex(1), Fixed(92)}, 8);
      ui.PushId(0xDE7);
      if (ui.Button(c[0], detecting_ ? L"Detecting..." : L"Detect Faces", ButtonStyle::Secondary, !base_.Empty() && !detecting_, "redact")) StartDetect(true);
      if (ui.Button(c[1], L"Detect Text", ButtonStyle::Secondary, !base_.Empty() && !detecting_, "text")) StartDetect(false);
      TextStyle ns; ns.size = dialog::smallFontSize; ns.wrap = true;
      p.Text(c[2], !notice_.empty() ? notice_ : (regions_.empty() ? L"Drag on the picture to add a region." : std::to_wstring(regions_.size()) + L" region(s)"), !notice_.empty() ? Color::Hex(0xB26A00) : pal.textTertiary, ns);
      if (ui.Button(c[3], L"Delete", ButtonStyle::Quiet, selected_ >= 0, "trash") && selected_ >= 0 && selected_ < (int)regions_.size()) { regions_.erase(regions_.begin() + selected_); selected_ = -1; }
      ui.PopId();
    }
    y += 38;
    {   // style
      Rect row{body.x + inset, y, body.w - 2 * inset, 30};
      auto c = ui.Cols(row, {Fixed(70), Fixed(150), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Cover", pal.text, ls);
      int s = ui.Segmented(c[1], {L"Solid", L"Blur"}, solid_ ? 0 : 1);
      solid_ = s == 0;
      if (solid_) {
        ui.PushId(0x5011D);
        for (int i = 0; i < 5; ++i) {
          ui.PushId(i);
          Rect sw{c[2].x + i * 36.f, c[2].cy() - 14, 28, 28};
          if (ui.Swatch(ui.Mirror(sw, c[2]), kCovers[i], kCovers[i], cover_ == i)) cover_ = i;
          ui.PopId();
        }
        ui.PopId();
      } else {
        ui.Slider(c[2], strength_, 0.1f, 1.f, 0.01f);
      }
    }
    FooterInfo fi;
    fi.action = L"Save Redacted Copy"; fi.actionEnabled = !base_.Empty() && !regions_.empty() && !Busy();
    fi.status = regions_.empty() ? L"Add at least one region." : L"The result is saved as a new image.";
    fi.statusIsError = regions_.empty();
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  struct Shared { std::mutex m; std::vector<VisionBox> boxes; std::wstring notice; };

  void DrawCanvas(Ui& ui, const Rect& well) {
    Painter& p = ui.P();
    ui.PushId(0x3ED);
    CanvasNavigate(ui, well, view_, base_.width, base_.height, ui.Id("cv"), 22.f, false);
    CanvasMap m = MapCanvas(well, base_.width, base_.height, view_, 22.f);
    if (!bmp_ && dc_) {
      std::vector<uint8_t> t = base_.px;
      for (size_t i = 0; i < (size_t)base_.width * base_.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
      D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
      ComPtr<ID2D1Bitmap1> b; if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(base_.width, base_.height), t.data(), base_.width * 4, &bp, &b))) bmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
    }
    p.PushClipRoundRect(well, dialog::wellRadius);
    if (bmp_) p.Bitmap(bmp_.Get(), m.image);
    // existing regions (latest on top); the selected one gets handles
    bool pressedOnRegion = false;
    for (int i = (int)regions_.size() - 1; i >= 0; --i) {
      ui.PushId(i);
      Rect sr = m.ToScreen(regions_[i].r.x, regions_[i].r.y, regions_[i].r.w, regions_[i].r.h);
      const bool sel = i == selected_;
      if (solid_) p.FillRect(sr, Color(kCovers[cover_].r, kCovers[cover_].g, kCovers[cover_].b, 0.78f));
      else p.FillRect(sr, Color(1, 1, 1, 0.30f));
      p.StrokeRoundRect(sr, 0, sel ? accent::base : Color(1, 1, 1, 0.9f), sel ? 2.f : 1.2f);
      RectEdit ed = RectHandles(ui, m, regions_[i].r, 0, sel, ui.Id("rg"));
      if (ed.pressed) { selected_ = i; pressedOnRegion = true; }
      ui.PopId();
    }
    // draw a new region on empty space
    const InputState& in = ui.In();
    static bool creating = false; static float sx = 0, sy = 0;
    if (in.pressed && !in.shift && !pressedOnRegion && m.image.Contains(in.mx, in.my) && well.Contains(in.mx, in.my)) {
      creating = true; sx = std::clamp(m.ToImageX(in.mx), 0.f, (float)base_.width); sy = std::clamp(m.ToImageY(in.my), 0.f, (float)base_.height);
      regions_.push_back({{sx, sy, 1, 1}}); selected_ = (int)regions_.size() - 1;
    }
    if (creating) {
      if (in.down && selected_ >= 0 && selected_ < (int)regions_.size()) {
        const float cx = std::clamp(m.ToImageX(in.mx), 0.f, (float)base_.width), cy = std::clamp(m.ToImageY(in.my), 0.f, (float)base_.height);
        regions_[selected_].r = {std::min(sx, cx), std::min(sy, cy), std::max(1.f, std::fabs(cx - sx)), std::max(1.f, std::fabs(cy - sy))};
        ui.RequestFrame();
      } else {
        creating = false;
        if (selected_ >= 0 && selected_ < (int)regions_.size() && (regions_[selected_].r.w < 8 || regions_[selected_].r.h < 8)) { regions_.erase(regions_.begin() + selected_); selected_ = -1; }
      }
    }
    if (m.image.Contains(in.mx, in.my)) ui.SetCursor((int)(intptr_t)IDC_CROSS);
    p.PopClip();
    for (const auto& k : in.keys) if (k.vk == VK_DELETE && selected_ >= 0 && selected_ < (int)regions_.size()) { regions_.erase(regions_.begin() + selected_); selected_ = -1; break; }
    ui.PopId();
  }

  void StartDetect(bool faces) {
    detecting_ = true; notice_.clear();
    auto sh = shared_; img::Bitmap copy = base_; HWND hw = Hwnd();
    std::thread([sh, copy = std::move(copy), faces, hw]() mutable {
      std::vector<VisionBox> boxes; std::wstring err;
      bool ok = faces ? DetectFaces(copy, boxes, &err) : DetectTextLines(copy, boxes, &err);
      { std::lock_guard lk(sh->m); sh->boxes = std::move(boxes); sh->notice = !ok ? err : (sh->boxes.empty() ? (faces ? L"No faces found." : L"No text found.") : L""); }
      PostMessageW(hw, WM_REDACT_DETECT, 0, 0);
    }).detach();
  }

  void Start() {
    if (base_.Empty() || regions_.empty() || Busy()) return;
    std::vector<Region> regs = regions_;
    const float bw = (float)base_.width, bh = (float)base_.height;
    const bool solid = solid_; const Color cover = kCovers[cover_]; const float strength = strength_;
    fs::path in = path_;
    Settings st = SettingsStore::Get().Snapshot();
    RunJob(L"Redacting...", [regs, bw, bh, solid, cover, strength, in, st](JobContext& ctx) -> JobResult {
      img::Bitmap full; std::wstring err; img::LoadOptions lo; lo.ctx = &ctx;
      if (!img::LoadBitmap(in, full, lo, &err)) return JobResult::Fail(err.empty() ? L"The image could not be opened." : err);
      ctx.SetProgress(0.4);
      ApplyRedaction(full, regs, full.width / bw, full.height / bh, solid, cover, strength);
      if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
      std::wstring fmt = full.info.format; if (!img::CanEncode(fmt)) fmt = full.info.hasAlpha ? L"png" : L"jpg";
      fs::path desired = DesiredOutputPath(in, fmt, st, L" (redacted)");
      OutputTransaction tx(desired, st.overwritePolicy);
      img::SaveOptions so; so.quality = std::max(90, st.imageQuality); so.preserveMetadata = false; so.ctx = &ctx;     // redacted copies never keep EXIF/GPS
      img::SaveResult sr = img::SaveBitmap(tx.TempPath(), full, fmt, so);
      if (!sr.ok) return JobResult::Fail(sr.error);
      std::wstring e; if (!tx.Commit(&e)) return JobResult::Fail(e);
      JobResult r = JobResult::Ok({tx.FinalPath()}); r.warnings = sr.warnings; r.warnings.push_back(L"Metadata (location, camera) was not copied to the redacted file.");
      return r;
    });
  }

  fs::path path_;
  img::Bitmap base_;
  std::wstring err_, notice_;
  std::vector<Region> regions_;
  int selected_ = -1, cover_ = 0;
  bool solid_ = true, detecting_ = false;
  float strength_ = 0.6f;
  CanvasView view_;
  ComPtr<ID2D1Bitmap> bmp_;
  std::shared_ptr<Shared> shared_;
};

} // namespace

ToolDialog* MakeRedactPhoto(const Selection& sel) { return new RedactDialog(sel); }

} // namespace filo::tools
