// Vector icon set (24x24 grid, stroke style similar to SF Symbols) drawn with Direct2D path geometry.
// Icons are authored as SVG path data in icons.cpp; there are no bitmap assets to license or scale.
#pragma once
#include "gfx/device.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filo::gfx {

// Parses SVG path data (M m L l H h V v C c S s Q q T t A a Z z) into a D2D geometry. Returns null on malformed input.
ComPtr<ID2D1PathGeometry> ParseSvgPath(std::string_view d, bool filled);

class IconCache {
 public:
  static IconCache& Get();
  // Draws icon `name` centred at (cx, cy) with a size of `sizePx` (the 24-unit box maps to sizePx) using `brush`.
  // `stroke` is in icon units (default 1.7). Returns false when the icon is unknown.
  bool Draw(ID2D1DeviceContext* dc, std::string_view name, float cx, float cy, float sizePx, ID2D1Brush* brush, float stroke = 1.7f);
  // Explicit pack drawing for settings previews; normal Draw follows the active appearance.
  bool DrawPack(ID2D1DeviceContext* dc, std::string_view pack, std::string_view name, float cx, float cy, float sizePx, ID2D1Brush* brush, float stroke = 1.7f);
  static std::vector<std::string> Names();
  bool Has(std::string_view name) const;
  void Clear();   // on device loss
 private:
  bool DrawClassic(ID2D1DeviceContext*, std::string_view, float, float, float, ID2D1Brush*, float);
  struct Entry { ComPtr<ID2D1PathGeometry> geo; bool filled = false; };
  std::unordered_map<std::string, Entry> cache_;
  ComPtr<ID2D1StrokeStyle> stroke_, squareStroke_;
};

} // namespace filo::gfx
