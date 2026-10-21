#include "engines/doc/text_io.h"
#include <algorithm>
#include <cstring>

namespace filo::doc {

TextEnc ParseEncodingName(std::string_view name) {
  std::string n = LowerAscii(name);
  n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == '-' || c == '_' || c == ' '; }), n.end());
  if (n == "utf8") return TextEnc::Utf8;
  if (n == "utf8bom" || n == "utf8withbom") return TextEnc::Utf8Bom;
  if (n == "utf16le" || n == "utf16") return TextEnc::Utf16LE;
  if (n == "utf16lebom" || n == "unicode") return TextEnc::Utf16LEBom;
  if (n == "utf16be") return TextEnc::Utf16BE;
  if (n == "utf16bebom") return TextEnc::Utf16BEBom;
  if (n == "utf32le") return TextEnc::Utf32LE;
  if (n == "utf32be") return TextEnc::Utf32BE;
  if (n == "windows1252" || n == "cp1252" || n == "ansi") return TextEnc::Cp1252;
  if (n == "windows1256" || n == "cp1256" || n == "arabic") return TextEnc::Cp1256;
  if (n == "windows1251" || n == "cp1251") return TextEnc::Cp1251;
  if (n == "windows1250" || n == "cp1250") return TextEnc::Cp1250;
  if (n == "windows1255" || n == "cp1255") return TextEnc::Cp1255;
  if (n == "latin1" || n == "iso88591") return TextEnc::Latin1;
  return TextEnc::Auto;
}

const char* EncodingName(TextEnc e) {
  switch (e) {
    case TextEnc::Utf8: return "utf-8"; case TextEnc::Utf8Bom: return "utf-8 (BOM)";
    case TextEnc::Utf16LE: case TextEnc::Utf16LEBom: return "utf-16le"; case TextEnc::Utf16BE: case TextEnc::Utf16BEBom: return "utf-16be";
    case TextEnc::Utf32LE: return "utf-32le"; case TextEnc::Utf32BE: return "utf-32be";
    case TextEnc::Cp1252: return "windows-1252"; case TextEnc::Cp1256: return "windows-1256"; case TextEnc::Cp1251: return "windows-1251";
    case TextEnc::Cp1250: return "windows-1250"; case TextEnc::Cp1255: return "windows-1255"; case TextEnc::Latin1: return "iso-8859-1";
    default: return "auto";
  }
}

static UINT CodePageOf(TextEnc e) {
  switch (e) {
    case TextEnc::Cp1252: return 1252; case TextEnc::Cp1256: return 1256; case TextEnc::Cp1251: return 1251;
    case TextEnc::Cp1250: return 1250; case TextEnc::Cp1255: return 1255; case TextEnc::Latin1: return 28591;
    default: return 0;
  }
}

static std::string FromWide(const std::wstring& w) { return Narrow(w); }

static std::string DecodeCodePage(const uint8_t* d, size_t n, UINT cp, bool& errors) {
  if (n == 0) return {};
  DWORD flags = MB_ERR_INVALID_CHARS;
  int len = MultiByteToWideChar(cp, flags, (const char*)d, (int)n, nullptr, 0);
  if (len <= 0) { flags = 0; errors = true; len = MultiByteToWideChar(cp, 0, (const char*)d, (int)n, nullptr, 0); }
  std::wstring w(len, L'\0');
  MultiByteToWideChar(cp, flags, (const char*)d, (int)n, w.data(), len);
  return FromWide(w);
}

static std::string DecodeUtf16(const uint8_t* d, size_t n, bool be) {
  std::wstring w; w.reserve(n / 2);
  for (size_t i = 0; i + 1 < n; i += 2) w.push_back((wchar_t)(be ? ((d[i] << 8) | d[i + 1]) : (d[i] | (d[i + 1] << 8))));
  // lone surrogates -> U+FFFD
  for (size_t i = 0; i < w.size(); ++i) {
    wchar_t c = w[i];
    if (c >= 0xD800 && c <= 0xDBFF) { if (i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) ++i; else w[i] = 0xFFFD; }
    else if (c >= 0xDC00 && c <= 0xDFFF) w[i] = 0xFFFD;
  }
  return FromWide(w);
}

