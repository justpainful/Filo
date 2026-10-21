// Reading-order text/image extraction on top of PDFium. See pdf_text.h for the public contract.
#include "engines/pdf/pdf_text.h"
#include "engines/pdf/pdf_doc.h"
#include "engines/pdf/pdf_internal.h"
#include "fpdfview.h"
#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_text.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>


namespace filo::pdf {
namespace detail {

bool IsRtlChar(wchar_t c) {
  return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool IsRtlText(const std::wstring& s) {
  int rtl = 0, ltr = 0;
  for (wchar_t c : s) {
    if (IsRtlChar(c)) ++rtl;
    else if (iswalpha(c)) ++ltr;
  }
  return rtl > 0 && rtl >= ltr;
}

bool NeedsSpaceBetween(const TextRun& a, const TextRun& b, bool rtlLine) {
  if (a.text.empty() || b.text.empty()) return false;
  if (iswspace(a.text.back()) || iswspace(b.text.front())) return false;
  float fs = std::max(a.fontSize, b.fontSize);
  if (fs <= 0) fs = 10;
  float gap = rtlLine ? (a.x - (b.x + b.w)) : (b.x - (a.x + a.w));
  return gap > 0.2f * fs;
}

std::vector<std::vector<int>> ClusterLines(const std::vector<TextRun>& R, std::vector<int> ids) {
  struct L { std::vector<int> runs; float top, bottom; };
  std::sort(ids.begin(), ids.end(), [&](int a, int b) {
    float ca = R[a].y + R[a].h * 0.5f, cb = R[b].y + R[b].h * 0.5f;
    return ca != cb ? ca < cb : R[a].x < R[b].x;
  });
  std::vector<L> lines;
  for (int id : ids) {
    const TextRun& r = R[id];
    int best = -1; float bestOv = 0;
    for (int k = (int)lines.size() - 1; k >= 0 && k >= (int)lines.size() - 8; --k) {
      float ov = std::min(r.y + r.h, lines[k].bottom) - std::max(r.y, lines[k].top);
      float mh = std::min(r.h, lines[k].bottom - lines[k].top);
      if (ov > 0 && ov >= 0.5f * mh && ov > bestOv) { best = k; bestOv = ov; }
    }
    if (best < 0) { lines.push_back({{id}, r.y, r.y + r.h}); }
    else {
      L& l = lines[best];
      l.runs.push_back(id);
      // grow only slowly so a tall run does not swallow the neighbouring lines
      l.top = std::min(l.top, r.y); l.bottom = std::max(l.bottom, r.y + r.h);
    }
  }
  // Lines may have been created slightly out of order; sort by centre.
  std::vector<std::vector<int>> out;
  std::stable_sort(lines.begin(), lines.end(), [](const L& a, const L& b) { return (a.top + a.bottom) < (b.top + b.bottom); });
  for (auto& l : lines) {
    int rtlN = 0, ltrN = 0;
    for (int id : l.runs) { (R[id].rtl ? rtlN : ltrN) += (int)R[id].text.size(); }
    bool rtl = rtlN > ltrN;
    std::stable_sort(l.runs.begin(), l.runs.end(), [&](int a, int b) {
      float ca = R[a].x + R[a].w * 0.5f, cb = R[b].x + R[b].w * 0.5f;
      return rtl ? ca > cb : ca < cb;
    });
    out.push_back(std::move(l.runs));
  }
  return out;
}

std::wstring JoinRunsText(const std::vector<TextRun>& R, const std::vector<int>& ids) {
  auto lines = ClusterLines(R, ids);
  std::wstring out;
  for (size_t li = 0; li < lines.size(); ++li) {
    if (li) out += L'\n';
    int rtlN = 0, ltrN = 0;
    for (int id : lines[li]) (R[id].rtl ? rtlN : ltrN) += (int)R[id].text.size();
    bool rtl = rtlN > ltrN;
    std::wstring line;
    const TextRun* prev = nullptr;
    for (int id : lines[li]) {
      if (prev && NeedsSpaceBetween(*prev, R[id], rtl)) line += L' ';
      line += R[id].text;
      prev = &R[id];
    }
    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
    out += line;
  }
  return out;
}

} // namespace detail

using namespace detail;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// geometry

struct Affine {            // page space (PDF, y up) -> display space (points, y down)
  double a = 1, b = 0, c = 0, d = -1, e = 0, f = 0;
  void Map(double x, double y, double& ox, double& oy) const { ox = a * x + c * y + e; oy = b * x + d * y + f; }
};
struct M3 { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };   // PDF matrix
M3 Mul(const M3& x, const M3& y) {   // apply x first, then y
  return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d,
          x.e * y.a + x.f * y.c + y.e, x.e * y.b + x.f * y.d + y.f};
}
void Apply(const M3& m, double x, double y, double& ox, double& oy) { ox = m.a * x + m.c * y + m.e; oy = m.b * x + m.d * y + m.f; }
M3 FromAffine(const Affine& a) { return {a.a, a.b, a.c, a.d, a.e, a.f}; }
M3 FromFs(const FS_MATRIX& m) { return {m.a, m.b, m.c, m.d, m.e, m.f}; }

double Snap(double v) {
  if (std::fabs(v) < 0.002) return 0;
  if (std::fabs(std::fabs(v) - 1) < 0.002) return v < 0 ? -1 : 1;
  return v;
}

Affine Calibrate(FPDF_PAGE pg, double W, double H) {
  const int S = 64;
  int sx = std::max(1, (int)std::lround(W * S)), sy = std::max(1, (int)std::lround(H * S));
  auto dev = [&](double x, double y, double& ox, double& oy) {
    int dx = 0, dy = 0;
    FPDF_PageToDevice(pg, 0, 0, sx, sy, 0, x, y, &dx, &dy);
    ox = dx / (double)S; oy = dy / (double)S;
  };
  double x0, y0, x1, y1, x2, y2;
  dev(0, 0, x0, y0); dev(1000, 0, x1, y1); dev(0, 1000, x2, y2);
  Affine A;
  A.a = Snap((x1 - x0) / 1000); A.b = Snap((y1 - y0) / 1000);
  A.c = Snap((x2 - x0) / 1000); A.d = Snap((y2 - y0) / 1000);
  A.e = x0; A.f = y0;
  return A;
}

// ---------------------------------------------------------------------------------------------------------------------
// glyph collection

struct FontRec { std::wstring name; bool bold = false, italic = false; };

struct Glyph {
  wchar_t ch[2] = {0, 0};
  int nch = 1;
  float l = 0, t = 0, r = 0, b = 0;   // display box (loose)
  float ox = 0, oy = 0;               // display origin (baseline start)
  float size = 0;
  int dir = 0;                        // 0: 0deg, 1: 90 (reads upward), 2: 180, 3: 270, 4: other
  uint32_t rgb = 0;
  int font = 0;
  bool brk = false;                   // PDFium reported a line break before this glyph
  bool space = false;
};

std::wstring StripSubset(std::string n) {
  if (n.size() > 7 && n[6] == '+') {
    bool ok = true; for (int i = 0; i < 6; ++i) if (!(n[i] >= 'A' && n[i] <= 'Z')) ok = false;
    if (ok) n = n.substr(7);
  }
  return Widen(n);
}
bool ContainsI(const std::wstring& s, const wchar_t* sub) { return ToLower(s).find(sub) != std::wstring::npos; }

wchar_t FoldPresentation(wchar_t c, std::wstring& out) {
  // Latin ligatures and Arabic presentation forms -> compatibility decomposition (NFKC); everything else untouched.
  if ((c >= 0xFB00 && c <= 0xFB06) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) {
    wchar_t in[2] = {c, 0}, buf[16] = {};
    int n = NormalizeString(NormalizationKC, in, 1, buf, 16);
    if (n > 0 && n < 16) { out.append(buf, n); return 0; }
  }
  out += c;
  return c;
}

struct Scratch {
  std::vector<Glyph> glyphs;
  std::vector<FontRec> fonts;
  int unmapped = 0;
  int total = 0;
};

void CollectGlyphs(FPDF_TEXTPAGE tp, const Affine& A, Scratch& sc) {
  int n = FPDFText_CountChars(tp);
  if (n <= 0) return;
  sc.glyphs.reserve(n);
  FPDF_PAGEOBJECT lastObj = nullptr;
  float objSize = 0; int objDir = 0; uint32_t objRgb = 0; int objFont = 0;
  std::map<std::pair<std::wstring, int>, int> fontIndex;
  bool pendingBreak = false;
  for (int i = 0; i < n; ++i) {
    unsigned int u = FPDFText_GetUnicode(tp, i);
    bool gen = FPDFText_IsGenerated(tp, i) != 0;
    if (gen) { if (u == '\r' || u == '\n') pendingBreak = true; continue; }
    ++sc.total;
    if (u == 0 || u == 0xFFFE || u == 0xFFFF) { ++sc.unmapped; continue; }
    if (u < 0x20) { if (u == '\t') u = ' '; else { pendingBreak = true; continue; } }
    if (u == 0xFFFD) { ++sc.unmapped; continue; }

    FPDF_PAGEOBJECT obj = FPDFText_GetTextObject(tp, i);
    if (obj != lastObj || !lastObj) {
      lastObj = obj;
      FS_MATRIX m{1, 0, 0, 1, 0, 0};
      FPDFText_GetMatrix(tp, i, &m);
      double det = std::fabs((double)m.a * m.d - (double)m.b * m.c);
      double sc2 = det > 1e-9 ? std::sqrt(det) : 1.0;
      objSize = (float)(FPDFText_GetFontSize(tp, i) * sc2);
      // direction of the text baseline in display space
      double vx = A.a * m.a + A.c * m.b, vy = A.b * m.a + A.d * m.b;
      double ang = std::atan2(-vy, vx) * 180.0 / 3.14159265358979;
      double q = ang / 90.0, rq = std::round(q);
      objDir = std::fabs(q - rq) * 90.0 <= 15.0 ? (((int)rq % 4) + 4) % 4 : 4;
      unsigned int R = 0, G = 0, B = 0, Al = 0;
      objRgb = FPDFText_GetFillColor(tp, i, &R, &G, &B, &Al) ? ((R & 255) << 16 | (G & 255) << 8 | (B & 255)) : 0;
      char nm[256] = {}; int flags = 0;
      unsigned long ln = FPDFText_GetFontInfo(tp, i, nm, sizeof(nm) - 1, &flags);
      std::wstring name = ln > 1 ? StripSubset(nm) : L"";
      int weight = FPDFText_GetFontWeight(tp, i);
      bool bold = weight >= 700 || (flags & 0x40000) || ContainsI(name, L"bold") || ContainsI(name, L"black") ||
                  ContainsI(name, L"heavy") || ContainsI(name, L"semibold") || ContainsI(name, L"demi");
      bool ital = (flags & 0x40) || ContainsI(name, L"italic") || ContainsI(name, L"oblique");
      int key = (bold ? 1 : 0) | (ital ? 2 : 0);
      auto it = fontIndex.find({name, key});
      if (it == fontIndex.end()) {
        sc.fonts.push_back({name, bold, ital});
        it = fontIndex.emplace(std::make_pair(name, key), (int)sc.fonts.size() - 1).first;
      }
      objFont = it->second;
    }
    FS_RECTF lb{};
    if (!FPDFText_GetLooseCharBox(tp, i, &lb)) { ++sc.unmapped; continue; }
    double xs[4], ys[4];
    A.Map(lb.left, lb.bottom, xs[0], ys[0]); A.Map(lb.right, lb.bottom, xs[1], ys[1]);
    A.Map(lb.right, lb.top, xs[2], ys[2]); A.Map(lb.left, lb.top, xs[3], ys[3]);
    Glyph g;
    g.l = (float)*std::min_element(xs, xs + 4); g.r = (float)*std::max_element(xs, xs + 4);
    g.t = (float)*std::min_element(ys, ys + 4); g.b = (float)*std::max_element(ys, ys + 4);
    double px = 0, py = 0, dox = 0, doy = 0;
    if (FPDFText_GetCharOrigin(tp, i, &px, &py)) { A.Map(px, py, dox, doy); g.ox = (float)dox; g.oy = (float)doy; }
    else { g.ox = g.l; g.oy = g.b; }
    g.size = objSize > 0 ? objSize : std::max(1.f, g.b - g.t);
    g.dir = objDir; g.rgb = objRgb; g.font = objFont;
    g.brk = pendingBreak; pendingBreak = false;
    g.space = (u == ' ' || u == 0xA0 || u == 0x2009 || u == 0x200A || u == 0x2002 || u == 0x2003);
    if (u > 0xFFFF) { u -= 0x10000; g.ch[0] = (wchar_t)(0xD800 + (u >> 10)); g.ch[1] = (wchar_t)(0xDC00 + (u & 0x3FF)); g.nch = 2; }
    else g.ch[0] = (wchar_t)u;
    sc.glyphs.push_back(g);
  }
  // PDFium reverses the constituent characters of right-to-left ligature glyphs (lam-alef is reported as alef,lam). Characters of
  // one glyph share an identical box; restore their logical order.
  auto& G = sc.glyphs;
  for (size_t i = 0; i + 1 < G.size();) {
    size_t j = i + 1;
    while (j < G.size() && IsRtlChar(G[j].ch[0]) && IsRtlChar(G[i].ch[0]) && std::fabs(G[j].l - G[i].l) < 0.05f &&
           std::fabs(G[j].r - G[i].r) < 0.05f && std::fabs(G[j].oy - G[i].oy) < 0.05f && !G[j].brk)
      ++j;
    if (j - i >= 2) std::reverse(G.begin() + i, G.begin() + j);
    i = j;
  }
}

// glyphs -> runs (PDFium order is kept inside a run; runs are later arranged by geometry)
void BuildRuns(const Scratch& sc, std::vector<TextRun>& runs) {
  const auto& G = sc.glyphs;
  struct Acc { TextRun r; double baseSum = 0; int n = 0; int dir = 0; float maxSize = 0; int font = -1; uint32_t rgb = 0; float sizeKey = 0; bool open = false; } a;
  size_t i = 0;
  auto close = [&] {
    if (!a.open) return;
    a.open = false;
    bool anyNonSpace = false;
    for (wchar_t c : a.r.text) if (!iswspace(c) && c != 0xA0) { anyNonSpace = true; break; }
    if (!anyNonSpace) { a = Acc(); return; }
    TextRun& r = a.r;
    r.w = r.w - r.x; r.h = r.h - r.y;   // w/h currently hold right/bottom
    r.baseline = (float)(a.baseSum / std::max(1, a.n));
    r.fontSize = a.sizeKey;
    r.rgb = a.rgb;
    r.angle = a.dir == 0 ? 0.f : a.dir == 1 ? 90.f : a.dir == 2 ? 180.f : a.dir == 3 ? 270.f : 45.f;
    r.rtl = IsRtlText(r.text);
    runs.push_back(std::move(r));
    a = Acc();
  };
  const Glyph* prev = nullptr;
  for (i = 0; i < G.size(); ++i) {
    const Glyph& g = G[i];
    bool vert = (g.dir == 1 || g.dir == 3);
    bool cont = a.open && prev;
    if (cont) {
      const Glyph& p = *prev;
      float s = std::max(g.size, p.size);
      bool pvert = (p.dir == 1 || p.dir == 3);
      if (g.dir != p.dir || vert != pvert || g.brk || g.dir == 4) cont = false;
      else {
        float baseDiff = vert ? std::fabs(g.ox - p.ox) : std::fabs(g.oy - p.oy);
        float gl = vert ? g.t : g.l, gr = vert ? g.b : g.r, pl = vert ? p.t : p.l, pr = vert ? p.b : p.r;
        float gap = std::max(gl - pr, pl - gr);
        if (baseDiff > 0.5f * s || gap > 1.0f * s || gap < -0.7f * s) cont = false;
        // style change (spaces adopt the style of their neighbours)
        if (cont && !g.space) {
          if (g.font != a.font || std::fabs(g.size - a.sizeKey) > 0.25f || g.rgb != a.rgb) cont = false;
        }
      }
    }
    if (!cont) {
      close();
      a.open = true;
      a.r.x = g.l; a.r.y = g.t; a.r.w = g.r; a.r.h = g.b;   // w/h temporarily right/bottom
      a.dir = g.dir; a.font = g.font; a.sizeKey = g.size; a.rgb = g.rgb;
      const FontRec& f = sc.fonts[g.font];
      a.r.fontName = f.name; a.r.bold = f.bold; a.r.italic = f.italic;
      if (g.space) { a.font = -1; }   // adopt style of the next real glyph
    } else {
      const Glyph& p = *prev;
      float gl = vert ? g.t : g.l, gr = vert ? g.b : g.r, pl = vert ? p.t : p.l, pr = vert ? p.b : p.r;
      float gap = std::max(gl - pr, pl - gr);
      if (gap > 0.22f * std::max(g.size, p.size) && !g.space && !p.space) a.r.text += L' ';
      a.r.x = std::min(a.r.x, g.l); a.r.y = std::min(a.r.y, g.t); a.r.w = std::max(a.r.w, g.r); a.r.h = std::max(a.r.h, g.b);
      if (a.font == -1 && !g.space) {
        a.font = g.font; a.sizeKey = g.size; a.rgb = g.rgb;
        const FontRec& f = sc.fonts[g.font];
        a.r.fontName = f.name; a.r.bold = f.bold; a.r.italic = f.italic;
      }
    }
    for (int k = 0; k < g.nch; ++k) FoldPresentation(g.ch[k], a.r.text);
    a.baseSum += vert ? g.ox : g.oy; ++a.n;
    prev = &g;
  }
  close();
}

// ---------------------------------------------------------------------------------------------------------------------
// reading order

struct Frag {
  float l, t, r, b;
  std::vector<int> runs;      // reading order inside the fragment
  int lineId = 0;             // visual line number (top to bottom)
  int table = -1;
  bool rtl = false;
};
struct Leaf { std::vector<int> frags; int column = 0; };

struct Layout {
  const std::vector<Frag>& F;
  float medianFs = 10;
  std::vector<Leaf> leaves;
  int maxCols = 1;
};

bool FindGutter(const Layout& L, const std::vector<int>& ids, float& gx1, float& gx2) {
  if (ids.size() < 4) return false;
  const auto& F = L.F;
  float rl = 1e9f, rr = -1e9f;
  for (int id : ids) { rl = std::min(rl, F[id].l); rr = std::max(rr, F[id].r); }
  float W = rr - rl;
  if (W < 6 * L.medianFs) return false;
  float minG = std::max(0.9f * L.medianFs, 8.f);
  // sweep: coverage count over x
  struct Ev { float x; int d; };
  std::vector<Ev> ev;
  for (int id : ids) { ev.push_back({F[id].l, +1}); ev.push_back({F[id].r, -1}); }
  std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.x != b.x ? a.x < b.x : a.d < b.d; });
  int N = (int)ids.size();
  int thr = std::max(1, (int)(0.1 * N));
  struct Cand { float x1, x2; };
  std::vector<Cand> cands;
  int cov = 0; float segStart = rl; bool inCand = false; float candStart = 0;
  for (size_t k = 0; k < ev.size(); ++k) {
    float x = ev[k].x;
    // state on [segStart, x) has coverage `cov`
    if (x > segStart) {
      bool low = cov <= thr;
      if (low && !inCand) { inCand = true; candStart = segStart; }
      if (!low && inCand) { inCand = false; if (segStart - candStart >= minG) cands.push_back({candStart, segStart}); }
    }
    cov += ev[k].d;
    segStart = x;
  }
  if (inCand && segStart - candStart >= minG) cands.push_back({candStart, segStart});
  float bestW = 0;
  bool found = false;
  for (auto& c : cands) {
    if (c.x1 <= rl + 1e-3f || c.x2 >= rr - 1e-3f) continue;    // margins are not gutters
    std::vector<float> lw, rw;
    int cross = 0;
    for (int id : ids) {
      if (F[id].r <= c.x1 + 0.5f) lw.push_back(F[id].r - F[id].l);
      else if (F[id].l >= c.x2 - 0.5f) rw.push_back(F[id].r - F[id].l);
      else ++cross;
    }
    if (lw.size() < 2 || rw.size() < 2) continue;
    if (cross > thr) continue;
    auto med = [](std::vector<float> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    if (med(lw) < 5 * L.medianFs || med(rw) < 5 * L.medianFs) continue;   // narrow stacks (numbers, TOC) are not columns
    float w = c.x2 - c.x1;
    if (w > bestW) { bestW = w; gx1 = c.x1; gx2 = c.x2; found = true; }
  }
  return found;
}

