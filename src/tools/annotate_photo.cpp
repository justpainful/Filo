// Annotate - draw on a photo: pen, marker, arrow, box, oval and text, in a few colours. The marks are burned into a new copy at the
// image's full resolution with the same drawing code the preview uses (what you see is what is saved). Undo removes the last mark.
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "core/output.h"
#include "core/settings.h"
#include "engines/image/bitmap.h"
#include "ui/canvas.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;

namespace {

enum class Kind { Pen, Marker, Arrow, Box, Oval, Text };

struct Shape {
  Kind kind = Kind::Pen;
  Color color;
  float width = 4;                                  // base-image pixels
  std::vector<std::pair<float, float>> pts;         // Pen/Marker: path; others: [start, end]; Text: [anchor]
  std::wstring text;
};

const Color kColors[] = {Color::Hex(0xD8383A), Color::Hex(0xF5841F), Color::Hex(0xF5C518), Color::Hex(0x2FA84F), Color::Hex(0x3B8BFF), Color::Hex(0x000000), Color::Hex(0xFFFFFF)};
constexpr int kNumColors = 7;

// One drawing routine for the preview (target = screen DIPs) and the export (target = full-size pixels): x' = ox + x * k.
void DrawShape(Painter& p, const Shape& s, float ox, float oy, float k) {
  auto X = [&](float v) { return ox + v * k; };
  auto Y = [&](float v) { return oy + v * k; };
  const float w = std::max(1.f, s.width * k);
  switch (s.kind) {
    case Kind::Pen: case Kind::Marker: {
      std::vector<std::pair<float, float>> pts; pts.reserve(s.pts.size());
      for (auto& q : s.pts) pts.push_back({X(q.first), Y(q.second)});
      if (s.kind == Kind::Marker) p.Polyline(pts, s.color.WithAlpha(0.38f), w * 3.2f);
      else p.Polyline(pts, s.color, w);
      break; }
    case Kind::Arrow: {
      if (s.pts.size() < 2) break;
      const float x0 = X(s.pts[0].first), y0 = Y(s.pts[0].second), x1 = X(s.pts[1].first), y1 = Y(s.pts[1].second);
      const float len = std::hypot(x1 - x0, y1 - y0);
      if (len < 1) break;
      const float ang = std::atan2(y1 - y0, x1 - x0), hl = std::min(len * 0.6f, 5.f * w + 8.f * k), ha = 0.52f;
      // shorten the shaft so the round cap does not poke through the tip
      p.Line(x0, y0, x1 - std::cos(ang) * hl * 0.45f, y1 - std::sin(ang) * hl * 0.45f, s.color, w, true);
      p.Polyline({{x1 - hl * std::cos(ang - ha), y1 - hl * std::sin(ang - ha)}, {x1, y1}, {x1 - hl * std::cos(ang + ha), y1 - hl * std::sin(ang + ha)}}, s.color, w);
      break; }
    case Kind::Box: case Kind::Oval: {
      if (s.pts.size() < 2) break;
      const float x0 = X(std::min(s.pts[0].first, s.pts[1].first)), y0 = Y(std::min(s.pts[0].second, s.pts[1].second));
      const float x1 = X(std::max(s.pts[0].first, s.pts[1].first)), y1 = Y(std::max(s.pts[0].second, s.pts[1].second));
      if (s.kind == Kind::Box) p.StrokeRoundRect({x0, y0, x1 - x0, y1 - y0}, w * 0.6f, s.color, w);
      else p.StrokeEllipse((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2, s.color, w);
      break; }
    case Kind::Text: {
      if (s.pts.empty() || s.text.empty()) break;
      TextStyle ts; ts.size = (14.f + s.width * 2.5f) * k; ts.weight = 700; ts.vcenter = false; ts.ellipsis = false; ts.wrap = false;
      const float x = X(s.pts[0].first), y = Y(s.pts[0].second);
      const Color shadow = (s.color.r + s.color.g + s.color.b) > 2.2f ? Color(0, 0, 0, 0.55f) : Color(1, 1, 1, 0.55f);
      p.Text({x + 1.2f * k, y + 1.2f * k, 4000, ts.size * 1.6f}, s.text, shadow, ts);
      p.Text({x, y, 4000, ts.size * 1.6f}, s.text, s.color, ts);
      break; }
  }
}

class AnnotateDialog : public ToolDialog {
 public:
  explicit AnnotateDialog(const Selection& sel) {
    selection_ = sel;
    if (sel.items.empty()) return;
    path_ = sel.items[0].path;
    img::LoadOptions lo; lo.maxSide = 1600;
    if (!img::LoadBitmap(path_, base_, lo, &err_)) { base_ = {}; if (err_.empty()) err_ = L"This image cannot be opened."; }
  }

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Annotate"; o.widthDip = 660; o.heightDip = 720; o.resizable = true; o.minWidthDip = 520; o.minHeightDip = 520;
    o.rtl = SettingsStore::Get().Snapshot().language == L"ar";
    return o;
  }
  std::string ToolId() const override { return "annotate"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["tool"] = (int)tool_; j["color"] = color_; j["width"] = width_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("tool")) tool_ = (Kind)std::clamp((int)j.Get("tool").AsInt(), 0, 5);
    if (j.Has("color")) color_ = std::clamp((int)j.Get("color").AsInt(), 0, kNumColors - 1);
    if (j.Has("width")) width_ = (float)std::clamp(j.Get("width").AsNumber(4), 1.0, 24.0);
    if (j.Has("shapes")) for (const Json& e : j.Get("shapes").Elements()) {       // automation: {"k":0..5,"c":idx,"w":px,"p":[[x,y],...] normalised 0..1,"t":"text"}
      Shape s; s.kind = (Kind)std::clamp((int)e.Get("k").AsInt(), 0, 5); s.color = kColors[std::clamp((int)e.Get("c").AsInt(), 0, kNumColors - 1)];
      s.width = (float)e.Get("w").AsNumber(width_); s.text = Widen(e.Get("t").AsString());
      for (const Json& q : e.Get("p").Elements()) if (q.Size() == 2) s.pts.push_back({(float)(q.At(0).AsNumber() * base_.width), (float)(q.At(1).AsNumber() * base_.height)});
      shapes_.push_back(std::move(s));
    }
  }
  void TestApply() override { Start(); }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    const Palette& pal = ui.Pal(); Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset, controlsH = 112;
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, std::max(180.f, body.h - 15 - controlsH - 12)};
    p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
    if (!base_.Empty()) DrawCanvas(ui, well);
    else { TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true; p.Text(well.Inset(30), err_, Color(1, 1, 1, 0.65f), es); }

    const float inset = 24; float y = well.bottom() + 12;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 30};
      auto c = ui.Cols(row, {Flex(1), Fixed(70), Fixed(70)}, 8);
      ui.PushId(0x7001);
      const int t = ui.Segmented(c[0], {L"Pen", L"Marker", L"Arrow", L"Box", L"Oval", L"Text"}, (int)tool_);
      tool_ = (Kind)t;
      if (ui.Button(c[1], L"Undo", ButtonStyle::Secondary, !shapes_.empty() && !Busy())) shapes_.pop_back();
      if (ui.Button(c[2], L"Clear", ButtonStyle::Quiet, !shapes_.empty() && !Busy())) shapes_.clear();
      ui.PopId();
    }
    y += 38;
    {
      Rect row{body.x + inset, y, body.w - 2 * inset, 30};
      auto c = ui.Cols(row, {Fixed(kNumColors * 34.f), Flex(1)}, 12);
      ui.PushId(0x7002);
      for (int i = 0; i < kNumColors; ++i) {
        ui.PushId(i);
        Rect sw{c[0].x + i * 34.f, c[0].cy() - 14, 28, 28};
        if (ui.Swatch(ui.Mirror(sw, c[0]), kColors[i], kColors[i], color_ == i)) color_ = i;
        ui.PopId();
      }
      if (tool_ == Kind::Text) ui.TextInput(c[1], text_, L"Type, then click the picture");
      else ui.Slider(c[1], width_, 1.f, 24.f, 1.f);
      ui.PopId();
    }
    FooterInfo fi;
    fi.action = L"Save a Copy"; fi.actionEnabled = !base_.Empty() && !shapes_.empty() && !Busy();
    fi.status = shapes_.empty() ? L"Draw on the picture; the original is never changed." : std::to_wstring(shapes_.size()) + (shapes_.size() == 1 ? L" mark" : L" marks");
    if (DrawFooter(ui, content, fi).action) Start();
    DrawJobPanel(ui, content);
  }

 private:
  void DrawCanvas(Ui& ui, const Rect& well) {
    Painter& p = ui.P();
    ui.PushId(0x3AA);
    CanvasNavigate(ui, well, view_, base_.width, base_.height, ui.Id("cv"), 22.f, false);
    CanvasMap m = MapCanvas(well, base_.width, base_.height, view_, 22.f);
    if (!bmp_ && dc_) bmp_ = MakeBitmap(dc_.Get(), base_);
    p.PushClipRoundRect(well, dialog::wellRadius);
    if (bmp_) p.Bitmap(bmp_.Get(), m.image);
    p.PushClip(m.image);
    for (const Shape& s : shapes_) DrawShape(p, s, m.image.x, m.image.y, m.scale);
    p.PopClip();
    const InputState& in = ui.In();
    const bool inside = m.image.Contains(in.mx, in.my) && well.Contains(in.mx, in.my);
    auto clampX = [&](float v) { return std::clamp(m.ToImageX(v), 0.f, (float)base_.width); };
    auto clampY = [&](float v) { return std::clamp(m.ToImageY(v), 0.f, (float)base_.height); };
    if (in.pressed && inside && !in.shift && !Busy()) {
      if (tool_ == Kind::Text) {
        if (!text_.empty()) { Shape s; s.kind = Kind::Text; s.color = kColors[color_]; s.width = width_; s.text = text_; s.pts = {{clampX(in.mx), clampY(in.my)}}; shapes_.push_back(std::move(s)); }
      } else {
        drawing_ = true;
        Shape s; s.kind = tool_; s.color = kColors[color_]; s.width = width_;
        s.pts = {{clampX(in.mx), clampY(in.my)}};
        if (tool_ == Kind::Arrow || tool_ == Kind::Box || tool_ == Kind::Oval) s.pts.push_back(s.pts[0]);
        shapes_.push_back(std::move(s));
      }
    }
    if (drawing_ && !shapes_.empty()) {
      Shape& s = shapes_.back();
      if (in.down) {
        const float x = clampX(in.mx), y = clampY(in.my);
        if (s.kind == Kind::Pen || s.kind == Kind::Marker) { if (std::hypot(x - s.pts.back().first, y - s.pts.back().second) * m.scale > 1.5f) s.pts.push_back({x, y}); }
        else s.pts[1] = {x, y};
        ui.RequestFrame();
      } else {
        drawing_ = false;
        if ((s.kind == Kind::Arrow || s.kind == Kind::Box || s.kind == Kind::Oval) && std::hypot(s.pts[1].first - s.pts[0].first, s.pts[1].second - s.pts[0].second) < 4) shapes_.pop_back();
      }
    }
    if (inside && !Busy()) ui.SetCursor((int)(intptr_t)(tool_ == Kind::Text ? IDC_IBEAM : IDC_CROSS));
    p.PopClip();
    ui.PopId();
  }

  static ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1DeviceContext* dc, const img::Bitmap& bm) {
    std::vector<uint8_t> t = bm.px;
    for (size_t i = 0; i < (size_t)bm.width * bm.height; ++i) { uint8_t* q = &t[i * 4]; const uint32_t a = q[3]; if (a != 255) { q[0] = (uint8_t)(q[0] * a / 255); q[1] = (uint8_t)(q[1] * a / 255); q[2] = (uint8_t)(q[2] * a / 255); } }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> b;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(bm.width, bm.height), t.data(), bm.width * 4, &bp, &b))) return nullptr;
    return ComPtr<ID2D1Bitmap>(static_cast<ID2D1Bitmap*>(b.Get()));
  }

  // Burns the marks into `full` (straight-alpha BGRA) with Direct2D on an offscreen target.
  bool Burn(img::Bitmap& full, float k, std::wstring* err) {
    auto& dev = gfx::Device::Get();
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(dev.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) { *err = L"Graphics are not available."; return false; }
    D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(full.width, full.height), nullptr, 0, &tp, &target))) { *err = L"The image is too large to annotate on this PC."; return false; }
    ComPtr<ID2D1Bitmap> src = MakeBitmap(dc.Get(), full);
    if (!src) { *err = L"The image is too large to annotate on this PC."; return false; }
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawBitmap(src.Get(), D2D1::RectF(0, 0, (float)full.width, (float)full.height), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
    Painter pp; pp.Begin(dc.Get(), 96.f, false);
    for (const Shape& s : shapes_) DrawShape(pp, s, 0, 0, k);
    pp.End();
    if (FAILED(dc->EndDraw())) { *err = L"Drawing failed."; return false; }
    dc->SetTarget(nullptr);
    D2D1_BITMAP_PROPERTIES1 rp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(full.width, full.height), nullptr, 0, &rp, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr))) { *err = L"Could not read the result back."; return false; }
    D2D1_MAPPED_RECT mr;
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) { *err = L"Could not read the result back."; return false; }
    for (int y = 0; y < full.height; ++y) {
      const uint8_t* s = mr.bits + (size_t)y * mr.pitch; uint8_t* d = full.Row(y);
      for (int x = 0; x < full.width; ++x) {
        const uint32_t a = s[x * 4 + 3];
        if (a == 255 || a == 0) { d[x * 4] = s[x * 4]; d[x * 4 + 1] = s[x * 4 + 1]; d[x * 4 + 2] = s[x * 4 + 2]; d[x * 4 + 3] = (uint8_t)a; }
        else { for (int c = 0; c < 3; ++c) d[x * 4 + c] = (uint8_t)std::min(255u, (s[x * 4 + c] * 255u + a / 2) / a); d[x * 4 + 3] = (uint8_t)a; }
      }
    }
    cpu->Unmap();
    return true;
  }

  void Start() {
    if (base_.Empty() || shapes_.empty() || Busy()) return;
    auto full = std::make_shared<img::Bitmap>(); std::wstring err;
    img::LoadOptions lo;
    if (!img::LoadBitmap(path_, *full, lo, &err)) { RunJob(L"Annotating...", [err](JobContext&) { return JobResult::Fail(err.empty() ? L"The image could not be opened." : err); }); return; }
    if (full->width > 16000 || full->height > 16000) { RunJob(L"Annotating...", [](JobContext&) { return JobResult::Fail(L"The image is too large to annotate."); }); return; }
    if (!Burn(*full, (float)full->width / (float)base_.width, &err)) { RunJob(L"Annotating...", [err](JobContext&) { return JobResult::Fail(err); }); return; }
    fs::path in = path_; Settings st = SettingsStore::Get().Snapshot();
    RunJob(L"Saving...", [full, in, st](JobContext& ctx) -> JobResult {
      std::wstring fmt = full->info.format; if (!img::CanEncode(fmt)) fmt = L"png";
      fs::path desired = DesiredOutputPath(in, fmt, st, L" (annotated)");
      OutputTransaction tx(desired, st.overwritePolicy);
      img::SaveOptions so; so.quality = std::max(90, st.imageQuality); so.preserveMetadata = st.preserveMetadata; so.ctx = &ctx;
      img::SaveResult sr = img::SaveBitmap(tx.TempPath(), *full, fmt, so);
      if (!sr.ok) return JobResult::Fail(sr.error);
      std::wstring e; if (!tx.Commit(&e)) return JobResult::Fail(e);
      JobResult r = JobResult::Ok({tx.FinalPath()}); r.warnings = sr.warnings; return r;
    });
  }

  fs::path path_;
  img::Bitmap base_;
  std::wstring err_, text_ = L"Text";
  std::vector<Shape> shapes_;
  Kind tool_ = Kind::Pen;
  int color_ = 0;
  float width_ = 4.f;
  bool drawing_ = false;
  CanvasView view_;
  ComPtr<ID2D1Bitmap> bmp_;
};

}  // namespace

ToolDialog* MakeAnnotatePhoto(const Selection& s) { return new AnnotateDialog(s); }

}  // namespace filo::tools
