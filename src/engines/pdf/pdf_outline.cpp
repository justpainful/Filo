#include "engines/pdf/pdf_outline.h"
#include "engines/pdf/pdf_rewrite.h"
#include "fpdfview.h"
#include "fpdf_doc.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace filo::pdf {

// =====================================================================================================================
// reading (PDFium)

static std::wstring Utf16Bytes(const std::vector<uint8_t>& b) {
  std::wstring s((const wchar_t*)b.data(), b.size() / 2);
  while (!s.empty() && s.back() == L'\0') s.pop_back();
  return s;
}

static void ReadLevel(FPDF_DOCUMENT doc, FPDF_BOOKMARK first, int pageOffset, int depth, int& budget, std::vector<OutlineNode>& out) {
  std::set<FPDF_BOOKMARK> seen;
  for (FPDF_BOOKMARK bm = first; bm && budget > 0; bm = FPDFBookmark_GetNextSibling(doc, bm)) {
    if (!seen.insert(bm).second) break;       // cycle
    --budget;
    OutlineNode n;
    unsigned long len = FPDFBookmark_GetTitle(bm, nullptr, 0);
    if (len > 2) {
      std::vector<uint8_t> b(len);
      FPDFBookmark_GetTitle(bm, b.data(), len);
      n.title = Utf16Bytes(b);
    }
    FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
    if (!dest) {
      FPDF_ACTION act = FPDFBookmark_GetAction(bm);
      if (act && FPDFAction_GetType(act) == PDFACTION_GOTO) dest = FPDFAction_GetDest(doc, act);
    }
    if (dest) {
      int pg = FPDFDest_GetDestPageIndex(doc, dest);
      if (pg >= 0) {
        n.page = pg + pageOffset;
        FPDF_BOOL hx = 0, hy = 0, hz = 0; FS_FLOAT x = 0, y = 0, z = 0;
        if (FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z) && hy) { n.hasTop = true; n.top = y; }
      }
    }
    if (depth < 12) {
      FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bm);
      if (child) ReadLevel(doc, child, pageOffset, depth + 1, budget, n.children);
    }
    out.push_back(std::move(n));
  }
}

std::vector<OutlineNode> ReadOutline(const PdfDocument& doc, int pageOffset) {
  PdfLock lock;
  std::vector<OutlineNode> out;
  if (!doc.Handle()) return out;
  FPDF_DOCUMENT d = (FPDF_DOCUMENT)doc.Handle();
  int budget = 20000;
  ReadLevel(d, FPDFBookmark_GetFirstChild(d, nullptr), pageOffset, 0, budget, out);
  return out;
}

int CountOutlineNodes(const std::vector<OutlineNode>& n) {
  int c = 0;
  for (auto& x : n) c += 1 + CountOutlineNodes(x.children);
  return c;
}

// =====================================================================================================================
// minimal PDF object model (enough for Catalog / Pages / Page / Info dictionaries)