int Order(Layout& L, std::vector<int> ids, int colBase, bool rtl, int depth) {
  if (ids.empty()) return 0;
  const auto& F = L.F;
  float gx1 = 0, gx2 = 0;
  if (depth < 6 && FindGutter(L, ids, gx1, gx2)) {
    std::sort(ids.begin(), ids.end(), [&](int a, int b) { return F[a].t != F[b].t ? F[a].t < F[b].t : F[a].l < F[b].l; });
    float mid = 0.5f * (gx1 + gx2);
    auto crossing = [&](int id) { return F[id].l < gx2 - 0.5f && F[id].r > gx1 + 0.5f; };
    std::vector<int> chunk, span;
    int maxCols = 1;
    auto flush = [&] {
      if (chunk.empty()) return;
      std::vector<int> lft, rgt;
      for (int id : chunk) ((F[id].l + F[id].r) * 0.5f < mid ? lft : rgt).push_back(id);
      auto& first = rtl ? rgt : lft; auto& second = rtl ? lft : rgt;
      int c1 = Order(L, first, colBase, rtl, depth + 1);
      int c2 = Order(L, second, colBase + c1, rtl, depth + 1);
      maxCols = std::max(maxCols, c1 + c2);
      chunk.clear();
    };
    auto emitSpan = [&] { if (!span.empty()) { L.leaves.push_back({span, colBase}); span.clear(); } };
    for (int id : ids) {
      if (crossing(id)) { flush(); span.push_back(id); }
      else { emitSpan(); chunk.push_back(id); }
    }
    flush(); emitSpan();
    return maxCols;
  }
  L.leaves.push_back({std::move(ids), colBase});
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// objects: rules + images

struct ObjCtx {
  FPDF_DOCUMENT doc;
  FPDF_PAGE page;
  Affine A;
  bool withImages;
  PageContent* out;
  int budget = 40000;       // path objects visited
};

void AddRule(PageContent& pc, double x1, double y1, double x2, double y2, double w) {
  RuleLine r;
  r.x1 = (float)x1; r.y1 = (float)y1; r.x2 = (float)x2; r.y2 = (float)y2; r.width = (float)w;
  if (std::fabs(r.x2 - r.x1) < std::fabs(r.y2 - r.y1)) { r.x1 = r.x2 = (r.x1 + r.x2) * 0.5f; if (r.y1 > r.y2) std::swap(r.y1, r.y2); }
  else { r.y1 = r.y2 = (r.y1 + r.y2) * 0.5f; if (r.x1 > r.x2) std::swap(r.x1, r.x2); }
  if (pc.rules.size() < 30000) pc.rules.push_back(r);
}

void HandlePath(ObjCtx& cx, FPDF_PAGEOBJECT obj, const M3& toDisp) {
  int fillmode = 0; FPDF_BOOL stroke = 0;
  if (!FPDFPath_GetDrawMode(obj, &fillmode, &stroke)) return;
  bool fill = fillmode != 0;
  if (!fill && !stroke) return;
  int ns = FPDFPath_CountSegments(obj);
  if (ns < 2 || ns > 20000) return;
  float sw = 1.f; FPDFPageObj_GetStrokeWidth(obj, &sw);
  double scale = std::sqrt(std::fabs(toDisp.a * toDisp.d - toDisp.b * toDisp.c));
  double lw = stroke ? sw * scale : 0;
  struct P { double x, y; };
  std::vector<P> sub;                  // current subpath in display space
  bool subCurvy = false;
  auto flushSub = [&](bool closed) {
    if (sub.size() >= 2 && !subCurvy) {
      if (stroke) {
        for (size_t k = 0; k + 1 < sub.size(); ++k) {
          double dx = sub[k + 1].x - sub[k].x, dy = sub[k + 1].y - sub[k].y;
          if ((std::fabs(dx) < 0.6 || std::fabs(dy) < 0.6) && (std::fabs(dx) + std::fabs(dy) >= 3))
            AddRule(*cx.out, sub[k].x, sub[k].y, sub[k + 1].x, sub[k + 1].y, lw);
        }
        if (closed && sub.size() >= 3) {
          double dx = sub.front().x - sub.back().x, dy = sub.front().y - sub.back().y;
          if ((std::fabs(dx) < 0.6 || std::fabs(dy) < 0.6) && (std::fabs(dx) + std::fabs(dy) >= 3))
            AddRule(*cx.out, sub.back().x, sub.back().y, sub.front().x, sub.front().y, lw);
        }
      } else if (fill) {
        // thin filled rectangle == drawn line
        double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
        bool axis = true;
        for (size_t k = 0; k < sub.size(); ++k) {
          minx = std::min(minx, sub[k].x); maxx = std::max(maxx, sub[k].x);
          miny = std::min(miny, sub[k].y); maxy = std::max(maxy, sub[k].y);
          if (k + 1 < sub.size() && std::fabs(sub[k + 1].x - sub[k].x) > 0.6 && std::fabs(sub[k + 1].y - sub[k].y) > 0.6) axis = false;
        }
        double w = maxx - minx, h = maxy - miny;
        if (axis && sub.size() <= 5) {
          if (h <= 2.6 && w >= 6) AddRule(*cx.out, minx, (miny + maxy) / 2, maxx, (miny + maxy) / 2, std::max(h, 0.3));
          else if (w <= 2.6 && h >= 6) AddRule(*cx.out, (minx + maxx) / 2, miny, (minx + maxx) / 2, maxy, std::max(w, 0.3));
        }
      }
    }
    sub.clear(); subCurvy = false;
  };
  for (int i = 0; i < ns; ++i) {
    FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, i);
    if (!seg) continue;
    float x = 0, y = 0;
    if (!FPDFPathSegment_GetPoint(seg, &x, &y)) continue;
    double dx, dy; Apply(toDisp, x, y, dx, dy);
    int type = FPDFPathSegment_GetType(seg);
    if (type == FPDF_SEGMENT_MOVETO) { flushSub(false); sub.push_back({dx, dy}); }
    else if (type == FPDF_SEGMENT_LINETO) { sub.push_back({dx, dy}); }
    else { subCurvy = true; }
    if (FPDFPathSegment_GetClose(seg)) flushSub(true);
  }
  flushSub(false);
}

