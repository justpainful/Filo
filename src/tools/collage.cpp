// Collage - combines 2 to 9 images into one picture: grid / rows / columns / feature layouts, spacing, rounded corners,
// background colour and aspect ratio. Pictures are centre-cropped to fill their cell (nothing is stretched).
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/output.h"
#include "core/settings.h"
#include "engines/image/bitmap.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

struct Cell { float x, y, w, h; };      // unit square

std::vector<Cell> Layout(int layout, int n) {
  std::vector<Cell> c;
  auto grid = [&](int rows, int cols) {                              // last row shares the full width
    int idx = 0;
    for (int r = 0; r < rows && idx < n; ++r) {
      const int inRow = std::min(cols, n - idx);
      for (int k = 0; k < inRow; ++k, ++idx) c.push_back({(float)k / inRow, (float)r / rows, 1.f / inRow, 1.f / rows});
    }
  };
  switch (layout) {
    case 1: grid(n, 1); break;                                       // rows
    case 2: grid(1, n); break;                                       // columns
    case 3: {                                                        // feature: first image big on the left, the rest stacked
      if (n == 1) { c.push_back({0, 0, 1, 1}); break; }
      c.push_back({0, 0, 0.62f, 1});
      for (int i = 1; i < n; ++i) c.push_back({0.62f, (float)(i - 1) / (n - 1), 0.38f, 1.f / (n - 1)});
      break; }
    case 4: {                                                        // feature on top
      if (n == 1) { c.push_back({0, 0, 1, 1}); break; }
      c.push_back({0, 0, 1, 0.62f});
      for (int i = 1; i < n; ++i) c.push_back({(float)(i - 1) / (n - 1), 0.62f, 1.f / (n - 1), 0.38f});
      break; }
    default: { const int cols = (int)std::ceil(std::sqrt((double)n)); grid((n + cols - 1) / cols, cols); }
  }
  return c;
}

const wchar_t* kLayoutNames[] = {L"Grid", L"Rows", L"Columns", L"Feature left", L"Feature top"};
struct Aspect { const wchar_t* name; float r; };
const Aspect kAspects[] = {{L"1:1", 1.f}, {L"4:3", 4.f / 3}, {L"3:4", 0.75f}, {L"16:9", 16.f / 9}, {L"9:16", 9.f / 16}, {L"3:2", 1.5f}};
const Color kBgs[] = {Color::Hex(0xFFFFFF), Color::Hex(0xF1EEE9), Color::Hex(0x1C1C1E), Color::Hex(0xFFD7A8), Color::Hex(0xBFD9FF)};

// Coverage (0..1) of a rounded rect at pixel centre (px,py) relative to the rect's top-left; r = corner radius.
float RoundCoverage(float px, float py, float w, float h, float r) {
  if (r <= 0.5f) return 1.f;
  const float qx = std::fabs(px - w / 2) - (w / 2 - r), qy = std::fabs(py - h / 2) - (h / 2 - r);
  const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
  const float d = std::hypot(ox, oy) + std::min(std::max(qx, qy), 0.f) - r;
  return std::clamp(0.5f - d, 0.f, 1.f);
}

