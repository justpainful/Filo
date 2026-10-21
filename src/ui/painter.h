// Painter: thin Direct2D/DirectWrite drawing helper used by every Filo UI surface (dialogs, shelf, HUD, settings).
// Works in DIPs: the D2D context DPI is set to the window DPI so layout is resolution independent and text stays crisp.
#pragma once
#include "gfx/device.h"
#include "design/tokens.h"
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <list>

namespace filo::ui {

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  float cx() const { return x + w * 0.5f; }
  float cy() const { return y + h * 0.5f; }
  bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  Rect Inset(float d) const { return {x + d, y + d, std::max(0.f, w - 2 * d), std::max(0.f, h - 2 * d)}; }
  Rect Inset(float dx, float dy) const { return {x + dx, y + dy, std::max(0.f, w - 2 * dx), std::max(0.f, h - 2 * dy)}; }
  Rect Offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  D2D1_RECT_F D2D() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Align { Start, Center, End };   // Start/End follow the text direction (RTL aware)

struct TextStyle {
  float size = ds::dialog::bodyFontSize;
  int weight = 400;                    // DWRITE_FONT_WEIGHT values (400 regular, 500 medium, 600 semibold, 700 bold)
  Align align = Align::Start;
  bool mono = false;
  bool ellipsis = true;                // trim with "..." when it does not fit
  bool wrap = false;
  float tracking = 0;                  // extra letter spacing in DIP
  bool vcenter = true;
  bool operator==(const TextStyle& o) const {
    return size == o.size && weight == o.weight && align == o.align && mono == o.mono && ellipsis == o.ellipsis && wrap == o.wrap && tracking == o.tracking && vcenter == o.vcenter;
  }
};

class Painter {
 public:
  Painter() = default;
  // Binds to a render target for one frame. `dpi` is the window DPI; `rtl` right-to-left reading order for text.
  void Begin(ID2D1DeviceContext* dc, float dpi, bool rtl);
  void End();
  ID2D1DeviceContext* DC() const { return dc_; }
  float Dpi() const { return dpi_; }
  bool Rtl() const { return rtl_; }
  void ClearRoundRect(const Rect& r, float radius);

  // --- shapes
  void FillRect(const Rect& r, const ds::Color& c);
  void FillRoundRect(const Rect& r, float radius, const ds::Color& c);
  void StrokeRoundRect(const Rect& r, float radius, const ds::Color& c, float width = 1.f);
  void FillRoundRectGradient(const Rect& r, float radius, const ds::Color& top, const ds::Color& bottom);
  void GlassClose(const Rect& r, bool dark, float hover, float pressed, bool enabled, bool live);
  void AcrylicBody(const Rect& r, float radius, bool dark, float intensity);
  void LiquidBody(const Rect& r, float radius, bool dark, float intensity);
  void LiquidRim(const Rect& r, float radius, bool dark, float reflections);
  void FillCircle(float cx, float cy, float radius, const ds::Color& c);
  void StrokeCircle(float cx, float cy, float radius, const ds::Color& c, float width = 1.f);
  void Polyline(const std::vector<std::pair<float, float>>& pts, const ds::Color& c, float width);   // one stroked path, round caps/joins (no alpha build-up at joints)
  void StrokeEllipse(float cx, float cy, float rx, float ry, const ds::Color& c, float width = 1.f);
  void Line(float x0, float y0, float x1, float y1, const ds::Color& c, float width = 1.f, bool round = false);
  void Shadow(const Rect& r, float radius, float blur, const ds::Color& c, float dy = 0);  // soft drop shadow (D2D shadow effect)
  void PushClipRoundRect(const Rect& r, float radius);
  void PushClip(const Rect& r);
  void PopClip();   // pops the most recent clip of either kind
  void PushRotate(float cx, float cy, float degrees);   // rotates everything drawn until PopTransform (DIP coordinates)
  void PopTransform();

  // --- text
  void Text(const Rect& r, const std::wstring& s, const ds::Color& c, const TextStyle& st = {});
  float TextWidth(const std::wstring& s, const TextStyle& st);
  struct Metrics { float w, h; int lines; };
  Metrics Measure(const std::wstring& s, const TextStyle& st, float maxW);
  // caret/selection support for text inputs: x offset of character index `pos` (UTF-16 units) within the laid-out text
  float CaretX(const std::wstring& s, const TextStyle& st, int pos);
  int HitIndex(const std::wstring& s, const TextStyle& st, float x);

  // --- icons & bitmaps
  void Icon(const std::string& name, float cx, float cy, float size, const ds::Color& c, float stroke = 1.7f);
  void Bitmap(ID2D1Bitmap* bmp, const Rect& dst, float opacity = 1.f, float cornerRadius = 0.f, bool linear = true);
  ComPtr<ID2D1SolidColorBrush> Brush(const ds::Color& c);

  void ResetCaches();   // device loss

 private:
  IDWriteTextFormat* Format(const TextStyle& st);
  ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, const TextStyle& st, float w, float h);
  ID2D1DeviceContext* dc_ = nullptr;
  float dpi_ = 96;
  bool rtl_ = false;
  struct ClipEntry { bool layer; };
  std::vector<ClipEntry> clips_;
  std::vector<D2D1_MATRIX_3X2_F> xforms_;
  ComPtr<ID2D1StrokeStyle> roundStroke_;
  ComPtr<ID2D1BitmapBrush> acrylicGrain_;
  ComPtr<ID2D1SolidColorBrush> brush_;                  // recoloured per draw call (cheap)
  std::unordered_map<std::string, ComPtr<IDWriteTextFormat>> formats_;
};

} // namespace filo::ui