namespace {

struct PVal;
using PDict = std::vector<std::pair<std::string, PVal>>;
struct PVal {
  enum T { Null, Bool, Int, Real, Name, Str, Arr, Dict, Ref } t = Null;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;               // name (without slash) or raw string bytes
  std::vector<PVal> arr;
  PDict dict;
  int refNum = 0, refGen = 0;
  const PVal* Find(const char* key) const {
    for (auto& kv : dict) if (kv.first == key) return &kv.second;
    return nullptr;
  }
  void Set(const std::string& key, PVal v) {
    for (auto& kv : dict) if (kv.first == key) { kv.second = std::move(v); return; }
    dict.emplace_back(key, std::move(v));
  }
  void Remove(const char* key) { dict.erase(std::remove_if(dict.begin(), dict.end(), [&](auto& kv) { return kv.first == key; }), dict.end()); }
};

PVal MakeRef(int n) { PVal v; v.t = PVal::Ref; v.refNum = n; return v; }
PVal MakeName(const std::string& s) { PVal v; v.t = PVal::Name; v.s = s; return v; }
PVal MakeInt(int64_t i) { PVal v; v.t = PVal::Int; v.i = i; return v; }
PVal MakeReal(double d) { PVal v; v.t = PVal::Real; v.d = d; return v; }
PVal MakeStr(const std::string& s) { PVal v; v.t = PVal::Str; v.s = s; return v; }
PVal MakeNull() { return PVal(); }

struct Reader {
  const std::string& b;
  size_t p = 0;
  explicit Reader(const std::string& buf, size_t pos = 0) : b(buf), p(pos) {}
  static bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0'; }
  static bool IsDelim(char c) { return std::strchr("()<>[]{}/%", c) != nullptr; }
  void SkipWs() {
    while (p < b.size()) {
      if (IsWs(b[p])) ++p;
      else if (b[p] == '%') { while (p < b.size() && b[p] != '\n' && b[p] != '\r') ++p; }
      else break;
    }
  }
  bool Eof() { SkipWs(); return p >= b.size(); }
  std::string Token() {
    SkipWs();
    size_t s = p;
    while (p < b.size() && !IsWs(b[p]) && !IsDelim(b[p])) ++p;
    return b.substr(s, p - s);
  }
  bool Parse(PVal& out, int depth = 0) {
    if (depth > 40) return false;
    SkipWs();
    if (p >= b.size()) return false;
    char c = b[p];
    if (c == '<' && p + 1 < b.size() && b[p + 1] == '<') {
      p += 2; out = PVal(); out.t = PVal::Dict;
      for (;;) {
        SkipWs();
        if (p + 1 < b.size() && b[p] == '>' && b[p + 1] == '>') { p += 2; return true; }
        if (p >= b.size() || b[p] != '/') return false;
        PVal key; if (!Parse(key, depth + 1)) return false;
        PVal val; if (!Parse(val, depth + 1)) return false;
        out.dict.emplace_back(key.s, std::move(val));
      }
    }
    if (c == '[') {
      ++p; out = PVal(); out.t = PVal::Arr;
      for (;;) {
        SkipWs();
        if (p < b.size() && b[p] == ']') { ++p; return true; }
        PVal v; if (!Parse(v, depth + 1)) return false;
        out.arr.push_back(std::move(v));
      }
    }
    if (c == '/') {
      ++p; size_t s = p;
      while (p < b.size() && !IsWs(b[p]) && !IsDelim(b[p])) ++p;
      std::string raw = b.substr(s, p - s), dec;
      for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' && i + 2 < raw.size() + 0 && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
          dec += (char)std::stoi(raw.substr(i + 1, 2), nullptr, 16); i += 2;
        } else dec += raw[i];
      }
      out = MakeName(dec);
      return true;
    }
    if (c == '(') {
      ++p; int nest = 1; std::string s;
      while (p < b.size() && nest > 0) {
        char ch = b[p++];
        if (ch == '\\' && p < b.size()) {
          char e = b[p++];
          switch (e) {
            case 'n': s += '\n'; break; case 'r': s += '\r'; break; case 't': s += '\t'; break; case 'b': s += '\b'; break; case 'f': s += '\f'; break;
            case '\r': if (p < b.size() && b[p] == '\n') ++p; break;
            case '\n': break;
            default:
              if (e >= '0' && e <= '7') { int v = e - '0'; for (int k = 0; k < 2 && p < b.size() && b[p] >= '0' && b[p] <= '7'; ++k) v = v * 8 + (b[p++] - '0'); s += (char)v; }
              else s += e;
          }
        } else if (ch == '(') { ++nest; s += ch; }
        else if (ch == ')') { if (--nest > 0) s += ch; }
        else s += ch;
      }
      out = MakeStr(s);
      return true;
    }
    if (c == '<') {
      ++p; std::string hex;
      while (p < b.size() && b[p] != '>') { if (isxdigit((unsigned char)b[p])) hex += b[p]; ++p; }
      ++p;
      if (hex.size() % 2) hex += '0';
      std::string s;
      for (size_t i = 0; i + 1 < hex.size(); i += 2) s += (char)std::stoi(hex.substr(i, 2), nullptr, 16);
      out = MakeStr(s);
      return true;
    }
    std::string t = Token();
    if (t.empty()) return false;
    if (t == "true") { out = PVal(); out.t = PVal::Bool; out.b = true; return true; }
    if (t == "false") { out = PVal(); out.t = PVal::Bool; out.b = false; return true; }
    if (t == "null") { out = MakeNull(); return true; }
    char* end = nullptr;
    bool isInt = t.find_first_of(".eE") == std::string::npos;
    if (isInt) {
      long long v = std::strtoll(t.c_str(), &end, 10);
      if (!end || *end) return false;
      // indirect reference "n g R"?
      size_t save = p;
      std::string t2 = Token();
      if (!t2.empty() && std::all_of(t2.begin(), t2.end(), [](char ch) { return isdigit((unsigned char)ch); })) {
        std::string t3 = Token();
        if (t3 == "R") { out = MakeRef((int)v); out.refGen = std::atoi(t2.c_str()); return true; }
      }
      p = save;
      out = MakeInt(v);
      return true;
    }
    double dv = std::strtod(t.c_str(), &end);
    if (!end || *end) return false;
    out = MakeReal(dv);
    return true;
  }
};