// Composes `images` (straight alpha) into `out` (opaque). Lengths (spacing, radius) are given for a 2048 px long side and scaled.
bool Compose(const std::vector<const img::Bitmap*>& images, int layout, float aspect, float spacing, float radius, const Color& bg, int longSide, img::Bitmap& out) {
  const int n = (int)images.size();
  if (n == 0) return false;
  int W = longSide, H = longSide;
  if (aspect >= 1) H = (int)std::lround(longSide / aspect); else W = (int)std::lround(longSide * aspect);
  out.Allocate(W, H);
  const uint8_t bb = (uint8_t)std::lround(bg.b * 255), bgc = (uint8_t)std::lround(bg.g * 255), br = (uint8_t)std::lround(bg.r * 255);
  for (int y = 0; y < H; ++y) { uint8_t* q = out.Row(y); for (int x = 0; x < W; ++x) { q[x * 4] = bb; q[x * 4 + 1] = bgc; q[x * 4 + 2] = br; q[x * 4 + 3] = 255; } }
  const float k = longSide / 2048.f;
  const float gap = spacing * k, rad = radius * k;
  const auto cells = Layout(layout, n);
  for (int i = 0; i < n && i < (int)cells.size(); ++i) {
    const Cell& c = cells[i];
    // cell rectangle inside the canvas with half a gap on inner edges and a full gap on the outer edges
    const float x0 = c.x * W + (c.x <= 0.001f ? gap : gap / 2), y0 = c.y * H + (c.y <= 0.001f ? gap : gap / 2);
    const float x1 = (c.x + c.w) * W - (c.x + c.w >= 0.999f ? gap : gap / 2), y1 = (c.y + c.h) * H - (c.y + c.h >= 0.999f ? gap : gap / 2);
    const int cw = (int)std::lround(x1 - x0), ch = (int)std::lround(y1 - y0);
    if (cw < 2 || ch < 2) continue;
    const img::Bitmap& src = *images[i];
    // centre-crop to the cell aspect, then resize
    const float ca = (float)cw / ch, sa = (float)src.width / src.height;
    int cx = 0, cy = 0, cwid = src.width, chei = src.height;
    if (sa > ca) { cwid = (int)std::lround(src.height * ca); cx = (src.width - cwid) / 2; } else { chei = (int)std::lround(src.width / ca); cy = (src.height - chei) / 2; }
    img::Bitmap crop; crop.Allocate(std::max(1, cwid), std::max(1, chei));
    for (int y = 0; y < crop.height; ++y) memcpy(crop.Row(y), src.Row(cy + y) + cx * 4, (size_t)crop.width * 4);
    img::Bitmap fit;
    if (!img::ResizeBitmap(crop, cw, ch, fit)) continue;
    const float r = std::min(rad, std::min(cw, ch) / 2.f);
    const int ox = (int)std::lround(x0), oy = (int)std::lround(y0);
    for (int y = 0; y < ch; ++y) {
      const int dy = oy + y; if (dy < 0 || dy >= H) continue;
      const uint8_t* s = fit.Row(y); uint8_t* d = out.Row(dy);
      for (int x = 0; x < cw; ++x) {
        const int dx = ox + x; if (dx < 0 || dx >= W) continue;
        float a = (s[x * 4 + 3] / 255.f) * RoundCoverage(x + 0.5f, y + 0.5f, (float)cw, (float)ch, r);
        if (a <= 0) continue;
        for (int ch3 = 0; ch3 < 3; ++ch3) d[dx * 4 + ch3] = (uint8_t)std::lround(s[x * 4 + ch3] * a + d[dx * 4 + ch3] * (1 - a));
      }
    }
  }
  return true;
}

class CollageDialog : public ToolDialog {
 public:
  explicit CollageDialog(const Selection& sel) { selection_ = sel; for (auto& i : sel.items) AddFile(i.path); }

