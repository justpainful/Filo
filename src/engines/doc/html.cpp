#include "engines/doc/html.h"
#include "engines/doc/text_io.h"
#include "engines/doc/xml.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace filo::doc {

namespace {

// ============================================================================================================================
// Reader
// ============================================================================================================================
struct HNode {
  std::string tag;                 // lower-case; empty for text
  xml::AttrList attrs;
  std::vector<std::unique_ptr<HNode>> kids;
  std::string text;
  bool isText = false;
  HNode* parent = nullptr;
  const std::string* Attr(const char* n) const { for (auto& a : attrs) if (a.first == n) return &a.second; return nullptr; }
  std::string AttrOr(const char* n) const { auto* a = Attr(n); return a ? *a : std::string(); }
};

const std::set<std::string>& VoidTags() {
  static const std::set<std::string> s = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr", "basefont", "frame"};
  return s;
}
bool IsBlockTag(const std::string& t) {
  static const std::set<std::string> s = {"address", "article", "aside", "blockquote", "body", "center", "dd", "details", "div", "dl", "dt", "fieldset", "figcaption", "figure",
    "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "html", "li", "main", "nav", "ol", "p", "pre", "section", "summary", "table", "ul", "tr", "td", "th", "thead", "tbody", "tfoot", "caption"};
  return s.count(t) > 0;
}

std::string ToLowerTag(std::string_view s) { return LowerAscii(s); }

class HtmlTree {
 public:
  std::unique_ptr<HNode> root = std::make_unique<HNode>();
  void Build(std::string_view h) {
    HNode* cur = root.get();
    size_t i = 0, n = h.size();
    std::string text;
    auto flush = [&] {
      if (text.empty()) return;
      auto t = std::make_unique<HNode>(); t->isText = true; t->text = std::move(text); text.clear(); t->parent = cur;
      cur->kids.push_back(std::move(t));
    };
    auto closeTo = [&](const char* tag) -> bool {           // pops to the nearest open `tag`; false if none
      for (HNode* p = cur; p && p != root.get(); p = p->parent) if (p->tag == tag) { cur = p->parent; return true; }
      return false;
    };
    auto hasOpen = [&](const char* tag, std::initializer_list<const char*> stops) -> bool {
      for (HNode* p = cur; p && p != root.get(); p = p->parent) {
        if (p->tag == tag) return true;
        for (auto* s : stops) if (p->tag == s) return false;
      }
      return false;
    };
    while (i < n) {
      if (h[i] != '<') {
        size_t j = h.find('<', i);
        if (j == std::string_view::npos) j = n;
        xml::DecodeEntitiesInto(h.substr(i, j - i), text);
        i = j;
        continue;
      }
      if (h.compare(i, 4, "<!--") == 0) { size_t e = h.find("-->", i + 4); i = e == std::string_view::npos ? n : e + 3; continue; }
      if (h.compare(i, 9, "<![CDATA[") == 0) { size_t e = h.find("]]>", i); text.append(h.substr(i + 9, (e == std::string_view::npos ? n : e) - i - 9)); i = e == std::string_view::npos ? n : e + 3; continue; }
      if (i + 1 < n && (h[i + 1] == '!' || h[i + 1] == '?')) { size_t e = h.find('>', i); i = e == std::string_view::npos ? n : e + 1; continue; }
      if (i + 1 < n && h[i + 1] == '/') {
        size_t e = h.find('>', i);
        if (e == std::string_view::npos) { i = n; break; }
        std::string name = ToLowerTag(TrimAscii(h.substr(i + 2, e - i - 2)));
        i = e + 1;
        flush();
        if (name == "p" && !hasOpen("p", {"td", "th", "li", "table", "body"})) {   // </p> with no open p creates an empty paragraph in HTML; ignore
          continue;
        }
        closeTo(name.c_str());
        continue;
      }
      size_t j = i + 1;
      if (j >= n || !(isalpha((unsigned char)h[j]))) { text.push_back('<'); ++i; continue; }
      size_t ns = j;
      while (j < n && !isspace((unsigned char)h[j]) && h[j] != '>' && h[j] != '/') ++j;
      std::string tag = ToLowerTag(h.substr(ns, j - ns));
      xml::AttrList attrs;
      bool selfClose = false;
      for (;;) {
        while (j < n && isspace((unsigned char)h[j])) ++j;
        if (j >= n) break;
        if (h[j] == '>') { ++j; break; }
        if (h[j] == '/') { selfClose = true; ++j; continue; }
        size_t as = j;
        while (j < n && !isspace((unsigned char)h[j]) && h[j] != '=' && h[j] != '>' && h[j] != '/') ++j;
        if (j == as) { ++j; continue; }
        std::string an = ToLowerTag(h.substr(as, j - as));
        while (j < n && isspace((unsigned char)h[j])) ++j;
        std::string av;
        if (j < n && h[j] == '=') {
          ++j;
          while (j < n && isspace((unsigned char)h[j])) ++j;
          if (j < n && (h[j] == '"' || h[j] == '\'')) {
            char q = h[j++]; size_t e = h.find(q, j); if (e == std::string_view::npos) e = n;
            xml::DecodeEntitiesInto(h.substr(j, e - j), av); j = e < n ? e + 1 : n;
          } else {
            size_t vs = j;
            while (j < n && !isspace((unsigned char)h[j]) && h[j] != '>') ++j;
            xml::DecodeEntitiesInto(h.substr(vs, j - vs), av);
          }
        }
        attrs.emplace_back(std::move(an), std::move(av));
      }
      i = j;
      flush();
      // implied end tags
      if (tag == "p" || tag == "div" || tag == "ul" || tag == "ol" || tag == "table" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" ||
          tag == "pre" || tag == "blockquote" || tag == "hr" || tag == "dl" || tag == "section" || tag == "article" || tag == "header" || tag == "footer" || tag == "form") {
        if (cur->tag == "p") cur = cur->parent;
      }
      if (tag == "li") { if (hasOpen("li", {"ul", "ol"})) closeTo("li"); }
      else if (tag == "dt" || tag == "dd") { if (hasOpen("dt", {"dl"})) closeTo("dt"); else if (hasOpen("dd", {"dl"})) closeTo("dd"); }
      else if (tag == "tr") { if (hasOpen("tr", {"table"})) closeTo("tr"); }
      else if (tag == "td" || tag == "th") { if (hasOpen("td", {"tr", "table"})) closeTo("td"); else if (hasOpen("th", {"tr", "table"})) closeTo("th"); }
      else if (tag == "thead" || tag == "tbody" || tag == "tfoot") { if (hasOpen("tbody", {"table"})) closeTo("tbody"); else if (hasOpen("thead", {"table"})) closeTo("thead"); else if (hasOpen("tfoot", {"table"})) closeTo("tfoot"); }
      auto nd = std::make_unique<HNode>();
      nd->tag = tag; nd->attrs = std::move(attrs); nd->parent = cur;
      HNode* np = nd.get();
      cur->kids.push_back(std::move(nd));
      if (tag == "script" || tag == "style") {         // raw text: skip to the end tag
        size_t e = i;
        std::string needle = "</" + tag;
        for (;;) {
          e = h.find('<', e);
          if (e == std::string_view::npos) { e = n; break; }
          if (e + needle.size() <= n && EqualsNoCase(h.substr(e, needle.size()), needle)) break;
          ++e;
        }
        if (tag == "style" || tag == "script") { auto t = std::make_unique<HNode>(); t->isText = true; t->text = std::string(h.substr(i, e - i)); t->parent = np; np->kids.push_back(std::move(t)); }
        size_t gt = e < n ? h.find('>', e) : std::string_view::npos;
        i = gt == std::string_view::npos ? n : gt + 1;
        continue;
      }
      if (!selfClose && !VoidTags().count(tag)) cur = np;
    }
    flush();
  }
};

// ---- CSS-ish helpers -------------------------------------------------------------------------------------------------------
std::string ColorToHex(std::string v) {
  v = LowerAscii(TrimAscii(v));
  static const std::pair<const char*, const char*> names[] = {{"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"}, {"green", "008000"}, {"blue", "0000FF"},
    {"yellow", "FFFF00"}, {"gray", "808080"}, {"grey", "808080"}, {"orange", "FFA500"}, {"purple", "800080"}, {"navy", "000080"}, {"teal", "008080"}, {"maroon", "800000"},
    {"silver", "C0C0C0"}, {"lime", "00FF00"}, {"aqua", "00FFFF"}, {"fuchsia", "FF00FF"}, {"olive", "808000"}, {"brown", "A52A2A"}, {"pink", "FFC0CB"}};
  for (auto& nm : names) if (v == nm.first) return nm.second;
  if (!v.empty() && v[0] == '#') {
    std::string h = v.substr(1);
    for (char c : h) if (!isxdigit((unsigned char)c)) return {};
    if (h.size() == 3) return UpperAscii(std::string{h[0], h[0], h[1], h[1], h[2], h[2]});
    if (h.size() == 6) return UpperAscii(h);
    return {};
  }
  if (StartsWith(v, "rgb(")) {
    int r = 0, g = 0, b = 0;
    if (sscanf(v.c_str() + 4, "%d , %d , %d", &r, &g, &b) == 3 || sscanf(v.c_str() + 4, "%d,%d,%d", &r, &g, &b) == 3)
      return Printf("%02X%02X%02X", std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
  }
  return {};
}

std::map<std::string, std::string> ParseStyle(const std::string& s) {
  std::map<std::string, std::string> m;
  for (auto& decl : SplitStr(s, ';')) {
    size_t c = decl.find(':');
    if (c == std::string::npos) continue;
    m[LowerAscii(TrimAscii(decl.substr(0, c)))] = TrimAscii(decl.substr(c + 1));
  }
  return m;
}

bool IsMonoFont(const std::string& f) {
  std::string l = LowerAscii(f);
  return l.find("courier") != std::string::npos || l.find("consolas") != std::string::npos || l.find("monospace") != std::string::npos || l.find("mono") != std::string::npos || l.find("menlo") != std::string::npos;
}

struct Reader {
  Document doc;
  const HtmlReadOptions& opt;
  Runs cur;
  struct Pending { Block::Kind kind = Block::Kind::Paragraph; int level = 0; bool ordered = false; int number = 0; };
  Pending pend;
  struct ListCtx { bool ordered; int counter; };
  std::vector<ListCtx> lists;
  int quoteDepth = 0;
  bool inPre = false;
  bool rtlCtx = false;
  int warnedImages = 0;
  explicit Reader(const HtmlReadOptions& o) : opt(o) {}

  // ---- inline text accumulation ----
  void AddText(const std::string& t, const Run& fmt) {
    if (t.empty()) return;
    std::string s;
    if (inPre) s = t;
    else {
      bool lastSpace = cur.empty() || cur.back().br || (!cur.back().text.empty() && (cur.back().text.back() == ' '));
      for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = (unsigned char)t[i];
        bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (ws) { if (!lastSpace) { s.push_back(' '); lastSpace = true; } }
        else { s.push_back((char)c); lastSpace = false; }
      }
    }
    if (s.empty()) return;
    Run r = fmt; r.text = std::move(s); r.br = false;
    cur.push_back(std::move(r));
  }
  void AddBreak(const Run& fmt) { Run r = fmt; r.br = true; r.text.clear(); cur.push_back(std::move(r)); }

  void Flush() {
    // trim
    while (!cur.empty() && !cur.back().br && TrimAscii(cur.back().text).empty() && !inPre) cur.pop_back();
    while (!cur.empty() && cur.back().br) cur.pop_back();
    if (!cur.empty() && !cur.back().br && !inPre) { auto& t = cur.back().text; while (!t.empty() && t.back() == ' ') t.pop_back(); }
    size_t lead = 0;
    while (lead < cur.size() && !cur[lead].br && !inPre && TrimAscii(cur[lead].text).empty()) ++lead;
    if (lead) cur.erase(cur.begin(), cur.begin() + lead);
    if (!cur.empty() && !cur.front().br && !inPre) { auto& t = cur.front().text; size_t k = 0; while (k < t.size() && t[k] == ' ') ++k; t.erase(0, k); }
    if (cur.empty()) { return; }
    Block b;
    b.kind = pend.kind; b.level = pend.level; b.ordered = pend.ordered; b.number = pend.number;
    if (pend.kind == Block::Kind::Paragraph && quoteDepth > 0) { b.kind = Block::Kind::Quote; b.level = quoteDepth - 1; }
    b.rtl = rtlCtx;
    b.runs = std::move(cur);
    cur.clear();
    doc.blocks.push_back(std::move(b));
    pend = Pending{};
  }

  std::shared_ptr<ImageData> LoadImage(const std::string& src, const std::string& alt) {
    if (!opt.loadImages || src.empty()) return nullptr;
    Bytes data;
    if (StartsWith(LowerAscii(src.substr(0, 11)), "data:")) {
      size_t comma = src.find(',');
      if (comma == std::string::npos) return nullptr;
      std::string head = LowerAscii(src.substr(5, comma - 5));
      if (head.find(";base64") == std::string::npos) return nullptr;
      data = Base64Decode(std::string_view(src).substr(comma + 1));
    } else if (opt.resolve) {
      std::string path = src;
      size_t q = path.find_first_of("?#"); if (q != std::string::npos) path.resize(q);
      if (!opt.resolve(path, data)) return nullptr;
    } else if (!opt.baseDir.empty()) {
      std::string l = LowerAscii(src.substr(0, 8));
      if (l.find("://") != std::string::npos || StartsWith(l, "mailto:") || StartsWith(l, "//")) return nullptr;
      std::string path = src;
      size_t q = path.find_first_of("?#"); if (q != std::string::npos) path.resize(q);
      std::string dec; for (size_t i = 0; i < path.size(); ++i) { if (path[i] == '%' && i + 2 < path.size() + 0 && isxdigit((unsigned char)path[i + 1]) && isxdigit((unsigned char)path[i + 2])) { dec.push_back((char)strtol(path.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; } else dec.push_back(path[i]); }
      if (!IsSafeZipEntryNameLike(dec)) return nullptr;
      fs::path p = opt.baseDir / fs::path(Widen(dec));
      std::error_code ec;
      if (!fs::is_regular_file(p, ec)) return nullptr;
      if (!ReadFileBytes(p, data, opt.maxImageBytes)) return nullptr;
    } else return nullptr;
    if (data.empty() || data.size() > opt.maxImageBytes) return nullptr;
    ImageInfo info;
    if (!ProbeImage(data.data(), data.size(), info)) return nullptr;
    auto img = std::make_shared<ImageData>();
    img->bytes = std::move(data); img->mime = info.mime; img->w = info.w; img->h = info.h; img->alt = alt;
    return img;
  }
  static bool IsSafeZipEntryNameLike(const std::string& s) {      // relative path without traversal / drive / device names
    if (s.empty() || s.find(':') != std::string::npos) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    std::string n = s; std::replace(n.begin(), n.end(), '\\', '/');
    for (auto& comp : SplitStr(n, '/')) if (comp == "..") return false;
    return true;
  }

  void EmitImage(const HNode* n) {
    std::string src = n->AttrOr("src"), alt = n->AttrOr("alt");
    auto img = LoadImage(src, alt);
    if (!img) {
      if (!alt.empty()) { Run r; r.text = "[" + alt + "]"; AddText(r.text, Run{}); }
      std::string l = LowerAscii(src.substr(0, 8));
      if (l.find("://") != std::string::npos || StartsWith(l, "//")) { if (warnedImages++ == 0) doc.Warn(L"Images hosted online are not downloaded; they were left out."); }
      else if (!src.empty() && warnedImages++ == 0) doc.Warn(L"An image referenced by the document could not be loaded and was left out.");
      return;
    }
    Flush();
    Block b; b.kind = Block::Kind::Image; b.image = img;
    doc.blocks.push_back(std::move(b));
  }

  // ---- inline-only collection (table cells) ----
  void CollectInline(const HNode* n, const Run& fmt, Runs& out, bool& lastBr) {
    if (n->isText) {
      std::string s; bool lastSpace = out.empty() || lastBr || (!out.back().text.empty() && out.back().text.back() == ' ');
      for (unsigned char c : n->text) {
        bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (ws) { if (!lastSpace) { s.push_back(' '); lastSpace = true; } } else { s.push_back((char)c); lastSpace = false; }
      }
      if (!s.empty()) { Run r = fmt; r.text = s; out.push_back(r); lastBr = false; }
      return;
    }
    const std::string& t = n->tag;
    if (t == "script" || t == "style" || t == "head") return;
    if (t == "br") { Run r = fmt; r.br = true; out.push_back(r); lastBr = true; return; }
    if (t == "img") { std::string alt = n->AttrOr("alt"); if (!alt.empty()) { Run r = fmt; r.text = alt; out.push_back(r); lastBr = false; } return; }
    Run f = Format(n, fmt);
    bool blockish = IsBlockTag(t) && t != "td" && t != "th";
    if (blockish && !out.empty() && !out.back().br) { Run r = fmt; r.br = true; out.push_back(r); lastBr = true; }
    for (auto& k : n->kids) CollectInline(k.get(), f, out, lastBr);
    if (blockish && !out.empty() && !out.back().br) { Run r = fmt; r.br = true; out.push_back(r); lastBr = true; }
  }

  Run Format(const HNode* n, Run f) {
    const std::string& t = n->tag;
    if (t == "b" || t == "strong") f.bold = true;
    else if (t == "i" || t == "em" || t == "cite" || t == "dfn" || t == "var") f.italic = true;
    else if (t == "u" || t == "ins") f.underline = true;
    else if (t == "s" || t == "strike" || t == "del") f.strike = true;
    else if (t == "code" || t == "kbd" || t == "samp" || t == "tt") f.code = true;
    else if (t == "sup") f.sup = true;
    else if (t == "sub") f.sub = true;
    else if (t == "a") { std::string href = n->AttrOr("href"); if (!href.empty()) f.link = href; }
    else if (t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6") {}
    else if (t == "font") {
      std::string c = ColorToHex(n->AttrOr("color")); if (!c.empty()) f.color = c;
      std::string face = n->AttrOr("face"); if (!face.empty()) { f.font = face; if (IsMonoFont(face)) f.code = true; }
    }
    if (auto* st = n->Attr("style")) {
      auto m = ParseStyle(*st);
      if (m.count("color")) { std::string c = ColorToHex(m["color"]); if (!c.empty()) f.color = c; }
      if (m.count("font-weight")) { std::string w = LowerAscii(m["font-weight"]); if (w == "bold" || w == "bolder" || (atoi(w.c_str()) >= 600)) f.bold = true; else if (w == "normal" || w == "400") f.bold = false; }
      if (m.count("font-style")) { std::string w = LowerAscii(m["font-style"]); if (w == "italic" || w == "oblique") f.italic = true; else if (w == "normal") f.italic = false; }
      if (m.count("text-decoration") || m.count("text-decoration-line")) {
        std::string w = LowerAscii(m.count("text-decoration") ? m["text-decoration"] : m["text-decoration-line"]);
        if (w.find("underline") != std::string::npos) f.underline = true;
        if (w.find("line-through") != std::string::npos) f.strike = true;
      }
      if (m.count("font-size")) {
        std::string w = LowerAscii(m["font-size"]); double v = atof(w.c_str());
        if (v > 0) { if (w.find("pt") != std::string::npos) f.sizePt = v; else if (w.find("px") != std::string::npos) f.sizePt = v * 0.75; }
      }
      if (m.count("font-family")) { f.font = m["font-family"]; size_t c = f.font.find(','); if (c != std::string::npos) f.font.resize(c); f.font = TrimAscii(f.font); f.font.erase(std::remove(f.font.begin(), f.font.end(), '"'), f.font.end()); f.font.erase(std::remove(f.font.begin(), f.font.end(), '\''), f.font.end()); if (IsMonoFont(f.font)) f.code = true; }
      if (m.count("direction")) f.rtl = LowerAscii(m["direction"]) == "rtl";
    }
    if (auto* d = n->Attr("dir")) { if (LowerAscii(*d) == "rtl") f.rtl = true; }
    return f;
  }

  Block ParseTable(const HNode* tbl) {
    Block b; b.kind = Block::Kind::Table;
    std::vector<const HNode*> rows;
    std::function<void(const HNode*)> collect = [&](const HNode* n) {
      for (auto& k : n->kids) {
        if (k->isText) continue;
        if (k->tag == "tr") rows.push_back(k.get());
        else if (k->tag == "thead" || k->tag == "tbody" || k->tag == "tfoot") collect(k.get());
      }
    };
    collect(tbl);
    bool firstAllHeader = false;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      std::vector<Cell> cells;
      bool allTh = true, any = false;
      for (auto& c : rows[ri]->kids) {
        if (c->isText || (c->tag != "td" && c->tag != "th")) continue;
        Cell cell; any = true;
        bool lastBr = true;
        Run base;
        if (c->tag == "th") { base.bold = false; cell.header = true; } else allTh = false;
        for (auto& k : c->kids) CollectInline(k.get(), base, cell.runs, lastBr);
        while (!cell.runs.empty() && (cell.runs.back().br || TrimAscii(cell.runs.back().text).empty())) cell.runs.pop_back();
        if (!cell.runs.empty() && !cell.runs.front().br) { auto& t = cell.runs.front().text; size_t k = 0; while (k < t.size() && t[k] == ' ') ++k; t.erase(0, k); }
        if (!cell.runs.empty() && !cell.runs.back().br) { auto& t = cell.runs.back().text; while (!t.empty() && t.back() == ' ') t.pop_back(); }
        std::string cs = c->AttrOr("colspan"); cell.colspan = std::clamp(atoi(cs.c_str()), 1, 64);
        if (ri == 0) {
          std::string al = LowerAscii(c->AttrOr("align"));
          if (auto* st = c->Attr("style")) { auto m = ParseStyle(*st); if (m.count("text-align")) al = LowerAscii(m["text-align"]); }
          char a = al == "center" ? 'c' : al == "right" ? 'r' : al == "left" ? 'l' : 0;
          for (int k = 0; k < cell.colspan; ++k) b.table.align.push_back(a);
        }
        cells.push_back(std::move(cell));
      }
      if (!any) continue;
      if (b.table.rows.empty()) firstAllHeader = allTh;
      b.table.rows.push_back(std::move(cells));
    }
    b.table.headerRow = firstAllHeader;
    b.table.rtl = rtlCtx;
    return b;
  }

  void Walk(const HNode* n, Run fmt) {
    if (n->isText) { AddText(n->text, fmt); return; }
    const std::string& t = n->tag;
    if (t == "script" || t == "style" || t == "head" || t == "template" || t == "noscript" || t == "svg" || t == "math") {
      if (t == "head") for (auto& k : n->kids) if (!k->isText && k->tag == "title" && doc.meta.title.empty()) doc.meta.title = TrimAscii(k->kids.empty() ? "" : k->kids[0]->text);
      return;
    }
    if (t == "title") return;
    bool savedRtl = rtlCtx;
    if (auto* d = n->Attr("dir")) { std::string v = LowerAscii(*d); if (v == "rtl") rtlCtx = true; else if (v == "ltr") rtlCtx = false; }
    if (auto* st = n->Attr("style")) { auto m = ParseStyle(*st); if (m.count("direction")) rtlCtx = LowerAscii(m["direction"]) == "rtl"; if ((m.count("page-break-after") && LowerAscii(m["page-break-after"]) == "always") || (m.count("break-after") && LowerAscii(m["break-after"]) == "page") || (m.count("page-break-before") && LowerAscii(m["page-break-before"]) == "always")) { Flush(); Block b; b.kind = Block::Kind::PageBreak; doc.blocks.push_back(b); } }
    if (t == "html") { std::string l = n->AttrOr("lang"); if (!l.empty() && doc.meta.language.empty()) doc.meta.language = l; }

    if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
      Flush();
      Run f = Format(n, fmt);
      for (auto& k : n->kids) Walk(k.get(), f);
      pend.kind = Block::Kind::Heading; pend.level = t[1] - '0';
      Flush();
      std::string id = n->AttrOr("id"); if (!id.empty() && !doc.blocks.empty() && doc.blocks.back().kind == Block::Kind::Heading) doc.blocks.back().anchor = id;
    }
    else if (t == "br") AddBreak(fmt);
    else if (t == "hr") { Flush(); Block b; b.kind = Block::Kind::Rule; doc.blocks.push_back(b); }
    else if (t == "img") EmitImage(n);
    else if (t == "ul" || t == "ol") {
      Flush();
      lists.push_back({t == "ol", 0});
      std::string st = n->AttrOr("start"); if (t == "ol" && !st.empty()) lists.back().counter = atoi(st.c_str()) - 1;
      for (auto& k : n->kids) Walk(k.get(), fmt);
      Flush();
      lists.pop_back();
    }
    else if (t == "li") {
      Flush();
      if (lists.empty()) lists.push_back({false, 0});     // stray <li>
      auto& L = lists.back();
      ++L.counter;
      pend.kind = Block::Kind::ListItem; pend.level = (int)lists.size() - 1; pend.ordered = L.ordered; pend.number = L.ordered ? L.counter : 0;
      // task list checkbox
      Run f = Format(n, fmt);
      for (auto& k : n->kids) Walk(k.get(), f);
      Flush();
      pend = Pending{};
    }
    else if (t == "input") {
      std::string ty = LowerAscii(n->AttrOr("type"));
      if (ty == "checkbox" && !pend.level + 1) {}
    }
    else if (t == "table") { Flush(); Block b = ParseTable(n); if (!b.table.rows.empty()) doc.blocks.push_back(std::move(b)); }
    else if (t == "pre") {
      Flush();
      Block b; b.kind = Block::Kind::Code;
      std::string text; std::string lang;
      std::function<void(const HNode*)> grab = [&](const HNode* x) {
        if (x->isText) { text += x->text; return; }
        if (x->tag == "br") { text.push_back('\n'); return; }
        if (x->tag == "code") { std::string c = x->AttrOr("class"); size_t p = c.find("language-"); if (p != std::string::npos) { size_t e = c.find(' ', p); lang = c.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9); } }
        for (auto& k : x->kids) grab(k.get());
      };
      grab(n);
      if (!text.empty() && text[0] == '\n') text.erase(0, 1);
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
      text = NormalizeNewlines(text);
      b.lang = lang;
      Run r; r.text = text; r.code = true; b.runs.push_back(r);
      doc.blocks.push_back(std::move(b));
    }
    else if (t == "blockquote") { Flush(); ++quoteDepth; for (auto& k : n->kids) Walk(k.get(), fmt); Flush(); --quoteDepth; }
    else if (t == "p" || t == "dd" || t == "figcaption" || t == "summary" || t == "caption") {
      Flush();
      Run f = Format(n, fmt);
      if (t == "dd") {}
      for (auto& k : n->kids) Walk(k.get(), f);
      Flush();
    }
    else if (t == "dt") { Flush(); Run f = Format(n, fmt); f.bold = true; for (auto& k : n->kids) Walk(k.get(), f); Flush(); }
    else if (t == "div" || t == "section" || t == "article" || t == "main" || t == "header" || t == "footer" || t == "nav" || t == "aside" || t == "center" || t == "figure" ||
             t == "body" || t == "html" || t == "form" || t == "fieldset" || t == "dl" || t == "details" || t == "address" || t == "tbody" || t == "thead" || t == "tr" || t == "td" || t == "th") {
      Flush();
      Run f = Format(n, fmt);
      for (auto& k : n->kids) Walk(k.get(), f);
      Flush();
      if (t == "center" && !doc.blocks.empty() && doc.blocks.back().align == 0) doc.blocks.back().align = 'c';
    }
    else {
      Run f = Format(n, fmt);
      for (auto& k : n->kids) Walk(k.get(), f);
    }
    rtlCtx = savedRtl;
  }
};

} // namespace

std::string DecodeHtmlBytes(const uint8_t* d, size_t n) {
  if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return DecodeText(d, n).utf8;
  if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) return DecodeText(d, n).utf8;
  std::string head = LowerAscii(std::string_view((const char*)d, std::min<size_t>(n, 4096)));
  size_t cs = head.find("charset");
  if (cs != std::string::npos) {
    size_t p = head.find('=', cs);
    if (p != std::string::npos) {
      ++p; while (p < head.size() && (head[p] == ' ' || head[p] == '"' || head[p] == '\'')) ++p;
      size_t e = p; while (e < head.size() && (isalnum((unsigned char)head[e]) || head[e] == '-' || head[e] == '_')) ++e;
      TextEnc enc = ParseEncodingName(head.substr(p, e - p));
      if (enc != TextEnc::Auto) return DecodeText(d, n, enc).utf8;
    }
  }
  return DecodeText(d, n).utf8;
}

