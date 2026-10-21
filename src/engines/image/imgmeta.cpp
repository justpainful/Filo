#include "engines/image/imgmeta.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace filo::img {

// ---------------------------------------------------------------------------------------------------------------------
// byte helpers
// ---------------------------------------------------------------------------------------------------------------------
static inline uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t Le32(const uint8_t* p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void PutBe32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static void PutBe16(Bytes& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)(v & 255)); }
static void PutLe32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v & 255)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 24)); }
static void Put(Bytes& b, const void* p, size_t n) { const uint8_t* s = (const uint8_t*)p; b.insert(b.end(), s, s + n); }
static void PutStr(Bytes& b, const char* s) { Put(b, s, strlen(s)); }

uint32_t Crc32(const uint8_t* d, size_t n, uint32_t seed) { return (uint32_t)mz_crc32(seed, d, n); }

// ---------------------------------------------------------------------------------------------------------------------
// EXIF (TIFF) walking
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct Tiff {
  const uint8_t* d; size_t n; bool le;
  bool Valid() const { return n >= 8 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) || (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42)); }
  uint16_t R16(size_t o) const { return le ? Le16(d + o) : Be16(d + o); }
  uint32_t R32(size_t o) const { return le ? Le32(d + o) : Be32(d + o); }
};
static Tiff MakeTiff(const uint8_t* d, size_t n) { Tiff t{d, n, n >= 2 && d[0] == 'I'}; return t; }

// Finds an entry in the IFD at `ifdOff`; returns the offset of the 12-byte entry or 0.
static size_t FindEntry(const Tiff& t, size_t ifdOff, uint16_t tag, size_t* nextIfdPtrOffset = nullptr) {
  if (ifdOff < 8 || ifdOff + 2 > t.n) return 0;
  uint16_t cnt = t.R16(ifdOff);
  size_t entries = ifdOff + 2;
  if (entries + (size_t)cnt * 12 + 4 > t.n) return 0;
  if (nextIfdPtrOffset) *nextIfdPtrOffset = entries + (size_t)cnt * 12;
  for (uint16_t i = 0; i < cnt; ++i) {
    size_t e = entries + (size_t)i * 12;
    if (t.R16(e) == tag) return e;
  }
  return 0;
}
static void W16(const Tiff& t, uint8_t* p, uint16_t v) { if (t.le) { p[0] = v & 255; p[1] = v >> 8; } else { p[0] = v >> 8; p[1] = v & 255; } }
static void W32(const Tiff& t, uint8_t* p, uint32_t v) {
  if (t.le) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); } else { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
}
} // namespace

int ExifOrientation(const Bytes& exif) {
  Tiff t = MakeTiff(exif.data(), exif.size());
  if (!t.Valid()) return 0;
  size_t e = FindEntry(t, t.R32(4), 0x0112);
  if (!e) return 0;
  if (t.R16(e + 2) != 3 || t.R32(e + 4) < 1) return 0;
  int v = t.R16(e + 8);
  return (v >= 1 && v <= 8) ? v : 0;
}

