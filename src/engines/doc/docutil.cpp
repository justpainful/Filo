#include "engines/doc/docutil.h"
#include <objbase.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace filo::doc {

// ---- UTF-8 -----------------------------------------------------------------------------------------------------------
uint32_t DecodeUtf8(std::string_view s, size_t& i) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  size_t n = s.size();
  uint8_t c = p[i];
  if (c < 0x80) { ++i; return c; }
  int len = 0; uint32_t cp = 0, minv = 0;
  if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; minv = 0x80; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; minv = 0x800; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; minv = 0x10000; }
  else { ++i; return 0xFFFD; }
  if (i + len > n) { ++i; return 0xFFFD; }
  for (int k = 1; k < len; ++k) {
    if ((p[i + k] & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (p[i + k] & 0x3F);
  }
  if (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return 0xFFFD; }
  i += len;
  return cp;
}

void AppendUtf8(std::string& o, uint32_t cp) {
  if (cp < 0x80) o.push_back((char)cp);
  else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
  else { o.push_back((char)(0xF0 | (cp >> 18))); o.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
}

bool IsValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    size_t before = i;
    uint32_t cp = DecodeUtf8(s, i);
    if (cp == 0xFFFD && !(i - before == 3 && (uint8_t)s[before] == 0xEF && (uint8_t)s[before + 1] == 0xBF && (uint8_t)s[before + 2] == 0xBD)) return false;
  }
  return true;
}

static bool XmlLegal(uint32_t c) {
  if (c == 9 || c == 10 || c == 13) return true;
  if (c < 0x20) return false;
  if (c >= 0xD800 && c <= 0xDFFF) return false;
  if (c == 0xFFFE || c == 0xFFFF) return false;
  return true;
}

std::string SanitizeText(std::string_view s) {
  std::string o; o.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if ((uint8_t)s[i] < 0x80) { uint8_t c = (uint8_t)s[i++]; if (XmlLegal(c)) o.push_back((char)c); continue; }
    uint32_t cp = DecodeUtf8(s, i);
    if (XmlLegal(cp)) AppendUtf8(o, cp);
  }
  return o;
}

std::u32string ToCodepoints(std::string_view s) {
  std::u32string r; r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) r.push_back(DecodeUtf8(s, i));
  return r;
}
std::string FromCodepoints(const std::u32string& s) {
  std::string o;
  for (char32_t c : s) AppendUtf8(o, c);
  return o;
}
size_t Utf8Length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
  return n;
}
std::wstring W(std::string_view u) { return Widen(u); }
std::string U(std::wstring_view w) { return Narrow(w); }

// ---- direction -------------------------------------------------------------------------------------------------------
bool IsRtlCodepoint(uint32_t c) {
  return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
         (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF) || c == 0x200F || c == 0x202B || c == 0x202E || c == 0x061C;
}
bool IsStrongLtrCodepoint(uint32_t c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  if (IsRtlCodepoint(c)) return false;
  if (c >= 0x00C0 && c <= 0x02AF && c != 0xD7 && c != 0xF7) return true;      // Latin supplements/extended
  if (c >= 0x0370 && c <= 0x058F) return true;                                // Greek, Cyrillic, Armenian
  if (c >= 0x0900 && c <= 0x1FFF) return true;                                // Indic, Thai, ... Georgian, Hangul Jamo...
  if (c >= 0x3040 && c <= 0xA4CF) return true;                                // kana, CJK ...
  if (c >= 0xAC00 && c <= 0xD7A3) return true;
  if (c >= 0xF900 && c <= 0xFAFF) return true;
  return false;
}
bool ContainsRtl(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) { if ((uint8_t)s[i] < 0x80) { ++i; continue; } if (IsRtlCodepoint(DecodeUtf8(s, i))) return true; }
  return false;
}
bool DetectRtl(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    uint32_t c = DecodeUtf8(s, i);
    if (IsRtlCodepoint(c)) return true;
    if (IsStrongLtrCodepoint(c)) return false;
  }
  return false;
}
std::vector<std::pair<std::string, bool>> SplitByDirection(std::string_view s) {
  std::vector<std::pair<std::string, bool>> r;
  bool cur = false; bool have = false;
  size_t i = 0;
  while (i < s.size()) {
    size_t st = i;
    uint32_t c = DecodeUtf8(s, i);
    int cls = IsRtlCodepoint(c) ? 1 : (IsStrongLtrCodepoint(c) ? 0 : -1);
    if (!have) { cur = cls == 1; have = true; r.push_back({std::string(), cur}); }
    else if (cls >= 0 && (cls == 1) != cur) { cur = cls == 1; r.push_back({std::string(), cur}); }
    r.back().first.append(s.substr(st, i - st));
  }
  return r;
}