Document ParseHtml(std::string_view html, const HtmlReadOptions& opt) {
  HtmlTree tree;
  tree.Build(html);
  Reader r(opt);
  Run base;
  // direction/lang from <html>/<body> handled inside Walk
  for (auto& k : tree.root->kids) r.Walk(k.get(), base);
  r.Flush();
  Document d = std::move(r.doc);
  FinalizeDocument(d);
  return d;
}

// ============================================================================================================================
// Writer
// ============================================================================================================================
std::string DefaultCss() {
  return "body{font-family:'Segoe UI',Calibri,Arial,sans-serif;line-height:1.55;max-width:50rem;margin:2rem auto;padding:0 1rem;color:#222}"
         "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.4em 0 .5em}"
         "table{border-collapse:collapse;margin:1em 0}th,td{border:1px solid #999;padding:.35em .6em;vertical-align:top}th{background:#f0f0f0}"
         "pre{background:#f6f6f6;padding:.8em;overflow:auto;border-radius:4px}code{font-family:Consolas,'Courier New',monospace}"
         "blockquote{border-inline-start:4px solid #ccc;margin:1em 0;padding:0 1em;color:#444}"
         "img{max-width:100%;height:auto}hr{border:0;border-top:1px solid #ccc}.footnote{font-size:.9em;color:#444}";
}

