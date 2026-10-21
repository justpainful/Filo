#include "engines/doc/markdown.h"
#include "engines/doc/html.h"
#include "engines/doc/text_io.h"
#include "engines/doc/xml.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace filo::doc {

namespace {

// ============================================================================================================================
// Inline parsing
// ============================================================================================================================
struct Piece { Run run; bool image = false; std::string url; };

struct MdContext {
  std::map<std::string, std::string> refs;      // lower-case label -> url
  std::set<std::string> footnotes;
  const MdReadOptions* opt = nullptr;
  Document* doc = nullptr;
};

bool IsWsCp(uint32_t c) { return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029; }
bool IsPunctCp(uint32_t c) {
  if (c < 128) return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
  return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF || (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
         (c >= 0x3001 && c <= 0x303F) || c == 0x60C || c == 0x61B || c == 0x61F || (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20);
}
bool IsAsciiPunct(char c) { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }

uint32_t PrevCp(std::string_view s, size_t i) {
  if (i == 0) return 0;
  size_t j = i - 1;
  while (j > 0 && ((uint8_t)s[j] & 0xC0) == 0x80) --j;
  size_t k = j;
  return DecodeUtf8(s, k);
}
uint32_t NextCp(std::string_view s, size_t i) { if (i >= s.size()) return 0; size_t k = i; return DecodeUtf8(s, k); }

struct Tok {
  enum Type { Text, Delim, Fixed } type = Text;
  Piece p;
  char ch = 0;
  int count = 0, origCount = 0;
  bool canOpen = false, canClose = false;
};

class InlineParser {
 public:
  explicit InlineParser(MdContext& c) : ctx_(c) {}

  std::vector<Piece> Parse(std::string_view s, Run base) {
    std::vector<Tok> toks;
    Run style = base;
    std::string buf;
    auto flush = [&] {
      if (buf.empty()) return;
      Tok t; t.type = Tok::Text; t.p.run = style; t.p.run.text = buf; toks.push_back(std::move(t)); buf.clear();
    };
    auto addFixed = [&](Piece p) { flush(); Tok t; t.type = Tok::Fixed; t.p = std::move(p); toks.push_back(std::move(t)); };

    size_t i = 0, n = s.size();
    while (i < n) {
      char c = s[i];
      if (c == '\\') {
        if (i + 1 < n && s[i + 1] == '\n') { Piece p; p.run = style; p.run.br = true; addFixed(std::move(p)); i += 2; while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i; continue; }
        if (i + 1 < n && IsAsciiPunct(s[i + 1])) { buf.push_back(s[i + 1]); i += 2; continue; }
        buf.push_back('\\'); ++i; continue;
      }
      if (c == '\n') {
        size_t trail = 0;
        while (!buf.empty() && buf.back() == ' ') { buf.pop_back(); ++trail; }
        if (buf.empty() && trail && !toks.empty() && toks.back().type == Tok::Text) { auto& t = toks.back().p.run.text; while (!t.empty() && t.back() == ' ') { t.pop_back(); ++trail; } }
        if (trail >= 2) { Piece p; p.run = style; p.run.br = true; addFixed(std::move(p)); }
        else buf.push_back(' ');
        ++i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        continue;
      }
      if (c == '`') {
        size_t j = i; while (j < n && s[j] == '`') ++j;
        size_t len = j - i;
        size_t k = j; bool found = false; size_t closeAt = 0;
        while (k < n) {
          if (s[k] == '`') { size_t m = k; while (m < n && s[m] == '`') ++m; if (m - k == len) { found = true; closeAt = k; break; } k = m; }
          else ++k;
        }
        if (found) {
          std::string code(s.substr(j, closeAt - j));
          for (auto& ch : code) if (ch == '\n') ch = ' ';
          if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos) code = code.substr(1, code.size() - 2);
          Piece p; p.run = style; p.run.code = true; p.run.text = code; addFixed(std::move(p));
          i = closeAt + len; continue;
        }
        buf.append(s.substr(i, len)); i = j; continue;
      }
      if (c == '<') {
        size_t e = s.find('>', i);
        if (e != std::string_view::npos && e - i < 2000) {
          std::string inner(s.substr(i + 1, e - i - 1));
          std::string low = LowerAscii(inner);
          // autolink
          if ((StartsWith(low, "http://") || StartsWith(low, "https://") || StartsWith(low, "mailto:") || StartsWith(low, "ftp://")) && inner.find(' ') == std::string::npos) {
            Piece p; p.run = style; p.run.link = inner; p.run.text = inner; addFixed(std::move(p)); i = e + 1; continue;
          }
          size_t at = inner.find('@');
          if (at != std::string::npos && at > 0 && inner.find(' ') == std::string::npos && inner.find('/') == std::string::npos && inner.find('.', at) != std::string::npos) {
            Piece p; p.run = style; p.run.link = "mailto:" + inner; p.run.text = inner; addFixed(std::move(p)); i = e + 1; continue;
          }
          // inline html tags
          std::string tag = low; bool closing = false;
          if (!tag.empty() && tag[0] == '/') { closing = true; tag = tag.substr(1); }
          size_t sp = tag.find_first_of(" \t/"); if (sp != std::string::npos) tag = tag.substr(0, sp);
          if (!tag.empty() && isalpha((unsigned char)tag[0])) {
            bool known = true;
            if (tag == "br") { Piece p; p.run = style; p.run.br = true; addFixed(std::move(p)); }
            else if (tag == "u" || tag == "ins") { flush(); style.underline = !closing; }
            else if (tag == "b" || tag == "strong") { flush(); style.bold = !closing; }
            else if (tag == "i" || tag == "em") { flush(); style.italic = !closing; }
            else if (tag == "s" || tag == "del" || tag == "strike") { flush(); style.strike = !closing; }
            else if (tag == "sup") { flush(); style.sup = !closing; }
            else if (tag == "sub") { flush(); style.sub = !closing; }
            else if (tag == "code" || tag == "kbd") { flush(); style.code = !closing; }
            else if (tag == "img" && !closing) {
              std::string src, alt;
              auto grab = [&](const char* name) { size_t p = low.find(std::string(name) + "="); if (p == std::string::npos) return std::string(); size_t q = p + strlen(name) + 1; if (q >= inner.size()) return std::string(); char qc = inner[q]; if (qc == '"' || qc == '\'') { size_t en = inner.find(qc, q + 1); return inner.substr(q + 1, en == std::string::npos ? std::string::npos : en - q - 1); } size_t en = inner.find_first_of(" />", q); return inner.substr(q, en == std::string::npos ? std::string::npos : en - q); };
              src = grab("src"); alt = grab("alt");
              Piece p; p.image = true; p.url = src; p.run = style; p.run.text = alt; addFixed(std::move(p));
            }
            else if (tag == "span" || tag == "font" || tag == "a" || tag == "abbr" || tag == "mark" || tag == "small" || tag == "big" || tag == "div" || tag == "p" || tag == "center" || tag == "bdi" || tag == "bdo" || tag == "q" || tag == "cite" || tag == "details" || tag == "summary") {}
            else known = false;
            if (known) { i = e + 1; continue; }
          }
          if (StartsWith(inner, "!--")) { size_t ce = s.find("-->", i); i = ce == std::string_view::npos ? n : ce + 3; continue; }
        }
        buf.push_back('<'); ++i; continue;
      }
      if (c == '&') {
        size_t e = s.find(';', i);
        if (e != std::string_view::npos && e - i <= 12) {
          std::string dec = xml::DecodeEntities(s.substr(i, e - i + 1));
          if (dec != s.substr(i, e - i + 1)) { buf += dec; i = e + 1; continue; }
        }
        buf.push_back('&'); ++i; continue;
      }
      if (c == '!' && i + 1 < n && s[i + 1] == '[') {
        size_t consumed = 0; Piece p;
        if (TryLink(s, i + 1, true, style, p, consumed)) { addFixed(std::move(p)); i += 1 + consumed; continue; }
        buf.push_back('!'); ++i; continue;
      }
      if (c == '[') {
        // footnote reference
        if (i + 1 < n && s[i + 1] == '^') {
          size_t e = s.find(']', i);
          if (e != std::string_view::npos) {
            std::string label(s.substr(i + 2, e - i - 2));
            if (ctx_.footnotes.count(label)) { Piece p; p.run = style; p.run.sup = true; p.run.text = label; addFixed(std::move(p)); i = e + 1; continue; }
          }
        }
        size_t consumed = 0; std::vector<Piece> inner; std::string url;
        if (TryLinkInline(s, i, style, inner, consumed)) { for (auto& p : inner) { Tok t; flush(); t.type = Tok::Fixed; t.p = std::move(p); toks.push_back(std::move(t)); } i += consumed; continue; }
        buf.push_back('['); ++i; continue;
      }
      if (c == '*' || c == '_' || c == '~') {
        size_t j = i; while (j < n && s[j] == c) ++j;
        int cnt = (int)(j - i);
        uint32_t before = PrevCp(s, i), after = NextCp(s, j);
        bool wsA = IsWsCp(after), wsB = IsWsCp(before), pA = IsPunctCp(after), pB = IsPunctCp(before);
        bool leftFl = !wsA && (!pA || wsB || pB);
        bool rightFl = !wsB && (!pB || wsA || pA);
        Tok t; t.type = Tok::Delim; t.ch = c; t.count = t.origCount = cnt;
        if (c == '_') { t.canOpen = leftFl && (!rightFl || pB); t.canClose = rightFl && (!leftFl || pA); }
        else if (c == '~') { t.canOpen = leftFl && cnt <= 2; t.canClose = rightFl && cnt <= 2; }
        else { t.canOpen = leftFl; t.canClose = rightFl; }
        flush();
        t.p.run = style;
        toks.push_back(std::move(t));
        i = j; continue;
      }
      // bare URL autolink
      if ((c == 'h' || c == 'w') && base.link.empty() && style.link.empty() && (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
        std::string_view rest = s.substr(i);
        bool isUrl = StartsWith(rest, "http://") || StartsWith(rest, "https://") || (StartsWith(rest, "www.") && rest.size() > 6);
        if (isUrl) {
          size_t j = i; while (j < n && !isspace((unsigned char)s[j]) && s[j] != '<') ++j;
          while (j > i && strchr(".,:;!?\"'*_~", s[j - 1])) --j;
          int depth = 0; for (size_t k = i; k < j; ++k) { if (s[k] == '(') ++depth; else if (s[k] == ')') --depth; }
          while (depth < 0 && j > i && s[j - 1] == ')') { --j; ++depth; }
          while (j > i && (s[j - 1] == ']' )) --j;
          if (j - i > 8) {
            std::string url(s.substr(i, j - i));
            Piece p; p.run = style; p.run.text = url; p.run.link = StartsWith(url, "www.") ? "http://" + url : url;
            addFixed(std::move(p)); i = j; continue;
          }
        }
      }
      buf.push_back(c); ++i;
    }
    flush();
    ProcessEmphasis(toks);
    std::vector<Piece> out;
    for (auto& t : toks) {
      if (t.type == Tok::Delim) { if (t.count > 0) { Piece p; p.run = t.p.run; p.run.text = std::string(t.count, t.ch); out.push_back(std::move(p)); } }
      else out.push_back(std::move(t.p));
    }
    return out;
  }

 private:
  MdContext& ctx_;

  static size_t FindClosingBracket(std::string_view s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
      char c = s[i];
      if (c == '\\') { ++i; continue; }
      if (c == '`') { size_t j = i; while (j < s.size() && s[j] == '`') ++j; size_t len = j - i; size_t k = j; bool found = false; while (k < s.size()) { if (s[k] == '`') { size_t m = k; while (m < s.size() && s[m] == '`') ++m; if (m - k == len) { found = true; i = m - 1; break; } k = m; } else ++k; } if (!found) i = j - 1; continue; }
      if (c == '[') ++depth;
      else if (c == ']') { --depth; if (depth == 0) return i; }
    }
    return std::string_view::npos;
  }

  static std::string Unescape(std::string_view s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size() && IsAsciiPunct(s[i + 1])) { o.push_back(s[++i]); continue; }
      o.push_back(s[i]);
    }
    return xml::DecodeEntities(o);
  }

  // Parses "(dest "title")" starting at s[p] == '('. On success returns the position after ')' and fills url.
  static bool ParseDest(std::string_view s, size_t p, std::string& url, size_t& endPos) {
    size_t i = p + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    std::string dest;
    if (i < s.size() && s[i] == '<') {
      size_t e = s.find('>', i);
      if (e == std::string_view::npos) return false;
      dest = std::string(s.substr(i + 1, e - i - 1)); i = e + 1;
    } else {
      int depth = 0; size_t st = i;
      for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
        if (c == '(') ++depth;
        else if (c == ')') { if (depth == 0) break; --depth; }
        else if (c == ' ' || c == '\t' || c == '\n') break;
      }
      dest = std::string(s.substr(st, i - st));
    }
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    if (i < s.size() && (s[i] == '"' || s[i] == '\'' || s[i] == '(')) {
      char q = s[i] == '(' ? ')' : s[i];
      size_t e = i + 1;
      while (e < s.size() && s[e] != q) { if (s[e] == '\\') ++e; ++e; }
      if (e >= s.size()) return false;
      i = e + 1;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    }
    if (i >= s.size() || s[i] != ')') return false;
    url = Unescape(dest);
    endPos = i + 1;
    return true;
  }

  bool ResolveRef(std::string_view s, size_t close, std::string_view text, std::string& url, size_t& endPos) {
    size_t after = close + 1;
    if (after < s.size() && s[after] == '[') {
      size_t e = s.find(']', after);
      if (e != std::string_view::npos) {
        std::string label = LowerAscii(s.substr(after + 1, e - after - 1));
        if (label.empty()) label = LowerAscii(text);
        auto it = ctx_.refs.find(label);
        if (it != ctx_.refs.end()) { url = it->second; endPos = e + 1; return true; }
      }
      return false;
    }
    auto it = ctx_.refs.find(LowerAscii(text));
    if (it != ctx_.refs.end()) { url = it->second; endPos = close + 1; return true; }
    return false;
  }

  // Link [text](url) / [text][ref]; produces already-processed pieces carrying the link.
  bool TryLinkInline(std::string_view s, size_t i, const Run& style, std::vector<Piece>& out, size_t& consumed) {
    size_t close = FindClosingBracket(s, i);
    if (close == std::string_view::npos) return false;
    std::string_view text = s.substr(i + 1, close - i - 1);
    std::string url; size_t endPos = 0;
    if (close + 1 < s.size() && s[close + 1] == '(') { if (!ParseDest(s, close + 1, url, endPos)) return false; }
    else if (!ResolveRef(s, close, text, url, endPos)) return false;
    Run base = style; if (!url.empty()) base.link = url;
    InlineParser inner(ctx_);
    out = inner.Parse(text, base);
    if (out.empty()) { Piece p; p.run = base; p.run.text = url; out.push_back(p); }
    consumed = endPos - i;
    return true;
  }
  bool TryLink(std::string_view s, size_t i, bool image, const Run& style, Piece& out, size_t& consumed) {
    size_t close = FindClosingBracket(s, i);
    if (close == std::string_view::npos) return false;
    std::string_view text = s.substr(i + 1, close - i - 1);
    std::string url; size_t endPos = 0;
    if (close + 1 < s.size() && s[close + 1] == '(') { if (!ParseDest(s, close + 1, url, endPos)) return false; }
    else if (!ResolveRef(s, close, text, url, endPos)) return false;
    InlineParser inner(ctx_);
    Run b; auto pieces = inner.Parse(text, b);
    std::string alt; for (auto& p : pieces) alt += p.run.text;
    out.image = image; out.url = url; out.run = style; out.run.text = alt;
    consumed = endPos - i;
    return true;
  }

  static void ProcessEmphasis(std::vector<Tok>& toks) {
    auto applyStyle = [&](size_t from, size_t to, int use, char ch) {
      for (size_t k = from; k < to; ++k) {
        Tok& t = toks[k];
        if (t.type == Tok::Delim) { t.type = Tok::Text; t.p.run.text = std::string(t.count, t.ch); t.count = 0; }
        if (t.type == Tok::Text || t.type == Tok::Fixed) {
          if (ch == '~') t.p.run.strike = true; else if (use == 2) t.p.run.bold = true; else t.p.run.italic = true;
        }
      }
    };
    for (size_t c = 0; c < toks.size();) {
      Tok& cl = toks[c];
      if (cl.type != Tok::Delim || !cl.canClose || cl.count == 0) { ++c; continue; }
      bool matched = false;
      for (size_t o = c; o-- > 0;) {
        Tok& op = toks[o];
        if (op.type != Tok::Delim || op.ch != cl.ch || !op.canOpen || op.count == 0) continue;
        if ((op.canClose || cl.canOpen) && (op.origCount + cl.origCount) % 3 == 0 && !(op.origCount % 3 == 0 && cl.origCount % 3 == 0) && cl.ch != '~') continue;
        int use = (cl.count >= 2 && op.count >= 2) ? 2 : 1;
        if (cl.ch == '~' && op.count != cl.count && use == 2) use = 1;
        applyStyle(o + 1, c, use, cl.ch);
        op.count -= use; cl.count -= use;
        matched = true;
        break;
      }
      if (!matched) ++c;
      else if (toks[c].count == 0) ++c;
    }
  }
};

