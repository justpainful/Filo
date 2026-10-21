#include "gfx/icons.h"
#include "gfx/icon_packs.h"
#include "design/theme.h"
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace filo::gfx {

namespace {

struct Scanner {
  std::string_view s; size_t i = 0;
  void skipSep() { while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == ',')) ++i; }
  bool atNumber() { skipSep(); return i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.'); }
  bool number(float& out) {
    skipSep();
    if (i >= s.size()) return false;
    size_t st = i;
    if (s[i] == '+' || s[i] == '-') ++i;
    bool dot = false, digits = false;
    while (i < s.size()) {
      char c = s[i];
      if (isdigit((unsigned char)c)) { digits = true; ++i; }
      else if (c == '.' && !dot) { dot = true; ++i; }
      else break;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E') && digits) {   // exponent
      size_t save = i; ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
      if (i < s.size() && isdigit((unsigned char)s[i])) { while (i < s.size() && isdigit((unsigned char)s[i])) ++i; } else i = save;
    }
    if (!digits) { i = st; return false; }
    out = (float)atof(std::string(s.substr(st, i - st)).c_str());
    return std::isfinite(out) && std::abs(out) <= 10000.f;
  }
  bool flag(float& out) {          // arc flags may be glued: "a1 1 0 011 2"
    skipSep();
    if (i < s.size() && (s[i] == '0' || s[i] == '1')) { out = s[i] == '1' ? 1.f : 0.f; ++i; return true; }
    return false;
  }
};

} // namespace

ComPtr<ID2D1PathGeometry> ParseSvgPath(std::string_view d, bool filled) {
  auto& dev = Device::Get();
  if (!dev.Ensure()) return {};
  ComPtr<ID2D1PathGeometry> geo;
  if (FAILED(dev.d2dFactory->CreatePathGeometry(&geo))) return {};
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(&sink))) return {};

  Scanner sc{d};
  D2D1_POINT_2F cur{0, 0}, start{0, 0}, lastCtl{0, 0};
  char cmd = 0, prev = 0;
  bool open = false;
  auto endFigure = [&](bool close) { if (open) { sink->EndFigure(close ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); open = false; } };
  auto begin = [&](D2D1_POINT_2F p) { endFigure(false); sink->BeginFigure(p, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW); open = true; start = p; };
  bool ok = true;

  while (true) {
    sc.skipSep();
    if (sc.i >= d.size()) break;
    char c = d[sc.i];
    if (isalpha((unsigned char)c)) { cmd = c; ++sc.i; } else if (!cmd) { ok = false; break; }
    else if (cmd == 'M') cmd = 'L'; else if (cmd == 'm') cmd = 'l';     // implicit lineto after moveto
    bool rel = islower((unsigned char)cmd);
    char C = (char)toupper((unsigned char)cmd);
    float v[7];
    auto need = [&](int n) { for (int k = 0; k < n; ++k) if (!sc.number(v[k])) return false; return true; };
    float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
    switch (C) {
      case 'Z': if (!open) { ok=false; break; } endFigure(true); cur = start; prev = 'Z'; cmd=0; continue;
      case 'M': if (!need(2)) { ok = false; break; } cur = {v[0] + ox, v[1] + oy}; begin(cur); break;
      case 'L': if (!need(2)) { ok = false; break; } if (!open) begin(cur); cur = {v[0] + ox, v[1] + oy}; sink->AddLine(cur); break;
      case 'H': if (!need(1)) { ok = false; break; } if (!open) begin(cur); cur.x = v[0] + ox; sink->AddLine(cur); break;
      case 'V': if (!need(1)) { ok = false; break; } if (!open) begin(cur); cur.y = v[0] + oy; sink->AddLine(cur); break;
      case 'C': {
        if (!need(6)) { ok = false; break; }
        if (!open) begin(cur);
        D2D1_BEZIER_SEGMENT b{{v[0] + ox, v[1] + oy}, {v[2] + ox, v[3] + oy}, {v[4] + ox, v[5] + oy}};
        sink->AddBezier(b); lastCtl = b.point2; cur = b.point3; break; }
      case 'S': {
        if (!need(4)) { ok = false; break; }
        if (!open) begin(cur);
        D2D1_POINT_2F c1 = (prev == 'C' || prev == 'S') ? D2D1_POINT_2F{2 * cur.x - lastCtl.x, 2 * cur.y - lastCtl.y} : cur;
        D2D1_BEZIER_SEGMENT b{c1, {v[0] + ox, v[1] + oy}, {v[2] + ox, v[3] + oy}};
        sink->AddBezier(b); lastCtl = b.point2; cur = b.point3; break; }
      case 'Q': {
        if (!need(4)) { ok = false; break; }
        if (!open) begin(cur);
        D2D1_QUADRATIC_BEZIER_SEGMENT q{{v[0] + ox, v[1] + oy}, {v[2] + ox, v[3] + oy}};
        sink->AddQuadraticBezier(q); lastCtl = q.point1; cur = q.point2; break; }
      case 'T': {
        if (!need(2)) { ok = false; break; }
        if (!open) begin(cur);
        D2D1_POINT_2F c1 = (prev == 'Q' || prev == 'T') ? D2D1_POINT_2F{2 * cur.x - lastCtl.x, 2 * cur.y - lastCtl.y} : cur;
        D2D1_QUADRATIC_BEZIER_SEGMENT q{c1, {v[0] + ox, v[1] + oy}};
        sink->AddQuadraticBezier(q); lastCtl = q.point1; cur = q.point2; break; }
      case 'A': {
        float rx, ry, rot, laf, sf, x, y;
        if (!sc.number(rx) || !sc.number(ry) || !sc.number(rot) || !sc.flag(laf) || !sc.flag(sf) || !sc.number(x) || !sc.number(y)) { ok = false; break; }
        if (!open) begin(cur);
        D2D1_ARC_SEGMENT a{};
        a.point = {x + ox, y + oy}; a.size = {rx, ry}; a.rotationAngle = rot;
        a.sweepDirection = sf != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
        a.arcSize = laf != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
        sink->AddArc(a); cur = a.point; break; }
      default: ok = false; break;
    }
    if (!ok) break;
    prev = C;
  }
  endFigure(false);
  if (FAILED(sink->Close())) ok=false;
  return ok ? geo : ComPtr<ID2D1PathGeometry>();
}