static void OpenRun(std::string& o, const Run& r, bool xhtml) {
  (void)xhtml;
  if (!r.color.empty() || r.sizePt > 0 || (!r.font.empty() && !r.code)) {
    o += "<span style=\"";
    if (!r.color.empty()) o += "color:#" + r.color + ";";
    if (r.sizePt > 0) o += Printf("font-size:%.1fpt;", r.sizePt);
    if (!r.font.empty() && !r.code) o += "font-family:" + XmlEscape(r.font, true) + ";";
    o += "\">";
  }
  if (r.bold) o += "<strong>";
  if (r.italic) o += "<em>";
  if (r.underline) o += "<u>";
  if (r.strike) o += "<s>";
  if (r.sup) o += "<sup>";
  if (r.sub) o += "<sub>";
  if (r.code) o += "<code>";
}
static void CloseRun(std::string& o, const Run& r) {
  if (r.code) o += "</code>";
  if (r.sub) o += "</sub>";
  if (r.sup) o += "</sup>";
  if (r.strike) o += "</s>";
  if (r.underline) o += "</u>";
  if (r.italic) o += "</em>";
  if (r.bold) o += "</strong>";
  if (!r.color.empty() || r.sizePt > 0 || (!r.font.empty() && !r.code)) o += "</span>";
}