bool DecodeBitmap(FPDF_BITMAP bmp, int& w, int& h, std::vector<uint8_t>& bgra, bool& alpha) {
  if (!bmp) return false;
  w = FPDFBitmap_GetWidth(bmp); h = FPDFBitmap_GetHeight(bmp);
  int stride = FPDFBitmap_GetStride(bmp);
  const uint8_t* p = (const uint8_t*)FPDFBitmap_GetBuffer(bmp);
  int fmt = FPDFBitmap_GetFormat(bmp);
  if (!p || w <= 0 || h <= 0 || (double)w * h > 400e6) return false;
  try { bgra.assign((size_t)w * h * 4, 255); } catch (...) { return false; }
  alpha = false;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = p + (size_t)y * stride; uint8_t* d = bgra.data() + (size_t)y * w * 4;
    switch (fmt) {
      case FPDFBitmap_Gray: for (int x = 0; x < w; ++x) { d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = s[x]; } break;
      case FPDFBitmap_BGR: for (int x = 0; x < w; ++x) { d[4 * x] = s[3 * x]; d[4 * x + 1] = s[3 * x + 1]; d[4 * x + 2] = s[3 * x + 2]; } break;
      case FPDFBitmap_BGRx: for (int x = 0; x < w; ++x) { d[4 * x] = s[4 * x]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x + 2]; } break;
      case FPDFBitmap_BGRA:
        std::memcpy(d, s, (size_t)w * 4);
        for (int x = 0; x < w; ++x) if (d[4 * x + 3] != 255) alpha = true;
        break;
      case FPDFBitmap_BGRA_Premul:
        for (int x = 0; x < w; ++x) {
          uint8_t a = s[4 * x + 3];
          d[4 * x + 3] = a;
          if (a) { for (int c = 0; c < 3; ++c) d[4 * x + c] = (uint8_t)std::min(255, s[4 * x + c] * 255 / a); } else { d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = 0; }
          if (a != 255) alpha = true;
        }
        break;
      default: return false;
    }
  }
  return true;
}

