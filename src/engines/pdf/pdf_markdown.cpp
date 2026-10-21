#include "engines/pdf/pdf_markdown.h"
#include "engines/pdf/pdf_internal.h"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace filo::pdf {
using namespace detail;

std::wstring EscapeMarkdown(const std::wstring& s, bool atLineStart) {
  std::wstring o;
  o.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    wchar_t c = s[i];
    switch (c) {
      case L'\\': case L'`': case L'*': case L'_': case L'[': case L']': case L'<': case L'>': case L'|': case L'~':
        o += L'\\'; o += c; break;
      case L'#':
        if (i == 0 && atLineStart) o += L'\\';
        o += c; break;
      case L'&':
        o += c; break;
      default: o += c;
    }
  }
  return o;
}

namespace {

struct Line {
  std::vector<const TextRun*> runs;
  int block = 0;
  float top = 0, bottom = 0, left = 0, right = 0, fs = 0;
  bool allBold = true, mono = false, rtl = false;
  std::wstring plain;
};

bool IsMonoName(const std::wstring& n) {
  std::wstring l = ToLower(n);
  for (auto* k : {L"courier", L"consolas", L"mono", L"lucidaconsole", L"lucida console", L"menlo", L"typewriter", L"code"})
    if (l.find(k) != std::wstring::npos) return true;
  return false;
}

struct Marker { enum Kind { None, Bullet, Number, Alpha } kind = None; size_t len = 0; int number = 0; std::wstring text; };

Marker DetectMarker(const std::wstring& s) {
  Marker m;
  if (s.empty()) return m;
  static const std::wstring bullets = L"•◦▪▫●○■□‣⁃·►▶➢➤✓✔–—-*";
  wchar_t c = s[0];
  if (bullets.find(c) != std::wstring::npos) {
    if (s.size() == 1 || iswspace(s[1])) { m.kind = Marker::Bullet; m.len = 1; m.text = s.substr(0, 1); return m; }
    return m;
  }
  size_t i = 0;
  bool paren = false;
  if (s[0] == L'(') { paren = true; i = 1; }
  size_t d0 = i;
  while (i < s.size() && iswdigit(s[i]) && i - d0 < 4) ++i;
  if (i > d0 && i < s.size() + 0) {
    if (i < s.size() && (s[i] == L'.' || s[i] == L')') && (i + 1 == s.size() || iswspace(s[i + 1]))) {
      m.kind = Marker::Number; m.len = i + 1; m.number = std::stoi(s.substr(d0, i - d0)); m.text = s.substr(0, i + 1);
      return m;
    }
    if (i == s.size()) return m;
  }
  // letters / roman numerals
  i = paren ? 1 : 0;
  size_t l0 = i;
  while (i < s.size() && iswalpha(s[i]) && i - l0 < 5) ++i;
  if (i > l0 && i < s.size() && (s[i] == L'.' || s[i] == L')') && (i + 1 == s.size() || iswspace(s[i + 1]))) {
    std::wstring tok = ToLower(s.substr(l0, i - l0));
    bool roman = tok.find_first_not_of(L"ivxlcdm") == std::wstring::npos;
    if (tok.size() == 1 || roman) {
      m.kind = Marker::Alpha; m.len = i + 1; m.text = s.substr(0, i + 1);
      return m;
    }
  }
  return m;
}

struct RV { std::wstring text; bool bold, italic; const TextRun* run; };

std::wstring RenderRuns(const std::vector<RV>& rvs, bool emphasis, bool rtl, bool escapeFirst) {
  std::wstring out;
  struct Cur { std::wstring text; bool bold = false, italic = false; bool open = false; } cur;
  auto flush = [&] {
    if (!cur.open) return;
    std::wstring core = cur.text, lead, trail;
    size_t a = 0; while (a < core.size() && iswspace(core[a])) ++a;
    lead = core.substr(0, a); core.erase(0, a);
    size_t b = core.size(); while (b > 0 && iswspace(core[b - 1])) --b;
    trail = core.substr(b); core.erase(b);
    if (core.empty()) { out += lead + trail; cur = Cur(); return; }
    const wchar_t* mk = (emphasis && cur.bold && cur.italic) ? L"***" : (emphasis && cur.bold) ? L"**" : (emphasis && cur.italic) ? L"*" : L"";
    bool first = out.empty() && lead.empty();
    out += lead + mk + EscapeMarkdown(core, first && escapeFirst) + mk + trail;
    cur = Cur();
  };
  const RV* prev = nullptr;
  for (auto& rv : rvs) {
    if (rv.text.empty()) continue;
    std::wstring t = rv.text;
    if (prev && NeedsSpaceBetween(*prev->run, *rv.run, rtl)) t = L" " + t;
    bool sameStyle = cur.open && cur.bold == rv.bold && cur.italic == rv.italic;
    if (!sameStyle) { flush(); cur.open = true; cur.bold = rv.bold; cur.italic = rv.italic; }
    cur.text += t;
    prev = &rv;
  }
  flush();
  size_t e = out.size();
  while (e > 0 && iswspace(out[e - 1])) --e;
  out.erase(e);
  return out;
}

std::wstring PlainOf(const std::vector<RV>& rvs, bool rtl) {
  std::wstring o;
  const RV* prev = nullptr;
  for (auto& rv : rvs) {
    if (prev && NeedsSpaceBetween(*prev->run, *rv.run, rtl)) o += L' ';
    o += rv.text;
    prev = &rv;
  }
  while (!o.empty() && iswspace(o.back())) o.pop_back();
  size_t a = 0; while (a < o.size() && iswspace(o[a])) ++a;
  return o.substr(a);
}

std::wstring EscapeLeading(std::wstring s) {
  // protect text that would otherwise open a list / quote / heading
  size_t i = 0;
  while (i < s.size() && iswdigit(s[i])) ++i;
  if (i > 0 && i < s.size() && (s[i] == L'.' || s[i] == L')') && i + 1 < s.size() && s[i + 1] == L' ') { s.insert(i, L"\\"); return s; }
  if (!s.empty() && (s[0] == L'-' || s[0] == L'+') && s.size() > 1 && s[1] == L' ') { s.insert(0, L"\\"); return s; }
  if (s.rfind(L"---", 0) == 0 || s.rfind(L"===", 0) == 0) s.insert(0, L"\\");
  return s;
}

std::wstring TableMarkdown(const TableCandidate& t) {
  std::wstring o;
  auto cellText = [&](const TableCell& c) {
    if (c.covered) return std::wstring();
    std::wstring tx;
    for (wchar_t ch : c.text) {
      if (ch == L'\n') tx += L"<br>";
      else tx += ch;
    }
    // escape everything except the <br> markers we just inserted
    std::wstring esc;
    size_t i = 0;
    while (i < tx.size()) {
      if (tx.compare(i, 4, L"<br>") == 0) { esc += L"<br>"; i += 4; continue; }
      esc += EscapeMarkdown(std::wstring(1, tx[i]), false);
      ++i;
    }
    return esc;
  };
  for (int r = 0; r < t.rows; ++r) {
    o += L"|";
    for (int c = 0; c < t.cols; ++c) { std::wstring ct = cellText(t.cells[r][c]); o += L" " + ct + L" |"; }
    o += L"\n";
    if (r == 0) { o += L"|"; for (int c = 0; c < t.cols; ++c) o += L" --- |"; o += L"\n"; }
  }
  return o;
}

struct Item {
  enum Kind { Block, Table } kind = Block;
  std::vector<Line> lines;
  int table = -1;
  float top = 0, left = 0, right = 0;
  std::wstring md;
  bool isList = false;
};

} // namespace