static std::string DecodeUtf32(const uint8_t* d, size_t n, bool be) {
  std::string o;
  for (size_t i = 0; i + 3 < n; i += 4) {
    uint32_t c = be ? ((uint32_t)d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3]) : ((uint32_t)d[i + 3] << 24 | d[i + 2] << 16 | d[i + 1] << 8 | d[i]);
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    AppendUtf8(o, c);
  }
  return o;
}

// Heuristic: Arabic text saved as windows-1256 is long runs of high bytes in 0xC1..0xED with almost nothing above 0xF3.
static bool LooksLikeCp1256(const uint8_t* d, size_t n) {
  size_t high = 0, arabic = 0, top = 0, inRun = 0, runLen = 0;
  size_t lim = std::min<size_t>(n, 65536);
  for (size_t i = 0; i < lim; ++i) {
    if (d[i] >= 0x80) {
      ++high; ++runLen;
      if (d[i] >= 0xC1 && d[i] <= 0xED && d[i] != 0xD7) ++arabic;
      if (d[i] >= 0xF4) ++top;
      if (runLen == 3) inRun += 3; else if (runLen > 3) ++inRun;
    } else runLen = 0;
  }
  if (high < 6) return false;
  return arabic * 100 >= high * 80 && top * 100 <= high * 8 && inRun * 100 >= high * 60;
}

DecodedText DecodeText(const uint8_t* d, size_t n, TextEnc forced) {
  DecodedText r;
  size_t off = 0;
  TextEnc enc = forced;
  // BOMs always win when they agree with / replace "auto"
  if (n >= 4 && d[0] == 0xFF && d[1] == 0xFE && d[2] == 0 && d[3] == 0) { enc = TextEnc::Utf32LE; off = 4; r.hadBom = true; }
  else if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0xFE && d[3] == 0xFF) { enc = TextEnc::Utf32BE; off = 4; r.hadBom = true; }
  else if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { if (forced == TextEnc::Auto || forced == TextEnc::Utf8 || forced == TextEnc::Utf8Bom) { enc = TextEnc::Utf8Bom; off = 3; r.hadBom = true; } }
  else if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { if (forced == TextEnc::Auto || forced == TextEnc::Utf16LE || forced == TextEnc::Utf16LEBom) { enc = TextEnc::Utf16LEBom; off = 2; r.hadBom = true; } }
  else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { if (forced == TextEnc::Auto || forced == TextEnc::Utf16BE || forced == TextEnc::Utf16BEBom) { enc = TextEnc::Utf16BEBom; off = 2; r.hadBom = true; } }

  if (enc == TextEnc::Auto) {
    size_t zerosEven = 0, zerosOdd = 0, lim = std::min<size_t>(n, 65536);
    for (size_t i = 0; i < lim; ++i) if (d[i] == 0) ((i & 1) ? zerosOdd : zerosEven)++;
    {
      size_t groups = lim / 4, le = 0, be = 0;
      for (size_t g = 0; g < groups; ++g) {
        const uint8_t* q = d + g * 4;
        if (q[2] == 0 && q[3] == 0 && (q[0] | q[1]) != 0) ++le;
        if (q[0] == 0 && q[1] == 0 && (q[2] | q[3]) != 0) ++be;
      }
      if (groups >= 2 && le * 10 >= groups * 9) enc = TextEnc::Utf32LE;
      else if (groups >= 2 && be * 10 >= groups * 9) enc = TextEnc::Utf32BE;
    }
    if (enc != TextEnc::Auto) {}
    else if (lim >= 4 && zerosOdd * 10 > lim / 2 * 3 && zerosEven * 20 < lim / 2) enc = TextEnc::Utf16LE;
    else if (lim >= 4 && zerosEven * 10 > lim / 2 * 3 && zerosOdd * 20 < lim / 2) enc = TextEnc::Utf16BE;
    else if (IsValidUtf8(std::string_view((const char*)d, n))) enc = TextEnc::Utf8;
    else if (LooksLikeCp1256(d, n)) enc = TextEnc::Cp1256;
    else {
      UINT acp = GetACP();
      switch (acp) {
        case 1256: enc = TextEnc::Cp1256; break; case 1251: enc = TextEnc::Cp1251; break; case 1250: enc = TextEnc::Cp1250; break;
        case 1255: enc = TextEnc::Cp1255; break; default: enc = TextEnc::Cp1252; break;
      }
    }
  }
  r.detected = enc;
  const uint8_t* p = d + off; size_t m = n - off;
  switch (enc) {
    case TextEnc::Utf8: case TextEnc::Utf8Bom: {
      if (IsValidUtf8(std::string_view((const char*)p, m))) r.utf8.assign((const char*)p, m);
      else { r.hadErrors = true; r.utf8 = SanitizeText(std::string_view((const char*)p, m)); }   // replaces invalid bytes with U+FFFD
      break;
    }
    case TextEnc::Utf16LE: case TextEnc::Utf16LEBom: r.utf8 = DecodeUtf16(p, m, false); break;
    case TextEnc::Utf16BE: case TextEnc::Utf16BEBom: r.utf8 = DecodeUtf16(p, m, true); break;
    case TextEnc::Utf32LE: r.utf8 = DecodeUtf32(p, m, false); break;
    case TextEnc::Utf32BE: r.utf8 = DecodeUtf32(p, m, true); break;
    default: r.utf8 = DecodeCodePage(p, m, CodePageOf(enc), r.hadErrors); break;
  }
  return r;
}