void HandleImage(ObjCtx& cx, FPDF_PAGEOBJECT obj, const M3& objToDisp) {
  // objToDisp maps the unit square (image space) to display space
  double ux[4] = {0, 1, 1, 0}, uy[4] = {0, 0, 1, 1}, dx[4], dy[4];
  for (int k = 0; k < 4; ++k) Apply(objToDisp, ux[k], uy[k], dx[k], dy[k]);
  ImageBlock ib;
  double minx = *std::min_element(dx, dx + 4), maxx = *std::max_element(dx, dx + 4);
  double miny = *std::min_element(dy, dy + 4), maxy = *std::max_element(dy, dy + 4);
  ib.x = (float)minx; ib.y = (float)miny; ib.w = (float)(maxx - minx); ib.h = (float)(maxy - miny);
  unsigned int pw = 0, ph = 0;
  if (FPDFImageObj_GetImagePixelSize(obj, &pw, &ph)) { ib.pixelW = (int)pw; ib.pixelH = (int)ph; }
  if (cx.withImages && pw > 0 && ph > 0 && (double)pw * ph < 300e6) {
    // orientation: where do the source pixel axes go in display space?
    // pixel (0,0) is the image's top-left == unit (0,1); +x pixel == unit +x; +y pixel == unit -y
    double tlx, tly, trx, try_, blx, bly;
    Apply(objToDisp, 0, 1, tlx, tly); Apply(objToDisp, 1, 1, trx, try_); Apply(objToDisp, 0, 0, blx, bly);
    double exx = trx - tlx, exy = try_ - tly, eyx = blx - tlx, eyy = bly - tly;
    auto sgn = [](double v, double ref) { return std::fabs(v) < 0.02 * std::max(1.0, ref) ? 0 : (v > 0 ? 1 : -1); };
    double mag = std::max({std::fabs(exx), std::fabs(exy), std::fabs(eyx), std::fabs(eyy), 1.0});
    int sxx = sgn(exx, mag), sxy = sgn(exy, mag), syx = sgn(eyx, mag), syy = sgn(eyy, mag);
    bool axisAligned = (std::abs(sxx) + std::abs(sxy) == 1) && (std::abs(syx) + std::abs(syy) == 1);
    bool upright = axisAligned && sxx == 1 && syy == 1;
    FPDF_BITMAP bmp = FPDFImageObj_GetBitmap(obj);
    if (bmp) {
      int w = 0, h = 0; std::vector<uint8_t> px; bool alpha = false;
      if (DecodeBitmap(bmp, w, h, px, alpha)) {
        if (axisAligned && !upright) {
          int dw = std::abs(sxx) * w + std::abs(syx) * h, dh = std::abs(sxy) * w + std::abs(syy) * h;
          std::vector<uint8_t> out((size_t)dw * dh * 4);
          int ox = (sxx < 0 ? w - 1 : 0) + (syx < 0 ? h - 1 : 0), oy = (sxy < 0 ? w - 1 : 0) + (syy < 0 ? h - 1 : 0);
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
              int X = ox + x * sxx + y * syx, Y = oy + x * sxy + y * syy;
              std::memcpy(&out[((size_t)Y * dw + X) * 4], &px[((size_t)y * w + x) * 4], 4);
            }
          px.swap(out); std::swap(w, dw); h = dh;
        }
        ib.bgra = std::move(px); ib.pixelW = w; ib.pixelH = h; ib.hasAlpha = alpha;
        if (upright && FPDFImageObj_GetImageFilterCount(obj) == 1) {
          char fn[32] = {};
          FPDFImageObj_GetImageFilter(obj, 0, fn, sizeof(fn) - 1);
          if (std::strcmp(fn, "DCTDecode") == 0) {
            unsigned long n = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
            if (n > 4 && n < (200u << 20)) { ib.rawJpeg.resize(n); FPDFImageObj_GetImageDataRaw(obj, ib.rawJpeg.data(), n); }
          }
        }
      }
      FPDFBitmap_Destroy(bmp);
    }
  }
  cx.out->images.push_back(std::move(ib));
}

