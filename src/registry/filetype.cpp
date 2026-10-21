#include "registry/filetype.h"
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace filo {

const wchar_t* GroupName(Group g) {
  switch (g) {
    case Group::Image: return L"image"; case Group::Video: return L"video"; case Group::Audio: return L"audio"; case Group::Pdf: return L"pdf";
    case Group::Document: return L"document"; case Group::Presentation: return L"presentation"; case Group::Spreadsheet: return L"spreadsheet";
    case Group::Subtitle: return L"subtitle"; case Group::Archive: return L"archive"; case Group::Folder: return L"folder"; case Group::Text: return L"text";
    default: return L"unknown";
  }
}

std::wstring CanonicalExt(const std::wstring& e0) {
  std::wstring e = ToLower(e0);
  static const std::unordered_map<std::wstring, std::wstring> m = {
    {L"jpeg", L"jpg"}, {L"jpe", L"jpg"}, {L"jfif", L"jpg"}, {L"tif", L"tiff"}, {L"heif", L"heic"}, {L"htm", L"html"}, {L"markdown", L"md"},
    {L"mpeg", L"mpg"}, {L"aif", L"aiff"}, {L"aifc", L"aiff"}, {L"tgz", L"gz"}, {L"mkv", L"mkv"}, {L"text", L"txt"}, {L"oga", L"ogg"}};
  auto it = m.find(e);
  return it == m.end() ? e : it->second;
}

Group GroupForExt(const std::wstring& e0) {
  static const std::unordered_map<std::wstring, Group> m = {
    {L"jpg", Group::Image}, {L"png", Group::Image}, {L"gif", Group::Image}, {L"bmp", Group::Image}, {L"tiff", Group::Image}, {L"webp", Group::Image},
    {L"heic", Group::Image}, {L"avif", Group::Image}, {L"svg", Group::Image}, {L"ico", Group::Image},
    {L"mp4", Group::Video}, {L"mov", Group::Video}, {L"mkv", Group::Video}, {L"webm", Group::Video}, {L"avi", Group::Video}, {L"wmv", Group::Video},
    {L"m4v", Group::Video}, {L"mpg", Group::Video}, {L"flv", Group::Video}, {L"3gp", Group::Video}, {L"ts", Group::Video},
    {L"mp3", Group::Audio}, {L"m4a", Group::Audio}, {L"wav", Group::Audio}, {L"flac", Group::Audio}, {L"ogg", Group::Audio}, {L"opus", Group::Audio},
    {L"aiff", Group::Audio}, {L"wma", Group::Audio}, {L"aac", Group::Audio},
    {L"pdf", Group::Pdf},
    {L"docx", Group::Document}, {L"doc", Group::Document}, {L"pages", Group::Document}, {L"md", Group::Document}, {L"txt", Group::Document},
    {L"epub", Group::Document}, {L"rtf", Group::Document}, {L"rtfd", Group::Document}, {L"textclipping", Group::Document}, {L"html", Group::Document},
    {L"odt", Group::Document},
    {L"pptx", Group::Presentation}, {L"ppt", Group::Presentation}, {L"odp", Group::Presentation},
    {L"xlsx", Group::Spreadsheet}, {L"xls", Group::Spreadsheet}, {L"ods", Group::Spreadsheet}, {L"csv", Group::Spreadsheet}, {L"tsv", Group::Spreadsheet},
    {L"srt", Group::Subtitle}, {L"vtt", Group::Subtitle},
    {L"zip", Group::Archive}, {L"tar", Group::Archive}, {L"gz", Group::Archive}, {L"rar", Group::Archive}, {L"7z", Group::Archive},
  };
  auto it = m.find(CanonicalExt(e0));
  return it == m.end() ? Group::Unknown : it->second;
}

static bool Has(const uint8_t* d, size_t n, size_t off, const char* s) {
  size_t l = strlen(s);
  return off + l <= n && memcmp(d + off, s, l) == 0;
}
static bool FindStr(const uint8_t* d, size_t n, const char* s, size_t from = 0, size_t limit = (size_t)-1) {
  size_t l = strlen(s);
  size_t end = std::min(n, limit);
  if (end < l) return false;
  for (size_t i = from; i + l <= end; ++i) if (d[i] == (uint8_t)s[0] && memcmp(d + i, s, l) == 0) return true;
  return false;
}

// Inspect the first local file headers of a ZIP container to tell OOXML / ODF / EPUB / Pages apart.
static std::wstring ClassifyZip(const uint8_t* d, size_t n) {
  size_t pos = 0; int seen = 0;
  bool word = false, xl = false, ppt = false, epub = false, pages = false;
  std::string mimetype;
  while (pos + 30 <= n && seen < 64 && d[pos] == 'P' && d[pos + 1] == 'K' && d[pos + 2] == 3 && d[pos + 3] == 4) {
    uint16_t method = d[pos + 8] | (d[pos + 9] << 8);
    uint32_t csize = d[pos + 18] | (d[pos + 19] << 8) | (d[pos + 20] << 16) | ((uint32_t)d[pos + 21] << 24);
    uint16_t nlen = d[pos + 26] | (d[pos + 27] << 8), xlen = d[pos + 28] | (d[pos + 29] << 8);
    if (pos + 30 + nlen > n) break;
    std::string name((const char*)d + pos + 30, nlen);
    if (name.rfind("word/", 0) == 0) word = true;
    else if (name.rfind("xl/", 0) == 0) xl = true;
    else if (name.rfind("ppt/", 0) == 0) ppt = true;
    else if (name == "META-INF/container.xml" || name == "OEBPS/content.opf") epub = true;
    else if (name == "Index/Document.iwa" || name == "index.xml" || name.rfind("Index/", 0) == 0) pages = true;
    if (name == "mimetype" && method == 0 && csize < 128 && pos + 30 + nlen + xlen + csize <= n) mimetype.assign((const char*)d + pos + 30 + nlen + xlen, csize);
    if (!(d[pos + 6] & 8) && csize != 0xFFFFFFFFu) pos += 30 + nlen + xlen + csize;   // sizes known: hop to the next header
    else break;                                                                       // data descriptor: stop (rely on what we saw)
    ++seen;
  }
  if (mimetype.find("epub") != std::string::npos) return L"epub";
  if (mimetype.find("opendocument.text") != std::string::npos) return L"odt";
  if (mimetype.find("opendocument.spreadsheet") != std::string::npos) return L"ods";
  if (mimetype.find("opendocument.presentation") != std::string::npos) return L"odp";
  if (word) return L"docx"; if (xl) return L"xlsx"; if (ppt) return L"pptx"; if (epub) return L"epub"; if (pages) return L"pages";
  return L"zip";
}

static bool LooksLikeText(const uint8_t* d, size_t n) {
  if (n == 0) return false;
  size_t bad = 0, lim = std::min<size_t>(n, 8192);
  for (size_t i = 0; i < lim; ++i) { uint8_t c = d[i]; if (c == 0) return false; if (c < 9 || (c > 13 && c < 32)) ++bad; }
  return bad * 100 < lim * 2;
}

FileKind ClassifyBytes(const uint8_t* d, size_t n, const std::wstring& declared) {
  FileKind k;
  k.declaredExt = CanonicalExt(declared);
  k.detectedByContent = true;
  auto set = [&](const wchar_t* f) { k.format = f; k.group = GroupForExt(f); };

  if (n >= 8 && Has(d, n, 0, "\x89PNG\r\n\x1a\n")) set(L"png");
  else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) set(L"jpg");
  else if (Has(d, n, 0, "GIF87a") || Has(d, n, 0, "GIF89a")) set(L"gif");
  else if (Has(d, n, 0, "BM") && n > 14 && (d[6] | d[7] | d[8] | d[9]) == 0) set(L"bmp");
  else if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) || (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42))) set(L"tiff");
  else if (n >= 12 && Has(d, n, 0, "RIFF") && Has(d, n, 8, "WEBP")) set(L"webp");
  else if (n >= 12 && Has(d, n, 0, "RIFF") && Has(d, n, 8, "WAVE")) set(L"wav");
  else if (n >= 12 && Has(d, n, 0, "RIFF") && Has(d, n, 8, "AVI ")) set(L"avi");
  else if (n >= 12 && Has(d, n, 0, "FORM") && (Has(d, n, 8, "AIFF") || Has(d, n, 8, "AIFC"))) set(L"aiff");
  else if (Has(d, n, 0, "fLaC")) set(L"flac");
  else if (Has(d, n, 0, "OggS")) set(FindStr(d, n, "OpusHead", 0, 512) ? L"opus" : L"ogg");
  else if (Has(d, n, 0, "%PDF-")) set(L"pdf");
  else if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) set(FindStr(d, n, "webm", 0, 64) ? L"webm" : L"mkv");
  else if (n >= 16 && d[0] == 0x30 && d[1] == 0x26 && d[2] == 0xB2 && d[3] == 0x75) set(k.declaredExt == L"wma" ? L"wma" : (k.declaredExt == L"wmv" ? L"wmv" : (FindStr(d, n, "V\0i\0d\0e\0o", 0, 8192) ? L"wmv" : L"wma")));
  else if (n >= 12 && Has(d, n, 4, "ftyp")) {
    char brand[5] = {(char)d[8], (char)d[9], (char)d[10], (char)d[11], 0};
    std::string b = brand;
    bool heic = b == "heic" || b == "heix" || b == "hevc" || b == "hevx" || b == "heim" || b == "heis" || b == "mif1" || b == "msf1";
    bool avif = b == "avif" || b == "avis";
    if (b == "mif1" || b == "msf1") {   // generic: look at compatible brands
      if (FindStr(d, n, "avif", 8, 64)) avif = true, heic = false;
      else if (FindStr(d, n, "heic", 8, 64)) heic = true;
    }
    if (avif) set(L"avif");
    else if (heic) set(L"heic");
    else if (b == "qt  ") set(L"mov");
    else if (b == "M4A " || b == "M4B ") set(L"m4a");
    else if (b == "3gp4" || b == "3gp5" || b == "3ge6") set(L"3gp");
    else set(k.declaredExt == L"m4a" ? L"m4a" : (k.declaredExt == L"mov" ? L"mov" : L"mp4"));
  }
  else if (n >= 3 && Has(d, n, 0, "ID3")) set(L"mp3");
  else if (n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) set(L"aac");                                   // ADTS
  else if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && ((d[1] >> 1) & 3) != 0 && ((d[1] >> 3) & 3) != 1) set(L"mp3");  // MPEG audio frame sync
  else if (n >= 4 && d[0] == 'P' && d[1] == 'K' && (d[2] == 3 || d[2] == 5)) { k.format = ClassifyZip(d, n); k.group = GroupForExt(k.format); }
  else if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B) set(L"gz");
  else if (Has(d, n, 0, "Rar!\x1a\x07")) set(L"rar");
  else if (Has(d, n, 0, "7z\xBC\xAF\x27\x1C")) set(L"7z");
  else if (n > 262 && Has(d, n, 257, "ustar")) set(L"tar");
  else if (Has(d, n, 0, "{\\rtf")) set(L"rtf");
  else if (n >= 8 && d[0] == 0xD0 && d[1] == 0xCF && d[2] == 0x11 && d[3] == 0xE0) {      // OLE2: doc/xls/ppt - extension decides
    std::wstring e = k.declaredExt; k.detectedByContent = false;
    if (e == L"xls") set(L"xls"); else if (e == L"ppt") set(L"ppt"); else set(L"doc");
  }
  else if (n >= 4 && Has(d, n, 0, "RIFF")) { k.format.clear(); }
  else {
    // text-ish formats: decided by content where possible, then by extension
    k.detectedByContent = false;
    std::wstring e = k.declaredExt;
    bool text = LooksLikeText(d, n);
    if (text || n == 0) {
      size_t off = (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
      if (Has(d, n, off, "WEBVTT")) { set(L"vtt"); k.detectedByContent = true; }
      else if (FindStr(d, n, "<svg", off, 2048)) { set(L"svg"); k.detectedByContent = true; }
      else if (e == L"srt" || e == L"vtt" || e == L"csv" || e == L"tsv" || e == L"md" || e == L"txt" || e == L"html" || e == L"textclipping") set(e.c_str());
      else if (e == L"svg") set(L"svg");
      else if (e.empty() || GroupForExt(e) == Group::Unknown) { k.format = L"txt"; k.group = Group::Text; }
      else { k.format = e; k.group = GroupForExt(e); }
    } else if (e == L"textclipping") { set(L"textclipping"); }
  }

  if (k.format.empty()) { k.group = Group::Unknown; }
  if (k.group != Group::Unknown && !k.declaredExt.empty() && k.detectedByContent) {
    // tolerate aliases that share a container: mp4/m4v/mov/3gp, mp3/aac, jpg aliases, etc.
    auto same = [&](const std::wstring& a, const std::wstring& b) {
      if (a == b) return true;
      auto grp = [](const std::wstring& x) { return (x == L"mp4" || x == L"m4v" || x == L"mov" || x == L"3gp") ? 1 : (x == L"m4a" || x == L"mp4") ? 2 : (x == L"ogg" || x == L"opus") ? 3 : (x == L"zip" || x == L"gz" || x == L"tar") ? 0 : 0; };
      return grp(a) != 0 && grp(a) == grp(b);
    };
    k.extensionMismatch = !same(k.format, k.declaredExt);
    // a plain ".zip" holding OOXML, or Office/EPUB packages with their proper extension, are not mismatches
    if (k.format == L"zip" && GroupForExt(k.declaredExt) == Group::Archive) k.extensionMismatch = false;
  }
  return k;
}