std::string RunsToHtml(const Runs& runs, bool xhtml) {
  std::string o;
  size_t i = 0;
  while (i < runs.size()) {
    const Run& r = runs[i];
    bool linked = !r.link.empty();
    if (linked) o += "<a href=\"" + XmlEscape(r.link, true) + "\">";
    size_t j = i;
    for (; j < runs.size() && runs[j].link == r.link; ++j) {
      const Run& x = runs[j];
      if (x.br) { o += xhtml ? "<br/>" : "<br>"; continue; }
      OpenRun(o, x, xhtml);
      o += XmlEscape(x.text);
      CloseRun(o, x);
    }
    if (linked) o += "</a>";
    i = j;
  }
  return o;
}

namespace {
struct HtmlOut {
  const std::vector<Block>& blocks;
  const HtmlWriteOptions& opt;
  std::string o;
  struct L { bool ordered; bool liOpen; };
  std::vector<L> stack;
  int hid;
  HtmlOut(const std::vector<Block>& b, const HtmlWriteOptions& op) : blocks(b), opt(op), hid(op.firstHeadingId) {}

  void CloseLists(size_t keep) {
    while (stack.size() > keep) {
      if (stack.back().liOpen) o += "</li>";
      o += stack.back().ordered ? "</ol>" : "</ul>";
      stack.pop_back();
    }
  }
  std::string Dir(bool rtl) const { return rtl ? " dir=\"rtl\"" : ""; }
  std::string Align(char a) const { return a == 'c' ? " style=\"text-align:center\"" : a == 'r' ? " style=\"text-align:right\"" : a == 'l' ? " style=\"text-align:left\"" : a == 'j' ? " style=\"text-align:justify\"" : ""; }