// ============================================================================================================================
// Block parsing
// ============================================================================================================================
struct ListMarker { size_t indent = 0; bool ordered = false; int number = 0; char delim = 0; size_t contentIndent = 0; bool empty = false; size_t contentStart = 0; };

size_t LeadingSpaces(const std::string& l) { size_t i = 0; while (i < l.size() && l[i] == ' ') ++i; return i; }
bool IsBlank(const std::string& l) { return l.find_first_not_of(" \t") == std::string::npos; }

std::string ExpandLeadingTabs(const std::string& l) {
  size_t i = 0; while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
  if (l.find('\t') == std::string::npos || l.find('\t') >= i) return l;
  std::string o; size_t col = 0;
  for (size_t k = 0; k < i; ++k) { if (l[k] == '\t') { size_t add = 4 - (col % 4); o.append(add, ' '); col += add; } else { o.push_back(' '); ++col; } }
  o.append(l, i, std::string::npos);
  return o;
}

bool ParseListMarker(const std::string& l, ListMarker& m, size_t maxIndent = 3) {
  size_t ind = LeadingSpaces(l);
  if (ind > maxIndent || ind >= l.size()) return false;
  size_t i = ind;
  char c = l[i];
  if (c == '-' || c == '+' || c == '*') {
    if (i + 1 < l.size() && l[i + 1] != ' ') return false;
    m.ordered = false; m.delim = c; m.number = 0; i += 1;
  } else if (c >= '0' && c <= '9') {
    size_t j = i; while (j < l.size() && isdigit((unsigned char)l[j]) && j - i < 9) ++j;
    if (j >= l.size() || (l[j] != '.' && l[j] != ')')) return false;
    if (j + 1 < l.size() && l[j + 1] != ' ') return false;
    m.ordered = true; m.number = atoi(l.substr(i, j - i).c_str()); m.delim = l[j]; i = j + 1;
  } else return false;
  m.indent = ind;
  size_t sp = 0; size_t k = i; while (k < l.size() && l[k] == ' ') { ++k; ++sp; }
  if (k >= l.size()) { m.empty = true; m.contentIndent = i + 1; m.contentStart = l.size(); return true; }
  if (sp > 4) { sp = 1; k = i + 1; }
  m.contentIndent = i + sp; m.contentStart = k;
  return true;
}