FileKind DetectFile(const fs::path& p) {
  FileKind k;
  k.path = p;
  k.declaredExt = CanonicalExt(ExtOf(p));
  DWORD attrs = GetFileAttributesW(LongPath(p).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return k;
  k.exists = true;
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    k.isDir = true; k.readable = true;
    k.format = k.declaredExt == L"rtfd" ? L"rtfd" : L"folder";
    k.group = k.declaredExt == L"rtfd" ? Group::Document : Group::Folder;
    return k;
  }
  k.cloudPlaceholder = (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
  WIN32_FILE_ATTRIBUTE_DATA ad{};
  if (GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &ad)) k.size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
  if (k.cloudPlaceholder) {            // never trigger a hydration just to look at the header
    k.readable = true; k.detectedByContent = false;
    k.format = k.declaredExt; k.group = GroupForExt(k.format);
    return k;
  }
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) { k.format = k.declaredExt; k.group = GroupForExt(k.format); return k; }
  std::vector<uint8_t> buf(65536);
  DWORD got = 0;
  ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
  CloseHandle(h);
  k.readable = true;
  FileKind c = ClassifyBytes(buf.data(), got, k.declaredExt);
  c.path = p; c.exists = true; c.readable = true; c.size = k.size; c.cloudPlaceholder = false;
  return c;
}

} // namespace filo