void WalkObject(ObjCtx& cx, FPDF_PAGEOBJECT obj, const M3& parentToDisp, int depth) {
  if (--cx.budget < 0) return;
  int type = FPDFPageObj_GetType(obj);
  FS_MATRIX fm{1, 0, 0, 1, 0, 0};
  bool hasM = FPDFPageObj_GetMatrix(obj, &fm) != 0;
  M3 om = hasM ? FromFs(fm) : M3{};
  if (type == FPDF_PAGEOBJ_PATH) {
    // path points are stored in the object's own space; its matrix is applied on top
    HandlePath(cx, obj, Mul(om, parentToDisp));
  } else if (type == FPDF_PAGEOBJ_IMAGE) {
    HandleImage(cx, obj, Mul(om, parentToDisp));
  } else if (type == FPDF_PAGEOBJ_FORM && depth < 4) {
    int n = FPDFFormObj_CountObjects(obj);
    M3 inner = Mul(om, parentToDisp);
    for (int i = 0; i < n && cx.budget > 0; ++i) {
      FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(obj, i);
      if (c) WalkObject(cx, c, inner, depth + 1);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------

float MedianFontSize(const std::vector<TextRun>& runs) {
  std::vector<std::pair<float, int>> v;
  for (auto& r : runs) v.push_back({r.fontSize, (int)r.text.size()});
  if (v.empty()) return 10.f;
  std::sort(v.begin(), v.end());
  int total = 0; for (auto& p : v) total += p.second;
  int acc = 0;
  for (auto& p : v) { acc += p.second; if (acc * 2 >= total) return std::max(4.f, p.first); }
  return 10.f;
}

void Arrange(PageContent& pc, const std::vector<int>& runTable) {
  std::vector<TextRun>& runs = pc.runs;
  std::vector<int> horiz, others;
  for (int i = 0; i < (int)runs.size(); ++i) {
    float a = runs[i].angle;
    bool h = (a == 0.f || a == 180.f);
    (h ? horiz : others).push_back(i);
  }
  float medFs = MedianFontSize(runs);
  // 1. fragments from the non-table runs, tables become one fragment each
  std::vector<int> free;
  for (int id : horiz) if (runTable[id] < 0) free.push_back(id);
  auto lines = ClusterLines(runs, free);
  std::vector<Frag> frags;
  int lineNo = 0;
  float pageRtlVotes = 0, pageLtrVotes = 0;
  for (auto& ln : lines) {
    int rtlN = 0, ltrN = 0;
    for (int id : ln) (runs[id].rtl ? rtlN : ltrN) += (int)runs[id].text.size();
    bool rtl = rtlN > ltrN;
    (rtl ? pageRtlVotes : pageLtrVotes) += (float)(rtlN + ltrN);
    Frag cur; bool open = false;
    auto closeF = [&] { if (open) { cur.lineId = lineNo; cur.rtl = rtl; frags.push_back(std::move(cur)); cur = Frag(); open = false; } };
    for (int id : ln) {
      const TextRun& r = runs[id];
      if (open) {
        float fs = std::max(r.fontSize, medFs * 0.5f);
        const TextRun& pr = runs[cur.runs.back()];
        float gap = rtl ? (pr.x - (r.x + r.w)) : (r.x - (pr.x + pr.w));
        if (gap > 1.3f * std::max(fs, pr.fontSize)) closeF();
      }
      if (!open) { cur.l = r.x; cur.t = r.y; cur.r = r.x + r.w; cur.b = r.y + r.h; open = true; }
      else { cur.l = std::min(cur.l, r.x); cur.t = std::min(cur.t, r.y); cur.r = std::max(cur.r, r.x + r.w); cur.b = std::max(cur.b, r.y + r.h); }
      cur.runs.push_back(id);
    }
    closeF();
    ++lineNo;
  }
  // table fragments
  {
    std::map<int, Frag> tf;
    for (int id : horiz) {
      int t = runTable[id];
      if (t < 0) continue;
      auto it = tf.find(t);
      const TextRun& r = runs[id];
      if (it == tf.end()) { Frag f; f.l = r.x; f.t = r.y; f.r = r.x + r.w; f.b = r.y + r.h; f.table = t; f.lineId = 0; tf[t] = f; it = tf.find(t); }
      Frag& f = it->second;
      f.l = std::min(f.l, r.x); f.t = std::min(f.t, r.y); f.r = std::max(f.r, r.x + r.w); f.b = std::max(f.b, r.y + r.h);
    }
    for (auto& [t, f] : tf) frags.push_back(f);   // runs of table fragments are filled in by the caller via runTable
  }
  bool pageRtl = pageRtlVotes > pageLtrVotes;
  Layout L{frags};
  L.medianFs = medFs;
  std::vector<int> all(frags.size());
  std::iota(all.begin(), all.end(), 0);
  int cols = Order(L, all, 0, pageRtl, 0);
  pc.columns = std::max(1, cols);

  // 2. emit runs in order
  std::vector<TextRun> out;
  out.reserve(runs.size());
  int line = -1, block = -1;
  int lastLineId = -1, lastLeaf = -1;
  const TextRun* prevLine = nullptr;      // last run of the previous visual line in this leaf
  float prevBottom = 0, prevFs = 0, prevLeft = 0, prevRight = 0; bool prevBold = false; bool havePrev = false;
  for (size_t li = 0; li < L.leaves.size(); ++li) {
    Leaf& leaf = L.leaves[li];
    // table fragments inside a leaf: sorted by top like lines
    std::stable_sort(leaf.frags.begin(), leaf.frags.end(), [&](int a, int b) {
      const Frag& fa = frags[a]; const Frag& fb = frags[b];
      if (fa.table >= 0 || fb.table >= 0) return fa.t < fb.t;
      if (fa.lineId != fb.lineId) return fa.lineId < fb.lineId;
      return fa.rtl ? fa.l > fb.l : fa.l < fb.l;
    });
    leaf.column = leaf.column;
    lastLineId = -1; havePrev = false; prevLine = nullptr;
    int leafBlockStart = block + 1; (void)leafBlockStart;
    bool newLeaf = true;
    for (int fi : leaf.frags) {
      const Frag& f = frags[fi];
      if (f.table >= 0) {
        // emit table runs row-major: the caller supplies them through runTable
        std::vector<int> ids;
        for (int id : horiz) if (runTable[id] == f.table) ids.push_back(id);
        // order: by cell position is done by the table code; here by (rowband, x) using lines inside the table
        auto tl = ClusterLines(runs, ids);
        ++block;
        for (auto& l2 : tl) {
          ++line;
          for (int id : l2) { TextRun r = runs[id]; r.line = line; r.block = block; r.column = leaf.column; r.table = f.table; out.push_back(std::move(r)); }
        }
        havePrev = false; lastLineId = -1; newLeaf = false;
        continue;
      }
      bool newLine = f.lineId != lastLineId;
      if (newLine) {
        ++line;
        // block decision
        const TextRun& first = runs[f.runs.front()];
        float top = f.t, fs = 0; bool bold = true;
        for (int id : f.runs) { fs = std::max(fs, runs[id].fontSize); if (!runs[id].bold) bold = false; }
        bool brk = newLeaf || !havePrev;
        if (!brk) {
          float gap = top - prevBottom;
          float refFs = std::max(prevFs, fs);
          if (gap > 0.35f * refFs) brk = true;
          else if (std::fabs(fs - prevFs) > 0.12f * std::max(fs, prevFs)) brk = true;
          else if (bold != prevBold) brk = true;
          else if (first.x - prevLeft > 1.2f * refFs && prevRight > f.r - 0.5f * refFs) brk = false;   // wrapped hanging text
        }
        if (brk) ++block;
        lastLineId = f.lineId;
        prevBottom = f.b; prevFs = fs; prevLeft = f.l; prevRight = f.r; prevBold = bold; havePrev = true; newLeaf = false;
      } else {
        prevRight = std::max(prevRight, f.r);
      }
      for (int id : f.runs) { TextRun r = runs[id]; r.line = line; r.block = block; r.column = leaf.column; out.push_back(std::move(r)); }
    }
    (void)lastLeaf; (void)prevLine;
  }
  // 3. everything that is not horizontal: own lines/blocks, appended in content order
  for (int id : others) {
    TextRun r = runs[id];
    r.line = ++line; r.block = ++block; r.column = 0;
    out.push_back(std::move(r));
  }
  runs.swap(out);
}

} // namespace

// =====================================================================================================================

namespace detail {
bool DecodeFpdfBitmap(void* bmp, int& w, int& h, std::vector<uint8_t>& bgra, bool& alpha) {
  return DecodeBitmap((FPDF_BITMAP)bmp, w, h, bgra, alpha);
}
void ArrangeForeignRuns(PageContent& pc) {
  std::vector<int> none(pc.runs.size(), -1);
  Arrange(pc, none);
}
} // namespace detail

bool ExtractPage(const PdfDocument& doc, int index, PageContent& pc, bool withImages) {
  PdfLock lock;
  PageHandle ph(doc, index);
  if (!ph) return false;
  FPDF_PAGE pg = (FPDF_PAGE)ph.get();
  pc = PageContent();
  pc.index = index;
  pc.width = FPDF_GetPageWidthF(pg);
  pc.height = FPDF_GetPageHeightF(pg);
  pc.rotation = std::max(0, FPDFPage_GetRotation(pg)) * 90;
  pc.label = doc.PageLabel(index);
  Affine A = Calibrate(pg, pc.width, pc.height);

  Scratch sc;
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
  if (tp) {
    CollectGlyphs(tp, A, sc);
    FPDFText_ClosePage(tp);
  }
  BuildRuns(sc, pc.runs);
  pc.unmappedGlyphs = sc.unmapped;
  pc.textUnreliable = sc.total > 0 && sc.unmapped * 2 > sc.total;
  for (auto& r : pc.runs) for (wchar_t c : r.text) if (!iswspace(c) && c != 0xA0) { pc.hasText = true; break; }

  // path + image objects
  {
    ObjCtx cx{(FPDF_DOCUMENT)doc.Handle(), pg, A, withImages, &pc};
    int n = FPDFPage_CountObjects(pg);
    M3 base = FromAffine(A);
    for (int i = 0; i < n && cx.budget > 0; ++i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pg, i);
      if (obj) WalkObject(cx, obj, base, 0);
    }
  }

  // scanned page: no text and an image covers most of the page
  if (!pc.hasText) {
    double cover = 0, sum = 0, page = (double)pc.width * pc.height;
    for (auto& im : pc.images) {
      double x1 = std::max(0.f, im.x), y1 = std::max(0.f, im.y), x2 = std::min(pc.width, im.x + im.w), y2 = std::min(pc.height, im.y + im.h);
      if (x2 > x1 && y2 > y1) { cover = std::max(cover, (x2 - x1) * (y2 - y1)); sum += (x2 - x1) * (y2 - y1); }
    }
    pc.looksScanned = page > 0 && (cover >= 0.5 * page || sum >= 0.7 * page);
  }

  // tables, then reading order
  TableResult tr = FindTables(pc);
  Arrange(pc, tr.runTable);
  // run -> table index after arrangement is carried inside Arrange (TextRun::table)
  return true;
}

bool ExtractPagesStreaming(const fs::path& file, const std::function<bool(PageContent&&)>& sink, JobContext* ctx, std::wstring* err,
                           const std::wstring& password, bool withImages) {
  auto o = PdfDocument::Open(file, password);
  if (!o.ok()) { if (err) *err = o.message; return false; }
  int n = o.doc->PageCount();
  for (int i = 0; i < n; ++i) {
    if (ctx && ctx->Cancelled()) { if (err) *err = L"Cancelled"; return false; }
    PageContent pc;
    if (!ExtractPage(*o.doc, i, pc, withImages)) {
      // an unreadable page must not abort the whole document: give an empty page of the right size
      pc = PageContent(); pc.index = i;
      o.doc->PageSize(i, &pc.width, &pc.height);
    }
    if (!sink(std::move(pc))) break;
    if (ctx) ctx->SetProgress((i + 1.0) / n);
  }
  return true;
}

bool ExtractPages(const fs::path& file, std::vector<PageContent>& out, JobContext* ctx, std::wstring* err, const std::wstring& password,
                  bool withImages) {
  out.clear();
  return ExtractPagesStreaming(file, [&](PageContent&& p) { out.push_back(std::move(p)); return true; }, ctx, err, password, withImages);
}

std::vector<TableCandidate> DetectTables(const PageContent& page) { return FindTables(page).tables; }

std::wstring PageToPlainText(const PageContent& page) {
  std::wstring out;
  const auto& R = page.runs;
  size_t i = 0;
  int lastBlock = -1;
  std::vector<TableCandidate> tables;
  bool haveTables = false;
  while (i < R.size()) {
    if (R[i].table >= 0) {
      if (!haveTables) { tables = DetectTables(page); haveTables = true; }
      int t = R[i].table;
      if (!out.empty() && out.back() != L'\n') out += L'\n';
      if (!out.empty() && out.size() >= 2 && out[out.size() - 2] != L'\n') out += L'\n';
      if (t < (int)tables.size()) {
        for (auto& row : tables[t].cells) {
          std::wstring line;
          bool first = true;
          for (auto& c : row) {
            if (!first) line += L'\t';
            first = false;
            if (c.covered) continue;
            std::wstring tx = c.text;
            for (auto& ch : tx) if (ch == L'\n' || ch == L'\t') ch = L' ';
            line += tx;
          }
          out += line; out += L'\n';
        }
      }
      while (i < R.size() && R[i].table == t) ++i;
      lastBlock = -2;
      continue;
    }
    // one visual line
    int ln = R[i].line;
    std::wstring line;
    int rtlN = 0, ltrN = 0;
    size_t j = i;
    for (; j < R.size() && R[j].line == ln && R[j].table < 0; ++j) (R[j].rtl ? rtlN : ltrN) += (int)R[j].text.size();
    bool rtl = rtlN > ltrN;
    for (size_t k = i; k < j; ++k) {
      if (k > i && NeedsSpaceBetween(R[k - 1], R[k], rtl)) line += L' ';
      else if (k > i && std::fabs(R[k].x - (R[k - 1].x + R[k - 1].w)) > 0 && false) line += L' ';
      line += R[k].text;
    }
    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
    if (lastBlock != -1 && lastBlock != R[i].block && !out.empty()) out += L'\n';   // blank line between blocks
    out += line; out += L'\n';
    lastBlock = R[i].block;
    i = j;
  }
  return out;
}

} // namespace filo::pdf