bool IsHrLine(const std::string& l) {
  size_t ind = LeadingSpaces(l);
  if (ind > 3 || ind >= l.size()) return false;
  char c = l[ind];
  if (c != '-' && c != '*' && c != '_') return false;
  int cnt = 0;
  for (size_t i = ind; i < l.size(); ++i) { if (l[i] == c) ++cnt; else if (l[i] != ' ' && l[i] != '\t') return false; }
  return cnt >= 3;
}

bool ParseAtx(const std::string& l, int& level, std::string& text) {
  size_t ind = LeadingSpaces(l);
  if (ind > 3) return false;
  size_t i = ind; while (i < l.size() && l[i] == '#') ++i;
  size_t cnt = i - ind;
  if (cnt < 1 || cnt > 6) return false;
  if (i < l.size() && l[i] != ' ' && l[i] != '\t') return false;
  std::string t = TrimAscii(l.substr(i));
  // closing sequence
  size_t e = t.size();
  while (e > 0 && t[e - 1] == '#') --e;
  if (e < t.size() && (e == 0 || t[e - 1] == ' ')) t = TrimAscii(t.substr(0, e));
  level = (int)cnt; text = t;
  return true;
}

bool ParseFence(const std::string& l, char& ch, size_t& len, std::string& info, size_t& indent) {
  indent = LeadingSpaces(l);
  if (indent > 3 || indent >= l.size()) return false;
  char c = l[indent];
  if (c != '`' && c != '~') return false;
  size_t i = indent; while (i < l.size() && l[i] == c) ++i;
  if (i - indent < 3) return false;
  std::string rest = TrimAscii(l.substr(i));
  if (c == '`' && rest.find('`') != std::string::npos) return false;
  ch = c; len = i - indent; info = rest;
  return true;
}

