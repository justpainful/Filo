// Redact Video: several rectangles, each with its own time window, Blur or Solid style. The preview applies the effect inside the
// time window only (blur is a small CPU box blur of the preview frame; the export uses ffmpeg boxblur / drawbox).
#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools {

using namespace filo::ds;
using namespace filo::ui;
using namespace vid;

namespace {

constexpr float kMinRegion = 16.f;
constexpr uint32_t kColors[] = {0x000000, 0xFFFFFF, 0x4B4B50, 0xE5484D, 0xF5A524, 0x30A46C, 0x3B8BFF, 0x8E4EC6};

struct Region {
  IRect r;
  double start = 0, end = 0;
  bool blur = true;
  uint32_t color = 0x000000;
  int strength = 24;
  ComPtr<ID2D1Bitmap1> blurBmp;      // preview cache
  uint64_t cacheSerial = ~0ull;
  int cacheKey = 0;
};

class RedactVideoDialog : public VideoToolDialog {
 public:
  explicit RedactVideoDialog(const Selection& sel) : VideoToolDialog(sel) {}

 protected:
  DialogOptions Options() const override {
    DialogOptions o; o.title = L"Redact Video"; o.widthDip = 648; o.heightDip = 800; o.resizable = true; o.minWidthDip = 560; o.minHeightDip = 680;
    o.rtl = IsRtl();
    return o;
  }
  std::string ToolId() const override { return "redact-video"; }
  Json SavePreset() const override { Json j = Json::MakeObject(); j["blur"] = defBlur_; j["strength"] = defStrength_; j["color"] = (int)defColor_; return j; }
  void LoadPreset(const Json& j) override {
    if (j.Has("blur")) defBlur_ = j.Get("blur").AsBool(true);
    if (j.Has("strength")) defStrength_ = std::clamp((int)j.Get("strength").AsInt(24), 2, 100);
    if (j.Has("color")) defColor_ = (uint32_t)j.Get("color").AsInt(0) & 0xFFFFFF;
    if (j.Has("regions")) {
      regions_.clear();
      for (const auto& e : j.Get("regions").Elements()) {
        Region r;
        r.r = {(int)e.Get("x").AsInt(), (int)e.Get("y").AsInt(), (int)e.Get("w").AsInt(), (int)e.Get("h").AsInt()};
        r.start = e.Get("start").AsNumber(0); r.end = e.Get("end").AsNumber(Dur());
        r.blur = e.Has("blur") ? e.Get("blur").AsBool(true) : defBlur_;
        r.strength = e.Has("strength") ? (int)e.Get("strength").AsInt(24) : defStrength_;
        r.color = e.Has("color") ? ((uint32_t)e.Get("color").AsInt(0) & 0xFFFFFF) : defColor_;
        regions_.push_back(std::move(r));
      }
      sel_ = regions_.empty() ? -1 : 0;
    }
  }
  void TestApply() override { StartRedact(); }
  void OnDeviceReset(bool rebuilt) override { VideoToolDialog::OnDeviceReset(rebuilt); for (auto& r : regions_) { r.blurBmp.Reset(); r.cacheSerial = ~0ull; } }