void MarkdownWriter::Analyze(const PageContent& pc) {
  for (auto& r : pc.runs) {
    if (r.table >= 0) continue;
    int key = (int)std::lround(r.fontSize * 2);
    sizeChars_[key] += (long)r.text.size();
  }
}

void MarkdownWriter::Prepare() {
  long best = -1; int bestKey = 22;
  for (auto& [k, n] : sizeChars_) if (n > best) { best = n; bestKey = k; }
  bodyFs_ = bestKey / 2.f;
  headingKeys_.clear();
  for (auto it = sizeChars_.rbegin(); it != sizeChars_.rend(); ++it)
    if (it->first / 2.f >= bodyFs_ * 1.15f && it->second > 0) headingKeys_.push_back(it->first);
  if (headingKeys_.size() > 6) headingKeys_.resize(6);
}

std::wstring MarkdownWriter::RenderPage(const PageContent& pc, const std::function<std::wstring(int, const ImageBlock&)>& saveImage) {
  std::vector<TableCandidate> tables;
  bool haveTables = false;
  std::vector<Item> items;

  // ---- lines -> blocks
  size_t i = 0;
  const auto& R = pc.runs;
  while (i < R.size()) {
    if (R[i].table >= 0) {
      if (!haveTables) { tables = DetectTables(pc); haveTables = true; }
      int t = R[i].table;
      Item it; it.kind = Item::Table; it.table = t;
      it.top = 1e9f; it.left = 1e9f; it.right = -1e9f;
      while (i < R.size() && R[i].table == t) { it.top = std::min(it.top, R[i].y); it.left = std::min(it.left, R[i].x); it.right = std::max(it.right, R[i].x + R[i].w); ++i; }
      items.push_back(std::move(it));
      continue;
    }
    Line ln;
    ln.block = R[i].block;
    int lineNo = R[i].line;
    ln.top = 1e9f; ln.bottom = -1e9f; ln.left = 1e9f; ln.right = -1e9f;
    int rtlN = 0, ltrN = 0;
    size_t j = i;
    for (; j < R.size() && R[j].line == lineNo && R[j].table < 0; ++j) {
      const TextRun& r = R[j];
      ln.runs.push_back(&r);
      ln.top = std::min(ln.top, r.y); ln.bottom = std::max(ln.bottom, r.y + r.h);
      ln.left = std::min(ln.left, r.x); ln.right = std::max(ln.right, r.x + r.w);
      ln.fs = std::max(ln.fs, r.fontSize);
      if (!r.bold) ln.allBold = false;
      (r.rtl ? rtlN : ltrN) += (int)r.text.size();
    }
    ln.rtl = rtlN > ltrN;
    bool allMono = true;
    for (auto* r : ln.runs) if (!IsMonoName(r->fontName)) allMono = false;
    ln.mono = allMono;
    std::vector<RV> rvs;
    for (auto* r : ln.runs) rvs.push_back({r->text, r->bold, r->italic, r});
    ln.plain = PlainOf(rvs, ln.rtl);
    i = j;
    if (ln.plain.empty()) continue;
    if (!items.empty() && items.back().kind == Item::Block && items.back().lines.back().block == ln.block) {
      Item& it = items.back();
      it.lines.push_back(std::move(ln));
      it.left = std::min(it.left, it.lines.back().left); it.right = std::max(it.right, it.lines.back().right);
    } else {
      Item it; it.kind = Item::Block; it.top = ln.top; it.left = ln.left; it.right = ln.right;
      it.lines.push_back(std::move(ln));
      items.push_back(std::move(it));
    }
  }

  // ---- render every item
  for (auto& it : items) {
    if (it.kind == Item::Table) {
      if (it.table >= 0 && it.table < (int)tables.size()) it.md = TableMarkdown(tables[it.table]);
      continue;
    }
    auto& L = it.lines;
    bool allMono = std::all_of(L.begin(), L.end(), [](const Line& l) { return l.mono; });
    if (allMono) {
      std::wstring code = L"```\n";
      for (auto& l : L) code += l.plain + L"\n";
      code += L"```\n";
      it.md = code;
      continue;
    }
    float blockFs = 0; bool allBold = true; size_t chars = 0;
    for (auto& l : L) { blockFs = std::max(blockFs, l.fs); allBold = allBold && l.allBold; chars += l.plain.size(); }
    // size based heading
    if (blockFs >= bodyFs_ * 1.15f && L.size() <= 4 && chars <= 200 && !headingKeys_.empty()) {
      int key = (int)std::lround(blockFs * 2);
      size_t lvl = 0, bestDiff = 1 << 30;
      for (size_t k = 0; k < headingKeys_.size(); ++k) { size_t d = (size_t)std::abs(headingKeys_[k] - key); if (d < bestDiff) { bestDiff = d; lvl = k; } }
      std::wstring text;
      for (size_t k = 0; k < L.size(); ++k) { if (k) text += L' '; text += L[k].plain; }
      it.md = std::wstring(lvl + 1, L'#') + L" " + EscapeMarkdown(text, false) + L"\n";
      continue;
    }
    // bold stand-alone label
    if (allBold && L.size() <= 2 && chars <= 100 && blockFs >= bodyFs_ * 0.95f) {
      std::wstring text;
      for (size_t k = 0; k < L.size(); ++k) { if (k) text += L' '; text += L[k].plain; }
      wchar_t last = text.empty() ? L' ' : text.back();
      if (last != L'.' && last != L',' && last != L';' && !DetectMarker(text).len) {
        size_t lvl = std::min<size_t>(6, headingKeys_.size() + 1);
        it.md = std::wstring(lvl, L'#') + L" " + EscapeMarkdown(text, false) + L"\n";
        continue;
      }
    }
    // lists / paragraphs
    std::vector<Marker> marks(L.size());
    for (size_t k = 0; k < L.size(); ++k) marks[k] = DetectMarker(L[k].plain);
    for (size_t k = 0; k < L.size(); ++k)
      if (marks[k].kind == Marker::Alpha) {
        bool sib = (k > 0 && marks[k - 1].kind != Marker::None) || (k + 1 < L.size() && marks[k + 1].kind != Marker::None);
        if (!sib) marks[k] = Marker();
      }
    float minX = 1e9f;
    for (size_t k = 0; k < L.size(); ++k) if (marks[k].kind != Marker::None) minX = std::min(minX, L[k].left);
    std::wstring out;
    std::wstring para;             // current paragraph / item text
    std::wstring itemPrefix;       // non-empty while building a list item
    auto lineMd = [&](const Line& l, size_t stripChars) {
      std::vector<RV> rvs;
      size_t strip = stripChars;
      for (auto* r : l.runs) {
        std::wstring t = r->text;
        if (strip) {
          size_t a = 0; while (a < t.size() && iswspace(t[a])) ++a;
          if (strip >= t.size() - a) { strip -= std::min(strip, t.size() - a); t.clear(); }
          else { t = t.substr(a + strip); strip = 0; }
          if (!t.empty()) { size_t b = 0; while (b < t.size() && iswspace(t[b])) ++b; t = t.substr(b); }
        }
        rvs.push_back({t, r->bold, r->italic, r});
      }
      return RenderRuns(rvs, true, l.rtl, false);
    };
    auto flushPara = [&] {
      if (para.empty() && itemPrefix.empty()) return;
      if (itemPrefix.empty()) out += EscapeLeading(para) + L"\n\n";
      else out += itemPrefix + para + L"\n";
      para.clear(); itemPrefix.clear();
    };
    for (size_t k = 0; k < L.size(); ++k) {
      if (marks[k].kind != Marker::None) {
        flushPara();
        int level = (int)std::floor((L[k].left - minX) / std::max(8.f, bodyFs_ * 1.2f) + 0.4f);
        level = std::clamp(level, 0, 5);
        std::wstring ind(level * 2, L' ');
        if (marks[k].kind == Marker::Number) itemPrefix = ind + std::to_wstring(marks[k].number) + L". ";
        else if (marks[k].kind == Marker::Alpha) itemPrefix = ind + L"- " + EscapeMarkdown(marks[k].text, false) + L" ";
        else itemPrefix = ind + L"- ";
        // marker may be its own run
        size_t strip = marks[k].len;
        para = lineMd(L[k], strip);
      } else {
        std::wstring t = lineMd(L[k], 0);
        if (t.empty()) continue;
        if (!para.empty()) para += L" ";
        para += t;
      }
    }
    bool hadList = false;
    for (auto& m : marks) if (m.kind != Marker::None) hadList = true;
    flushPara();
    it.isList = hadList && std::all_of(marks.begin(), marks.end(), [](const Marker& m) { return m.kind != Marker::None; });
    if (hadList) out += L"\n";
    // trim trailing blank lines (one remains between items)
    while (out.size() >= 2 && out[out.size() - 1] == L'\n' && out[out.size() - 2] == L'\n') out.pop_back();
    it.md = out;
  }

  // ---- images: insert before the first item below them in the same horizontal band
  std::vector<std::pair<size_t, std::wstring>> inserts;   // (item index, markdown)
  for (size_t k = 0; k < pc.images.size(); ++k) {
    const ImageBlock& im = pc.images[k];
    if (im.pixelW < 12 || im.pixelH < 12 || im.w < 8 || im.h < 8) continue;
    if (pc.looksScanned) continue;
    std::wstring rel = saveImage ? saveImage((int)k, im) : L"";
    if (rel.empty()) continue;
    size_t at = items.size();
    for (size_t q = 0; q < items.size(); ++q) {
      float ov = std::min(items[q].right, im.x + im.w) - std::max(items[q].left, im.x);
      float mw = std::min(items[q].right - items[q].left, im.w);
      if (items[q].top >= im.y - 2 && ov > 0.3f * std::max(1.f, mw)) { at = q; break; }
    }
    inserts.push_back({at, L"![Page " + std::to_wstring(pc.index + 1) + L" image " + std::to_wstring(k + 1) + L"](" + rel + L")\n"});
  }

  std::wstring page;
  for (size_t q = 0; q <= items.size(); ++q) {
    for (auto& [at, md] : inserts) if (at == q) { page += md + L"\n"; }
    if (q < items.size() && !items[q].md.empty()) {
      std::wstring md = items[q].md;
      while (md.size() >= 2 && md[md.size() - 1] == L'\n' && md[md.size() - 2] == L'\n') md.pop_back();
      page += md;
      if (page.back() != L'\n') page += L'\n';
      bool tightNext = items[q].isList && q + 1 < items.size() && items[q + 1].isList &&
                       std::none_of(inserts.begin(), inserts.end(), [&](auto& in) { return in.first == q + 1; });
      if (!tightNext) page += L"\n";
    }
  }
  while (page.size() >= 2 && page[page.size() - 1] == L'\n' && page[page.size() - 2] == L'\n') page.pop_back();
  if (page.empty()) return L"";
  std::wstring result;
  if (wrote_ && opt_.pageBreaks) result += L"\n---\n\n";
  else if (wrote_) result += L"\n";
  result += page;
  wrote_ = true;
  return result;
}

} // namespace filo::pdf
