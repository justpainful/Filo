// Table detection: ruling-line grids (with merged cells) and conservative aligned-text grids.
#include "engines/pdf/pdf_internal.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cwctype>

namespace filo::pdf::detail {
namespace {

struct HLine { float y, x1, x2; };
struct VLine { float x, y1, y2; };

struct DSU {
  std::vector<int> p;
  explicit DSU(int n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int f(int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
  void u(int a, int b) { a = f(a); b = f(b); if (a != b) p[b] = a; }
};

std::vector<float> ClusterPositions(std::vector<float> v, float tol) {
  std::sort(v.begin(), v.end());
  std::vector<float> out;
  size_t i = 0;
  while (i < v.size()) {
    size_t j = i; float sum = v[i]; int n = 1;
    while (j + 1 < v.size() && v[j + 1] - v[j] <= tol) { ++j; sum += v[j]; ++n; }
    out.push_back(sum / n);
    i = j + 1;
  }
  return out;
}

// Fraction of [a,b] covered by the union of [lo,hi] intervals.
float Coverage(std::vector<std::pair<float, float>> iv, float a, float b) {
  if (b <= a) return 1;
  std::sort(iv.begin(), iv.end());
  float covered = 0, cur = a;
  for (auto& p : iv) {
    float lo = std::max(p.first, cur), hi = std::min(p.second, b);
    if (hi > lo) { covered += hi - lo; cur = hi; }
    else if (p.second > cur) cur = std::max(cur, std::min(p.second, b));
  }
  return covered / (b - a);
}

int FindIndex(const std::vector<float>& pos, float v, float tol) {
  // largest i with pos[i] <= v + tol
  int idx = -1;
  for (int i = 0; i < (int)pos.size(); ++i) if (pos[i] <= v + tol) idx = i; else break;
  return idx;
}

bool IsListMarker(const std::wstring& s) {
  std::wstring t = Trim(s);
  if (t.empty()) return false;
  if (t.size() == 1) {
    static const std::wstring marks = L"•◦▪▫●○■□–—-*·►▶‣⁃➤→";
    return marks.find(t[0]) != std::wstring::npos;
  }
  size_t i = 0;
  if (t[0] == L'(') ++i;
  size_t d = i;
  while (i < t.size() && iswalnum(t[i]) && i - d < 4) ++i;
  if (i == d || i >= t.size()) return false;
  if (t[i] != L'.' && t[i] != L')') return false;
  return i + 1 == t.size();
}

bool BoldRow(const std::vector<TextRun>& runs, const std::vector<std::vector<int>>& cellRuns) {
  int n = 0, b = 0;
  for (auto& c : cellRuns) for (int id : c) { ++n; if (runs[id].bold) ++b; }
  return n > 0 && b == n;
}

// ---------------------------------------------------------------------------------------------------------------------
// ruled tables

void FindRuled(const PageContent& page, std::vector<TableCandidate>& tables, std::vector<std::vector<int>>& tableRuns) {
  std::vector<HLine> H; std::vector<VLine> V;
  for (auto& r : page.rules) {
    if (r.y1 == r.y2) { if (r.x2 - r.x1 >= 6) H.push_back({r.y1, r.x1, r.x2}); }
    else if (r.x1 == r.x2) { if (r.y2 - r.y1 >= 6) V.push_back({r.x1, r.y1, r.y2}); }
  }
  if (H.size() + V.size() > 6000 || H.size() < 2 || V.size() < 2) return;
  // merge collinear overlapping segments
  auto mergeH = [](std::vector<HLine> v) {
    std::sort(v.begin(), v.end(), [](const HLine& a, const HLine& b) { return a.y != b.y ? a.y < b.y : a.x1 < b.x1; });
    std::vector<HLine> out;
    for (auto& h : v) {
      bool merged = false;
      for (int k = (int)out.size() - 1; k >= 0 && out[k].y >= h.y - 1.5f; --k)
        if (std::fabs(out[k].y - h.y) <= 1.5f && h.x1 <= out[k].x2 + 3.f) { out[k].x2 = std::max(out[k].x2, h.x2); merged = true; break; }
      if (!merged) out.push_back(h);
    }
    return out;
  };
  auto mergeV = [](std::vector<VLine> v) {
    std::sort(v.begin(), v.end(), [](const VLine& a, const VLine& b) { return a.x != b.x ? a.x < b.x : a.y1 < b.y1; });
    std::vector<VLine> out;
    for (auto& h : v) {
      bool merged = false;
      for (int k = (int)out.size() - 1; k >= 0 && out[k].x >= h.x - 1.5f; --k)
        if (std::fabs(out[k].x - h.x) <= 1.5f && h.y1 <= out[k].y2 + 3.f) { out[k].y2 = std::max(out[k].y2, h.y2); merged = true; break; }
      if (!merged) out.push_back(h);
    }
    return out;
  };
  H = mergeH(std::move(H)); V = mergeV(std::move(V));
  int nh = (int)H.size(), nv = (int)V.size();
  DSU d(nh + nv);
  const float tol = 2.f;
  for (int i = 0; i < nh; ++i)
    for (int j = 0; j < nv; ++j)
      if (V[j].x >= H[i].x1 - tol && V[j].x <= H[i].x2 + tol && H[i].y >= V[j].y1 - tol && H[i].y <= V[j].y2 + tol) d.u(i, nh + j);
  std::vector<std::vector<int>> comps(nh + nv);
  for (int i = 0; i < nh + nv; ++i) comps[d.f(i)].push_back(i);

  for (auto& comp : comps) {
    if (comp.size() < 4) continue;
    std::vector<float> yv, xv;
    std::vector<HLine> ch; std::vector<VLine> cv;
    for (int id : comp) { if (id < nh) { ch.push_back(H[id]); yv.push_back(H[id].y); } else { cv.push_back(V[id - nh]); xv.push_back(V[id - nh].x); } }
    if (ch.size() < 2 || cv.size() < 2) continue;
    auto ys = ClusterPositions(yv, 1.5f), xs = ClusterPositions(xv, 1.5f);
    int rows = (int)ys.size() - 1, cols = (int)xs.size() - 1;
    if (rows < 1 || cols < 1 || rows * cols < 2) continue;
    if (rows > 400 || cols > 60) continue;

    // separators
    auto vSep = [&](int c, int r) {   // vertical line at xs[c] over row r
      std::vector<std::pair<float, float>> iv;
      for (auto& v : cv) if (std::fabs(v.x - xs[c]) <= 2.f) iv.push_back({v.y1, v.y2});
      return Coverage(iv, ys[r], ys[r + 1]) >= 0.7f;
    };
    auto hSep = [&](int r, int c) {   // horizontal line at ys[r] over column c
      std::vector<std::pair<float, float>> iv;
      for (auto& h : ch) if (std::fabs(h.y - ys[r]) <= 2.f) iv.push_back({h.x1, h.x2});
      return Coverage(iv, xs[c], xs[c + 1]) >= 0.7f;
    };
    DSU cell(rows * cols);
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < cols; ++c) {
        if (c + 1 < cols && !vSep(c + 1, r)) cell.u(r * cols + c, r * cols + c + 1);
        if (r + 1 < rows && !hSep(r + 1, c)) cell.u(r * cols + c, (r + 1) * cols + c);
      }
    // each merged set must be a rectangle
    struct Box { int r0 = 1 << 30, c0 = 1 << 30, r1 = -1, c1 = -1, n = 0; };
    std::vector<Box> boxes(rows * cols);
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < cols; ++c) {
        Box& b = boxes[cell.f(r * cols + c)];
        b.r0 = std::min(b.r0, r); b.c0 = std::min(b.c0, c); b.r1 = std::max(b.r1, r); b.c1 = std::max(b.c1, c); ++b.n;
      }
    bool rect = true;
    for (auto& b : boxes) if (b.n && b.n != (b.r1 - b.r0 + 1) * (b.c1 - b.c0 + 1)) rect = false;
    if (!rect) { cell = DSU(rows * cols); for (auto& b : boxes) b = Box(); for (int i = 0; i < rows * cols; ++i) { boxes[i].r0 = boxes[i].r1 = i / cols; boxes[i].c0 = boxes[i].c1 = i % cols; boxes[i].n = 1; } }

    TableCandidate t;
    t.ruled = true; t.rows = rows; t.cols = cols;
    t.x = xs.front(); t.y = ys.front(); t.w = xs.back() - xs.front(); t.h = ys.back() - ys.front();
    t.cells.assign(rows, std::vector<TableCell>(cols));
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < cols; ++c) {
        TableCell& tc = t.cells[r][c];
        tc.row = r; tc.col = c;
        tc.x = xs[c]; tc.y = ys[r]; tc.w = xs[c + 1] - xs[c]; tc.h = ys[r + 1] - ys[r];
        const Box& b = boxes[cell.f(r * cols + c)];
        if (b.r0 == r && b.c0 == c) {
          tc.rowSpan = b.r1 - b.r0 + 1; tc.colSpan = b.c1 - b.c0 + 1;
          tc.w = xs[b.c1 + 1] - xs[c]; tc.h = ys[b.r1 + 1] - ys[r];
        } else tc.covered = true;
      }
    // assign runs
    std::vector<std::vector<std::vector<int>>> cellRuns(rows, std::vector<std::vector<int>>(cols));
    std::vector<int> mine;
    for (int i = 0; i < (int)page.runs.size(); ++i) {
      const TextRun& r = page.runs[i];
      float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
      if (cx < t.x - 1 || cx > t.x + t.w + 1 || cy < t.y - 1 || cy > t.y + t.h + 1) continue;
      int c = std::clamp(FindIndex(xs, cx, 0.f), 0, cols - 1), rr = std::clamp(FindIndex(ys, cy, 0.f), 0, rows - 1);
      const Box& b = boxes[cell.f(rr * cols + c)];
      cellRuns[b.r0][b.c0].push_back(i);
      mine.push_back(i);
    }
    int nonEmpty = 0;
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < cols; ++c) {
        t.cells[r][c].text = JoinRunsText(page.runs, cellRuns[r][c]);
        if (!t.cells[r][c].text.empty()) ++nonEmpty;
      }
    if (nonEmpty < 2) continue;
    std::vector<std::vector<int>> firstRow;
    for (int c = 0; c < cols; ++c) if (!cellRuns[0][c].empty()) firstRow.push_back(cellRuns[0][c]);
    t.headerRow = rows > 1 && BoldRow(page.runs, firstRow);
    tables.push_back(std::move(t));
    tableRuns.push_back(std::move(mine));
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// aligned (borderless) tables

struct ACell { float l, r; std::vector<int> runs; int chars = 0, words = 0; };
struct ARow { std::vector<ACell> cells; float top, bottom, fs; };

void FindAligned(const PageContent& page, const std::vector<char>& taken, std::vector<TableCandidate>& tables,
                 std::vector<std::vector<int>>& tableRuns) {
  std::vector<int> ids;
  for (int i = 0; i < (int)page.runs.size(); ++i) {
    if (taken[i]) continue;
    float a = page.runs[i].angle;
    if (a == 0.f || a == 180.f) ids.push_back(i);
  }
  if (ids.size() < 6) return;
  auto lines = ClusterLines(page.runs, ids);
  std::vector<ARow> rows;
  for (auto& ln : lines) {
    ARow row; row.top = 1e9f; row.bottom = -1e9f; row.fs = 0;
    int rtlN = 0, ltrN = 0;
    for (int id : ln) (page.runs[id].rtl ? rtlN : ltrN) += (int)page.runs[id].text.size();
    bool rtl = rtlN > ltrN;
    // within a line the runs are in reading order; for geometry use left-to-right order
    std::vector<int> byX = ln;
    std::sort(byX.begin(), byX.end(), [&](int a, int b) { return page.runs[a].x < page.runs[b].x; });
    for (int id : byX) {
      const TextRun& r = page.runs[id];
      row.top = std::min(row.top, r.y); row.bottom = std::max(row.bottom, r.y + r.h); row.fs = std::max(row.fs, r.fontSize);
      float gapLimit = 1.0f * std::max(r.fontSize, 6.f);
      if (!row.cells.empty() && r.x - row.cells.back().r <= gapLimit) {
        ACell& c = row.cells.back(); c.r = std::max(c.r, r.x + r.w); c.runs.push_back(id);
      } else { ACell c; c.l = r.x; c.r = r.x + r.w; c.runs.push_back(id); row.cells.push_back(std::move(c)); }
    }
    for (auto& c : row.cells) {
      std::wstring tx = JoinRunsText(page.runs, c.runs);
      c.chars = (int)tx.size();
      c.words = 1; for (wchar_t ch : tx) if (ch == L' ') ++c.words;
    }
    (void)rtl;
    rows.push_back(std::move(row));
  }
  float medFs = 10; { std::vector<float> v; for (auto& r : rows) v.push_back(r.fs); if (!v.empty()) { std::sort(v.begin(), v.end()); medFs = std::max(4.f, v[v.size() / 2]); } }
  const float minGap = std::max(0.8f * medFs, 6.f);

  size_t i = 0;
  while (i < rows.size()) {
    if (rows[i].cells.size() < 2) { ++i; continue; }
    // grow a group starting at row i
    struct Col { float l, r; };
    auto buildCols = [&](const ARow& r0) {
      std::vector<Col> cols;
      for (auto& c : r0.cells) cols.push_back({c.l, c.r});
      return cols;
    };
    std::vector<Col> cols = buildCols(rows[i]);
    size_t j = i + 1;
    auto compatible = [&](const ARow& r, std::vector<Col>& cs, bool apply) {
      // no cell may bridge a gap between columns or sit inside a gap
      std::vector<Col> n = cs;
      for (auto& c : r.cells) {
        int hit = -1; int cnt = 0;
        for (int k = 0; k < (int)n.size(); ++k) if (c.l < n[k].r + 0.5f && c.r > n[k].l - 0.5f) { ++cnt; hit = k; }
        if (cnt == 0) return false;      // inside a gap
        if (cnt > 1) return false;       // bridges columns
        n[hit].l = std::min(n[hit].l, c.l); n[hit].r = std::max(n[hit].r, c.r);
      }
      // columns must stay separated
      for (size_t k = 0; k + 1 < n.size(); ++k) if (n[k + 1].l - n[k].r < minGap * 0.5f) return false;
      if (apply) cs = n;
      return true;
    };
    for (; j < rows.size(); ++j) {
      float gap = rows[j].top - rows[j - 1].bottom;
      float lh = std::max(rows[j].bottom - rows[j].top, 1.f);
      if (gap > 1.2f * lh) break;
      if (!compatible(rows[j], cols, true)) break;
    }
    size_t n = j - i;
    // validate
    bool ok = n >= 3 && cols.size() >= 2;
    int multi = 0, cellsN = 0, chars = 0, words = 0;
    if (ok) {
      for (size_t k = i; k < j; ++k) {
        if (rows[k].cells.size() >= 2) ++multi;
        for (auto& c : rows[k].cells) { ++cellsN; chars += c.chars; words += c.words; }
      }
      if (multi * 5 < (int)n * 3) ok = false;                                   // at least 60% multi-cell rows
      if (cellsN && (double)words / cellsN > 5.0) ok = false;                   // prose, not cells
      if (cellsN && cols.size() == 2 && (double)chars / cellsN > 32.0) ok = false;
      if (cellsN && (double)chars / cellsN > 40.0) ok = false;
    }
    if (ok) {
      // a first column that only holds list markers is a list, not a table
      int markers = 0, firstCol = 0;
      for (size_t k = i; k < j; ++k)
        for (auto& cell : rows[k].cells)
          if (cell.l < cols[0].r + 0.5f && cell.r > cols[0].l - 0.5f) {
            ++firstCol;
            if (IsListMarker(JoinRunsText(page.runs, cell.runs))) ++markers;
          }
      if (firstCol > 0 && markers * 10 >= firstCol * 8) ok = false;
    }
    if (ok) {
      // alignment consistency per column (left / right / centre)
      int goodCols = 0;
      for (size_t c = 0; c < cols.size(); ++c) {
        std::vector<float> L, R, C;
        for (size_t k = i; k < j; ++k)
          for (auto& cell : rows[k].cells)
            if (cell.l < cols[c].r + 0.5f && cell.r > cols[c].l - 0.5f) { L.push_back(cell.l); R.push_back(cell.r); C.push_back((cell.l + cell.r) / 2); }
        auto spreadOk = [&](std::vector<float> v) {
          if (v.size() < 2) return true;
          std::sort(v.begin(), v.end());
          float med = v[v.size() / 2]; int nearN = 0;
          for (float x : v) if (std::fabs(x - med) <= std::max(2.f, 0.3f * medFs)) ++nearN;
          return nearN * 10 >= (int)v.size() * 6;
        };
        if (spreadOk(L) || spreadOk(R) || spreadOk(C)) ++goodCols;
      }
      if (goodCols * 2 < (int)cols.size()) ok = false;
    }
    if (!ok) { i = std::max(i + 1, ok ? j : i + 1); continue; }

    TableCandidate t;
    t.ruled = false; t.rows = (int)n; t.cols = (int)cols.size();
    float x0 = cols.front().l, x1 = cols.back().r, y0 = rows[i].top, y1 = rows[j - 1].bottom;
    t.x = x0; t.y = y0; t.w = x1 - x0; t.h = y1 - y0;
    t.cells.assign(n, std::vector<TableCell>(cols.size()));
    std::vector<int> mine;
    std::vector<std::vector<int>> firstRow;
    for (size_t k = i; k < j; ++k) {
      size_t r = k - i;
      for (size_t c = 0; c < cols.size(); ++c) {
        TableCell& tc = t.cells[r][c];
        tc.row = (int)r; tc.col = (int)c; tc.x = cols[c].l; tc.y = rows[k].top; tc.w = cols[c].r - cols[c].l; tc.h = rows[k].bottom - rows[k].top;
      }
      for (auto& cell : rows[k].cells) {
        size_t c = 0;
        for (size_t q = 0; q < cols.size(); ++q) if (cell.l < cols[q].r + 0.5f && cell.r > cols[q].l - 0.5f) c = q;
        TableCell& tc = t.cells[r][c];
        tc.text = JoinRunsText(page.runs, cell.runs);
        for (int id : cell.runs) mine.push_back(id);
        if (r == 0) firstRow.push_back(cell.runs);
      }
    }
    t.headerRow = n > 1 && BoldRow(page.runs, firstRow);
    tables.push_back(std::move(t));
    tableRuns.push_back(std::move(mine));
    i = j;
  }
}

} // namespace

TableResult FindTables(const PageContent& page) {
  TableResult res;
  res.runTable.assign(page.runs.size(), -1);
  std::vector<TableCandidate> tables;
  std::vector<std::vector<int>> truns;
  FindRuled(page, tables, truns);
  std::vector<char> taken(page.runs.size(), 0);
  for (auto& v : truns) for (int id : v) taken[id] = 1;
  FindAligned(page, taken, tables, truns);

  std::vector<int> order(tables.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return tables[a].y != tables[b].y ? tables[a].y < tables[b].y : tables[a].x < tables[b].x;
  });
  for (size_t k = 0; k < order.size(); ++k) {
    for (int id : truns[order[k]]) res.runTable[id] = (int)k;
    res.tables.push_back(std::move(tables[order[k]]));
  }
  return res;
}

} // namespace filo::pdf::detail