namespace {
struct IconDef { const char* name; const char* path; bool filled; };
// 24x24 grid, authored for 1.7 stroke, round caps/joins.
const IconDef kIcons[] = {
  {"compress",   "M4 4l6 6M10 5v5H5M20 20l-6-6M14 19v-5h5", false},
  {"metadata",   "M3.5 12.2V5A1.5 1.5 0 0 1 5 3.5h7.2a1.5 1.5 0 0 1 1.06.44l7.3 7.3a1.5 1.5 0 0 1 0 2.12l-7.2 7.2a1.5 1.5 0 0 1-2.12 0l-7.3-7.3a1.5 1.5 0 0 1-.44-1.06zM8 8.5h.01", false},
  {"edit",       "M4 7h8M16 7h4M4 17h4M12 17h8M14 4.5v5M10 14.5v5", false},
  {"annotate",   "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8.5 15.5l1-3.5 5.8-5.8a1.4 1.4 0 0 1 2 2L11.5 14z", false},
  {"addbg",      "M4 5.5h16A1.5 1.5 0 0 1 21.5 7v10a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 17V7A1.5 1.5 0 0 1 4 5.5zM2.5 16l5-4.5 4 3.5 3-2.5 7 5M8 9.2a1.2 1.2 0 1 0 .01 0", false},
  {"crop",       "M7 3v14h14M3 7h14v14", false},
  {"redact",     "M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12zM12 9a3 3 0 1 0 .01 0M4 4l16 16", false},
  {"trim",       "M6.5 7.5a2.5 2.5 0 1 0 .01 0M6.5 16.5a2.5 2.5 0 1 0 .01 0M8.5 8.8L20 18M8.5 15.2L20 6", false},
  {"speed",      "M4.5 17a8.5 8.5 0 1 1 15 0M12 13l4-5M12 13h.01", false},
  {"join",       "M3 8h7v8H3zM14 8h7v8h-7zM10 12h4", false},
  {"frames",     "M4 7h13v13H4zM7 4h13v13", false},
  {"split",      "M12 3v18M3 8l4 4-4 4M21 8l-4 4 4 4", false},
  {"normalize",  "M4 10v4M8 6v12M12 9v6M16 4v16M20 11v2", false},
  {"visualizer", "M3 12h2l2-6 3 12 3-9 2 5 2-2h4", false},
  {"channels",   "M7 8v8M3 10v4M11 5v14M15 8v8M19 10v4", false},
  {"bleep",      "M4 9v6h3l5 4V5L7 9zM16 9a4 4 0 0 1 0 6M18.5 6.5a8 8 0 0 1 0 11", false},
  {"merge",      "M4 4h9v9H4zM11 11h9v9h-9z", false},
  {"organize",   "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z", false},
  {"share",      "M12 15V3M8 7l4-4 4 4M5 12v7a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-7", false},
  {"paperclip",  "M21.44 11.05l-9.19 9.19a6 6 0 0 1-8.49-8.49l9.19-9.19a4 4 0 0 1 5.66 5.66l-9.2 9.19a2 2 0 0 1-2.83-2.83l8.49-8.48", false},
  {"float",      "M5 8l7-5 7 5-7 5zM5 13l7 5 7-5", false},
  {"copy",       "M9 9h10a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1V10a1 1 0 0 1 1-1zM15 5.5V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v9a1 1 0 0 0 1 1h.5", false},
  {"move",       "M12 3v18M3 12h18M8 7l4-4 4 4M8 17l4 4 4-4M7 8l-4 4 4 4M17 8l4 4-4 4", false},
  {"copypath",   "M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1", false},
  {"terminal",   "M3.5 5h17a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1h-17a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zM6.5 10l3 2.5-3 2.5M12 15h5", false},
  {"convert",    "M4 8h13M13 4l4 4-4 4M20 16H7M11 12l-4 4 4 4", false},
  {"extract",    "M4 8h16v12H4zM3 4h18v4H3zM12 11v6M9 14l3 3 3-3", false},
  {"archive",    "M4 8h16v12H4zM3 4h18v4H3zM10 12h4", false},
  {"qr",         "M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h2v2h-2zM18 14h2v2M14 18h2v2h4v-2", false},
  {"removeaudio","M4 9v6h3l5 4V5L7 9zM16 9l5 6M21 9l-5 6", false},
  {"rotate",     "M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7", false},
  {"trash",      "M5 7h14M9 7V4h6v3M7 7l1 13h8l1-13M10 11v6M14 11v6", false},
  {"play",       "M8 5l12 7-12 7z", true},
  {"pause",      "M7 5h3v14H7zM14 5h3v14h-3z", true},
  {"stop",       "M6 6h12v12H6z", true},
  {"close",      "M6 6l12 12M18 6L6 18", false},
  {"reset",      "M5 12a7 7 0 1 1 2 5M5 6v6h6", false},
  {"zoom",       "M10.5 3a7.5 7.5 0 1 0 0 15 7.5 7.5 0 0 0 0-15zM16 16l5 5", false},
  {"chevronup",  "M6 15l6-6 6 6", false},
  {"chevrondown","M6 9l6 6 6-6", false},
  {"chevronleft","M15 6l-6 6 6 6", false},
  {"chevronright","M9 6l6 6-6 6", false},
  {"folder",     "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z", false},
  {"image",      "M4 5.5h16A1.5 1.5 0 0 1 21.5 7v10a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 17V7A1.5 1.5 0 0 1 4 5.5zM2.5 16l5-4.5 4 3.5 3-2.5 7 5M8 9.2a1.2 1.2 0 1 0 .01 0", false},
  {"video",      "M3 6h12a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1zM16 10l6-3v10l-6-3", false},
  {"audio",      "M9 18V6l10-2v12M9 18a3 3 0 1 1-.01 0M19 16a3 3 0 1 1-.01 0", false},
  {"document",   "M6 3h8l5 5v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM14 3v5h5M8 13h8M8 17h6", false},
  {"check",      "M5 12.5l4.5 4.5L19 7.5", false},
  {"warning",    "M12 4l9 16H3zM12 10v4M12 17.5h.01", false},
  {"more",       "M5 12h.01M12 12h.01M19 12h.01", false},
  {"add",        "M12 5v14M5 12h14", false},
  {"remove",     "M5 12h14", false},
  {"grip",       "M9 6h.01M15 6h.01M9 12h.01M15 12h.01M9 18h.01M15 18h.01", false},
  {"mono",       "M12 5a7 7 0 1 0 .01 0", false},
  {"pin",        "M9 3h6l-1 6 3 3v2H7v-2l3-3zM12 14v7", false},
  {"settings",   "M4 7h10M18 7h2M4 17h2M10 17h10M16 4.5v5M8 14.5v5", false},
  {"star",       "M12 3l2.7 5.8 6.3.7-4.7 4.3 1.3 6.2L12 16.9 6.4 20l1.3-6.2L3 9.5l6.3-.7z", false},
  {"swap",       "M7 4L3 8l4 4M3 8h14M17 12l4 4-4 4M21 16H7", false},
  {"layers",     "M12 3l9 5-9 5-9-5zM3 13l9 5 9-5", false},
  {"text",       "M5 6V4h14v2M12 4v16M9 20h6", false},
  {"arrow",      "M5 19L19 5M9 5h10v10", false},
  {"line",       "M5 19L19 5", false},
  {"rect",       "M4 6h16v12H4z", false},
  {"circle",     "M12 4a8 8 0 1 0 .01 0", false},
  {"pen",        "M4 20l1-4L16.5 4.5a2 2 0 0 1 3 3L8 19z", false},
  {"highlighter","M4 20h6M9 15l6-6 3 3-6 6zM15 9l3-3 3 3-3 3", false},
  {"numbered",   "M12 3a9 9 0 1 0 .01 0M11 8l-1.5 1M11 8v8", false},
  {"magnifier",  "M10 4a6 6 0 1 0 .01 0M14.5 14.5L20 20M8 10h4M10 8v4", false},
};
} // namespace