bool SplitTableRow(const std::string& l, std::vector<std::string>& cells) {
  std::string t = TrimAscii(l);
  if (t.empty() || t.find('|') == std::string::npos) return false;
  if (t.front() == '|') t.erase(0, 1);
  if (!t.empty() && t.back() == '|') { size_t bs = 0; for (size_t i = t.size() - 1; i > 0 && t[i - 1] == '\\'; --i) ++bs; if (bs % 2 == 0) t.pop_back(); }
  cells.clear();
  std::string cur;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += "\\|"; ++i; continue; }
    if (t[i] == '|') { cells.push_back(TrimAscii(cur)); cur.clear(); continue; }
    cur.push_back(t[i]);
  }
  cells.push_back(TrimAscii(cur));
  return true;
}
bool IsDelimRow(const std::string& l, std::vector<char>& align) {
  std::vector<std::string> cells;
  if (!SplitTableRow(l, cells)) {
    // single column table "---|" is not supported
    return false;
  }
  align.clear();
  for (auto& c : cells) {
    if (c.empty()) return false;
    size_t i = 0; bool l1 = false, r1 = false;
    if (c[i] == ':') { l1 = true; ++i; }
    size_t j = c.size(); if (j > i && c[j - 1] == ':') { r1 = true; --j; }
    if (j <= i) return false;
    for (size_t k = i; k < j; ++k) if (c[k] != '-') return false;
    if (j - i < 1) return false;
    align.push_back(l1 && r1 ? 'c' : r1 ? 'r' : l1 ? 'l' : 0);
  }
  return true;
}

