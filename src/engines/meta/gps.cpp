// HasGps: native JPEG parse (EXIF GPS IFD + XMP exif:GPSLatitude) for speed, ExifTool for every other format.
#include "engines/meta/internal.h"
#include "registry/filetype.h"
#include <cstring>

namespace filo::meta {

using namespace detail;

namespace {

uint16_t R16(const uint8_t* p, bool le) { return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]); }
uint32_t R32(const uint8_t* p, bool le) { return le ? (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) : (((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]); }

// returns 1 = GPS location found, 0 = EXIF parsed and none, -1 = could not parse
int ExifHasGps(const uint8_t* t, size_t n) {
  if (n < 8) return -1;
  bool le;
  if (t[0] == 'I' && t[1] == 'I') le = true; else if (t[0] == 'M' && t[1] == 'M') le = false; else return -1;
  if (R16(t + 2, le) != 42) return -1;
  uint32_t ifd0 = R32(t + 4, le);
  if ((size_t)ifd0 + 2 > n) return -1;
  uint16_t cnt = R16(t + ifd0, le);
  uint32_t gpsOff = 0;
  for (uint16_t i = 0; i < cnt; ++i) {
    size_t e = (size_t)ifd0 + 2 + (size_t)i * 12;
    if (e + 12 > n) return -1;
    if (R16(t + e, le) == 0x8825) gpsOff = R32(t + e + 8, le);
  }
  if (!gpsOff) return 0;
  if ((size_t)gpsOff + 2 > n) return -1;
  uint16_t gc = R16(t + gpsOff, le);
  bool lat = false, lon = false;
  for (uint16_t i = 0; i < gc; ++i) {
    size_t e = (size_t)gpsOff + 2 + (size_t)i * 12;
    if (e + 12 > n) return -1;
    uint16_t tag = R16(t + e, le), type = R16(t + e + 2, le);
    uint32_t count = R32(t + e + 4, le);
    if (tag == 2 && type == 5 && count == 3) lat = true;
    if (tag == 4 && type == 5 && count == 3) lon = true;
  }
  return lat && lon ? 1 : 0;
}

// nullopt = not a parseable JPEG
std::optional<bool> JpegHasGps(const fs::path& file) {
  HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  auto close = OnExit([&] { CloseHandle(h); });
  std::vector<uint8_t> buf(1u << 20);
  DWORD got = 0;
  if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) || got < 4) return std::nullopt;
  if (buf[0] != 0xFF || buf[1] != 0xD8) return std::nullopt;
  size_t pos = 2;
  bool parsedExif = false, xmpSeen = false;
  while (pos + 4 <= got) {
    if (buf[pos] != 0xFF) return parsedExif || xmpSeen ? std::optional<bool>(false) : std::nullopt;
    uint8_t m = buf[pos + 1];
    if (m == 0xFF) { ++pos; continue; }
    if (m == 0xDA || m == 0xD9) break;                                        // start of scan / end of image
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; }
    size_t len = (size_t)(buf[pos + 2] << 8 | buf[pos + 3]);
    if (len < 2) return std::nullopt;
    size_t seg = pos + 4, end = pos + 2 + len;
    if (end > got) end = got;
    if (m == 0xE1) {
      if (end - seg >= 6 && std::memcmp(&buf[seg], "Exif\0\0", 6) == 0) {
        int r = ExifHasGps(&buf[seg + 6], end - seg - 6);
        if (r == 1) return true;
        if (r == 0) parsedExif = true;
      } else if (end - seg > 29 && std::memcmp(&buf[seg], "http://ns.adobe.com/xap/1.0/", 28) == 0) {
        xmpSeen = true;
        std::string xmp((const char*)&buf[seg + 29], end - seg - 29);
        if (xmp.find("GPSLatitude") != std::string::npos && xmp.find("GPSLongitude") != std::string::npos) return true;
      }
    }
    pos += 2 + len;
  }
  return false;
}

} // namespace

bool HasGps(const fs::path& file) {
  FileKind k = DetectFile(file);
  if (!k.exists || k.isDir || !k.readable || k.cloudPlaceholder) return false;
  if (k.format == L"jpg") {
    if (auto r = JpegHasGps(file)) return *r;
  }
  if (!ExifToolAvailable()) return false;
  EtRun r = RunExifTool({L"-j", L"-G1", L"-n", L"-Composite:GPSLatitude", L"-Composite:GPSLongitude", L"-GPSLatitude", L"-GPSLongitude", L"-GPSCoordinates", L"-Keys:Location", file.wstring()}, nullptr, 60000);
  if (!r.started || r.timedOut) return false;
  auto j = Json::Parse(r.out);
  if (!j || !j->IsArray() || j->Size() == 0) return false;
  bool lat = false, lon = false, coords = false;
  for (auto& [key, v] : j->At(0).Items()) {
    if (key == "SourceFile") continue;
    size_t c = key.find(':');
    std::string tag = c == std::string::npos ? key : key.substr(c + 1);
    if (tag == "GPSLatitude") lat = true;
    else if (tag == "GPSLongitude") lon = true;
    else if (tag == "GPSCoordinates" || tag == "Location") coords = true;
  }
  return (lat && lon) || coords;
}

} // namespace filo::meta