// ---- escaping --------------------------------------------------------------------------------------------------------
std::string XmlEscape(std::string_view in, bool attr) {
  std::string s = SanitizeText(in);
  std::string o; o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += attr ? "&quot;" : "\""; break;
      case '\n': if (attr) o += "&#10;"; else o.push_back(c); break;
      case '\r': if (attr) o += "&#13;"; else o.push_back(c); break;
      case '\t': if (attr) o += "&#9;"; else o.push_back(c); break;
      default: o.push_back(c);
    }
  }
  return o;
}
std::string HtmlEscape(std::string_view s) { return XmlEscape(s, false); }

std::string XmlUnescapeBasic(std::string_view s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      size_t e = s.find(';', i);
      if (e != std::string_view::npos && e - i < 12) {
        std::string_view n = s.substr(i + 1, e - i - 1);
        if (n == "amp") { o.push_back('&'); i = e; continue; }
        if (n == "lt") { o.push_back('<'); i = e; continue; }
        if (n == "gt") { o.push_back('>'); i = e; continue; }
        if (n == "quot") { o.push_back('"'); i = e; continue; }
        if (n == "apos") { o.push_back('\''); i = e; continue; }
      }
    }
    o.push_back(s[i]);
  }
  return o;
}

// ---- strings ---------------------------------------------------------------------------------------------------------
std::string LowerAscii(std::string_view s) { std::string r(s); for (auto& c : r) if (c >= 'A' && c <= 'Z') c += 32; return r; }
std::string UpperAscii(std::string_view s) { std::string r(s); for (auto& c : r) if (c >= 'a' && c <= 'z') c -= 32; return r; }
std::string TrimAscii(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}
bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool EqualsNoCase(std::string_view a, std::string_view b) { return a.size() == b.size() && LowerAscii(a) == LowerAscii(b); }
std::vector<std::string> SplitLines(std::string_view s) {
  std::vector<std::string> r;
  size_t i = 0, st = 0;
  while (i < s.size()) {
    if (s[i] == '\n') { r.emplace_back(s.substr(st, i - st)); st = ++i; }
    else if (s[i] == '\r') { r.emplace_back(s.substr(st, i - st)); ++i; if (i < s.size() && s[i] == '\n') ++i; st = i; }
    else ++i;
  }
  if (st < s.size()) r.emplace_back(s.substr(st));
  return r;
}
std::vector<std::string> SplitStr(std::string_view s, char sep) {
  std::vector<std::string> r; size_t st = 0;
  for (size_t i = 0; i <= s.size(); ++i) if (i == s.size() || s[i] == sep) { r.emplace_back(s.substr(st, i - st)); st = i + 1; }
  return r;
}
std::string Printf(const char* fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return {};
  if ((size_t)n < sizeof buf) return std::string(buf, n);
  std::string big(n + 1, '\0');
  va_start(ap, fmt); vsnprintf(big.data(), big.size(), fmt, ap); va_end(ap);
  big.resize(n);
  return big;
}
std::string Hex2(unsigned v) { char b[8]; snprintf(b, sizeof b, "%02X", v & 0xFF); return b; }

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Base64Encode(const uint8_t* d, size_t n) {
  std::string o; o.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16;
    if (i + 1 < n) v |= d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    o.push_back(kB64[(v >> 18) & 63]); o.push_back(kB64[(v >> 12) & 63]);
    o.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
    o.push_back(i + 2 < n ? kB64[v & 63] : '=');
  }
  return o;
}
Bytes Base64Decode(std::string_view s) {
  static int8_t tab[256]; static bool init = false;
  if (!init) { memset(tab, -1, sizeof tab); for (int i = 0; i < 64; ++i) tab[(uint8_t)kB64[i]] = (int8_t)i; tab[(uint8_t)'-'] = 62; tab[(uint8_t)'_'] = 63; init = true; }
  Bytes o; uint32_t acc = 0; int bits = 0;
  for (unsigned char c : s) {
    if (c == '=') break;
    int8_t v = tab[c];
    if (v < 0) continue;       // whitespace / line breaks
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(acc >> bits)); acc &= (1u << bits) - 1; }
  }
  return o;
}