const std::set<std::string>& HtmlBlockTags() {
  static const std::set<std::string> s = {"address", "article", "aside", "blockquote", "body", "center", "dd", "details", "dialog", "div", "dl", "dt", "fieldset", "figcaption", "figure",
    "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "html", "iframe", "li", "main", "nav", "ol", "p", "pre", "section", "summary", "table", "tbody", "td", "th", "thead", "tr", "ul"};
  return s;
}
bool IsHtmlBlockStart(const std::string& l) {
  size_t ind = LeadingSpaces(l);
  if (ind > 3 || ind + 2 > l.size() || l[ind] != '<') return false;
  if (l.compare(ind, 4, "<!--") == 0) return true;
  size_t i = ind + 1; if (i < l.size() && l[i] == '/') ++i;
  size_t st = i; while (i < l.size() && isalnum((unsigned char)l[i])) ++i;
  if (i == st) return false;
  std::string tag = LowerAscii(l.substr(st, i - st));
  if (!HtmlBlockTags().count(tag)) return false;
  return i >= l.size() || l[i] == ' ' || l[i] == '>' || l[i] == '/' || l[i] == '\t';
}

class MdBlockParser {
 public:
  MdBlockParser(MdContext& c, Document& d) : ctx_(c), doc_(d), inl_(c) {}

  void Parse(const std::vector<std::string>& lines, std::vector<Block>& out, int quoteDepth, int listLevel) {
    size_t i = 0, n = lines.size();
    while (i < n) {
      const std::string& line = lines[i];
      if (IsBlank(line)) { ++i; continue; }
      size_t ind = LeadingSpaces(line);
      // fenced code
      char fch; size_t flen, find; std::string info;
      if (ParseFence(line, fch, flen, info, find)) {
        Block b; b.kind = Block::Kind::Code; b.lang = info.substr(0, info.find(' '));
        std::string code; size_t j = i + 1; bool closed = false;
        for (; j < n; ++j) {
          const std::string& l = lines[j];
          size_t ci = LeadingSpaces(l);
          if (ci <= 3 && ci < l.size() && l[ci] == fch) { size_t k = ci; while (k < l.size() && l[k] == fch) ++k; if (k - ci >= flen && IsBlank(l.substr(k))) { closed = true; break; } }
          std::string content = l;
          size_t strip = std::min(find, LeadingSpaces(content)); content.erase(0, strip);
          code += content; code.push_back('\n');
        }
        (void)closed;
        if (!code.empty()) code.pop_back();
        Run r; r.text = code; r.code = true; b.runs.push_back(r);
        out.push_back(std::move(b));
        i = closed ? j + 1 : j;
        continue;
      }
      // ATX heading
      int lvl; std::string htext;
      if (ParseAtx(line, lvl, htext)) {
        Block b; b.kind = Block::Kind::Heading; b.level = lvl; b.runs = InlineRuns(htext);
        out.push_back(std::move(b)); ++i; continue;
      }
      if (IsHrLine(line)) { Block b; b.kind = Block::Kind::Rule; out.push_back(std::move(b)); ++i; continue; }
      // block quote
      if (ind <= 3 && ind < line.size() && line[ind] == '>') {
        std::vector<std::string> q;
        while (i < n) {
          const std::string& l = lines[i];
          size_t li = LeadingSpaces(l);
          if (li <= 3 && li < l.size() && l[li] == '>') { size_t p = li + 1; if (p < l.size() && l[p] == ' ') ++p; q.push_back(l.substr(p)); ++i; }
          else if (!IsBlank(l) && !q.empty() && !IsBlank(q.back()) && !StartsBlock(l)) { q.push_back(l); ++i; }   // lazy continuation
          else break;
        }
        Parse(q, out, quoteDepth + 1, listLevel);
        continue;
      }
      // link reference definition / footnote definition
      {
        size_t p = ind;
        if (ind <= 3 && p < line.size() && line[p] == '[') {
          size_t e = line.find("]:", p);
          if (e != std::string::npos) {
            std::string label = line.substr(p + 1, e - p - 1);
            if (!label.empty() && label[0] == '^') {
              Block b; b.kind = Block::Kind::Footnote; b.label = label.substr(1);
              std::string text = TrimAscii(line.substr(e + 2));
              size_t j = i + 1;
              while (j < n && (LeadingSpaces(lines[j]) >= 4 || (!IsBlank(lines[j]) && !StartsBlock(lines[j]) && !text.empty() && false))) { text += " " + TrimAscii(lines[j]); ++j; }
              b.runs = InlineRuns(text);
              out.push_back(std::move(b)); i = j; continue;
            }
            if (!label.empty() && label.find(']') == std::string::npos) {
              std::string rest = TrimAscii(line.substr(e + 2));
              if (!rest.empty()) {
                std::string url = rest.substr(0, rest.find_first_of(" \t"));
                if (url.size() > 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
                (void)url;
                ++i; continue;                 // collected in the pre-pass
              }
            }
          }
        }
      }
      // list
      ListMarker m;
      if (ParseListMarker(line, m)) { i = ParseList(lines, i, out, quoteDepth, listLevel); continue; }
      // html block
      if (IsHtmlBlockStart(line)) {
        std::string html; size_t j = i;
        if (line.find("<!--") != std::string::npos) {
          for (; j < n; ++j) { html += lines[j] + "\n"; if (lines[j].find("-->") != std::string::npos) { ++j; break; } }
          i = j; continue;      // comments are dropped
        }
        for (; j < n && !IsBlank(lines[j]); ++j) html += lines[j] + "\n";
        HtmlReadOptions ho; ho.baseDir = ctx_.opt ? ctx_.opt->baseDir : fs::path(); ho.loadImages = ctx_.opt ? ctx_.opt->loadImages : true;
        Document sub = ParseHtml(html, ho);
        for (auto& w : sub.warnings) doc_.Warn(w);
        for (auto& b : sub.blocks) out.push_back(std::move(b));
        i = j; continue;
      }
      // table
      std::vector<char> align;
      if (i + 1 < n && line.find('|') != std::string::npos && IsDelimRow(lines[i + 1], align)) {
        std::vector<std::string> head;
        if (SplitTableRow(line, head) && head.size() == align.size()) {
          Block b; b.kind = Block::Kind::Table; b.table.headerRow = true; b.table.align = align;
          auto mkRow = [&](const std::vector<std::string>& cells, bool header) {
            std::vector<Cell> row;
            for (size_t k = 0; k < align.size(); ++k) {
              Cell c; c.header = header;
              if (k < cells.size()) c.runs = InlineRunsNoImages(cells[k]);
              row.push_back(std::move(c));
            }
            b.table.rows.push_back(std::move(row));
          };
          mkRow(head, true);
          size_t j = i + 2;
          while (j < n && !IsBlank(lines[j]) && lines[j].find('|') != std::string::npos) {
            if (StartsBlockNotTable(lines[j])) break;
            std::vector<std::string> cells; SplitTableRow(lines[j], cells); mkRow(cells, false); ++j;
          }
          out.push_back(std::move(b));
          i = j; continue;
        }
      }
      // indented code
      if (ind >= 4) {
        std::string code; size_t j = i; std::string pendingBlank;
        while (j < n) {
          if (IsBlank(lines[j])) { pendingBlank += "\n"; ++j; continue; }
          if (LeadingSpaces(lines[j]) < 4) break;
          code += pendingBlank; pendingBlank.clear();
          code += lines[j].substr(4) + "\n"; ++j;
        }
        if (!code.empty()) code.pop_back();
        Block b; b.kind = Block::Kind::Code; Run r; r.text = code; r.code = true; b.runs.push_back(r);
        out.push_back(std::move(b));
        i = j; continue;
      }
      // paragraph (with setext headings)
      {
        std::vector<std::string> para; size_t j = i;
        int setext = 0;
        while (j < n) {
          const std::string& l = lines[j];
          if (IsBlank(l)) break;
          if (!para.empty()) {
            size_t li = LeadingSpaces(l);
            if (li <= 3) {
              std::string t = TrimAscii(l);
              if (!t.empty() && t.find_first_not_of('=') == std::string::npos) { setext = 1; ++j; break; }
              if (!t.empty() && t.find_first_not_of('-') == std::string::npos) { setext = 2; ++j; break; }
            }
            if (StartsBlock(l)) break;
            std::vector<char> al2;
            if (j + 1 < n && l.find('|') != std::string::npos && IsDelimRow(lines[j + 1], al2)) break;
          }
          para.push_back(l); ++j;
        }
        std::string text;
        for (size_t k = 0; k < para.size(); ++k) {
          std::string t = para[k];
          size_t b = t.find_first_not_of(" \t"); t = b == std::string::npos ? "" : t.substr(b);
          if (k + 1 == para.size()) { while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back(); }
          text += t; if (k + 1 < para.size()) text += "\n";
        }
        if (setext) {
          Block b; b.kind = Block::Kind::Heading; b.level = setext; b.runs = InlineRuns(ReplaceNewlines(text));
          out.push_back(std::move(b));
        } else {
          EmitParagraph(text, out, quoteDepth);
        }
        i = j; continue;
      }
    }
  }

  std::vector<Block> ParseAll(const std::vector<std::string>& lines) {
    std::vector<Block> out;
    Parse(lines, out, 0, 0);
    return out;
  }

 private:
  MdContext& ctx_;
  Document& doc_;
  InlineParser inl_;

  static std::string ReplaceNewlines(std::string s) { for (auto& c : s) if (c == '\n') c = ' '; return s; }

  bool StartsBlock(const std::string& l) {
    char ch; size_t len, ind2; std::string info; int lv; std::string t;
    if (ParseFence(l, ch, len, info, ind2)) return true;
    if (ParseAtx(l, lv, t)) return true;
    if (IsHrLine(l)) return true;
    size_t ind = LeadingSpaces(l);
    if (ind <= 3 && ind < l.size() && l[ind] == '>') return true;
    ListMarker m;
    if (ParseListMarker(l, m) && !m.empty && (!m.ordered || m.number == 1)) return true;
    if (IsHtmlBlockStart(l) && l.find("<!--") == std::string::npos) return true;
    return false;
  }
  bool StartsBlockNotTable(const std::string& l) { return StartsBlock(l); }

  Runs InlineRuns(const std::string& text) {
    Run base;
    auto pieces = inl_.Parse(text, base);
    Runs r;
    for (auto& p : pieces) {
      if (p.image) { Run x = p.run; x.text = p.run.text; if (!x.text.empty()) r.push_back(x); continue; }
      r.push_back(p.run);
    }
    return r;
  }
  Runs InlineRunsNoImages(const std::string& text) {
    Runs r = InlineRuns(text);
    // <br> inside cells stay as line breaks
    return r;
  }

  std::shared_ptr<ImageData> LoadImage(const std::string& url, const std::string& alt) {
    if (!ctx_.opt || !ctx_.opt->loadImages || url.empty()) return nullptr;
    Bytes data;
    if (StartsWith(LowerAscii(url.substr(0, 5)), "data:")) {
      size_t comma = url.find(',');
      if (comma == std::string::npos || LowerAscii(url.substr(0, comma)).find(";base64") == std::string::npos) return nullptr;
      data = Base64Decode(std::string_view(url).substr(comma + 1));
    } else {
      std::string l = LowerAscii(url.substr(0, 8));
      if (l.find("://") != std::string::npos || StartsWith(l, "//") || ctx_.opt->baseDir.empty()) return nullptr;
      std::string path = url;
      size_t q = path.find_first_of("?#"); if (q != std::string::npos) path.resize(q);
      std::string dec;
      for (size_t i = 0; i < path.size(); ++i) { if (path[i] == '%' && i + 2 < path.size() + 0 && isxdigit((unsigned char)path[i + 1]) && isxdigit((unsigned char)path[i + 2])) { dec.push_back((char)strtol(path.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; } else dec.push_back(path[i]); }
      if (dec.empty() || dec.find(':') != std::string::npos || dec[0] == '/' || dec[0] == '\\') return nullptr;
      std::string n2 = dec; std::replace(n2.begin(), n2.end(), '\\', '/');
      for (auto& comp : SplitStr(n2, '/')) if (comp == "..") return nullptr;
      std::error_code ec;
      fs::path p = ctx_.opt->baseDir / fs::path(Widen(dec));
      if (!fs::is_regular_file(p, ec) || !ReadFileBytes(p, data, 64u << 20)) return nullptr;
    }
    ImageInfo info;
    if (data.empty() || data.size() > (64u << 20) || !ProbeImage(data.data(), data.size(), info)) return nullptr;
    auto img = std::make_shared<ImageData>();
    img->bytes = std::move(data); img->mime = info.mime; img->w = info.w; img->h = info.h; img->alt = alt;
    return img;
  }

  void EmitParagraph(const std::string& text, std::vector<Block>& out, int quoteDepth) {
    Run base;
    auto pieces = inl_.Parse(text, base);
    Runs cur;
    auto flushPara = [&] {
      Runs m = MergeRuns(cur);
      cur.clear();
      bool any = false; for (auto& r : m) if (r.br || !TrimAscii(r.text).empty()) any = true;
      if (!any) return;
      // trim edges
      if (!m.front().br) { auto& t = m.front().text; size_t k = 0; while (k < t.size() && t[k] == ' ') ++k; t.erase(0, k); }
      if (!m.back().br) { auto& t = m.back().text; while (!t.empty() && t.back() == ' ') t.pop_back(); }
      Block b; b.kind = quoteDepth > 0 ? Block::Kind::Quote : Block::Kind::Paragraph; b.level = quoteDepth > 0 ? quoteDepth - 1 : 0;
      b.runs = std::move(m);
      out.push_back(std::move(b));
    };
    for (auto& p : pieces) {
      if (p.image) {
        auto img = LoadImage(p.url, p.run.text);
        if (img) { flushPara(); Block b; b.kind = Block::Kind::Image; b.image = img; out.push_back(std::move(b)); }
        else {
          std::string l = LowerAscii(p.url.substr(0, 8));
          if (l.find("://") != std::string::npos || StartsWith(l, "//")) doc_.Warn(L"Images hosted online are not downloaded; their descriptions were kept instead.");
          else doc_.Warn(L"An image referenced by the document could not be loaded and was replaced by its description.");
          if (!p.run.text.empty()) { Run r = p.run; r.text = "[" + p.run.text + "]"; cur.push_back(r); }
        }
        continue;
      }
      cur.push_back(p.run);
    }
    flushPara();
  }

  size_t ParseList(const std::vector<std::string>& lines, size_t i, std::vector<Block>& out, int quoteDepth, int listLevel) {
    size_t n = lines.size();
    ListMarker first;
    ParseListMarker(lines[i], first);
    int counter = first.number;
    size_t baseIndent = first.indent;
    bool firstItem = true;
    while (i < n) {
      ListMarker m;
      if (!ParseListMarker(lines[i], m) || m.indent > baseIndent + 3) break;
      if (!firstItem && (m.ordered != first.ordered || m.delim != first.delim)) { if (m.indent <= baseIndent) break; }
      if (m.indent > baseIndent && !firstItem) break;
      if (IsHrLine(lines[i]) && !m.ordered) break;
      firstItem = false;
      std::vector<std::string> item;
      item.push_back(m.empty ? std::string() : lines[i].substr(m.contentStart));
      size_t j = i + 1;
      size_t ci = m.contentIndent;
      while (j < n) {
        const std::string& l = lines[j];
        if (IsBlank(l)) {
          size_t k = j; while (k < n && IsBlank(lines[k])) ++k;
          if (k < n && LeadingSpaces(lines[k]) > m.indent) { for (size_t b = j; b < k; ++b) item.push_back(""); j = k; continue; }
          break;
        }
        size_t li = LeadingSpaces(l);
        if (li > m.indent) { item.push_back(l.substr(std::min(li, ci))); ++j; continue; }
        ListMarker m2;
        if (ParseListMarker(l, m2) && m2.indent <= m.indent) break;
        if (!IsBlank(item.back()) && !StartsBlock(l)) { item.push_back(l); ++j; continue; }     // lazy continuation
        break;
      }
      // task list marker
      int checked = -1;
      if (!item.empty()) {
        std::string& f = item[0];
        if (f.size() >= 3 && f[0] == '[' && (f[1] == ' ' || f[1] == 'x' || f[1] == 'X') && f[2] == ']' && (f.size() == 3 || f[3] == ' ')) { checked = f[1] == ' ' ? 0 : 1; f = f.size() > 4 ? f.substr(4) : std::string(); }
      }
      std::vector<Block> sub;
      Parse(item, sub, quoteDepth, listLevel + 1);
      Block li;
      li.kind = Block::Kind::ListItem; li.level = listLevel; li.ordered = first.ordered; li.number = first.ordered ? counter : 0; li.checked = checked;
      size_t start = 0;
      if (!sub.empty() && (sub[0].kind == Block::Kind::Paragraph || sub[0].kind == Block::Kind::Quote) && !m.empty) { li.runs = std::move(sub[0].runs); start = 1; }
      out.push_back(std::move(li));
      for (size_t k = start; k < sub.size(); ++k) {
        // continuation paragraphs of an item stay attached visually as plain paragraphs
        out.push_back(std::move(sub[k]));
      }
      ++counter;
      i = j;
      // blank lines between items
      while (i < n && IsBlank(lines[i])) {
        size_t k = i; while (k < n && IsBlank(lines[k])) ++k;
        ListMarker m3;
        if (k < n && ParseListMarker(lines[k], m3) && m3.indent <= baseIndent + 0 && m3.ordered == first.ordered && m3.delim == first.delim) { i = k; break; }
        return k > i ? i : i;
      }
    }
    return i;
  }
};

void CollectDefinitions(const std::vector<std::string>& lines, MdContext& ctx) {
  bool inFence = false; char fch = 0; size_t flen = 0;
  for (auto& l : lines) {
    char c; size_t len, ind; std::string info;
    if (ParseFence(l, c, len, info, ind)) {
      if (!inFence) { inFence = true; fch = c; flen = len; continue; }
      if (c == fch && len >= flen && info.empty()) { inFence = false; continue; }
    }
    if (inFence) continue;
    size_t ind2 = LeadingSpaces(l);
    if (ind2 > 3 || ind2 >= l.size() || l[ind2] != '[') continue;
    size_t e = l.find("]:", ind2);
    if (e == std::string::npos) continue;
    std::string label = l.substr(ind2 + 1, e - ind2 - 1);
    if (label.empty()) continue;
    if (label[0] == '^') { ctx.footnotes.insert(label.substr(1)); continue; }
    std::string rest = TrimAscii(l.substr(e + 2));
    if (rest.empty()) continue;
    std::string url = rest.substr(0, rest.find_first_of(" \t"));
    if (url.size() > 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
    ctx.refs[LowerAscii(label)] = url;
  }
}

} // namespace

Document ParseMarkdown(std::string_view md, const MdReadOptions& opt) {
  Document doc;
  std::string text = SanitizeText(md);
  if (StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
  std::vector<std::string> lines;
  for (auto& l : SplitLines(text)) lines.push_back(ExpandLeadingTabs(l));
  // YAML front matter -> metadata
  if (!lines.empty() && lines[0] == "---") {
    size_t e = 1; while (e < lines.size() && lines[e] != "---" && lines[e] != "...") ++e;
    if (e < lines.size() && e > 1) {
      bool yaml = true;
      for (size_t k = 1; k < e; ++k) if (!IsBlank(lines[k]) && lines[k].find(':') == std::string::npos && lines[k][0] != ' ' && lines[k][0] != '-' && lines[k][0] != '#') yaml = false;
      if (yaml) {
        for (size_t k = 1; k < e; ++k) {
          size_t c = lines[k].find(':');
          if (c == std::string::npos) continue;
          std::string key = LowerAscii(TrimAscii(lines[k].substr(0, c))), val = TrimAscii(lines[k].substr(c + 1));
          if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) val = val.substr(1, val.size() - 2);
          if (key == "title") doc.meta.title = val; else if (key == "author") doc.meta.author = val; else if (key == "lang" || key == "language") doc.meta.language = val;
        }
        lines.erase(lines.begin(), lines.begin() + e + 1);
      }
    }
  }
  MdContext ctx; ctx.opt = &opt; ctx.doc = &doc;
  CollectDefinitions(lines, ctx);
  MdBlockParser p(ctx, doc);
  doc.blocks = p.ParseAll(lines);
  FinalizeDocument(doc);
  return doc;
}

// ============================================================================================================================
// Writer
// ============================================================================================================================
namespace {

std::string EscapeMdText(std::string_view s, bool inTable) {
  std::string o; o.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    switch (c) {
      case '\\': case '*': case '_': case '`': case '[': case ']': case '~': o.push_back('\\'); o.push_back(c); break;
      case '|': if (inTable) { o.push_back('\\'); } o.push_back(c); break;
      case '<': {
        char nx = i + 1 < s.size() ? s[i + 1] : 0;
        if (isalpha((unsigned char)nx) || nx == '/' || nx == '!' || nx == '?') o += "\\<"; else o.push_back(c);
        break;
      }
      case '&': {
        size_t e = s.find(';', i);
        if (e != std::string_view::npos && e - i <= 10 && e > i + 1) {
          bool ok = true; for (size_t k = i + 1; k < e; ++k) if (!isalnum((unsigned char)s[k]) && s[k] != '#') ok = false;
          if (ok) { o += "\\&"; break; }
        }
        o.push_back(c); break;
      }
      case '\n': o.push_back(' '); break;
      default: o.push_back(c);
    }
  }
  return o;
}

// Escapes block-level triggers at the start of a line.
std::string EscapeLineStart(std::string s) {
  size_t i = 0; while (i < s.size() && s[i] == ' ') ++i;
  if (i >= s.size()) return s;
  char c = s[i];
  auto esc = [&](size_t pos) { s.insert(pos, "\\"); };
  if (c == '#') { size_t k = i; while (k < s.size() && s[k] == '#') ++k; if (k - i <= 6 && (k >= s.size() || s[k] == ' ')) esc(i); }
  else if (c == '>') esc(i);
  else if ((c == '-' || c == '+') && (i + 1 >= s.size() || s[i + 1] == ' ')) esc(i);
  else if (c == '-' || c == '=') { size_t k = i; while (k < s.size() && s[k] == c) ++k; if (k >= s.size() && k - i >= 1 && c == '=') esc(i); else if (k >= s.size() && k - i >= 3) esc(i); }
  else if (isdigit((unsigned char)c)) {
    size_t k = i; while (k < s.size() && isdigit((unsigned char)s[k])) ++k;
    if (k < s.size() && (s[k] == '.' || s[k] == ')') && (k + 1 >= s.size() || s[k + 1] == ' ')) s.insert(k, "\\");
  }
  return s;
}

struct Fmt { bool b = false, i = false, s = false, u = false, sup = false, sub = false; };

std::string InlineToMd(const Runs& runs, bool inTable, bool embedImagesUnused = true) {
  (void)embedImagesUnused;
  std::string o;
  std::vector<char> stack;     // 'b','i','s','u','p'(sup),'d'(sub)
  std::string pendingTrail;
  auto marker = [](char k, bool open) -> std::string {
    switch (k) { case 'b': return "**"; case 'i': return "*"; case 's': return "~~"; case 'u': return open ? "<u>" : "</u>"; case 'p': return open ? "<sup>" : "</sup>"; case 'd': return open ? "<sub>" : "</sub>"; }
    return "";
  };
  auto closeTo = [&](size_t keep) { while (stack.size() > keep) { o += marker(stack.back(), false); stack.pop_back(); } };
  size_t idx = 0;
  while (idx < runs.size()) {
    const Run& r = runs[idx];
    if (r.br) { closeTo(0); o += pendingTrail; pendingTrail.clear(); o += inTable ? "<br>" : "\\\n"; ++idx; continue; }
    // group linked runs
    if (!r.link.empty()) {
      size_t j = idx; while (j < runs.size() && runs[j].link == r.link && !runs[j].br) ++j;
      closeTo(0); o += pendingTrail; pendingTrail.clear();
      Runs inner(runs.begin() + idx, runs.begin() + j);
      for (auto& x : inner) x.link.clear();
      std::string txt = InlineToMd(inner, inTable);
      std::string url = r.link;
      std::string eu; for (char ch : url) { if (ch == ' ') eu += "%20"; else if (ch == '(') eu += "%28"; else if (ch == ')') eu += "%29"; else if (ch == '<') eu += "%3C"; else if (ch == '>') eu += "%3E"; else eu.push_back(ch); }
      o += "[" + txt + "](" + eu + ")";
      idx = j; continue;
    }
    std::string text = r.text;
    size_t lead = 0; while (lead < text.size() && (text[lead] == ' ' || text[lead] == '\t')) ++lead;
    size_t endc = text.size(); while (endc > lead && (text[endc - 1] == ' ' || text[endc - 1] == '\t')) --endc;
    std::string leadWs = text.substr(0, lead), core = text.substr(lead, endc - lead), trail = text.substr(endc);
    if (core.empty()) { o += pendingTrail + leadWs + trail; pendingTrail.clear(); ++idx; continue; }
    std::vector<char> need;
    if (r.bold) need.push_back('b');
    if (r.italic) need.push_back('i');
    if (r.strike) need.push_back('s');
    if (r.underline) need.push_back('u');
    if (r.sup) need.push_back('p');
    if (r.sub) need.push_back('d');
    // find first stack element not needed
    size_t keep = 0;
    while (keep < stack.size() && std::find(need.begin(), need.end(), stack[keep]) != need.end()) ++keep;
    bool closing = keep < stack.size();
    if (closing) { closeTo(keep); o += pendingTrail; pendingTrail.clear(); }
    else if (!pendingTrail.empty()) { o += pendingTrail; pendingTrail.clear(); }
    o += leadWs;
    for (char k : need) if (std::find(stack.begin(), stack.end(), k) == stack.end()) { o += marker(k, true); stack.push_back(k); }
    if (r.code) {
      size_t maxTicks = 0, cur = 0;
      for (char c : core) { if (c == '`') { ++cur; maxTicks = std::max(maxTicks, cur); } else cur = 0; }
      std::string fence(maxTicks + 1, '`');
      std::string c2 = core; if (inTable) { std::string t; for (char ch : c2) { if (ch == '|') t += "\\|"; else t.push_back(ch); } c2 = t; }
      bool pad = !c2.empty() && (c2.front() == '`' || c2.back() == '`' );
      o += fence + (pad ? " " : "") + c2 + (pad ? " " : "") + fence;
    } else o += EscapeMdText(core, inTable);
    pendingTrail = trail;
    ++idx;
  }
  closeTo(0);
  o += pendingTrail;
  return o;
}

std::string ImageToMd(const Block& b, const MdWriteOptions& opt) {
  if (!b.image) return "";
  if (!opt.embedImages) return b.image->alt.empty() ? std::string("[image]") : EscapeMdText("[" + b.image->alt + "]", false);
  std::string mime = b.image->mime.empty() ? "image/png" : b.image->mime;
  std::string alt = EscapeMdText(b.image->alt, false);
  return "![" + alt + "](data:" + mime + ";base64," + Base64Encode(b.image->bytes.data(), b.image->bytes.size()) + ")";
}

} // namespace

std::string WriteMarkdown(const Document& d, const MdWriteOptions& opt) {
  std::string out;
  struct Lv { bool ordered; int counter; };
  std::vector<Lv> lists;
  auto sep = [&](bool tight) { if (!out.empty()) out += tight ? "\n" : "\n\n"; };
  bool prevWasList = false;
  Block::Kind prevKind = Block::Kind::Paragraph;
  int prevQuoteLevel = -1;
  for (size_t bi = 0; bi < d.blocks.size(); ++bi) {
    const Block& b = d.blocks[bi];
    bool isList = b.kind == Block::Kind::ListItem;
    if (!isList) lists.clear();
    switch (b.kind) {
      case Block::Kind::Heading: {
        sep(false);
        std::string t = InlineToMd(b.runs, false);
        for (auto& c : t) if (c == '\n') c = ' ';
        out += std::string((size_t)std::clamp(b.level, 1, 6), '#') + " " + t;
        break;
      }
      case Block::Kind::Paragraph: {
        sep(false);
        std::string t = InlineToMd(b.runs, false);
        // escape block triggers on each line
        std::string res; size_t st = 0;
        while (true) { size_t e = t.find('\n', st); std::string ln = t.substr(st, e == std::string::npos ? std::string::npos : e - st); res += EscapeLineStart(ln); if (e == std::string::npos) break; res += "\n"; st = e + 1; }
        out += res;
        break;
      }
      case Block::Kind::Quote: {
        bool contiguous = prevKind == Block::Kind::Quote;
        if (contiguous) out += "\n>\n"; else sep(false);
        std::string t = InlineToMd(b.runs, false);
        std::string prefix; for (int k = 0; k <= b.level; ++k) prefix += "> ";
        std::string res; size_t st = 0;
        while (true) { size_t e = t.find('\n', st); res += prefix + t.substr(st, e == std::string::npos ? std::string::npos : e - st); if (e == std::string::npos) break; res += "\n"; st = e + 1; }
        out += res;
        prevQuoteLevel = b.level;
        break;
      }
      case Block::Kind::ListItem: {
        size_t lvl = (size_t)b.level;
        if (!prevWasList) { lists.clear(); sep(false); } else sep(true);
        if (lists.size() > lvl + 1) lists.resize(lvl + 1);
        while (lists.size() < lvl + 1) lists.push_back({b.ordered, 0});
        Lv& L = lists[lvl];
        if (L.ordered != b.ordered) { L.ordered = b.ordered; L.counter = 0; }
        std::string marker;
        if (b.ordered) { if (L.counter == 0 && b.number > 0) L.counter = b.number; else ++L.counter; marker = std::to_string(L.counter) + ". "; }
        else marker = "- ";
        std::string t = InlineToMd(b.runs, false);
        std::string indent(lvl * 4, ' ');
        std::string cont(indent.size() + marker.size(), ' ');
        std::string task = b.checked == 1 ? "[x] " : b.checked == 0 ? "[ ] " : "";
        std::string res = indent + marker + task; size_t st = 0; bool firstLine = true;
        while (true) { size_t e = t.find('\n', st); std::string ln = t.substr(st, e == std::string::npos ? std::string::npos : e - st); res += (firstLine ? "" : cont) + (firstLine ? EscapeLineStart(ln) : ln); firstLine = false; if (e == std::string::npos) break; res += "\n"; st = e + 1; }
        out += res;
        break;
      }
      case Block::Kind::Code: {
        sep(false);
        std::string code = PlainText(b.runs);
        size_t maxTicks = 2, cur = 0;
        for (char c : code) { if (c == '`') { ++cur; maxTicks = std::max(maxTicks, cur); } else cur = 0; }
        std::string fence(maxTicks + 1, '`');
        out += fence + b.lang + "\n" + code + "\n" + fence;
        break;
      }
      case Block::Kind::Rule: sep(false); out += "---"; break;
      case Block::Kind::PageBreak: sep(false); out += "<div style=\"page-break-after: always\"></div>"; break;
      case Block::Kind::Image: sep(false); out += ImageToMd(b, opt); break;
      case Block::Kind::Footnote: sep(false); out += "[^" + b.label + "]: " + InlineToMd(b.runs, false); break;
      case Block::Kind::Table: {
        sep(false);
        const Table& t = b.table;
        size_t cols = t.Cols();
        if (t.rows.empty() || cols == 0) break;
        auto rowStr = [&](const std::vector<Cell>& row) {
          std::string s = "|";
          size_t col = 0;
          for (auto& c : row) {
            std::string txt = InlineToMd(c.runs, true);
            for (auto& ch : txt) if (ch == '\n') ch = ' ';
            s += " " + txt + " |";
            for (int k = 1; k < c.colspan; ++k) { s += "  |"; }
            col += (size_t)std::max(1, c.colspan);
          }
          for (; col < cols; ++col) s += "  |";
          return s;
        };
        out += rowStr(t.rows[0]) + "\n|";
        for (size_t c = 0; c < cols; ++c) {
          char a = c < t.align.size() ? t.align[c] : 0;
          out += a == 'c' ? " :---: |" : a == 'r' ? " ---: |" : a == 'l' ? " :--- |" : " --- |";
        }
        for (size_t r = 1; r < t.rows.size(); ++r) out += "\n" + rowStr(t.rows[r]);
        break;
      }
    }
    prevWasList = isList;
    prevKind = b.kind;
    (void)prevQuoteLevel;
  }
  out += "\n";
  return out;
}

} // namespace filo::doc