void AppendName(std::string& o, const std::string& n) {
  o += '/';
  for (unsigned char c : n) {
    if (c < 33 || c > 126 || std::strchr("()<>[]{}/%#", c)) { char b[8]; snprintf(b, sizeof b, "#%02X", c); o += b; }
    else o += (char)c;
  }
}
void Serialize(const PVal& v, std::string& o) {
  switch (v.t) {
    case PVal::Null: o += "null"; break;
    case PVal::Bool: o += v.b ? "true" : "false"; break;
    case PVal::Int: o += std::to_string(v.i); break;
    case PVal::Real: { char b[64]; snprintf(b, sizeof b, "%.4f", v.d); o += b; break; }
    case PVal::Name: AppendName(o, v.s); break;
    case PVal::Str: {
      o += '<';
      for (unsigned char c : v.s) { char b[4]; snprintf(b, sizeof b, "%02X", c); o += b; }
      o += '>';
      break;
    }
    case PVal::Arr: o += '['; for (size_t i = 0; i < v.arr.size(); ++i) { if (i) o += ' '; Serialize(v.arr[i], o); } o += ']'; break;
    case PVal::Dict:
      o += "<<";
      for (auto& kv : v.dict) { AppendName(o, kv.first); o += ' '; Serialize(kv.second, o); o += ' '; }
      o += ">>";
      break;
    case PVal::Ref: o += std::to_string(v.refNum) + " " + std::to_string(v.refGen) + " R"; break;
  }
}

PVal TextString(const std::wstring& w) {
  bool ascii = true;
  for (wchar_t c : w) if (c < 32 || c > 126) ascii = false;
  std::string s;
  if (ascii) s = Narrow(w);
  else {
    s += static_cast<char>(0xFE); s += static_cast<char>(0xFF);
    for (wchar_t c : w) { s += (char)((c >> 8) & 255); s += (char)(c & 255); }
  }
  return MakeStr(s);
}

struct XrefInfo {
  std::map<int, size_t> offsets;
  PVal trailer;
};

bool ReadXref(const std::string& data, size_t off, XrefInfo& xi, std::wstring* why) {
  std::set<size_t> visited;
  bool first = true;
  while (true) {
    if (off >= data.size() || !visited.insert(off).second) { *why = L"cross-reference table not found"; return false; }
    Reader r(data, off);
    std::string t = r.Token();
    if (t != "xref") { *why = L"cross-reference streams are not supported"; return false; }
    for (;;) {
      size_t save = r.p;
      std::string a = r.Token();
      if (a == "trailer") break;
      std::string c = r.Token();
      if (a.empty() || c.empty() || !isdigit((unsigned char)a[0]) || !isdigit((unsigned char)c[0])) { r.p = save; *why = L"damaged cross-reference table"; return false; }
      int start = std::atoi(a.c_str()), count = std::atoi(c.c_str());
      if (count < 0 || count > 10000000) { *why = L"damaged cross-reference table"; return false; }
      for (int i = 0; i < count; ++i) {
        std::string o = r.Token(), g = r.Token(), k = r.Token();
        if (k != "n" && k != "f") { *why = L"damaged cross-reference table"; return false; }
        if (k == "n") { int num = start + i; if (!xi.offsets.count(num)) xi.offsets[num] = (size_t)std::strtoull(o.c_str(), nullptr, 10); }
      }
    }
    PVal tr;
    if (!r.Parse(tr) || tr.t != PVal::Dict) { *why = L"damaged trailer"; return false; }
    if (tr.Find("Encrypt")) { *why = L"encrypted files are not supported"; return false; }
    if (tr.Find("XRefStm")) { *why = L"hybrid cross-reference files are not supported"; return false; }
    if (first) { xi.trailer = tr; first = false; }
    const PVal* prev = tr.Find("Prev");
    if (!prev || (prev->t != PVal::Int && prev->t != PVal::Real)) return true;
    off = (size_t)(prev->t == PVal::Int ? prev->i : (int64_t)prev->d);
  }
}