  void Image(const Block& b, int idx) {
    if (!b.image) return;
    std::string src;
    if (opt.embedImages || !opt.imageHref) src = "data:" + (b.image->mime.empty() ? std::string("image/png") : b.image->mime) + ";base64," + Base64Encode(b.image->bytes.data(), b.image->bytes.size());
    else src = opt.imageHref(b, idx);
    o += "<p class=\"img\"><img src=\"" + XmlEscape(src, true) + "\" alt=\"" + XmlEscape(b.image->alt, true) + "\"";
    if (b.image->w > 0 && b.image->h > 0) o += Printf(" width=\"%d\" height=\"%d\"", b.image->w, b.image->h);
    o += opt.xhtml ? "/></p>\n" : "></p>\n";
  }

  void Render(size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      const Block& b = blocks[i];
      if (b.kind != Block::Kind::ListItem) CloseLists(0);
      switch (b.kind) {
        case Block::Kind::Heading: {
          int lv = std::clamp(b.level, 1, 6);
          std::string id = b.anchor;
          if (opt.headingAnchors) id = Printf("h-%d", hid++);
          o += Printf("<h%d", lv);
          if (!id.empty()) o += " id=\"" + XmlEscape(id, true) + "\"";
          o += Dir(b.rtl) + Align(b.align) + ">" + RunsToHtml(b.runs, opt.xhtml) + Printf("</h%d>\n", lv);
          break;
        }
        case Block::Kind::Paragraph:
          o += "<p" + Dir(b.rtl) + Align(b.align) + ">" + RunsToHtml(b.runs, opt.xhtml) + "</p>\n";
          break;
        case Block::Kind::Quote: {
          int lv = b.level + 1;
          for (int k = 0; k < lv; ++k) o += "<blockquote" + std::string(k == lv - 1 ? Dir(b.rtl) : "") + ">";
          o += "<p>" + RunsToHtml(b.runs, opt.xhtml) + "</p>";
          for (int k = 0; k < lv; ++k) o += "</blockquote>";
          o += "\n";
          break;
        }
        case Block::Kind::ListItem: {
          size_t want = (size_t)b.level + 1;
          if (stack.size() > want) CloseLists(want);
          if (stack.size() == want && stack.back().ordered != b.ordered) CloseLists(want - 1);
          while (stack.size() < want) {
            bool ord = stack.size() + 1 == want ? b.ordered : false;
            if (!stack.empty() && !stack.back().liOpen) { o += "<li>"; stack.back().liOpen = true; }
            o += ord ? "<ol" : "<ul";
            if (ord && stack.size() + 1 == want && b.number > 1) o += Printf(" start=\"%d\"", b.number);
            if (b.rtl && stack.size() + 1 == want) o += " dir=\"rtl\"";
            o += ">";
            stack.push_back({ord, false});
          }
          if (stack.back().liOpen) { o += "</li>\n"; stack.back().liOpen = false; }
          o += "<li>";
          stack.back().liOpen = true;
          if (b.checked >= 0) o += b.checked ? "\xE2\x98\x91 " : "\xE2\x98\x90 ";
          o += RunsToHtml(b.runs, opt.xhtml);
          break;
        }
        case Block::Kind::Table: {
          const Table& t = b.table;
          o += "<table" + Dir(t.rtl) + ">\n";
          for (size_t r = 0; r < t.rows.size(); ++r) {
            o += "<tr>";
            size_t col = 0;
            for (auto& c : t.rows[r]) {
              bool th = (r == 0 && t.headerRow) || c.header;
              std::string al; if (col < t.align.size() && t.align[col]) al = Align(t.align[col]);
              o += th ? "<th" : "<td";
              if (c.colspan > 1) o += Printf(" colspan=\"%d\"", c.colspan);
              if (c.rtl && !t.rtl) o += " dir=\"rtl\"";
              o += al + ">" + RunsToHtml(c.runs, opt.xhtml) + (th ? "</th>" : "</td>");
              col += (size_t)std::max(1, c.colspan);
            }
            o += "</tr>\n";
          }
          o += "</table>\n";
          break;
        }
        case Block::Kind::Image: Image(b, (int)i); break;
        case Block::Kind::PageBreak: o += opt.xhtml ? "<div style=\"page-break-after:always\"></div>\n" : "<div style=\"page-break-after:always\"></div>\n"; break;
        case Block::Kind::Rule: o += opt.xhtml ? "<hr/>\n" : "<hr>\n"; break;
        case Block::Kind::Code: {
          o += "<pre" + Dir(false) + "><code";
          if (!b.lang.empty()) o += " class=\"language-" + XmlEscape(b.lang, true) + "\"";
          o += ">" + XmlEscape(PlainText(b.runs)) + "</code></pre>\n";
          break;
        }
        case Block::Kind::Footnote:
          o += "<p class=\"footnote\"" + Dir(b.rtl) + " id=\"fn-" + XmlEscape(b.label, true) + "\"><sup>" + XmlEscape(b.label) + "</sup> " + RunsToHtml(b.runs, opt.xhtml) + "</p>\n";
          break;
      }
    }
    CloseLists(0);
  }
};
} // namespace