// ---- files -----------------------------------------------------------------------------------------------------------
bool ReadFileBytes(const fs::path& p, Bytes& out, uint64_t maxBytes, std::wstring* err) {
  out.clear();
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = L"The file could not be opened."; return false; }
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  if ((uint64_t)sz.QuadPart > maxBytes) { CloseHandle(h); if (err) *err = L"The file is too large to convert."; return false; }
  out.resize((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    DWORD want = (DWORD)std::min<size_t>(out.size() - done, 64u << 20);
    if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) break;
    done += got;
  }
  CloseHandle(h);
  out.resize(done);
  return true;
}

bool WriteFileBytes(const fs::path& p, const void* data, size_t n, std::wstring* err) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = L"The output file could not be created."; return false; }
  const uint8_t* d = static_cast<const uint8_t*>(data);
  size_t done = 0; bool ok = true;
  while (done < n) {
    DWORD w = 0; DWORD want = (DWORD)std::min<size_t>(n - done, 64u << 20);
    if (!WriteFile(h, d + done, want, &w, nullptr) || w == 0) { ok = false; break; }
    done += w;
  }
  CloseHandle(h);
  if (!ok && err) *err = L"The output file could not be written (disk full?).";
  return ok;
}

std::string SafeName(std::string_view s, size_t maxLen) {
  std::string o;
  size_t i = 0;
  while (i < s.size() && o.size() < maxLen) {
    size_t st = i;
    uint32_t c = DecodeUtf8(s, i);
    if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') { o.push_back('_'); continue; }
    o.append(s.substr(st, i - st));
  }
  while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
  if (o.empty()) o = "file";
  return o;
}

// ---- images ----------------------------------------------------------------------------------------------------------
bool ProbeImage(const uint8_t* d, size_t n, ImageInfo& info) {
  if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
    info.mime = "image/png"; info.w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]; info.h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    return info.w > 0 && info.h > 0;
  }
  if (n >= 10 && memcmp(d, "GIF8", 4) == 0) { info.mime = "image/gif"; info.w = d[6] | (d[7] << 8); info.h = d[8] | (d[9] << 8); return info.w > 0 && info.h > 0; }
  if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
    info.mime = "image/bmp"; info.w = (int)(d[18] | (d[19] << 8) | (d[20] << 16) | (d[21] << 24)); int h = (int)(d[22] | (d[23] << 8) | (d[24] << 16) | (d[25] << 24)); info.h = h < 0 ? -h : h;
    return info.w > 0 && info.h > 0;
  }
  if (n >= 30 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
    info.mime = "image/webp";
    if (memcmp(d + 12, "VP8X", 4) == 0) { info.w = 1 + (d[24] | (d[25] << 8) | (d[26] << 16)); info.h = 1 + (d[27] | (d[28] << 8) | (d[29] << 16)); }
    else if (memcmp(d + 12, "VP8 ", 4) == 0) { info.w = (d[26] | (d[27] << 8)) & 0x3FFF; info.h = (d[28] | (d[29] << 8)) & 0x3FFF; }
    else if (memcmp(d + 12, "VP8L", 4) == 0) { uint32_t b = d[21] | (d[22] << 8) | (d[23] << 16) | ((uint32_t)d[24] << 24); info.w = (b & 0x3FFF) + 1; info.h = ((b >> 14) & 0x3FFF) + 1; }
    return info.w > 0 && info.h > 0;
  }
  if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
    size_t i = 2;
    while (i + 9 < n) {
      if (d[i] != 0xFF) { ++i; continue; }
      uint8_t m = d[i + 1];
      if (m == 0xFF) { ++i; continue; }
      if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
      size_t len = (d[i + 2] << 8) | d[i + 3];
      if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
        info.mime = "image/jpeg"; info.h = (d[i + 5] << 8) | d[i + 6]; info.w = (d[i + 7] << 8) | d[i + 8];
        return info.w > 0 && info.h > 0;
      }
      if (len < 2) return false;
      i += 2 + len;
    }
  }
  return false;
}
std::string ImageExtForMime(std::string_view m) {
  if (m == "image/png") return "png";
  if (m == "image/jpeg" || m == "image/jpg") return "jpeg";
  if (m == "image/gif") return "gif";
  if (m == "image/bmp") return "bmp";
  if (m == "image/webp") return "webp";
  if (m == "image/svg+xml") return "svg";
  return "bin";
}

ComScope::ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
ComScope::~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }

} // namespace filo::doc