  void Draw(Ui& ui, const Rect& content, const Rect&) override {
    BeginFrame(ui);
    const Palette& pal = ui.Pal();
    Painter& p = ui.P();
    const Rect body = BodyAbove(content);
    const float pad = dialog::contentInset;
    const float listH = 104;
    const float below = 10 + 34 + 2 + 34 + 8 + 28 + 6 + listH + 8 + 30 + 8 + 30 + 8;
    const float wellH = std::max(150.f, body.h - 15 - below);
    Rect well{body.x + pad, body.y + 15, body.w - 2 * pad, wellH};
    WellGeom g = DrawWell(ui, well);
    const bool en = Usable() && !Busy() && view_.IsOpen();
    EditRegions(ui, g, en);
    SinkOptions so; so.clickToggles = false; so.arrowsStep = false;
    DrawWellSink(ui, g, so);
    if (Usable()) { Keys(ui); DrawOverlays(ui, g); }

    float y = well.bottom() + 10;
    DrawTransport(ui, {well.x, y, well.w, 34}, -1, 0);
    y += 34 + 2;
    DrawScrubber(ui, {well.x + 10, y, well.w - 20, 34});
    y += 34 + 8;

    // ---- list header
    {
      Rect row{well.x, y, well.w, 28};
      auto c = ui.Cols(row, {Flex(1), Fixed(130)}, 8);
      ui.SectionTitle({c[0].x, row.y + 3, c[0].w, 22}, L"Hidden areas" + (regions_.empty() ? std::wstring() : L"  " + std::to_wstring(regions_.size())));
      if (ui.Button({c[1].x, row.cy() - 13, c[1].w, 26}, L"Add area", ButtonStyle::Secondary, en, "add")) AddRegion();
    }
    y += 28 + 6;
    DrawList(ui, {well.x - 4, y, well.w + 8, listH});
    y += listH + 8;

    // ---- style of the selected area
    const bool has = sel_ >= 0 && sel_ < (int)regions_.size();
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(58), Fixed(150), Flex(1)}, 10);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Style", has ? pal.text : pal.textDisabled, ls);
      if (has) {
        Region& r = regions_[sel_];
        const int ns = ui.Segmented({c[1].x, row.cy() - 13, c[1].w, 26}, {L"Blur", L"Solid"}, r.blur ? 0 : 1, en);
        if (ns != (r.blur ? 0 : 1)) { r.blur = ns == 0; defBlur_ = r.blur; }
        if (r.blur) {
          auto cc = ui.Cols(c[2], {Fixed(66), Flex(1), Fixed(52)}, 8);
          p.Text(cc[0], L"Strength", pal.text, ls);
          float v = (float)r.strength;
          ui.PushId(0x7F10);
          if (ui.Slider({cc[1].x, row.cy() - 13, cc[1].w, 26}, v, 4.f, 80.f, 1.f, 0.f, en)) { r.strength = (int)std::lround(v); defStrength_ = r.strength; }
          ui.PopId();
          TextStyle vs; vs.size = dialog::bodyFontSize; vs.align = Align::End;
          p.Text(cc[2], std::to_wstring(r.strength) + L" px", pal.textSecondary, vs);
        } else {
          const float sw = 26, gap = 9;
          float x = c[2].x;
          if (ui.Rtl()) x = c[2].right() - sw;
          ui.PushId(0x7F11);
          for (int i = 0; i < 8; ++i) {
            ui.PushId(i);
            const Color col = Color::Hex(kColors[i]);
            Rect sr{ui.Rtl() ? x - i * (sw + gap) : x + i * (sw + gap), row.cy() - sw / 2, sw, sw};
            if (ui.Swatch(sr, col, Mix(col, Color(0, 0, 0, 1), 0.12f), r.color == kColors[i], L"") && en) { r.color = kColors[i]; defColor_ = r.color; }
            ui.PopId();
          }
          ui.PopId();
        }
      } else {
        TextStyle hs; hs.size = 12.5f; hs.wrap = true;
        p.Text({c[1].x, row.y, c[1].w + c[2].w + 10, row.h}, L"Draw a rectangle on the picture to hide an area.", pal.textTertiary, hs);
      }
    }
    y += 30 + 8;

    // ---- time window of the selected area
    {
      Rect row{well.x, y, well.w, 30};
      auto c = ui.Cols(row, {Fixed(58), Fixed(96), Fixed(96), Fixed(108), Flex(1)}, 8);
      TextStyle ls; ls.size = dialog::bodyFontSize;
      p.Text(c[0], L"Visible", has ? pal.text : pal.textDisabled, ls);
      const double pos = view_.IsOpen() ? view_.Pos() : 0.0, fr = 1.0 / Fps();
      ui.PushId(0x7F20);
      if (ui.Button({c[1].x, row.cy() - 13, c[1].w, 26}, L"Start here", ButtonStyle::Secondary, en && has && pos < regions_[std::max(0, sel_)].end - 0.1)) regions_[sel_].start = std::round(pos / fr) * fr;
      if (ui.Button({c[2].x, row.cy() - 13, c[2].w, 26}, L"End here", ButtonStyle::Secondary, en && has && pos > regions_[std::max(0, sel_)].start + 0.1)) regions_[sel_].end = std::round(pos / fr) * fr;
      if (ui.Button({c[3].x, row.cy() - 13, c[3].w, 26}, L"Whole video", ButtonStyle::Secondary, en && has)) { regions_[sel_].start = 0; regions_[sel_].end = Dur(); }
      ui.PopId();
      TextStyle ts = MonoStyle(13.f, 500, Align::End);
      if (has) p.Text(c[4], FormatTime(regions_[sel_].start, true) + L" - " + FormatTime(regions_[sel_].end, true), pal.textSecondary, ts);
    }

    // ---- footer
    FooterInfo fi;
    fi.action = L"Redact";
    bool valid = !regions_.empty();
    for (auto& r : regions_) if (r.r.w < 8 || r.r.h < 8) valid = false;
    fi.actionEnabled = Usable() && !Busy() && valid;
    if (!Usable()) { fi.status = ProblemText(); fi.statusIsError = true; }
    else if (regions_.empty()) fi.status = L"Draw on the picture to add an area to hide.";
    else fi.status = std::to_wstring(regions_.size()) + (regions_.size() == 1 ? L" area" : L" areas") + L" will be hidden. The original is never changed.";
    const bool enter = ConsumeEnter();
    FooterResult fr2 = DrawFooter(ui, content, fi);
    if ((fr2.action || enter) && fi.actionEnabled) StartRedact();
    DrawJobPanel(ui, content);
    if (Phase() == JobPhase::Done && LastResult().ok) {
      std::vector<std::wstring> lines;
      lines.push_back(std::to_wstring(regions_.size()) + (regions_.size() == 1 ? L" area hidden" : L" areas hidden"));
      lines.push_back(L"Audio and everything outside the areas are unchanged.");
      DrawDoneNote(ui, content, lines);
    }
  }

 private:
  // ---------------------------------------------------------------- regions
  void AddRegion() {
    const int W = SrcW(), H = SrcH();
    Region r;
    const float w = W * 0.3f, h = H * 0.3f;
    r.r = Quantize(FRect{(W - w) / 2, (H - h) / 2, w, h}, W, H);
    r.start = 0; r.end = Dur(); r.blur = defBlur_; r.strength = defStrength_; r.color = defColor_;
    regions_.push_back(std::move(r));
    sel_ = (int)regions_.size() - 1;
  }

  void Keys(Ui& ui) {
    if (!SinkFocused(ui) || sel_ < 0 || sel_ >= (int)regions_.size()) return;
    const int W = SrcW(), H = SrcH();
    IRect& r = regions_[sel_].r;
    for (const auto& k : ui.In().keys) {
      const int n = k.shift ? 20 : 2;
      if (k.vk == VK_LEFT) r.x = std::max(0, r.x - n);
      else if (k.vk == VK_RIGHT) r.x = std::min((W - r.w) & ~1, r.x + n);
      else if (k.vk == VK_UP) r.y = std::max(0, r.y - n);
      else if (k.vk == VK_DOWN) r.y = std::min((H - r.h) & ~1, r.y + n);
      else if (k.vk == VK_DELETE) { regions_.erase(regions_.begin() + sel_); sel_ = std::min(sel_, (int)regions_.size() - 1); return; }
    }
  }

  void EditRegions(Ui& ui, const WellGeom& g, bool en) {
    const int W = SrcW(), H = SrcH();
    if (!Usable() || W <= 0) return;
    const Rect img = g.image;
    const InputState& in = ui.In();
    Interaction it = ui.Behavior(img.Inset(-12), ui.Id("regionedit"), en, false);
    auto screen = [&](const Region& r) { return VideoToScreen(g, (float)r.r.x, (float)r.r.y, (float)r.r.w, (float)r.r.h); };
    const float vx = (in.mx - img.x) / g.scale, vy = (in.my - img.y) / g.scale;
    // hover info (cursor)
    int hoverFlags = 0; bool hoverInside = false;
    if (sel_ >= 0 && sel_ < (int)regions_.size()) hoverFlags = HitHandle(screen(regions_[sel_]), in.mx, in.my, 10.f, &hoverInside);
    if (it.pressed) {
      bool begun = false;
      if (sel_ >= 0 && sel_ < (int)regions_.size()) {
        const Region& r = regions_[sel_];
        const FRect fr{(float)r.r.x, (float)r.r.y, (float)r.r.w, (float)r.r.h};
        if (hoverFlags) { drag_.Begin(2, hoverFlags, vx, vy, fr); begun = true; }
        else if (hoverInside) { drag_.Begin(1, 0, vx, vy, fr); begun = true; }
      }
      if (!begun) for (int i = (int)regions_.size() - 1; i >= 0; --i) {
        if (screen(regions_[i]).Contains(in.mx, in.my)) {
          sel_ = i;
          const Region& r = regions_[i];
          drag_.Begin(1, 0, vx, vy, {(float)r.r.x, (float)r.r.y, (float)r.r.w, (float)r.r.h});
          begun = true; break;
        }
      }
      if (!begun) {
        Region r;
        const float sx = std::clamp(vx, 0.f, (float)W), sy = std::clamp(vy, 0.f, (float)H);
        r.r = {(int)sx & ~1, (int)sy & ~1, 2, 2};
        r.start = 0; r.end = Dur(); r.blur = defBlur_; r.strength = defStrength_; r.color = defColor_;
        regions_.push_back(std::move(r));
        sel_ = (int)regions_.size() - 1;
        creating_ = true;
        drag_.Begin(3, 0, sx, sy, {sx, sy, 0, 0});
      }
    }
    if (drag_.Active()) {
      if (in.down && sel_ >= 0 && sel_ < (int)regions_.size()) {
        FRect nr = drag_.Update(vx, vy, W, H, 0.0, kMinRegion);
        regions_[sel_].r = Quantize(nr, W, H);
        ui.RequestFrame();
        if (drag_.mode == 1) ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
        else if (drag_.mode == 2) ui.SetCursor(CursorForFlags(drag_.flags));
        else ui.SetCursor((int)(intptr_t)IDC_CROSS);
      } else {
        drag_.End();
        if (creating_ && sel_ >= 0 && sel_ < (int)regions_.size() && (regions_[sel_].r.w < kMinRegion || regions_[sel_].r.h < kMinRegion)) { regions_.erase(regions_.begin() + sel_); sel_ = (int)regions_.size() - 1; }
        creating_ = false;
      }
    } else if (it.hot) {
      if (hoverFlags) ui.SetCursor(CursorForFlags(hoverFlags));
      else if (hoverInside) ui.SetCursor((int)(intptr_t)IDC_SIZEALL);
      else ui.SetCursor((int)(intptr_t)IDC_CROSS);
    }
  }

  // ---------------------------------------------------------------- preview effect + outlines
  void BuildBlur(Region& r, const WellGeom&) {
    const int fw = view_.FrameW(), fh = view_.FrameH();
    const std::vector<uint8_t>& px = view_.Pixels();
    if (fw <= 0 || fh <= 0 || px.size() < (size_t)fw * fh * 4 || !dc_) return;
    const int key = r.r.x * 31 + r.r.y * 17 + r.r.w * 13 + r.r.h * 7 + r.strength * 3;
    if (r.blurBmp && r.cacheSerial == view_.Serial() && r.cacheKey == key) return;
    const float sx = (float)fw / std::max(1, SrcW()), sy = (float)fh / std::max(1, SrcH());
    const int x0 = std::clamp((int)std::floor(r.r.x * sx), 0, fw - 1), y0 = std::clamp((int)std::floor(r.r.y * sy), 0, fh - 1);
    const int x1 = std::clamp((int)std::ceil((r.r.x + r.r.w) * sx), x0 + 1, fw), y1 = std::clamp((int)std::ceil((r.r.y + r.r.h) * sy), y0 + 1, fh);
    const int rw = x1 - x0, rh = y1 - y0;
    int d = 1; while ((rw / d) * (rh / d) > 60000) ++d;
    const int dw = std::max(3, rw / d), dh = std::max(3, rh / d);
    std::vector<uint8_t> sub((size_t)dw * dh * 4);
    for (int y = 0; y < dh; ++y) {
      const int ya = y0 + (int)((int64_t)y * rh / dh), yb = std::max(ya + 1, y0 + (int)((int64_t)(y + 1) * rh / dh));
      for (int x = 0; x < dw; ++x) {
        const int xa = x0 + (int)((int64_t)x * rw / dw), xb = std::max(xa + 1, x0 + (int)((int64_t)(x + 1) * rw / dw));
        int s[3] = {0, 0, 0}, n = 0;
        for (int yy = ya; yy < yb && yy < fh; ++yy) for (int xx = xa; xx < xb && xx < fw; ++xx) { const uint8_t* q = &px[((size_t)yy * fw + xx) * 4]; s[0] += q[0]; s[1] += q[1]; s[2] += q[2]; ++n; }
        n = std::max(n, 1);
        uint8_t* o = &sub[((size_t)y * dw + x) * 4];
        o[0] = (uint8_t)(s[0] / n); o[1] = (uint8_t)(s[1] / n); o[2] = (uint8_t)(s[2] / n); o[3] = 255;
      }
    }
    const int radius = std::max(1, (int)std::lround(r.strength * sx / d));
    BoxBlurBGRA(sub, dw, dh, radius);
    r.blurBmp.Reset();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(dw, dh), sub.data(), dw * 4, &bp, &r.blurBmp))) { r.cacheSerial = view_.Serial(); r.cacheKey = key; }
  }

  void DrawOverlays(Ui& ui, const WellGeom& g) {
    Painter& p = ui.P();
    const double pos = view_.IsOpen() ? view_.Pos() : 0.0, dur = Dur();
    for (int i = 0; i < (int)regions_.size(); ++i) {
      Region& r = regions_[i];
      const Rect sr = VideoToScreen(g, (float)r.r.x, (float)r.r.y, (float)r.r.w, (float)r.r.h);
      const bool active = pos >= r.start - 1e-3 && (pos <= r.end + 1e-3 || r.end >= dur - 1e-3);
      if (active && view_.Bitmap()) {
        if (r.blur) { BuildBlur(r, g); if (r.blurBmp) p.Bitmap(r.blurBmp.Get(), sr, 1.f, 0.f, true); }
        else p.FillRect(sr, Color::Hex(r.color));
      }
      const bool sel = i == sel_;
      const Color line = sel ? accent::base : Color(1, 1, 1, active ? 0.85f : 0.40f);
      p.StrokeRoundRect(sr, 0, Color(0, 0, 0, 0.35f), sel ? 3.5f : 2.5f);
      p.StrokeRoundRect(sr, 0, line, sel ? 2.f : 1.3f);
      // number badge
      Rect b{sr.x + 4, sr.y + 4, 20, 18};
      if (sr.w > 40 && sr.h > 30) {
        p.FillRoundRect(b, 6, sel ? accent::base : Color(0, 0, 0, 0.62f));
        TextStyle ts; ts.size = 11.5f; ts.weight = 700; ts.align = Align::Center;
        p.Text(b, std::to_wstring(i + 1), Color(1, 1, 1, 0.97f), ts);
      }
      if (sel) {
        const float hs = 9;
        const float xs[3] = {sr.x, sr.cx(), sr.right()}, ys[3] = {sr.y, sr.cy(), sr.bottom()};
        for (int a = 0; a < 3; ++a) for (int c = 0; c < 3; ++c) {
          if (a == 1 && c == 1) continue;
          if ((a == 1 || c == 1) && (sr.w < 60 || sr.h < 60) && false) continue;
          Rect h{xs[a] - hs / 2, ys[c] - hs / 2, hs, hs};
          p.FillRoundRect(h, 3, Color(1, 1, 1, 0.98f));
          p.StrokeRoundRect(h, 3, accent::base, 1.5f);
        }
      }
    }
  }

  // ---------------------------------------------------------------- list with per-area mini timelines
  void DrawList(Ui& ui, const Rect& vp) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    if (regions_.empty()) {
      p.FillRoundRect(vp.Inset(4, 0), control::listRowRadius, Color(pal.text.r, pal.text.g, pal.text.b, 0.045f));
      p.StrokeRoundRect(vp.Inset(4, 0), control::listRowRadius, pal.divider, 1.f);
      TextStyle es; es.size = 13; es.align = Align::Center; es.wrap = true;
      p.Icon("redact", vp.cx(), vp.y + vp.h * 0.34f, 22, pal.textTertiary, 1.6f);
      p.Text({vp.x + 30, vp.y + vp.h * 0.34f + 16, vp.w - 60, 36}, L"Drag on the picture to cover a face, a plate or any detail", pal.textTertiary, es);
      return;
    }
    const float rowH = 40, gap = 6;
    const float contentH = regions_.size() * (rowH + gap);
    const float off = ui.BeginScroll(vp, contentH, ui.Id("regions"));
    const double dur = Dur();
    int removeIdx = -1;
    for (int i = 0; i < (int)regions_.size(); ++i) {
      Rect row{vp.x + 4, vp.y + i * (rowH + gap) - off, vp.w - 8 - (contentH > vp.h ? 8.f : 0.f), rowH};
      if (row.bottom() < vp.y - 2 || row.y > vp.bottom() + 2) continue;
      Region& r = regions_[i];
      ui.PushId(i);
      const bool sel = i == sel_;
      auto c = ui.Cols(row.Inset(10, 0), {Fixed(24), Fixed(54), Flex(1), Fixed(120), Fixed(24)}, 10);
      // priority: delete button and mini timeline first, then the row itself
      Interaction idel = ui.Behavior({c[4].x, row.cy() - 12, 24, 24}, ui.Id("del"), !Busy(), false);
      const Rect bar{c[2].x, row.cy() - 9, c[2].w, 18};
      MiniBar(ui, bar, i, r, dur);
      Interaction it = ui.Behavior(row, ui.Id("row"), !Busy(), false);
      const float hov = ui.Anim(ui.Id("hov"), it.hot ? 1.f : 0.f);
      p.FillRoundRect(row, 12, Mix(pal.fieldFill, accent::base.WithAlpha(sel ? 0.20f : 0.10f), std::max(hov, sel ? 1.f : 0.f)));
      p.StrokeRoundRect(row, 12, Mix(pal.divider, accent::base.WithAlpha(0.55f), sel ? 1.f : hov), sel ? 1.5f : 1.f);
      DrawMiniBar(ui, bar, r, dur, sel);
      TextStyle ns; ns.size = 12.5f; ns.weight = 700; ns.align = Align::Center;
      p.FillCircle(c[0].cx(), row.cy(), 11, sel ? accent::base : Color(pal.text.r, pal.text.g, pal.text.b, 0.16f));
      p.Text(c[0], std::to_wstring(i + 1), sel ? Color(1, 1, 1, 1) : pal.text, ns);
      TextStyle st; st.size = 13; st.weight = 500;
      if (r.blur) p.Text(c[1], L"Blur", pal.text, st);
      else {
        p.FillRoundRect({c[1].x, row.cy() - 7, 14, 14}, 4, Color::Hex(r.color));
        p.StrokeRoundRect({c[1].x, row.cy() - 7, 14, 14}, 4, Color(0, 0, 0, 0.25f), 1.f);
        p.Text({c[1].x + 20, c[1].y, c[1].w - 20, c[1].h}, L"Solid", pal.text, st);
      }
      p.Text(c[3], FormatTime(r.start, false) + L" - " + FormatTime(r.end, false), pal.textSecondary, MonoStyle(12.5f, 400, Align::End));
      p.FillCircle(c[4].cx(), row.cy(), 11, idel.hot ? Color(0.9f, 0.25f, 0.25f, 0.9f) : Color(0, 0, 0, 0));
      p.Icon("close", c[4].cx(), row.cy(), 12, idel.hot ? Color(1, 1, 1, 1) : pal.textSecondary, 1.8f);
      if (idel.clicked) removeIdx = i;
      else if (it.clicked && !miniMoved_) { sel_ = i; }
      ui.PopId();
    }
    ui.EndScroll();
    if (removeIdx >= 0) { regions_.erase(regions_.begin() + removeIdx); sel_ = std::min(sel_, (int)regions_.size() - 1); }
  }

  void DrawMiniBar(Ui& ui, const Rect& bar, const Region& r, double dur, bool sel) {
    Painter& p = ui.P();
    const Palette& pal = ui.Pal();
    p.FillRoundRect(bar.Inset(0, 5), 4, pal.trackInactive);
    const float xa = bar.x + (float)(std::clamp(r.start, 0.0, dur) / dur) * bar.w, xb = bar.x + (float)(std::clamp(r.end, 0.0, dur) / dur) * bar.w;
    p.FillRoundRect({xa, bar.y + 3, std::max(6.f, xb - xa), bar.h - 6}, 6, accent::base.WithAlpha(sel ? 0.95f : 0.65f));
    p.FillRoundRect({xa + 1, bar.y + 5, 3, bar.h - 10}, 1.5f, Color(1, 1, 1, 0.85f));
    p.FillRoundRect({std::max(xa + 5, xb - 4), bar.y + 5, 3, bar.h - 10}, 1.5f, Color(1, 1, 1, 0.85f));
    // playhead
    const float px = bar.x + (float)(std::clamp(view_.IsOpen() ? view_.Pos() : 0.0, 0.0, dur) / dur) * bar.w;
    p.FillRect({px - 0.75f, bar.y + 1, 1.5f, bar.h - 2}, pal.text);
  }

  void MiniBar(Ui& ui, const Rect& bar, int idx, Region& r, double dur) {
    Interaction it = ui.Behavior(bar.Inset(-3, -4), ui.Id("mini"), !Busy(), false);
    const InputState& in = ui.In();
    const float xa = bar.x + (float)(r.start / dur) * bar.w, xb = bar.x + (float)(r.end / dur) * bar.w;
    if (it.pressed) {
      sel_ = idx; miniMoved_ = false;
      miniRow_ = idx;
      if (std::fabs(in.mx - xa) <= 8 && std::fabs(in.mx - xa) <= std::fabs(in.mx - xb)) miniMode_ = 1;
      else if (std::fabs(in.mx - xb) <= 8) miniMode_ = 2;
      else if (in.mx > xa && in.mx < xb) { miniMode_ = 3; miniGrab_ = (in.mx - bar.x) / bar.w * dur - r.start; }
      else miniMode_ = (std::fabs(in.mx - xa) < std::fabs(in.mx - xb)) ? 1 : 2;
    }
    if (it.active && miniRow_ == idx && miniMode_) {
      const double fr = 1.0 / Fps();
      const double t = std::clamp((double)((in.mx - bar.x) / bar.w), 0.0, 1.0) * dur;
      if (it.dragging) miniMoved_ = true;
      if (miniMode_ == 1) { r.start = std::clamp(std::round(t / fr) * fr, 0.0, r.end - 0.1); if (view_.IsOpen()) view_.Seek(r.start, true); }
      else if (miniMode_ == 2) { r.end = std::clamp(std::round(t / fr) * fr, r.start + 0.1, dur); if (view_.IsOpen()) view_.Seek(std::max(0.0, r.end - fr), true); }
      else { const double len = r.end - r.start; r.start = std::clamp(std::round((t - miniGrab_) / fr) * fr, 0.0, dur - len); r.end = r.start + len; }
      ui.RequestFrame();
    }
    if (it.released) { if (miniRow_ == idx) miniMode_ = 0; }
    if (it.hot) ui.SetCursor((int)(intptr_t)IDC_SIZEWE);
  }

  void StartRedact() {
    media::RedactRequest req;
    const double dur = Dur();
    for (auto& r : regions_) {
      media::RedactRegion q;
      q.x = r.r.x; q.y = r.r.y; q.w = r.r.w; q.h = r.r.h;
      q.start = r.start <= 0.001 ? 0 : r.start;
      q.end = r.end >= dur - 0.001 ? 0 : r.end;
      q.style = r.blur ? media::RedactStyle::Blur : media::RedactStyle::Solid;
      q.color = r.color; q.blurStrength = r.strength;
      req.regions.push_back(q);
    }
    Settings st = SettingsStore::Get().Snapshot();
    req.hardware = st.hardwareAcceleration; req.crf = 0; req.stripMetadata = !st.preserveMetadata;
    const fs::path in = path_;
    RunJob(L"Hiding areas...", [in, req](JobContext& ctx) -> JobResult { return media::RedactVideo(in, req, ctx); });
  }

  std::vector<Region> regions_;
  int sel_ = -1;
  RectDrag drag_;
  bool creating_ = false, miniMoved_ = false;
  int miniRow_ = -1, miniMode_ = 0;
  double miniGrab_ = 0;
  bool defBlur_ = true;
  int defStrength_ = 24;
  uint32_t defColor_ = 0x000000;
};

} // namespace

ToolDialog* MakeRedactVideo(const Selection& sel) { return new RedactVideoDialog(sel); }

} // namespace filo::tools