std::string RenderBlocksHtml(const std::vector<Block>& blocks, size_t from, size_t to, const HtmlWriteOptions& opt) {
  HtmlOut h(blocks, opt);
  h.Render(from, std::min(to, blocks.size()));
  return std::move(h.o);
}

std::string WriteHtml(const Document& d, const HtmlWriteOptions& opt) {
  std::string body = RenderBlocksHtml(d.blocks, 0, d.blocks.size(), opt);
  if (!opt.fullDocument) return body;
  std::string title = opt.title.empty() ? d.meta.title : opt.title;
  std::string lang = !opt.lang.empty() ? opt.lang : (!d.meta.language.empty() ? d.meta.language : (d.rtl ? "ar" : "en"));
  std::string o;
  if (opt.xhtml) o += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  o += "<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"";
  if (opt.epubTypes) o += " xmlns:epub=\"http://www.idpf.org/2007/ops\"";
  o += " lang=\"" + XmlEscape(lang, true) + "\"";
  if (opt.xhtml) o += " xml:lang=\"" + XmlEscape(lang, true) + "\"";
  if (d.rtl) o += " dir=\"rtl\"";
  o += ">\n<head>\n<meta charset=\"utf-8\"" + std::string(opt.xhtml ? "/>" : ">") + "\n";
  o += "<title>" + XmlEscape(title.empty() ? std::string("Document") : title) + "</title>\n";
  std::string css = opt.css.empty() ? DefaultCss() : opt.css;
  o += "<style>" + css + "</style>\n</head>\n<body>\n" + body + "</body>\n</html>\n";
  return o;
}

} // namespace filo::doc
