#include "engines/doc/textfmt.h"
#include "engines/doc/xml.h"
#include <algorithm>
#include <cstring>

namespace filo::doc {

// ---- reader ----------------------------------------------------------------------------------------------------------------
namespace {
bool ParseBullet(const std::string& line, size_t& indent, bool& ordered, int& number, std::string& rest) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  indent = i;
  if (i >= line.size()) return false;
  // bullets: - * + and typical unicode bullets followed by a space
  static const char* bullets[] = {"\xE2\x80\xA2", "\xC2\xB7", "\xE2\x96\xAA", "\xE2\x80\xA3", "\xE2\x97\xA6", "\xE2\x80\x93", "\xE2\x97\x8F"};
  if ((line[i] == '-' || line[i] == '*' || line[i] == '+') && i + 1 < line.size() && line[i + 1] == ' ' && line.find_first_not_of(" \t", i + 1) != std::string::npos) {
    ordered = false; number = 0; rest = line.substr(i + 2); return true;
  }
  for (auto* b : bullets) {
    size_t l = strlen(b);
    if (line.compare(i, l, b) == 0 && i + l < line.size() && line[i + l] == ' ') { ordered = false; number = 0; rest = line.substr(i + l + 1); return true; }
  }
  size_t j = i; while (j < line.size() && isdigit((unsigned char)line[j]) && j - i < 4) ++j;
  if (j > i && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') { ordered = true; number = atoi(line.substr(i, j - i).c_str()); rest = line.substr(j + 2); return true; }
  return false;
}
}

Document ParseText(std::string_view in, const TextReadOptions& opt) {
  Document doc;
  std::string text = SanitizeText(in);
  for (auto& c : text) if (c == '\f') c = '\n';
  std::vector<std::string> lines = SplitLines(text);
  size_t i = 0, n = lines.size();
  auto blank = [](const std::string& l) { return l.find_first_not_of(" \t\xC2\xA0") == std::string::npos; };
  while (i < n) {
    if (blank(lines[i])) { ++i; continue; }
    // paragraph chunk
    Runs runs;
    auto flushPara = [&] {
      while (!runs.empty() && runs.back().br) runs.pop_back();
      if (runs.empty()) return;
      Block b; b.kind = Block::Kind::Paragraph; b.runs = std::move(runs); runs.clear();
      doc.blocks.push_back(std::move(b));
    };
    while (i < n && !blank(lines[i])) {
      const std::string& l = lines[i];
      size_t ind; bool ordered; int num; std::string rest;
      // setext heading
      if (opt.detectHeadings && runs.empty() && i + 1 < n) {
        std::string t = TrimAscii(lines[i + 1]);
        if (t.size() >= 3 && l.size() < 120 && !TrimAscii(l).empty() && (t.find_first_not_of('=') == std::string::npos || t.find_first_not_of('-') == std::string::npos) && ParseBullet(l, ind, ordered, num, rest) == false) {
          Block b; b.kind = Block::Kind::Heading; b.level = t[0] == '=' ? 1 : 2; b.runs.push_back(MakeRun(TrimAscii(l)));
          doc.blocks.push_back(std::move(b)); i += 2; continue;
        }
      }
      if (opt.detectLists && ParseBullet(l, ind, ordered, num, rest)) {
        flushPara();
        Block b; b.kind = Block::Kind::ListItem; b.level = (int)std::min<size_t>(ind / 2, 5); b.ordered = ordered; b.number = num;
        std::string body = TrimAscii(rest);
        ++i;
        while (i < n && !blank(lines[i])) {                    // continuation lines: more indented and not a bullet
          size_t ind2; bool o2; int n2; std::string r2;
          if (ParseBullet(lines[i], ind2, o2, n2, r2)) break;
          size_t li = lines[i].find_first_not_of(" \t");
          if (li == std::string::npos || li <= ind) break;
          body += " " + TrimAscii(lines[i]); ++i;
        }
        b.runs.push_back(MakeRun(body));
        doc.blocks.push_back(std::move(b));
        continue;
      }
      std::string t = l;
      // leading indentation is dropped (a paragraph indent carries no meaning in the model)
      size_t lead = t.find_first_not_of(" \t"); if (lead != std::string::npos) t.erase(0, lead);
      while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
      if (!runs.empty()) { if (opt.reflow) runs.push_back(MakeRun(" ")); else { Run br; br.br = true; runs.push_back(br); } }
      runs.push_back(MakeRun(t));
      ++i;
    }
    flushPara();
  }
  FinalizeDocument(doc);
  return doc;
}

// ---- writer ----------------------------------------------------------------------------------------------------------------
static std::string WrapLine(const std::string& s, int cols) {
  if (cols <= 0 || (int)Utf8Length(s) <= cols) return s;
  std::string out, line; int len = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find(' ', i);
    std::string w = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
    int wl = (int)Utf8Length(w);
    if (len > 0 && len + 1 + wl > cols) { out += line + "\n"; line.clear(); len = 0; }
    if (len > 0) { line += " "; ++len; }
    line += w; len += wl;
    if (e == std::string::npos) break;
    i = e + 1;
  }
  return out + line;
}

