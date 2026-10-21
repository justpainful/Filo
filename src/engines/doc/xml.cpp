#include "engines/doc/xml.h"
#include "engines/doc/text_io.h"
#include <unknwn.h>
#include <objidl.h>
#include <oaidl.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <unordered_map>
#include <wrl/client.h>
#include <cstdlib>

namespace filo::doc::xml {

uint32_t LookupEntity(std::string_view n) {
  static const std::unordered_map<std::string, uint32_t> t = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 160}, {"iexcl", 161}, {"cent", 162}, {"pound", 163},
    {"curren", 164}, {"yen", 165}, {"brvbar", 166}, {"sect", 167}, {"uml", 168}, {"copy", 169}, {"ordf", 170}, {"laquo", 171}, {"not", 172},
    {"shy", 173}, {"reg", 174}, {"macr", 175}, {"deg", 176}, {"plusmn", 177}, {"sup2", 178}, {"sup3", 179}, {"acute", 180}, {"micro", 181},
    {"para", 182}, {"middot", 183}, {"cedil", 184}, {"sup1", 185}, {"ordm", 186}, {"raquo", 187}, {"frac14", 188}, {"frac12", 189},
    {"frac34", 190}, {"iquest", 191}, {"Agrave", 192}, {"Aacute", 193}, {"Acirc", 194}, {"Atilde", 195}, {"Auml", 196}, {"Aring", 197},
    {"AElig", 198}, {"Ccedil", 199}, {"Egrave", 200}, {"Eacute", 201}, {"Ecirc", 202}, {"Euml", 203}, {"Igrave", 204}, {"Iacute", 205},
    {"Icirc", 206}, {"Iuml", 207}, {"ETH", 208}, {"Ntilde", 209}, {"Ograve", 210}, {"Oacute", 211}, {"Ocirc", 212}, {"Otilde", 213},
    {"Ouml", 214}, {"times", 215}, {"Oslash", 216}, {"Ugrave", 217}, {"Uacute", 218}, {"Ucirc", 219}, {"Uuml", 220}, {"Yacute", 221},
    {"THORN", 222}, {"szlig", 223}, {"agrave", 224}, {"aacute", 225}, {"acirc", 226}, {"atilde", 227}, {"auml", 228}, {"aring", 229},
    {"aelig", 230}, {"ccedil", 231}, {"egrave", 232}, {"eacute", 233}, {"ecirc", 234}, {"euml", 235}, {"igrave", 236}, {"iacute", 237},
    {"icirc", 238}, {"iuml", 239}, {"eth", 240}, {"ntilde", 241}, {"ograve", 242}, {"oacute", 243}, {"ocirc", 244}, {"otilde", 245},
    {"ouml", 246}, {"divide", 247}, {"oslash", 248}, {"ugrave", 249}, {"uacute", 250}, {"ucirc", 251}, {"uuml", 252}, {"yacute", 253},
    {"thorn", 254}, {"yuml", 255}, {"OElig", 338}, {"oelig", 339}, {"Scaron", 352}, {"scaron", 353}, {"Yuml", 376}, {"fnof", 402},
    {"circ", 710}, {"tilde", 732}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D},
    {"lrm", 0x200E}, {"rlm", 0x200F}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026},
    {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"euro", 0x20AC}, {"trade", 0x2122},
    {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194}, {"rArr", 0x21D2}, {"hArr", 0x21D4},
    {"minus", 0x2212}, {"infin", 0x221E}, {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265}, {"asymp", 0x2248}, {"sum", 0x2211},
    {"radic", 0x221A}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"pi", 0x3C0}, {"sigma", 0x3C3},
    {"omega", 0x3C9}, {"Omega", 0x3A9}, {"hearts", 0x2665}, {"check", 0x2713},
  };
  auto it = t.find(std::string(n));
  return it == t.end() ? 0 : it->second;
}

void DecodeEntitiesInto(std::string_view s, std::string& out) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c != '&') {
      size_t j = s.find('&', i);
      if (j == std::string_view::npos) j = s.size();
      out.append(s.substr(i, j - i));
      i = j;
      continue;
    }
    size_t e = s.find(';', i + 1);
    if (e == std::string_view::npos || e - i > 32) { out.push_back('&'); ++i; continue; }
    std::string_view name = s.substr(i + 1, e - i - 1);
    uint32_t cp = 0;
    if (!name.empty() && name[0] == '#') {
      char* endp = nullptr;
      std::string num(name.substr(1));
      if (!num.empty() && (num[0] == 'x' || num[0] == 'X')) cp = (uint32_t)strtoul(num.c_str() + 1, &endp, 16);
      else cp = (uint32_t)strtoul(num.c_str(), &endp, 10);
      if (!endp || *endp != 0 || num.size() <= (num[0] == 'x' || num[0] == 'X' ? 1u : 0u)) cp = 0;
      if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    } else cp = LookupEntity(name);
    if (cp == 0) { out.push_back('&'); ++i; continue; }
    AppendUtf8(out, cp);
    i = e + 1;
  }
}