void ExifPatch(Bytes& exif, int newOrientation, int pixelW, int pixelH) {
  Tiff t = MakeTiff(exif.data(), exif.size());
  if (!t.Valid()) return;
  size_t ifd0 = t.R32(4), nextPtr = 0;
  size_t e = FindEntry(t, ifd0, 0x0112, &nextPtr);
  if (e && newOrientation > 0 && t.R16(e + 2) == 3) W16(t, exif.data() + e + 8, (uint16_t)newOrientation);
  if (!nextPtr) FindEntry(t, ifd0, 0xFFFF, &nextPtr);
  if (nextPtr && nextPtr + 4 <= exif.size()) W32(t, exif.data() + nextPtr, 0);     // unlink IFD1 (thumbnail)
  if (pixelW > 0 && pixelH > 0) {
    size_t ep = FindEntry(t, ifd0, 0x8769);
    if (ep && t.R16(ep + 2) == 4) {
      size_t exifIfd = t.R32(ep + 8);
      auto setDim = [&](uint16_t tag, int v) {
        size_t de = FindEntry(t, exifIfd, tag);
        if (!de) return;
        uint16_t type = t.R16(de + 2);
        if (type == 3 && v <= 65535) W16(t, exif.data() + de + 8, (uint16_t)v);
        else if (type == 4) W32(t, exif.data() + de + 8, (uint32_t)v);
      };
      setDim(0xA002, pixelW);
      setDim(0xA003, pixelH);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// ICC
// ---------------------------------------------------------------------------------------------------------------------
std::string IccColorSpace(const Bytes& icc) {
  if (icc.size() < 128 || memcmp(icc.data() + 36, "acsp", 4) != 0) return "";
  return std::string((const char*)icc.data() + 16, 4);
}

static bool IccFindTag(const Bytes& icc, const char* sig, size_t& off, size_t& len) {
  if (icc.size() < 132) return false;
  uint32_t n = Be32(icc.data() + 128);
  for (uint32_t i = 0; i < n && 132 + (size_t)i * 12 + 12 <= icc.size(); ++i) {
    const uint8_t* e = icc.data() + 132 + (size_t)i * 12;
    if (memcmp(e, sig, 4) == 0) {
      off = Be32(e + 4); len = Be32(e + 8);
      return off + len <= icc.size();
    }
  }
  return false;
}

std::wstring IccDescription(const Bytes& icc) {
  size_t off, len;
  if (!IccFindTag(icc, "desc", off, len) || len < 12) return L"";
  const uint8_t* p = icc.data() + off;
  if (memcmp(p, "desc", 4) == 0) {                       // v2 textDescriptionType
    uint32_t cnt = Be32(p + 8);
    if (12 + (size_t)cnt <= len) { std::string s((const char*)p + 12, cnt); while (!s.empty() && s.back() == 0) s.pop_back(); return Widen(s); }
  } else if (memcmp(p, "mluc", 4) == 0 && len >= 28) {   // v4 multiLocalizedUnicodeType: first record, UTF-16BE
    uint32_t slen = Be32(p + 20), soff = Be32(p + 24);
    if (soff + slen <= len) { std::wstring w; for (uint32_t i = 0; i + 1 < slen; i += 2) w.push_back((wchar_t)Be16(p + soff + i)); return w; }
  }
  return L"";
}

bool IccIsSrgb(const Bytes& icc) {
  std::wstring d = ToLower(IccDescription(icc));
  return d.find(L"srgb") != std::wstring::npos;
}

Bytes SrgbIccProfile() {
  // Layout: header(128) | tag table | desc | cprt | wtpt | rXYZ | gXYZ | bXYZ | curv (shared by r/g/b TRC)
  struct Tag { const char* sig; Bytes data; };
  auto s15 = [](double v) { return (uint32_t)(int32_t)std::lround(v * 65536.0); };
  auto xyz = [&](double x, double y, double z) { Bytes b; PutStr(b, "XYZ "); PutBe32(b, 0); PutBe32(b, s15(x)); PutBe32(b, s15(y)); PutBe32(b, s15(z)); return b; };
  Bytes desc; { const char* name = "sRGB IEC61966-2.1 (Filo)"; PutStr(desc, "desc"); PutBe32(desc, 0); PutBe32(desc, (uint32_t)strlen(name) + 1); PutStr(desc, name); desc.push_back(0);
    PutBe32(desc, 0); PutBe32(desc, 0); PutBe16(desc, 0); desc.push_back(0); desc.insert(desc.end(), 67, 0); }
  Bytes cprt; { PutStr(cprt, "text"); PutBe32(cprt, 0); PutStr(cprt, "Public domain sRGB profile generated by Filo"); cprt.push_back(0); }
  Bytes curv; { PutStr(curv, "curv"); PutBe32(curv, 0); PutBe32(curv, 1024);
    for (int i = 0; i < 1024; ++i) { double x = i / 1023.0; double y = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); PutBe16(curv, (uint16_t)std::lround(y * 65535.0)); } }
  std::vector<Tag> tags = {{"desc", desc}, {"cprt", cprt}, {"wtpt", xyz(0.9505, 1.0, 1.0890)},
                           {"rXYZ", xyz(0.43607, 0.22249, 0.01392)}, {"gXYZ", xyz(0.38515, 0.71687, 0.09708)}, {"bXYZ", xyz(0.14307, 0.06061, 0.71410)}};
  // The ICC media white point of a D50 profile is D50; use the Bradford-adapted colorants above with D50 white.
  tags[2].data = xyz(0.96420, 1.0, 0.82491);
  Bytes out(128, 0);
  size_t tableSize = 4 + 9 * 12;
  size_t pos = 128 + tableSize;
  struct Placed { const char* sig; size_t off, len; };
  std::vector<Placed> placed;
  Bytes body;
  auto place = [&](const char* sig, const Bytes& d, long sharedOff = -1, size_t sharedLen = 0) {
    if (sharedOff >= 0) { placed.push_back({sig, (size_t)sharedOff, sharedLen}); return (long)sharedOff; }
    while ((pos + body.size()) % 4) body.push_back(0);
    size_t off = pos + body.size();
    body.insert(body.end(), d.begin(), d.end());
    placed.push_back({sig, off, d.size()});
    return (long)off;
  };
  for (auto& t : tags) place(t.sig, t.data);
  long co = place("rTRC", curv);
  place("gTRC", curv, co, curv.size());
  place("bTRC", curv, co, curv.size());
  while (body.size() % 4) body.push_back(0);
  std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return strcmp(a.sig, b.sig) < 0; });
  PutBe32(out, 0); // placeholder
  out.resize(128);
  PutBe32(out, (uint32_t)placed.size());
  for (auto& p : placed) { PutStr(out, p.sig); PutBe32(out, (uint32_t)p.off); PutBe32(out, (uint32_t)p.len); }
  out.insert(out.end(), body.begin(), body.end());
  // header
  auto set32 = [&](size_t o, uint32_t v) { out[o] = (uint8_t)(v >> 24); out[o + 1] = (uint8_t)(v >> 16); out[o + 2] = (uint8_t)(v >> 8); out[o + 3] = (uint8_t)v; };
  set32(0, (uint32_t)out.size());
  set32(8, 0x02100000);
  memcpy(&out[12], "mntr", 4); memcpy(&out[16], "RGB ", 4); memcpy(&out[20], "XYZ ", 4);
  out[25] = 0; out[27] = 1; out[29] = 1; // date 0000-01-01
  out[24] = 0x07; out[25] = 0xE8;        // year 2024
  memcpy(&out[36], "acsp", 4); memcpy(&out[40], "MSFT", 4);
  set32(68, s15(0.9642)); set32(72, s15(1.0)); set32(76, s15(0.8249));
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// JPEG
// ---------------------------------------------------------------------------------------------------------------------
static bool IsSof(uint8_t m) { return (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC; }

bool ParseJpeg(const Bytes& f, JpegInfo& info, Metadata& meta) {
  if (f.size() < 4 || f[0] != 0xFF || f[1] != 0xD8) return false;
  size_t pos = 2;
  std::map<int, Bytes> iccParts; int iccCount = 0;
  while (pos + 4 <= f.size()) {
    if (f[pos] != 0xFF) { ++pos; continue; }
    uint8_t m = f[pos + 1];
    if (m == 0xFF) { ++pos; continue; }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; }
    if (m == 0xD9) break;
    if (pos + 4 > f.size()) break;
    size_t len = Be16((f.data() + pos + 2));
    if (len < 2 || pos + 2 + len > f.size()) break;
    const uint8_t* d = (f.data() + pos + 4);
    size_t dl = len - 2;
    if (IsSof(m)) {
      if (dl >= 6) {
        info.precision = d[0]; info.height = Be16(d + 1); info.width = Be16(d + 3); info.components = d[5];
        info.progressive = (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE);
        info.arithmetic = (m >= 0xC9);
        info.lossless = (m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF);
      }
    } else if (m == 0xE0 && dl >= 5 && memcmp(d, "JFIF\0", 5) == 0) info.jfif = true;
    else if (m == 0xE1 && dl > 6 && memcmp(d, "Exif\0\0", 6) == 0 && meta.exif.empty()) meta.exif.assign(d + 6, d + dl);
    else if (m == 0xE1 && dl > 29 && memcmp(d, "http://ns.adobe.com/xap/1.0/\0", 29) == 0 && meta.xmp.empty()) meta.xmp.assign(d + 29, d + dl);
    else if (m == 0xE2 && dl > 14 && memcmp(d, "ICC_PROFILE\0", 12) == 0) { iccParts[d[12]] = Bytes(d + 14, d + dl); iccCount = std::max<int>(iccCount, d[13]); }
    else if (m == 0xEE && dl >= 12 && memcmp(d, "Adobe", 5) == 0) { info.adobe = true; info.adobeTransform = d[11]; }
    if (m == 0xDA) break;                                          // start of scan: everything we need precedes it
    pos += 2 + len;
  }
  if (!iccParts.empty()) for (auto& [k, v] : iccParts) meta.icc.insert(meta.icc.end(), v.begin(), v.end());
  info.complete = false;                                          // entropy-coded data never contains an unstuffed FF D9
  for (size_t i = f.size(); i >= 2; --i) if (f[i - 2] == 0xFF && f[i - 1] == 0xD9) { info.complete = true; break; }
  return info.width > 0;
}

bool InjectJpegMeta(Bytes& file, const Metadata& meta, std::vector<std::wstring>* dropped) {
  if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8) return false;
  Bytes out; out.reserve(file.size() + meta.exif.size() + meta.xmp.size() + meta.icc.size() + 256);
  out.push_back(0xFF); out.push_back(0xD8);
  size_t pos = 2;
  // keep a leading JFIF APP0
  if (pos + 4 <= file.size() && file[pos] == 0xFF && file[pos + 1] == 0xE0) {
    size_t len = Be16((file.data() + pos + 2));
    if (pos + 2 + len <= file.size()) { out.insert(out.end(), file.begin() + pos, file.begin() + pos + 2 + len); pos += 2 + len; }
  }
  auto segment = [&](uint8_t marker, const char* ident, size_t identLen, const uint8_t* d, size_t n, int extra0 = -1, int extra1 = -1) {
    size_t payload = identLen + n + (extra0 >= 0 ? 2 : 0);
    out.push_back(0xFF); out.push_back(marker);
    PutBe16(out, (uint16_t)(payload + 2));
    Put(out, ident, identLen);
    if (extra0 >= 0) { out.push_back((uint8_t)extra0); out.push_back((uint8_t)extra1); }
    Put(out, d, n);
  };
  if (!meta.exif.empty()) {
    if (meta.exif.size() + 8 <= 65535) segment(0xE1, "Exif\0\0", 6, meta.exif.data(), meta.exif.size());
    else if (dropped) dropped->push_back(L"EXIF data was too large for a JPEG and was dropped");
  }
  if (!meta.xmp.empty()) {
    if (meta.xmp.size() + 31 <= 65535) segment(0xE1, "http://ns.adobe.com/xap/1.0/\0", 29, meta.xmp.data(), meta.xmp.size());
    else if (dropped) dropped->push_back(L"XMP data was too large for a JPEG and was dropped");
  }
  if (!meta.icc.empty()) {
    const size_t chunk = 65519 - 14 + 2 - 2;     // 65533 - 14 - ... keep well under the limit
    size_t per = 60000; (void)chunk;
    size_t total = (meta.icc.size() + per - 1) / per;
    if (total < 255) for (size_t i = 0; i < total; ++i) {
      size_t o = i * per, n = std::min(per, meta.icc.size() - o);
      segment(0xE2, "ICC_PROFILE\0", 12, meta.icc.data() + o, n, (int)(i + 1), (int)total);
    }
  }
  // copy the remaining segments, skipping existing Exif/XMP/ICC APPs
  while (pos + 2 <= file.size()) {
    if (file[pos] != 0xFF) { out.insert(out.end(), file.begin() + pos, file.end()); pos = file.size(); break; }
    uint8_t m = file[pos + 1];
    if (m == 0xDA || m == 0xD9) { out.insert(out.end(), file.begin() + pos, file.end()); pos = file.size(); break; }
    if (pos + 4 > file.size()) break;
    size_t len = Be16((file.data() + pos + 2));
    if (len < 2 || pos + 2 + len > file.size()) { out.insert(out.end(), file.begin() + pos, file.end()); pos = file.size(); break; }
    const uint8_t* d = (file.data() + pos + 4); size_t dl = len - 2;
    bool skip = (m == 0xE1 && ((dl > 6 && memcmp(d, "Exif\0\0", 6) == 0) || (dl > 29 && memcmp(d, "http://ns.adobe.com/xap/1.0/\0", 29) == 0)))
             || (m == 0xE2 && dl > 12 && memcmp(d, "ICC_PROFILE\0", 12) == 0);
    if (!skip) out.insert(out.end(), file.begin() + pos, file.begin() + pos + 2 + len);
    pos += 2 + len;
  }
  file.swap(out);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------------------------------------------------
bool ParsePng(const Bytes& f, PngInfo& info, Metadata& meta) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (f.size() < 33 || memcmp(f.data(), sig, 8) != 0) return false;
  size_t pos = 8;
  while (pos + 12 <= f.size()) {
    uint32_t len = Be32((f.data() + pos));
    if (pos + 12 + (size_t)len > f.size()) break;
    const uint8_t* t = (f.data() + pos + 4);
    const uint8_t* d = (f.data() + pos + 8);
    if (!memcmp(t, "IHDR", 4) && len >= 13) { info.width = Be32(d); info.height = Be32(d + 4); info.bitDepth = d[8]; info.colorType = d[9]; }
    else if (!memcmp(t, "acTL", 4)) info.animated = true;
    else if (!memcmp(t, "sRGB", 4)) info.hasSrgbChunk = true;
    else if (!memcmp(t, "tRNS", 4)) info.hasTrns = true;
    else if (!memcmp(t, "eXIf", 4) && meta.exif.empty()) { meta.exif.assign(d, d + len); }
    else if (!memcmp(t, "iCCP", 4) && meta.icc.empty()) {
      size_t i = 0; while (i < len && d[i]) ++i;
      if (i + 2 < len) {
        size_t outLen = 0;
        void* p = tinfl_decompress_mem_to_heap(d + i + 2, len - i - 2, &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (p) { if (outLen < (64u << 20)) meta.icc.assign((uint8_t*)p, (uint8_t*)p + outLen); mz_free(p); }
      }
    } else if (!memcmp(t, "iTXt", 4) && meta.xmp.empty()) {
      static const char kw[] = "XML:com.adobe.xmp";
      if (len > sizeof(kw) + 4 && memcmp(d, kw, sizeof(kw)) == 0) {      // keyword incl. NUL
        size_t i = sizeof(kw);
        uint8_t compFlag = d[i], compMethod = d[i + 1]; (void)compMethod;
        i += 2;
        while (i < len && d[i]) ++i; ++i;            // language tag
        while (i < len && d[i]) ++i; ++i;            // translated keyword
        if (i <= len) {
          if (!compFlag) meta.xmp.assign(d + i, d + len);
          else { size_t outLen = 0; void* p = tinfl_decompress_mem_to_heap(d + i, len - i, &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER); if (p) { meta.xmp.assign((uint8_t*)p, (uint8_t*)p + outLen); mz_free(p); } }
        }
      }
    }
    if (!memcmp(t, "IEND", 4)) { info.complete = true; break; }
    pos += 12 + (size_t)len;
  }
  return info.width > 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// WebP
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct RiffChunk { char id[4]; size_t off, len; };   // off = payload offset
static bool RiffChunks(const Bytes& f, std::vector<RiffChunk>& out) {
  if (f.size() < 20 || memcmp(f.data(), "RIFF", 4) != 0 || memcmp(f.data() + 8, "WEBP", 4) != 0) return false;
  size_t end = std::min<size_t>(f.size(), 8 + (size_t)Le32((f.data() + 4)));
  size_t pos = 12;
  while (pos + 8 <= end) {
    RiffChunk c; memcpy(c.id, (f.data() + pos), 4); c.len = Le32((f.data() + pos + 4)); c.off = pos + 8;
    if (c.off + c.len > f.size()) { c.len = f.size() - c.off; out.push_back(c); break; }
    out.push_back(c);
    pos = c.off + c.len + (c.len & 1);
  }
  return true;
}
static bool Is(const RiffChunk& c, const char* id) { return memcmp(c.id, id, 4) == 0; }
}

bool ParseWebp(const Bytes& f, WebpInfo& info, Metadata& meta) {
  std::vector<RiffChunk> ch;
  if (!RiffChunks(f, ch)) return false;
  info.complete = (size_t)Le32((f.data() + 4)) + 8 <= f.size();
  for (auto& c : ch) {
    const uint8_t* d = (f.data() + c.off);
    if (Is(c, "VP8X") && c.len >= 10) {
      uint8_t flags = d[0];
      info.hasAlpha = (flags & 0x10) != 0; info.animated = (flags & 0x02) != 0;
      info.width = 1 + (d[4] | (d[5] << 8) | (d[6] << 16)); info.height = 1 + (d[7] | (d[8] << 8) | (d[9] << 16));
    } else if (Is(c, "VP8L") && c.len >= 5 && d[0] == 0x2F) {
      uint32_t bits = Le32(d + 1);
      if (!info.width) { info.width = (bits & 0x3FFF) + 1; info.height = ((bits >> 14) & 0x3FFF) + 1; }
      if (bits & (1u << 28)) info.hasAlpha = true;
      info.lossless = true;
    } else if (Is(c, "VP8 ") && c.len >= 10 && d[3] == 0x9D && d[4] == 0x01 && d[5] == 0x2A) {
      if (!info.width) { info.width = Le16(d + 6) & 0x3FFF; info.height = Le16(d + 8) & 0x3FFF; }
    } else if (Is(c, "ALPH")) info.hasAlpha = true;
    else if (Is(c, "ANMF")) { ++info.frames; info.animated = true; }
    else if (Is(c, "ICCP")) meta.icc.assign(d, d + c.len);
    else if (Is(c, "EXIF")) { size_t o = (c.len > 6 && memcmp(d, "Exif\0\0", 6) == 0) ? 6 : 0; meta.exif.assign(d + o, d + c.len); }
    else if (Is(c, "XMP ")) meta.xmp.assign(d, d + c.len);
  }
  if (info.animated && info.frames > 1) --info.frames;      // frames counted from 1
  if (info.frames < 1) info.frames = 1;
  return info.width > 0;
}

bool InjectWebpMeta(Bytes& f, const Metadata& meta) {
  std::vector<RiffChunk> ch;
  if (!RiffChunks(f, ch)) return false;
  WebpInfo wi; Metadata dummy;
  if (!ParseWebp(f, wi, dummy)) return false;
  uint8_t flags = 0;
  if (wi.hasAlpha) flags |= 0x10;
  if (wi.animated) flags |= 0x02;
  if (!meta.icc.empty()) flags |= 0x20;
  if (!meta.exif.empty()) flags |= 0x08;
  if (!meta.xmp.empty()) flags |= 0x04;
  Bytes out; PutStr(out, "RIFF"); PutLe32(out, 0); PutStr(out, "WEBP");
  auto chunk = [&](const char* id, const uint8_t* d, size_t n) { PutStr(out, id); PutLe32(out, (uint32_t)n); Put(out, d, n); if (n & 1) out.push_back(0); };
  Bytes vp8x(10, 0); vp8x[0] = flags;
  uint32_t w1 = wi.width - 1, h1 = wi.height - 1;
  vp8x[4] = (uint8_t)w1; vp8x[5] = (uint8_t)(w1 >> 8); vp8x[6] = (uint8_t)(w1 >> 16); vp8x[7] = (uint8_t)h1; vp8x[8] = (uint8_t)(h1 >> 8); vp8x[9] = (uint8_t)(h1 >> 16);
  chunk("VP8X", vp8x.data(), 10);
  if (!meta.icc.empty()) chunk("ICCP", meta.icc.data(), meta.icc.size());
  for (auto& c : ch) {
    if (Is(c, "VP8X") || Is(c, "ICCP") || Is(c, "EXIF") || Is(c, "XMP ")) continue;
    chunk(std::string(c.id, 4).c_str(), (f.data() + c.off), c.len);
  }
  if (!meta.exif.empty()) chunk("EXIF", meta.exif.data(), meta.exif.size());
  if (!meta.xmp.empty()) chunk("XMP ", meta.xmp.data(), meta.xmp.size());
  uint32_t sz = (uint32_t)out.size() - 8;
  out[4] = (uint8_t)sz; out[5] = (uint8_t)(sz >> 8); out[6] = (uint8_t)(sz >> 16); out[7] = (uint8_t)(sz >> 24);
  f.swap(out);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// HEIF / AVIF (ISO-BMFF)
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct Box { uint32_t type; size_t start, hdr, size; size_t PayloadOff() const { return start + hdr; } size_t End() const { return start + size; } };
static uint32_t Fourcc(const char* s) { return Be32((const uint8_t*)s); }
static bool NextBox(const Bytes& f, size_t pos, size_t end, Box& b) {
  if (pos + 8 > end) return false;
  uint64_t sz = Be32((f.data() + pos)); b.type = Be32((f.data() + pos + 4)); b.start = pos; b.hdr = 8;
  if (sz == 1) { if (pos + 16 > end) return false; sz = ((uint64_t)Be32((f.data() + pos + 8)) << 32) | Be32((f.data() + pos + 12)); b.hdr = 16; }
  else if (sz == 0) sz = end - pos;
  if (sz < b.hdr || pos + sz > end) return false;
  b.size = (size_t)sz;
  return true;
}
struct Item { uint32_t id = 0; uint32_t type = 0; std::string contentType; };
struct Extent { uint64_t offset, length; };
struct IlocEntry { uint32_t id; int method; uint64_t baseOffset; std::vector<Extent> extents; };
struct Prop { uint32_t type; size_t off, len; };
}

bool ParseHeif(const Bytes& f, HeifInfo& info, Metadata& meta) {
  Box ftyp;
  if (!NextBox(f, 0, f.size(), ftyp) || ftyp.type != Fourcc("ftyp") || ftyp.size < 16) return false;
  for (size_t o = ftyp.PayloadOff() + 8; o + 4 <= ftyp.End(); o += 4) { uint32_t b = Be32((f.data() + o)); if (b == Fourcc("avis") || b == Fourcc("msf1") || b == Fourcc("hevc")) info.isSequence = true; }
  // locate meta
  Box meta_{}; bool found = false;
  for (size_t pos = 0; pos < f.size();) { Box b; if (!NextBox(f, pos, f.size(), b)) break; if (b.type == Fourcc("meta")) { meta_ = b; found = true; break; } pos = b.End(); }
  if (!found) return false;
  size_t mpos = meta_.PayloadOff() + 4, mend = meta_.End();       // meta is a full box
  uint32_t primary = 0;
  std::vector<Item> items;
  std::vector<IlocEntry> iloc;
  std::vector<Prop> props;
  struct Assoc { uint32_t item; std::vector<int> idx; };
  std::vector<Assoc> assocs;
  for (size_t pos = mpos; pos < mend;) {
    Box b; if (!NextBox(f, pos, mend, b)) break;
    size_t p = b.PayloadOff();
    if (b.type == Fourcc("pitm") && p + 6 <= b.End()) { primary = f[p] == 0 ? Be16((f.data() + p + 4)) : Be32((f.data() + p + 4)); }
    else if (b.type == Fourcc("iinf") && p + 6 <= b.End()) {
      uint8_t ver = f[p]; size_t q = p + 4; uint32_t cnt; if (ver == 0) { cnt = Be16((f.data() + q)); q += 2; } else { cnt = Be32((f.data() + q)); q += 4; }
      for (uint32_t i = 0; i < cnt && q < b.End(); ++i) {
        Box ib; if (!NextBox(f, q, b.End(), ib)) break;
        if (ib.type == Fourcc("infe")) {
          size_t r = ib.PayloadOff(); uint8_t iv = f[r]; r += 4; Item it;
          if (iv >= 2) {
            if (iv == 2) { it.id = Be16((f.data() + r)); r += 2; } else { it.id = Be32((f.data() + r)); r += 4; }
            r += 2; if (r + 4 <= ib.End()) { it.type = Be32((f.data() + r)); r += 4; }
            while (r < ib.End() && f[r]) ++r; ++r;                    // item name
            if (it.type == Fourcc("mime") && r < ib.End()) { size_t e = r; while (e < ib.End() && f[e]) ++e; it.contentType.assign((const char*)(f.data() + r), e - r); }
            items.push_back(it);
          }
        }
        q = ib.End();
      }
    } else if (b.type == Fourcc("iloc") && p + 8 <= b.End()) {
      uint8_t ver = f[p]; size_t q = p + 4;
      int offSize = f[q] >> 4, lenSize = f[q] & 15; int baseSize = f[q + 1] >> 4, idxSize = (ver == 1 || ver == 2) ? (f[q + 1] & 15) : 0; q += 2;
      uint32_t cnt; if (ver < 2) { cnt = Be16((f.data() + q)); q += 2; } else { cnt = Be32((f.data() + q)); q += 4; }
      auto rd = [&](int sz) -> uint64_t { uint64_t v = 0; for (int i = 0; i < sz && q < b.End(); ++i) v = (v << 8) | f[q++]; return v; };
      for (uint32_t i = 0; i < cnt && q + 4 < b.End(); ++i) {
        IlocEntry e; e.id = (ver < 2) ? (uint32_t)rd(2) : (uint32_t)rd(4); e.method = 0;
        if (ver == 1 || ver == 2) e.method = (int)(rd(2) & 15);
        rd(2); e.baseOffset = rd(baseSize); uint32_t ec = (uint32_t)rd(2);
        for (uint32_t k = 0; k < ec && q < b.End(); ++k) { if (idxSize) rd(idxSize); Extent x; x.offset = rd(offSize); x.length = rd(lenSize); e.extents.push_back(x); }
        iloc.push_back(e);
      }
    } else if (b.type == Fourcc("iprp")) {
      for (size_t pp = p; pp < b.End();) {
        Box c; if (!NextBox(f, pp, b.End(), c)) break;
        if (c.type == Fourcc("ipco")) {
          for (size_t qq = c.PayloadOff(); qq < c.End();) { Box pr; if (!NextBox(f, qq, c.End(), pr)) break; props.push_back({pr.type, pr.PayloadOff(), pr.size - pr.hdr}); qq = pr.End(); }
        } else if (c.type == Fourcc("ipma") && c.PayloadOff() + 8 <= c.End()) {
          size_t q = c.PayloadOff(); uint8_t ver = f[q]; uint32_t flags = (f[q + 1] << 16) | (f[q + 2] << 8) | f[q + 3]; q += 4;
          uint32_t cnt = Be32((f.data() + q)); q += 4;
          for (uint32_t i = 0; i < cnt && q + 3 <= c.End(); ++i) {
            Assoc a; if (ver < 1) { a.item = Be16((f.data() + q)); q += 2; } else { a.item = Be32((f.data() + q)); q += 4; }
            int n = f[q++];
            for (int k = 0; k < n && q < c.End(); ++k) {
              int idx; if (flags & 1) { idx = Be16((f.data() + q)) & 0x7FFF; q += 2; } else { idx = f[q] & 0x7F; q += 1; }
              a.idx.push_back(idx);
            }
            assocs.push_back(a);
          }
        }
        pp = c.End();
      }
    }
    pos = b.End();
  }
  // primary item geometry
  for (auto& a : assocs) {
    if (a.item != primary) continue;
    for (int idx : a.idx) {
      if (idx < 1 || idx > (int)props.size()) continue;
      const Prop& pr = props[idx - 1];
      if (pr.type == Fourcc("ispe") && pr.len >= 12) { info.width = Be32((f.data() + pr.off + 4)); info.height = Be32((f.data() + pr.off + 8)); }
      else if (pr.type == Fourcc("clap") && pr.len >= 32) {
        auto frac = [&](size_t o) { uint32_t n = Be32((f.data() + pr.off + o)), d = Be32((f.data() + pr.off + o + 4)); return d ? (double)n / d : 0.0; };
        info.cropWidth = (int)std::lround(frac(0)); info.cropHeight = (int)std::lround(frac(8));
      }
      else if (pr.type == Fourcc("irot") && pr.len >= 1) info.rotationQuarterTurns = f[pr.off] & 3;
      else if (pr.type == Fourcc("imir") && pr.len >= 1) info.mirror = f[pr.off] & 1;
      else if (pr.type == Fourcc("colr") && pr.len >= 4 && meta.icc.empty()) {
        uint32_t ct = Be32((f.data() + pr.off));
        if ((ct == Fourcc("prof") || ct == Fourcc("rICC")) && pr.len > 4) meta.icc.assign(f.begin() + pr.off + 4, f.begin() + pr.off + pr.len);
      }
    }
  }
  for (auto& it : items) if (it.type == Fourcc("av01") || it.type == Fourcc("hvc1")) { /* alpha is an auxiliary item of one of these */ }
  // alpha auxiliary: any 'auxC' property mentioning alpha
  for (auto& pr : props) if (pr.type == Fourcc("auxC")) {
    std::string s((const char*)(f.data() + pr.off + 4), std::min<size_t>(pr.len > 4 ? pr.len - 4 : 0, 80));
    if (s.find("alpha") != std::string::npos || s.find("auxid:1") != std::string::npos) info.hasAlphaAux = true;
  }
  // Exif / XMP items
  auto itemData = [&](uint32_t id, Bytes& out) {
    for (auto& e : iloc) if (e.id == id && e.method == 0) {
      for (auto& x : e.extents) {
        uint64_t off = e.baseOffset + x.offset;
        if (off + x.length > f.size()) return false;
        out.insert(out.end(), f.begin() + off, f.begin() + off + x.length);
      }
      return true;
    }
    return false;
  };
  for (auto& it : items) {
    if (it.type == Fourcc("Exif") && meta.exif.empty()) {
      Bytes d; if (itemData(it.id, d) && d.size() > 8) { uint32_t skip = Be32(d.data()); if (4 + (size_t)skip < d.size()) meta.exif.assign(d.begin() + 4 + skip, d.end()); }
    } else if (it.type == Fourcc("mime") && it.contentType.find("rdf+xml") != std::string::npos && meta.xmp.empty()) {
      itemData(it.id, meta.xmp);
    }
  }
  return info.width > 0;
}

} // namespace filo::img