std::string WriteText(const Document& d, const TextWriteOptions& opt) {
  std::string out;
  bool lastWasList = false;
  struct Cnt { int c = 0; bool ordered = false; };
  std::vector<Cnt> counters;
  auto sepBlank = [&] { if (!out.empty() && !EndsWith(out, "\n\n")) out += (EndsWith(out, "\n") ? "\n" : "\n\n"); };
  for (auto& b : d.blocks) {
    bool isList = b.kind == Block::Kind::ListItem;
    if (!isList && lastWasList) sepBlank();
    switch (b.kind) {
      case Block::Kind::Heading: sepBlank(); out += WrapLine(PlainText(b.runs), opt.wrapColumns) + "\n"; break;
      case Block::Kind::Paragraph: sepBlank(); out += WrapLine(PlainText(b.runs), opt.wrapColumns) + "\n"; break;
      case Block::Kind::Quote: { sepBlank(); std::string t = PlainText(b.runs); std::string pre((size_t)(b.level + 1) * 2, '>'); pre += " "; size_t st = 0; while (true) { size_t e = t.find('\n', st); out += pre + t.substr(st, e == std::string::npos ? std::string::npos : e - st) + "\n"; if (e == std::string::npos) break; st = e + 1; } break; }
      case Block::Kind::ListItem: {
        if (!lastWasList) { sepBlank(); counters.clear(); }
        size_t lvl = (size_t)b.level;
        if (counters.size() > lvl + 1) counters.resize(lvl + 1);
        while (counters.size() < lvl + 1) counters.push_back({});
        Cnt& c = counters[lvl];
        if (c.ordered != b.ordered) { c.ordered = b.ordered; c.c = 0; }
        std::string marker;
        if (b.ordered && opt.numberLists) { c.c = (c.c == 0 && b.number > 0) ? b.number : c.c + 1; marker = std::to_string(c.c) + ". "; }
        else marker = b.checked == 1 ? "[x] " : b.checked == 0 ? "[ ] " : "\xE2\x80\xA2 ";
        std::string t = PlainText(b.runs);
        std::string ind(lvl * 2, ' ');
        std::string res = ind + marker; size_t st = 0; bool first = true;
        while (true) { size_t e = t.find('\n', st); res += (first ? "" : ind + std::string(marker.size(), ' ')) + t.substr(st, e == std::string::npos ? std::string::npos : e - st); first = false; if (e == std::string::npos) break; res += "\n"; st = e + 1; }
        out += res + "\n";
        break;
      }
      case Block::Kind::Table: {
        sepBlank();
        for (auto& row : b.table.rows) {
          std::string line; bool first = true;
          for (auto& c : row) {
            std::string t = PlainText(c.runs);
            for (auto& ch : t) if (ch == '\n' || ch == '\t') ch = ' ';
            if (!first) line += "\t";
            line += t; first = false;
            for (int k = 1; k < c.colspan; ++k) line += "\t";
          }
          out += line + "\n";
        }
        break;
      }
      case Block::Kind::Code: sepBlank(); out += PlainText(b.runs) + "\n"; break;
      case Block::Kind::Rule: sepBlank(); out += "----------------------------------------\n"; break;
      case Block::Kind::Footnote: sepBlank(); out += "[" + b.label + "] " + PlainText(b.runs) + "\n"; break;
      case Block::Kind::Image: case Block::Kind::PageBreak: break;
    }
    lastWasList = isList;
  }
  while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
  return out;
}