IconCache& IconCache::Get() { static IconCache* c = new IconCache(); return *c; }

bool IconCache::Has(std::string_view name) const {
  for (auto& d : kIcons) if (name == d.name) return true;
  return false;
}

void IconCache::Clear() { cache_.clear(); stroke_.Reset(); squareStroke_.Reset(); }

bool IconCache::DrawClassic(ID2D1DeviceContext* dc, std::string_view name, float cx, float cy, float sizePx, ID2D1Brush* brush, float stroke) {
  const IconDef* def = nullptr;
  for (auto& d : kIcons) if (name == d.name) { def = &d; break; }
  if (!def) return false;
  std::string key(name);
  auto it = cache_.find(key);
  if (it == cache_.end()) {
    Entry e; e.geo = ParseSvgPath(def->path, def->filled); e.filled = def->filled;
    it = cache_.emplace(key, std::move(e)).first;
  }
  if (!it->second.geo) return false;
  if (!stroke_) {
    D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f};
    Device::Get().d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &stroke_);
  }
  D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
  const float s = sizePx / 24.f;
  dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(cx - sizePx / 2, cy - sizePx / 2) * old);
  if (it->second.filled) dc->FillGeometry(it->second.geo.Get(), brush);
  else dc->DrawGeometry(it->second.geo.Get(), brush, stroke, stroke_.Get());
  dc->SetTransform(old);
  return true;
}