 protected:
  struct Pic { fs::path path; img::Bitmap bm; };
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Collage"; o.widthDip = 640; o.heightDip = 740; o.resizable = true; o.minWidthDip = 520; o.minHeightDip = 560;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "collage"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["layout"] = layout_; j["aspect"] = aspect_; j["spacing"] = spacing_; j["radius"] = radius_; j["bg"] = bg_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("layout")) layout_ = std::clamp((int)j.Get("layout").AsInt(), 0, 4);
    if (j.Has("aspect")) aspect_ = std::clamp((int)j.Get("aspect").AsInt(), 0, 5);
    if (j.Has("spacing")) spacing_ = (float)std::clamp(j.Get("spacing").AsNumber(24), 0.0, 80.0);
    if (j.Has("radius")) radius_ = (float)std::clamp(j.Get("radius").AsNumber(32), 0.0, 120.0);
    if (j.Has("bg")) bg_ = std::clamp((int)j.Get("bg").AsInt(), 0, 4);
    dirty_ = true;
  }
  void TestApply() override { Start(); }
  bool AcceptsFileDrop() const override { return !Busy(); }
  void OnFilesDropped(const std::vector<fs::path>& f) override { for (auto& p : f) AddFile(p); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset, ctlH = 190;
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, std::max(160.f, body.h - 15 - ctlH - 12)};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (dirty_ && pics_.size() >= 1) Rebuild();
    if (dc_ && preview_.width > 0 && !previewBmp_) MakePreviewBitmap();
    if (previewBmp_) {
      const float s = std::min((well.w - 44) / preview_.width, (well.h - 44) / preview_.height);
      Rect d{well.cx() - preview_.width * s / 2, well.cy() - preview_.height * s / 2, preview_.width * s, preview_.height * s};
      p.Shadow(d, 4, 14, Color(0, 0, 0, 0.4f), 3); p.Bitmap(previewBmp_.Get(), d);
    } else { TextStyle es; es.size = 14; es.align = Align::Center; p.Text(well, pics_.size() < 2 ? L"Drop two or more pictures here." : L"...", Color(1, 1, 1, 0.65f), es); }

    float y = well.bottom() + 10; const float inset = 24;
    {   // picture strip
      Rect row{body.x + inset, y, body.w - 2 * inset, 50};
      const float th = 44, gap = 8;
      for (int i = 0; i < (int)pics_.size(); ++i) {
        Rect t{row.x + i * (th + gap), row.y + 3, th, th};
        if (t.right() > row.right() - 150) break;
        ui.PushId(i);
        Interaction it = ui.Behavior(t, ui.Id("t"), true, false);
        p.FillRoundRect(t, 8, Color(0, 0, 0, 0.12f));
        if (ID2D1Bitmap* b = Thumb(pics_[i].path, 160)) {
          auto sz = b->GetSize(); const float s = std::max(t.w / sz.width, t.h / sz.height);
          p.PushClipRoundRect(t, 8); p.Bitmap(b, {t.cx() - sz.width * s / 2, t.cy() - sz.height * s / 2, sz.width * s, sz.height * s}); p.PopClip();
        }
        if (i == sel_) p.StrokeRoundRect(t, 8, accent::base, 2.f);
        if (it.clicked) sel_ = i;
        ui.PopId();
      }
      const float bx = row.right();
      ui.PushId(0xC011);
      if (ui.IconButton({bx - 28, row.y + 12, 26, 26}, "close", sel_ >= 0 && pics_.size() > 1, L"Remove", false)) { pics_.erase(pics_.begin() + sel_); sel_ = std::min(sel_, (int)pics_.size() - 1); dirty_ = true; }
      if (ui.IconButton({bx - 60, row.y + 12, 26, 26}, "chevronright", sel_ >= 0 && sel_ + 1 < (int)pics_.size(), L"Move later", true)) { std::swap(pics_[sel_], pics_[sel_ + 1]); ++sel_; dirty_ = true; }
      if (ui.IconButton({bx - 92, row.y + 12, 26, 26}, "chevronleft", sel_ > 0, L"Move earlier", true)) { std::swap(pics_[sel_], pics_[sel_ - 1]); --sel_; dirty_ = true; }
      ui.PopId();
    }
    y += 56;
    TextStyle ls; ls.size = dialog::bodyFontSize;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 28}; auto c = ui.Cols(row, {Fixed(92), Fixed(150), Fixed(70), Flex(1)}, 8);
      p.Text(c[0], L"Layout", pal.text, ls);
      std::vector<std::wstring> names(std::begin(kLayoutNames), std::end(kLayoutNames));
      ui.PushId(0xA1); if (ui.Dropdown(c[1], names, layout_)) dirty_ = true; ui.PopId();
      p.Text(c[2], L"Shape", pal.text, ls);
      std::vector<std::wstring> an; for (auto& a : kAspects) an.push_back(a.name);
      ui.PushId(0xA2); if (ui.Dropdown(c[3], an, aspect_)) dirty_ = true; ui.PopId();
    }
    y += 36;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 26}; auto c = ui.Cols(row, {Fixed(92), Flex(1), Fixed(10), Fixed(74), Flex(1)}, 8);
      p.Text(c[0], L"Spacing", pal.text, ls);
      ui.PushId(0xA3); if (ui.Slider(c[1], spacing_, 0.f, 80.f, 1.f, 0.f)) dirty_ = true; ui.PopId();
      p.Text(c[3], L"Corners", pal.text, ls);
      ui.PushId(0xA4); if (ui.Slider(c[4], radius_, 0.f, 120.f, 1.f, 0.f)) dirty_ = true; ui.PopId();
    }
    y += 36;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 30}; auto c = ui.Cols(row, {Fixed(92), Flex(1)}, 8);
      p.Text(c[0], L"Background", pal.text, ls);
      ui.PushId(0xA5);
      for (int i = 0; i < 5; ++i) { ui.PushId(i); Rect sw{c[1].x + i * 38.f, c[1].cy() - 14, 28, 28}; if (ui.Swatch(ui.Mirror(sw, c[1]), kBgs[i], kBgs[i], bg_ == i)) { bg_ = i; dirty_ = true; } ui.PopId(); }
      ui.PopId();
    }
    FooterInfo fi;
    fi.action = L"Save Collage"; fi.actionEnabled = pics_.size() >= 2 && !Busy();
    fi.status = pics_.size() < 2 ? L"Add at least two pictures." : (pics_.size() > 9 ? L"Using the first 9 pictures." : L"Saved as a new picture; originals are never changed.");
    fi.statusIsError = pics_.size() < 2;
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  void AddFile(const fs::path& path) {
    if (DetectFile(path).group != Group::Image || pics_.size() >= 12) return;
    for (auto& p : pics_) if (p.path == path) return;
    Pic pic; pic.path = path;
    img::LoadOptions lo; lo.maxSide = 1200; std::wstring err;
    if (!img::LoadBitmap(path, pic.bm, lo, &err)) return;
    pics_.push_back(std::move(pic)); if (sel_ < 0) sel_ = 0;
    dirty_ = true;
  }
  std::vector<const img::Bitmap*> Used() const { std::vector<const img::Bitmap*> v; for (size_t i = 0; i < pics_.size() && i < 9; ++i) v.push_back(&pics_[i].bm); return v; }
  void Rebuild() {
    dirty_ = false; previewBmp_.Reset();
    Compose(Used(), layout_, kAspects[aspect_].r, spacing_, radius_, kBgs[bg_], 900, preview_);
  }
  void MakePreviewBitmap() {
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(preview_.width, preview_.height), preview_.px.data(), preview_.width * 4, &bp, &b))) previewBmp_ = ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
  }
  void Start() {
    if (pics_.size() < 2 || Busy()) return;
    std::vector<fs::path> files; for (size_t i = 0; i < pics_.size() && i < 9; ++i) files.push_back(pics_[i].path);
    const int layout = layout_; const float asp = kAspects[aspect_].r, sp = spacing_, rd = radius_; const Color bg = kBgs[bg_];
    Settings st = SettingsStore::Get().Snapshot();
    RunJob(L"Making the collage...", [files, layout, asp, sp, rd, bg, st](JobContext& ctx) -> JobResult {
      std::vector<img::Bitmap> loaded(files.size());
      for (size_t i = 0; i < files.size(); ++i) {
        if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
        ctx.SetProgress(0.6 * i / files.size());
        std::wstring err; img::LoadOptions lo; lo.maxSide = 2600; lo.ctx = &ctx;
        if (!img::LoadBitmap(files[i], loaded[i], lo, &err)) return JobResult::Fail(files[i].filename().wstring() + L": " + (err.empty() ? L"cannot be opened." : err));
      }
      std::vector<const img::Bitmap*> ptrs; for (auto& b : loaded) ptrs.push_back(&b);
      img::Bitmap out;
      ctx.SetProgress(0.65);
      if (!Compose(ptrs, layout, asp, sp, rd, bg, 3072, out)) return JobResult::Fail(L"The collage could not be created.");
      fs::path desired = DesiredOutputPath(files[0], L"jpg", st, L" (collage)");
      OutputTransaction tx(desired, st.overwritePolicy);
      img::SaveOptions so; so.quality = std::max(90, st.imageQuality); so.preserveMetadata = false; so.ctx = &ctx;
      img::SaveResult sr = img::SaveBitmap(tx.TempPath(), out, L"jpg", so);
      if (!sr.ok) return JobResult::Fail(sr.error);
      std::wstring e; if (!tx.Commit(&e)) return JobResult::Fail(e);
      return JobResult::Ok({tx.FinalPath()});
    });
  }

  std::vector<Pic> pics_;
  int sel_ = -1, layout_ = 0, aspect_ = 0, bg_ = 0;
  float spacing_ = 24, radius_ = 32;
  bool dirty_ = true;
  img::Bitmap preview_;
  ComPtr<ID2D1Bitmap> previewBmp_;
};

}  // namespace

ToolDialog* MakeCollage(const Selection& s) { return new CollageDialog(s); }

}  // namespace filo::tools