Bytes EncodeText(std::string_view utf8, TextEnc enc) {
  Bytes o;
  auto put16 = [&](uint16_t c, bool be) { if (be) { o.push_back((uint8_t)(c >> 8)); o.push_back((uint8_t)c); } else { o.push_back((uint8_t)c); o.push_back((uint8_t)(c >> 8)); } };
  switch (enc) {
    case TextEnc::Auto: case TextEnc::Utf8: case TextEnc::Utf8Bom: {
      if (enc == TextEnc::Utf8Bom) { o.push_back(0xEF); o.push_back(0xBB); o.push_back(0xBF); }
      o.insert(o.end(), utf8.begin(), utf8.end());
      break;
    }
    case TextEnc::Utf16LE: case TextEnc::Utf16LEBom: case TextEnc::Utf16BE: case TextEnc::Utf16BEBom: {
      bool be = enc == TextEnc::Utf16BE || enc == TextEnc::Utf16BEBom;
      if (enc == TextEnc::Utf16LEBom || enc == TextEnc::Utf16BEBom) put16(0xFEFF, be);
      std::wstring w = Widen(utf8);
      for (wchar_t c : w) put16((uint16_t)c, be);
      break;
    }
    case TextEnc::Utf32LE: case TextEnc::Utf32BE: {
      bool be = enc == TextEnc::Utf32BE;
      size_t i = 0;
      while (i < utf8.size()) {
        uint32_t c = DecodeUtf8(utf8, i);
        if (be) { o.push_back(c >> 24); o.push_back(c >> 16); o.push_back(c >> 8); o.push_back(c); }
        else { o.push_back(c); o.push_back(c >> 8); o.push_back(c >> 16); o.push_back(c >> 24); }
      }
      break;
    }
    default: {
      std::wstring w = Widen(utf8);
      if (w.empty()) break;
      UINT cp = CodePageOf(enc);
      BOOL usedDefault = FALSE;
      int len = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, "?", &usedDefault);
      o.resize(len);
      WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), (char*)o.data(), len, "?", &usedDefault);
      break;
    }
  }
  return o;
}

std::string NormalizeNewlines(std::string_view s) {
  std::string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') { o.push_back('\n'); if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
    else o.push_back(s[i]);
  }
  return o;
}
std::string ToCrLf(std::string_view s) {
  std::string o; o.reserve(s.size() + s.size() / 16);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') o += "\r\n";
    else if (s[i] == '\r') { o += "\r\n"; if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
    else o.push_back(s[i]);
  }
  return o;
}

} // namespace filo::doc