std::vector<std::string> IconCache::Names() {
  std::vector<std::string> names;for(const auto& d:kIcons)names.push_back(d.name);return names;
}
bool IconCache::Draw(ID2D1DeviceContext* dc,std::string_view name,float cx,float cy,float size,ID2D1Brush* brush,float stroke) {
  return DrawPack(dc,ds::Theme::Get().State().iconPack,name,cx,cy,size,brush,stroke);
}
bool IconCache::DrawPack(ID2D1DeviceContext* dc,std::string_view id,std::string_view name,float cx,float cy,float size,ID2D1Brush* brush,float stroke) {
  const auto& pack=IconPacks::Get().Resolve(id);
  auto icon=pack.icons.find(std::string(name));
  if(icon==pack.icons.end())return DrawClassic(dc,name,cx,cy,size,brush,stroke);
  // Compile every layer first: a failed geometry inherits the complete Classic glyph.
  std::vector<Entry*> layers;
  for(size_t i=0;i<icon->second.size();++i) {
    auto key=pack.id+":"+std::string(name)+":"+std::to_string(i);
    auto it=cache_.find(key);
    if(it==cache_.end()) {
      const auto& l=icon->second[i];Entry e;e.geo=ParseSvgPath(l.path,l.filled);e.filled=l.filled;
      it=cache_.emplace(key,std::move(e)).first;
    }
    if(!it->second.geo)return DrawClassic(dc,name,cx,cy,size,brush,stroke);
    layers.push_back(&it->second);
  }
  auto& style=pack.round?stroke_:squareStroke_;
  if(!style) {
    D2D1_STROKE_STYLE_PROPERTIES sp{};
    sp.startCap=sp.endCap=sp.dashCap=pack.round?D2D1_CAP_STYLE_ROUND:D2D1_CAP_STYLE_FLAT;
    sp.lineJoin=pack.round?D2D1_LINE_JOIN_ROUND:D2D1_LINE_JOIN_MITER;sp.miterLimit=4;
    Device::Get().d2dFactory->CreateStrokeStyle(sp,nullptr,0,&style);
  }
  D2D1_MATRIX_3X2_F old;dc->GetTransform(&old);float a=brush->GetOpacity();
  float scale=size/24.f;dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(cx-size/2,cy-size/2)*old);
  for(size_t i=0;i<layers.size();++i) {
    const auto& l=icon->second[i];brush->SetOpacity(a*l.opacity);
    if(l.filled)dc->FillGeometry(layers[i]->geo.Get(),brush);
    else dc->DrawGeometry(layers[i]->geo.Get(),brush,l.stroke*(stroke/1.7f),style.Get());
  }
  brush->SetOpacity(a);dc->SetTransform(old);return true;
}

} // namespace filo::gfx