static bool IsNameChar(unsigned char c) { return c > 0x7F || isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.'; }

bool Parse(std::string_view data, const Sax& sax, std::string* err) {
  std::string transcoded;
  // encoding sniffing
  if (data.size() >= 2 && (uint8_t)data[0] == 0xFF && (uint8_t)data[1] == 0xFE) { transcoded = DecodeText((const uint8_t*)data.data(), data.size(), TextEnc::Utf16LE).utf8; data = transcoded; }
  else if (data.size() >= 2 && (uint8_t)data[0] == 0xFE && (uint8_t)data[1] == 0xFF) { transcoded = DecodeText((const uint8_t*)data.data(), data.size(), TextEnc::Utf16BE).utf8; data = transcoded; }
  else {
    if (data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) data.remove_prefix(3);
    if (StartsWith(data, "<?xml")) {
      size_t e = data.find("?>");
      std::string decl = LowerAscii(data.substr(0, e == std::string_view::npos ? 100 : e));
      if (decl.find("iso-8859-1") != std::string::npos || decl.find("windows-1252") != std::string::npos) {
        transcoded = DecodeText((const uint8_t*)data.data(), data.size(), TextEnc::Cp1252).utf8; data = transcoded;
      }
    }
  }
  size_t i = 0, n = data.size();
  std::vector<std::string> stack;
  bool sawRoot = false;
  std::string text;
  auto flush = [&] { if (!text.empty()) { if (sax.text && !stack.empty()) sax.text(text); text.clear(); } };
  while (i < n) {
    if (data[i] != '<') {
      size_t j = data.find('<', i);
      if (j == std::string_view::npos) j = n;
      DecodeEntitiesInto(data.substr(i, j - i), text);
      i = j;
      continue;
    }
    if (data.compare(i, 4, "<!--") == 0) { size_t e = data.find("-->", i + 4); i = e == std::string_view::npos ? n : e + 3; continue; }
    if (data.compare(i, 9, "<![CDATA[") == 0) {
      size_t e = data.find("]]>", i + 9);
      size_t stop = e == std::string_view::npos ? n : e;
      text.append(data.substr(i + 9, stop - i - 9));
      i = e == std::string_view::npos ? n : e + 3;
      continue;
    }
    if (data.compare(i, 2, "<?") == 0) { size_t e = data.find("?>", i + 2); i = e == std::string_view::npos ? n : e + 2; continue; }
    if (data.compare(i, 2, "<!") == 0) {         // DOCTYPE (with optional internal subset)
      int depth = 0; size_t j = i + 2;
      for (; j < n; ++j) { if (data[j] == '[') ++depth; else if (data[j] == ']') --depth; else if (data[j] == '>' && depth <= 0) break; }
      i = j < n ? j + 1 : n;
      continue;
    }
    if (data.compare(i, 2, "</") == 0) {
      flush();
      size_t e = data.find('>', i);
      if (e == std::string_view::npos) { i = n; break; }
      std::string name = TrimAscii(data.substr(i + 2, e - i - 2));
      i = e + 1;
      // pop to the matching open element (tolerant of mismatches)
      size_t k = stack.size();
      while (k > 0 && stack[k - 1] != name) --k;
      if (k == 0) continue;
      while (stack.size() >= k) { if (sax.end) sax.end(stack.back()); stack.pop_back(); }
      continue;
    }
    // start tag
    size_t j = i + 1;
    if (j >= n || !(IsNameChar((unsigned char)data[j]))) { text.push_back('<'); ++i; continue; }
    size_t ns = j;
    while (j < n && IsNameChar((unsigned char)data[j])) ++j;
    std::string name(data.substr(ns, j - ns));
    AttrList attrs;
    bool selfClose = false;
    for (;;) {
      while (j < n && isspace((unsigned char)data[j])) ++j;
      if (j >= n) break;
      if (data[j] == '>') { ++j; break; }
      if (data[j] == '/') { selfClose = true; ++j; continue; }
      size_t as = j;
      while (j < n && IsNameChar((unsigned char)data[j])) ++j;
      if (j == as) { ++j; continue; }
      std::string an(data.substr(as, j - as));
      while (j < n && isspace((unsigned char)data[j])) ++j;
      std::string av;
      if (j < n && data[j] == '=') {
        ++j;
        while (j < n && isspace((unsigned char)data[j])) ++j;
        if (j < n && (data[j] == '"' || data[j] == '\'')) {
          char q = data[j++]; size_t e = data.find(q, j);
          if (e == std::string_view::npos) e = n;
          DecodeEntitiesInto(data.substr(j, e - j), av);
          j = e < n ? e + 1 : n;
        } else {
          size_t vs = j;
          while (j < n && !isspace((unsigned char)data[j]) && data[j] != '>' && !(data[j] == '/' && j + 1 < n && data[j + 1] == '>')) ++j;
          DecodeEntitiesInto(data.substr(vs, j - vs), av);
        }
      }
      attrs.emplace_back(std::move(an), std::move(av));
    }
    i = j;
    flush();
    sawRoot = true;
    if (stack.size() > 512) { if (err) *err = "XML nesting too deep"; return false; }
    if (sax.start) sax.start(name, attrs);
    if (selfClose) { if (sax.end) sax.end(name); }
    else stack.push_back(std::move(name));
  }
  flush();
  while (!stack.empty()) { if (sax.end) sax.end(stack.back()); stack.pop_back(); }
  if (!sawRoot) { if (err) *err = "no XML element found"; return false; }
  return true;
}

// ---- DOM -------------------------------------------------------------------------------------------------------------
const std::string* Node::Attr(std::string_view n) const {
  for (auto& a : attrs) if (a.first == n) return &a.second;
  for (auto& a : attrs) if (LocalName(a.first) == n) return &a.second;
  return nullptr;
}
const Node* Node::Child(std::string_view local) const {
  for (auto& k : kids) if (k->Is(local)) return k.get();
  return nullptr;
}
std::vector<const Node*> Node::Children(std::string_view local) const {
  std::vector<const Node*> r;
  for (auto& k : kids) if (k->Is(local)) r.push_back(k.get());
  return r;
}
std::string Node::InnerText() const {
  if (isText) return text;
  std::string s;
  for (auto& k : kids) s += k->InnerText();
  return s;
}

std::unique_ptr<Node> ParseDom(std::string_view data, std::string* err, size_t maxNodes) {
  auto root = std::make_unique<Node>();
  std::vector<Node*> st{root.get()};
  size_t count = 0;
  bool overflow = false;
  Sax sax;
  sax.start = [&](std::string_view name, const AttrList& attrs) {
    if (++count > maxNodes) { overflow = true; return; }
    auto nd = std::make_unique<Node>();
    nd->name = std::string(name); nd->attrs = attrs;
    Node* p = nd.get();
    st.back()->kids.push_back(std::move(nd));
    st.push_back(p);
  };
  sax.end = [&](std::string_view) { if (st.size() > 1) st.pop_back(); };
  sax.text = [&](const std::string& t) {
    if (++count > maxNodes) { overflow = true; return; }
    auto nd = std::make_unique<Node>();
    nd->isText = true; nd->text = t;
    st.back()->kids.push_back(std::move(nd));
  };
  if (!Parse(data, sax, err)) return nullptr;
  if (overflow) { if (err) *err = "XML too large"; return nullptr; }
  for (auto& k : root->kids) if (!k->isText) { auto out = std::move(k); return out; }
  if (err) *err = "no root element";
  return nullptr;
}

// ---- XmlLite ---------------------------------------------------------------------------------------------------------
bool CheckWellFormed(const uint8_t* data, size_t n, std::string* why) {
  ComScope com;
  Microsoft::WRL::ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(data, (UINT)n));
  if (!stream) { if (why) *why = "stream creation failed"; return false; }
  Microsoft::WRL::ComPtr<IXmlReader> reader;
  if (FAILED(CreateXmlReader(__uuidof(IXmlReader), (void**)reader.GetAddressOf(), nullptr))) { if (why) *why = "XmlLite unavailable"; return false; }
  reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Parse);
  if (FAILED(reader->SetInput(stream.Get()))) { if (why) *why = "XmlLite input failed"; return false; }
  XmlNodeType nt;
  HRESULT hr;
  bool root = false;
  while ((hr = reader->Read(&nt)) == S_OK) if (nt == XmlNodeType_Element) root = true;
  if (FAILED(hr)) {
    UINT line = 0; reader->GetLineNumber(&line);
    if (why) *why = Printf("line %u: XML error 0x%08lX", line, (unsigned long)hr);
    return false;
  }
  if (!root) { if (why) *why = "no root element"; return false; }
  return true;
}

} // namespace filo::doc::xml