bool ParseObjectEx(const std::string& data, size_t off, PVal& out, size_t* after) {
  if (off >= data.size()) return false;
  Reader r(data, off);
  std::string n = r.Token(), g = r.Token(), kw = r.Token();
  if (kw != "obj") return false;
  if (!r.Parse(out)) return false;
  if (after) *after = r.p;
  return true;
}
bool ParseObjectAt(const std::string& data, size_t off, PVal& out) { return ParseObjectEx(data, off, out, nullptr); }

} // namespace

bool AppendOutlinesAndInfo(const fs::path& pdf, const std::vector<OutlineNode>& roots, const std::map<std::string, std::wstring>& info,
                           std::wstring* err) {
  auto fail = [&](const std::wstring& m) { if (err) *err = m; return false; };
  std::string data;
  {
    std::ifstream f(pdf, std::ios::binary | std::ios::ate);
    if (!f) return fail(L"file could not be read");
    auto sz = (size_t)f.tellg();
    if (sz > (size_t)1500 << 20) return fail(L"file too large for outline update");
    data.resize(sz);
    f.seekg(0);
    f.read(data.data(), (std::streamsize)sz);
  }
  size_t sx = data.rfind("startxref");
  if (sx == std::string::npos) return fail(L"startxref not found");
  Reader sr(data, sx + 9);
  std::string offTok = sr.Token();
  size_t xrefOff = (size_t)std::strtoull(offTok.c_str(), nullptr, 10);
  XrefInfo xi;
  std::wstring why;
  if (!ReadXref(data, xrefOff, xi, &why)) return fail(why);
  const PVal* sizeV = xi.trailer.Find("Size");
  const PVal* rootV = xi.trailer.Find("Root");
  if (!sizeV || !rootV || rootV->t != PVal::Ref) return fail(L"trailer incomplete");
  int size = (int)sizeV->i;

  auto objAt = [&](int num, PVal& v) {
    auto it = xi.offsets.find(num);
    return it != xi.offsets.end() && ParseObjectAt(data, it->second, v);
  };
  PVal catalog;
  if (!objAt(rootV->refNum, catalog) || catalog.t != PVal::Dict) return fail(L"catalog not found");

  // page objects in order
  std::vector<int> pageObjs;
  if (!roots.empty()) {
    const PVal* pagesRef = catalog.Find("Pages");
    if (!pagesRef || pagesRef->t != PVal::Ref) return fail(L"page tree not found");
    std::set<int> seen;
    std::function<void(int, int)> walk = [&](int num, int depth) {
      if (depth > 32 || !seen.insert(num).second) return;
      PVal node;
      if (!objAt(num, node) || node.t != PVal::Dict) return;
      const PVal* kids = node.Find("Kids");
      if (kids && kids->t == PVal::Arr) { for (auto& k : kids->arr) if (k.t == PVal::Ref) walk(k.refNum, depth + 1); }
      else pageObjs.push_back(num);
    };
    walk(pagesRef->refNum, 0);
    if (pageObjs.empty()) return fail(L"no pages found");
  }

  std::vector<std::pair<int, std::string>> newObjs;     // objnum -> body (without "N 0 obj"/"endobj")
  int nextNum = size;

  // outline objects
  int outlinesRoot = 0;
  if (!roots.empty()) {
    struct Flat { const OutlineNode* n; int num, parent, prev = 0, next = 0, first = 0, last = 0, count = 0; };
    std::vector<Flat> flat;
    outlinesRoot = nextNum++;
    std::function<int(const std::vector<OutlineNode>&, int, int&, int&)> build = [&](const std::vector<OutlineNode>& nodes, int parentNum, int& firstOut, int& lastOut) -> int {
      int prevNum = 0, total = 0;
      for (auto& n : nodes) {
        size_t idx = flat.size();
        flat.push_back({&n, nextNum++, parentNum});
        int num = flat[idx].num;
        if (prevNum) { flat[idx].prev = prevNum; for (auto& f : flat) if (f.num == prevNum) f.next = num; }
        else firstOut = num;
        prevNum = num; lastOut = num;
        int cf = 0, cl = 0;
        int desc = build(n.children, num, cf, cl);
        flat[idx].first = cf; flat[idx].last = cl; flat[idx].count = desc;
        total += 1 + desc;
      }
      return total;
    };
    int first = 0, last = 0;
    int totalCount = build(roots, outlinesRoot, first, last);
    {
      PVal o; o.t = PVal::Dict;
      o.Set("Type", MakeName("Outlines")); o.Set("First", MakeRef(first)); o.Set("Last", MakeRef(last)); o.Set("Count", MakeInt(totalCount));
      std::string b; Serialize(o, b); newObjs.emplace_back(outlinesRoot, b);
    }
    for (auto& f : flat) {
      PVal o; o.t = PVal::Dict;
      o.Set("Title", TextString(f.n->title));
      o.Set("Parent", MakeRef(f.parent));
      if (f.prev) o.Set("Prev", MakeRef(f.prev));
      if (f.next) o.Set("Next", MakeRef(f.next));
      if (f.first) { o.Set("First", MakeRef(f.first)); o.Set("Last", MakeRef(f.last)); o.Set("Count", MakeInt(f.count)); }
      if (f.n->page >= 0 && f.n->page < (int)pageObjs.size()) {
        PVal dest; dest.t = PVal::Arr;
        dest.arr.push_back(MakeRef(pageObjs[f.n->page]));
        if (f.n->hasTop) { dest.arr.push_back(MakeName("XYZ")); dest.arr.push_back(MakeNull()); dest.arr.push_back(MakeReal(f.n->top)); dest.arr.push_back(MakeNull()); }
        else dest.arr.push_back(MakeName("Fit"));
        o.Set("Dest", dest);
      }
      std::string b; Serialize(o, b); newObjs.emplace_back(f.num, b);
    }
    catalog.Remove("Outlines");
    catalog.Set("Outlines", MakeRef(outlinesRoot));
    if (!catalog.Find("PageMode")) catalog.Set("PageMode", MakeName("UseOutlines"));
    std::string b; Serialize(catalog, b); newObjs.emplace_back(rootV->refNum, b);
  }

  // info dictionary
  int infoNum = 0;
  const PVal* infoRef = xi.trailer.Find("Info");
  if (!info.empty()) {
    PVal o; o.t = PVal::Dict;
    if (infoRef && infoRef->t == PVal::Ref) { PVal old; if (objAt(infoRef->refNum, old) && old.t == PVal::Dict) o = old; }
    for (auto& [k, v] : info) { o.Remove(k.c_str()); if (!v.empty()) o.Set(k, TextString(v)); }
    SYSTEMTIME st; GetSystemTime(&st);
    char date[64]; snprintf(date, sizeof date, "D:%04d%02d%02d%02d%02d%02d+00'00'", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    o.Set("ModDate", MakeStr(date));
    infoNum = nextNum++;
    std::string b; Serialize(o, b); newObjs.emplace_back(infoNum, b);
  }
  if (newObjs.empty()) return true;

  // assemble the update
  std::string add;
  if (data.empty() || (data.back() != '\n' && data.back() != '\r')) add += "\r\n";
  std::map<int, size_t> newOffsets;
  for (auto& [num, body] : newObjs) {
    newOffsets[num] = data.size() + add.size();
    add += std::to_string(num) + " 0 obj\r\n" + body + "\r\nendobj\r\n";
  }
  size_t xrefStart = data.size() + add.size();
  add += "xref\r\n";
  std::vector<int> nums;
  for (auto& kv : newOffsets) nums.push_back(kv.first);
  for (size_t i = 0; i < nums.size();) {
    size_t j = i;
    while (j + 1 < nums.size() && nums[j + 1] == nums[j] + 1) ++j;
    add += std::to_string(nums[i]) + " " + std::to_string(j - i + 1) + "\r\n";
    for (size_t k = i; k <= j; ++k) { char b[40]; snprintf(b, sizeof b, "%010llu 00000 n\r\n", (unsigned long long)newOffsets[nums[k]]); add += b; }
    i = j + 1;
  }
  PVal tr; tr.t = PVal::Dict;
  tr.Set("Size", MakeInt(nextNum));
  tr.Set("Root", *rootV);
  if (infoNum) tr.Set("Info", MakeRef(infoNum)); else if (infoRef) tr.Set("Info", *infoRef);
  tr.Set("Prev", MakeInt((int64_t)xrefOff));
  if (const PVal* id = xi.trailer.Find("ID")) tr.Set("ID", *id);
  std::string trs; Serialize(tr, trs);
  add += "trailer\r\n" + trs + "\r\nstartxref\r\n" + std::to_string(xrefStart) + "\r\n%%EOF\r\n";

  std::ofstream f(pdf, std::ios::binary | std::ios::app);
  if (!f) return fail(L"file could not be updated");
  f.write(add.data(), (std::streamsize)add.size());
  f.flush();
  if (!f) {
    f.close();
    std::error_code ec; fs::resize_file(pdf, data.size(), ec);
    return fail(L"file could not be updated");
  }
  return true;
}

// =====================================================================================================================
// garbage collection

namespace {
void CollectRefs(const PVal& v, std::vector<int>& out, int depth = 0) {
  if (depth > 60) return;
  switch (v.t) {
    case PVal::Ref: out.push_back(v.refNum); break;
    case PVal::Arr: for (auto& e : v.arr) CollectRefs(e, out, depth + 1); break;
    case PVal::Dict: for (auto& kv : v.dict) CollectRefs(kv.second, out, depth + 1); break;
    default: break;
  }
}
}

bool RewritePdf(const fs::path& pdf, const RewriteOptions& opt, RewriteStats* stats, std::wstring* err) {
  auto fail = [&](const std::wstring& m) { if (err) *err = m; return false; };
  RewriteStats local;
  RewriteStats& st = stats ? *stats : local;
  st = RewriteStats();
  std::string data;
  {
    std::ifstream f(fs::path(LongPath(pdf)), std::ios::binary | std::ios::ate);
    if (!f) return fail(L"file could not be read");
    auto sz = (size_t)f.tellg();
    if (sz > (size_t)1500 << 20) return fail(L"file too large");
    data.resize(sz);
    f.seekg(0);
    f.read(data.data(), (std::streamsize)sz);
  }
  st.before = data.size();
  size_t sx = data.rfind("startxref");
  if (sx == std::string::npos) return fail(L"startxref not found");
  Reader sr(data, sx + 9);
  size_t xrefOff = (size_t)std::strtoull(sr.Token().c_str(), nullptr, 10);
  XrefInfo xi;
  std::wstring why;
  if (!ReadXref(data, xrefOff, xi, &why)) return fail(why);
  const PVal* rootV = xi.trailer.Find("Root");
  const PVal* sizeV = xi.trailer.Find("Size");
  if (!rootV || rootV->t != PVal::Ref || !sizeV) return fail(L"trailer incomplete");
  if (xi.trailer.Find("Prev")) return fail(L"incremental files are not supported");
  int size = (int)sizeV->i;

  // object spans: from its offset to the next object (or to the xref table)
  std::vector<size_t> offs;
  for (auto& kv : xi.offsets) offs.push_back(kv.second);
  std::sort(offs.begin(), offs.end());
  auto spanEnd = [&](size_t off) {
    auto it = std::upper_bound(offs.begin(), offs.end(), off);
    return it == offs.end() ? xrefOff : std::min(*it, xrefOff);
  };

  std::set<int> reach;
  std::vector<int> stack{rootV->refNum};
  if (const PVal* inf = xi.trailer.Find("Info")) if (inf->t == PVal::Ref) stack.push_back(inf->refNum);
  while (!stack.empty()) {
    int n = stack.back(); stack.pop_back();
    if (!reach.insert(n).second) continue;
    auto it = xi.offsets.find(n);
    if (it == xi.offsets.end()) { reach.erase(n); continue; }       // dangling reference: nothing to keep
    PVal v;
    if (!ParseObjectAt(data, it->second, v)) return fail(L"an object could not be parsed");
    std::vector<int> refs;
    CollectRefs(v, refs);
    for (int r : refs) if (!reach.count(r)) stack.push_back(r);
  }
  st.removedObjects = (int)xi.offsets.size() - (int)reach.size();

  auto intOf = [&](const PVal& v, int64_t& out) {
    if (v.t == PVal::Int) { out = v.i; return true; }
    if (v.t == PVal::Ref) { PVal o; auto it = xi.offsets.find(v.refNum); if (it != xi.offsets.end() && ParseObjectAt(data, it->second, o) && o.t == PVal::Int) { out = o.i; return true; } }
    return false;
  };
  auto resolve = [&](const PVal& v, PVal& out) {
    if (v.t != PVal::Ref) { out = v; return true; }
    auto it = xi.offsets.find(v.refNum);
    return it != xi.offsets.end() && ParseObjectAt(data, it->second, out);
  };
  // number of colour components of a /ColorSpace value (8 bit device / ICC / Cal spaces only), 0 = unsupported
  auto componentsOf = [&](const PVal& csIn) -> int {
    PVal cs;
    if (!resolve(csIn, cs)) return 0;
    if (cs.t == PVal::Name) return (cs.s == "DeviceGray" || cs.s == "G") ? 1 : (cs.s == "DeviceRGB" || cs.s == "RGB") ? 3 : 0;
    if (cs.t == PVal::Arr && !cs.arr.empty() && cs.arr[0].t == PVal::Name) {
      const std::string& k = cs.arr[0].s;
      if (k == "CalGray") return 1;
      if (k == "CalRGB") return 3;
      if (k == "ICCBased" && cs.arr.size() > 1) {
        PVal icc; int64_t n = 0;
        if (cs.arr[1].t == PVal::Ref && resolve(cs.arr[1], icc) && icc.t == PVal::Dict && icc.Find("N") && intOf(*icc.Find("N"), n) && (n == 1 || n == 3)) return (int)n;
      }
    }
    return 0;
  };

  size_t firstObj = offs.empty() ? 0 : offs.front();
  std::string out = data.substr(0, firstObj);
  std::map<int, size_t> newOff;
  for (int n : reach) {
    if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) { st.cancelled = true; return fail(L"Cancelled"); }
    size_t off = xi.offsets[n], end = spanEnd(off);
    if (end <= off || end > data.size()) return fail(L"object layout not understood");
    newOff[n] = out.size();
    bool replaced = false;
    if (opt.imageHook) {
      PVal d; size_t after = 0;
      if (ParseObjectEx(data, off, d, &after) && d.t == PVal::Dict) {
        const PVal* sub = d.Find("Subtype");
        if (sub && sub->t == PVal::Name && sub->s == "Image" && !d.Find("SMask") && !d.Find("Mask") && !d.Find("Decode") && !d.Find("SMaskInData") && !d.Find("Matte")) {
          const PVal* im = d.Find("ImageMask");
          bool stencil = im && im->t == PVal::Bool && im->b;
          int64_t w = 0, h = 0, bpc = 0, len = 0;
          const PVal* cs = d.Find("ColorSpace");
          const PVal* bp = d.Find("BitsPerComponent");
          const PVal* lenV = d.Find("Length");
          int comps = cs ? componentsOf(*cs) : 0;
          if (!stencil && comps && bp && intOf(*bp, bpc) && bpc == 8 && d.Find("Width") && intOf(*d.Find("Width"), w) && d.Find("Height") && intOf(*d.Find("Height"), h) &&
              lenV && intOf(*lenV, len) && w > 0 && h > 0 && len > 0) {
            std::string filter;
            bool okFilter = true;
            const PVal* f = d.Find("Filter");
            if (f) {
              if (f->t == PVal::Name) filter = f->s;
              else if (f->t == PVal::Arr && f->arr.size() == 1 && f->arr[0].t == PVal::Name) filter = f->arr[0].s;
              else okFilter = false;
              if (filter != "FlateDecode" && filter != "DCTDecode") okFilter = false;
            }
            RewriteImage ri;
            ri.objNum = n; ri.width = (int)w; ri.height = (int)h; ri.comps = comps; ri.filter = filter;
            ri.predColors = comps; ri.predColumns = (int)w;
            if (okFilter) {
              const PVal* dp = d.Find("DecodeParms");
              PVal dpv;
              if (dp) {
                if (dp->t == PVal::Dict) dpv = *dp;
                else if (dp->t == PVal::Arr && dp->arr.size() == 1 && dp->arr[0].t == PVal::Dict) dpv = dp->arr[0];
                else if (dp->t == PVal::Arr && dp->arr.size() == 1 && dp->arr[0].t == PVal::Null) {}
                else okFilter = false;
              }
              if (okFilter && dpv.t == PVal::Dict) {
                int64_t v = 0;
                if (dpv.Find("Predictor") && intOf(*dpv.Find("Predictor"), v)) ri.predictor = (int)v;
                if (dpv.Find("Colors") && intOf(*dpv.Find("Colors"), v)) ri.predColors = (int)v;
                if (dpv.Find("Columns") && intOf(*dpv.Find("Columns"), v)) ri.predColumns = (int)v;
                if (dpv.Find("BitsPerComponent") && intOf(*dpv.Find("BitsPerComponent"), v) && v != 8) okFilter = false;
              }
            }
            size_t p = after;
            while (p < end && Reader::IsWs(data[p])) ++p;
            if (okFilter && data.compare(p, 6, "stream") == 0) {
              p += 6;
              if (p < end && data[p] == '\r') ++p;
              if (p < end && data[p] == '\n') ++p;
              if (p + (size_t)len <= end) {
                ri.data = std::string_view(data.data() + p, (size_t)len);
                RewriteReplacement rep;
                if (opt.imageHook(ri, rep) && !rep.jpeg.empty() && rep.jpeg.size() < (size_t)len) {
                  PVal nd = d;
                  nd.Set("Width", MakeInt(rep.width)); nd.Set("Height", MakeInt(rep.height));
                  nd.Set("Filter", MakeName("DCTDecode")); nd.Remove("DecodeParms");
                  nd.Set("Length", MakeInt((int64_t)rep.jpeg.size()));
                  std::string body = std::to_string(n) + " 0 obj\r\n";
                  Serialize(nd, body);
                  body += "\r\nstream\r\n";
                  body.append(rep.jpeg);
                  body += "\r\nendstream\r\nendobj\r\n";
                  out += body;
                  replaced = true;
                  ++st.replaced;
                }
              }
            }
          }
        }
      }
    }
    if (!replaced) {
      out.append(data, off, end - off);
      if (out.back() != '\n' && out.back() != '\r') out += "\r\n";
    }
  }
  size_t xrefStart = out.size();
  std::vector<int> freeList;
  for (int n = 1; n < size; ++n) if (!newOff.count(n)) freeList.push_back(n);
  out += "xref\r\n0 " + std::to_string(size) + "\r\n";
  char line[40];
  snprintf(line, sizeof line, "%010d 65535 f\r\n", freeList.empty() ? 0 : freeList.front());
  out += line;
  size_t fi = 0;
  for (int n = 1; n < size; ++n) {
    auto it = newOff.find(n);
    if (it != newOff.end()) snprintf(line, sizeof line, "%010llu 00000 n\r\n", (unsigned long long)it->second);
    else { ++fi; snprintf(line, sizeof line, "%010d 00001 f\r\n", fi < freeList.size() ? freeList[fi] : 0); }
    out += line;
  }
  PVal tr; tr.t = PVal::Dict;
  tr.Set("Size", MakeInt(size));
  tr.Set("Root", *rootV);
  if (const PVal* inf = xi.trailer.Find("Info")) tr.Set("Info", *inf);
  if (const PVal* id = xi.trailer.Find("ID")) tr.Set("ID", *id);
  std::string trs; Serialize(tr, trs);
  out += "trailer\r\n" + trs + "\r\nstartxref\r\n" + std::to_string(xrefStart) + "\r\n%%EOF\r\n";
  st.after = out.size();
  if (out.size() >= data.size()) return true;                      // nothing gained: leave the file as it is

  fs::path tmp = pdf; tmp += L".gc";
  {
    std::ofstream f(fs::path(LongPath(tmp)), std::ios::binary | std::ios::trunc);
    if (!f) return fail(L"temporary file could not be created");
    f.write(out.data(), (std::streamsize)out.size());
    f.flush();
    if (!f) { f.close(); std::error_code ec; fs::remove(tmp, ec); return fail(L"temporary file could not be written"); }
  }
  if (!MoveFileExW(LongPath(tmp).c_str(), LongPath(pdf).c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::error_code ec; fs::remove(tmp, ec);
    return fail(L"the file could not be replaced");
  }
  return true;
}

bool GarbageCollectPdf(const fs::path& pdf, uint64_t* bytesBefore, uint64_t* bytesAfter, std::wstring* err) {
  RewriteStats st;
  bool ok = RewritePdf(pdf, RewriteOptions(), &st, err);
  if (bytesBefore) *bytesBefore = st.before;
  if (bytesAfter) *bytesAfter = st.after;
  return ok;
}

} // namespace filo::pdf