// ---- textClipping ----------------------------------------------------------------------------------------------------------
namespace {

struct PNode {
  enum Kind { Null, Bool, Int, Real, Str, Data, Array, Dict } kind = Null;
  std::string str;                       // Str (UTF-8) or Data (raw bytes)
  int64_t i = 0;
  std::vector<PNode> items;              // Array items / Dict values
  std::vector<std::string> keys;         // Dict keys
  const PNode* Get(const std::string& k) const { for (size_t x = 0; x < keys.size(); ++x) if (keys[x] == k) return &items[x]; return nullptr; }
};

PNode FromXmlPlist(const xml::Node& n) {
  PNode p;
  std::string t(n.Local());
  if (t == "dict") {
    p.kind = PNode::Dict;
    std::string key;
    for (auto& k : n.kids) {
      if (k->isText) continue;
      if (k->Is("key")) key = k->InnerText();
      else { p.keys.push_back(key); p.items.push_back(FromXmlPlist(*k)); key.clear(); }
    }
  } else if (t == "array") { p.kind = PNode::Array; for (auto& k : n.kids) if (!k->isText) p.items.push_back(FromXmlPlist(*k)); }
  else if (t == "string") { p.kind = PNode::Str; p.str = n.InnerText(); }
  else if (t == "data") { p.kind = PNode::Data; Bytes b = Base64Decode(n.InnerText()); p.str.assign(b.begin(), b.end()); }
  else if (t == "integer") { p.kind = PNode::Int; p.i = atoll(n.InnerText().c_str()); }
  else if (t == "true" || t == "false") { p.kind = PNode::Bool; p.i = t == "true"; }
  return p;
}

struct BPlist {
  const uint8_t* d; size_t n; int offSize = 0, refSize = 0; uint64_t numObjects = 0, top = 0, offTable = 0;
  uint64_t Be(size_t pos, int bytes) const { uint64_t v = 0; for (int k = 0; k < bytes; ++k) v = (v << 8) | d[pos + k]; return v; }
  bool Init() {
    if (n < 40 || memcmp(d, "bplist00", 8) != 0) return false;
    const uint8_t* t = d + n - 32;
    offSize = t[6]; refSize = t[7];
    for (int k = 0; k < 8; ++k) { numObjects = (numObjects << 8) | t[8 + k]; top = (top << 8) | t[16 + k]; offTable = (offTable << 8) | t[24 + k]; }
    if (offSize < 1 || offSize > 8 || refSize < 1 || refSize > 8 || numObjects == 0 || numObjects > 1000000 || offTable >= n) return false;
    if (offTable + numObjects * offSize > n) return false;
    return top < numObjects;
  }
  bool Obj(uint64_t idx, PNode& out, int depth) const {
    if (depth > 32 || idx >= numObjects) return false;
    uint64_t off = Be((size_t)(offTable + idx * offSize), offSize);
    if (off >= n) return false;
    uint8_t m = d[off]; uint8_t type = m >> 4, low = m & 15;
    size_t pos = (size_t)off + 1;
    auto len = [&](uint64_t& l) -> bool {
      l = low;
      if (low == 15) {
        if (pos >= n) return false;
        uint8_t im = d[pos++]; if ((im >> 4) != 1) return false;
        int bytes = 1 << (im & 15); if (pos + bytes > n || bytes > 8) return false;
        l = Be(pos, bytes); pos += bytes;
      }
      return true;
    };
    switch (type) {
      case 0: out.kind = (low == 8 || low == 9) ? PNode::Bool : PNode::Null; out.i = low == 9; return true;
      case 1: { int bytes = 1 << low; if (bytes > 8 || pos + bytes > n) return false; out.kind = PNode::Int; out.i = (int64_t)Be(pos, bytes); return true; }
      case 2: out.kind = PNode::Real; return true;
      case 3: out.kind = PNode::Real; return true;
      case 4: { uint64_t l; if (!len(l) || pos + l > n) return false; out.kind = PNode::Data; out.str.assign((const char*)d + pos, (size_t)l); return true; }
      case 5: { uint64_t l; if (!len(l) || pos + l > n) return false; out.kind = PNode::Str; out.str.assign((const char*)d + pos, (size_t)l); return true; }
      case 6: { uint64_t l; if (!len(l) || pos + l * 2 > n) return false; out.kind = PNode::Str; out.str = DecodeText(d + pos, (size_t)l * 2, TextEnc::Utf16BE).utf8; return true; }
      case 0xA: {
        uint64_t l; if (!len(l) || l > 100000 || pos + l * refSize > n) return false;
        out.kind = PNode::Array;
        for (uint64_t k = 0; k < l; ++k) { PNode c; if (!Obj(Be((size_t)(pos + k * refSize), refSize), c, depth + 1)) return false; out.items.push_back(std::move(c)); }
        return true;
      }
      case 0xD: {
        uint64_t l; if (!len(l) || l > 100000 || pos + l * refSize * 2 > n) return false;
        out.kind = PNode::Dict;
        for (uint64_t k = 0; k < l; ++k) {
          PNode key, val;
          if (!Obj(Be((size_t)(pos + k * refSize), refSize), key, depth + 1)) return false;
          if (!Obj(Be((size_t)(pos + (l + k) * refSize), refSize), val, depth + 1)) return false;
          out.keys.push_back(key.str); out.items.push_back(std::move(val));
        }
        return true;
      }
    }
    return false;
  }
};

bool ExtractClippingText(const PNode& root, std::string& utf8) {
  const PNode* uti = root.Get("UTI-Data");
  const PNode* src = uti && uti->kind == PNode::Dict ? uti : &root;
  if (auto* p = src->Get("public.utf8-plain-text")) { utf8 = p->str; return true; }
  if (auto* p = src->Get("public.utf16-external-plain-text")) { utf8 = DecodeText((const uint8_t*)p->str.data(), p->str.size()).utf8; return true; }
  if (auto* p = src->Get("public.utf16-plain-text")) { utf8 = DecodeText((const uint8_t*)p->str.data(), p->str.size(), TextEnc::Utf16LE).utf8; return true; }
  if (auto* p = src->Get("com.apple.traditional-mac-plain-text")) {
    int n = MultiByteToWideChar(10000, 0, p->str.data(), (int)p->str.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(10000, 0, p->str.data(), (int)p->str.size(), w.data(), n);
    utf8 = Narrow(w); return true;
  }
  return false;
}
} // namespace

bool ParseTextClipping(const Bytes& data, std::string& utf8, std::wstring* err) {
  if (data.empty()) {
    if (err) *err = L"This .textClipping file is empty. macOS stores clipping text in a resource fork, which does not exist on Windows file systems, so there is nothing to read.";
    return false;
  }
  PNode root;
  bool parsed = false;
  BPlist bp{data.data(), data.size()};
  if (bp.Init()) parsed = bp.Obj(bp.top, root, 0);
  else {
    std::string_view sv((const char*)data.data(), data.size());
    if (sv.find("<plist") != std::string_view::npos) {
      auto dom = xml::ParseDom(sv);
      if (dom) {
        const xml::Node* dict = dom->Is("plist") ? dom->Child("dict") : (dom->Is("dict") ? dom.get() : nullptr);
        if (dict) { root = FromXmlPlist(*dict); parsed = true; }
      }
    }
  }
  if (parsed && root.kind == PNode::Dict && ExtractClippingText(root, utf8)) {
    utf8 = SanitizeText(utf8);
    return true;
  }
  if (parsed) {
    if (err) *err = L"This .textClipping file does not contain text that can be read here (macOS keeps the older clipping format in a resource fork, which Windows file systems cannot store).";
    return false;
  }
  // not a plist: a plain-text file with the .textClipping extension
  DecodedText dt = DecodeText(data);
  size_t ctrl = 0; for (unsigned char c : dt.utf8) if (c < 9 || (c > 13 && c < 32)) ++ctrl;
  if (ctrl * 20 < dt.utf8.size() + 1 && !dt.utf8.empty()) { utf8 = SanitizeText(dt.utf8); return true; }
  if (err) *err = L"The .textClipping file is not in a readable format (it may rely on a macOS resource fork).";
  return false;
}

} // namespace filo::doc
