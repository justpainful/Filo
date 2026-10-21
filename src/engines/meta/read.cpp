// ReadMetadata: ExifTool (print-converted + raw, two runs in parallel) + ffprobe media properties -> grouped fields.
#include "engines/common/ffmpeg.h"
#include "engines/meta/internal.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace filo::meta {

using namespace detail;

namespace {

std::string NumToString(double d) {
  if (std::floor(d) == d && std::fabs(d) < 1e15) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)d); return b; }
  char b[48];
  snprintf(b, sizeof b, "%.15g", d);
  return b;
}

std::string Stringify(const Json& v) {
  switch (v.type()) {
    case Json::Type::String: return v.AsString();
    case Json::Type::Number: return NumToString(v.AsNumber());
    case Json::Type::Bool: return v.AsBool() ? "True" : "False";
    case Json::Type::Array: {
      std::string s;
      bool first = true;
      for (auto& e : v.Elements()) {
        if (!first) s += ", ";
        first = false;
        s += e.IsObject() ? e.Dump() : Stringify(e);
      }
      return s;
    }
    case Json::Type::Object: return v.Dump();
    default: return {};
  }
}

std::wstring Display(const Json& v) {
  std::string s = Stringify(v);
  if (s.rfind("(Binary data ", 0) == 0) {
    size_t sp = s.find(' ', 13);
    return L"Binary data (" + Widen(s.substr(13, sp == std::string::npos ? std::string::npos : sp - 13)) + L" bytes)";
  }
  return Widen(s);
}

struct Parsed {
  bool ok = false;
  Json obj;
  std::string error;
};

Parsed ParseRun(const EtRun& r) {
  Parsed p;
  if (!r.started) { p.error = "ExifTool could not be started."; return p; }
  if (r.cancelled) { p.error = "Cancelled"; return p; }
  if (r.timedOut) { p.error = "ExifTool did not finish in time."; return p; }
  auto j = Json::Parse(r.out);
  if (!j || !j->IsArray() || j->Size() == 0) { p.error = "ExifTool could not read this file."; return p; }
  p.obj = j->At(0);
  p.ok = true;
  return p;
}

bool ParseCoordinates(const std::string& s, double& lat, double& lon, double* alt, bool& hasAlt) {
  // "+24.7136+046.6753+012.000/" (ISO 6709) or "24.7136 46.6753 12" or "24.7136, 46.6753"
  std::vector<double> nums;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '+' || s[i] == '-' || isdigit((unsigned char)s[i]) || s[i] == '.') {
      char* end = nullptr;
      double d = strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i) { ++i; continue; }
      nums.push_back(d);
      i = (size_t)(end - s.c_str());
    } else ++i;
  }
  if (nums.size() < 2) return false;
  lat = nums[0]; lon = nums[1];
  hasAlt = nums.size() >= 3;
  if (hasAlt && alt) *alt = nums[2];
  return std::fabs(lat) <= 90 && std::fabs(lon) <= 180;
}

} // namespace

MetadataSet ReadMetadata(const fs::path& file, JobContext* ctx) {
  MetadataSet ms;
  FileKind kind = DetectFile(file);
  if (!kind.exists) { ms.error = L"The file is no longer available."; return ms; }
  if (kind.isDir) { ms.error = L"This is a folder, not a file."; return ms; }
  if (!kind.readable) { ms.error = L"The file cannot be opened (access denied or in use)."; return ms; }
  if (kind.cloudPlaceholder) { ms.error = L"This file is stored online only (cloud placeholder). Download it first, then try again."; return ms; }
  if (!ExifToolAvailable()) { ms.error = L"ExifTool is not installed, so metadata cannot be read."; return ms; }
  ms.format = kind.format;
  ms.canWrite = CanWrite(kind.format) && kind.size > 0;

  const std::wstring path = file.wstring();
  EtRun printed, raw;
  std::optional<ff::MediaInfo> media;
  Kind k = KindOf(kind.format);
  {
    std::thread tRaw([&] { raw = RunExifTool({L"-j", L"-G1", L"-a", L"-struct", L"-n", path}, ctx, 120000); });
    std::thread tMedia;
    if ((k == Kind::Audio || k == Kind::Video) && ff::Available()) tMedia = std::thread([&] { media = ff::Probe(file, 30000); });
    printed = RunExifTool({L"-j", L"-G1", L"-a", L"-struct", path}, ctx, 120000);
    tRaw.join();
    if (tMedia.joinable()) tMedia.join();
  }
  if (printed.cancelled || raw.cancelled) { ms.error = L"Cancelled"; return ms; }
  Parsed A = ParseRun(printed), B = ParseRun(raw);
  if (!A.ok) {
    ms.error = A.error.empty() ? L"ExifTool could not read this file." : Widen(A.error);
    return ms;
  }
  if (A.obj.Has("ExifTool:Error")) {
    std::wstring e = Widen(Stringify(A.obj.Get("ExifTool:Error")));
    ms.error = e == L"Unknown file type" ? L"This file type is not supported for metadata." : (L"The file could not be read: " + e);
    return ms;
  }
  {
    // ExifTool falls back to "TXT" for content it does not recognise: a .jpg / .mp3 / .pdf that is really plain text is damaged
    std::string ft = Stringify(A.obj.Get("File:FileType"));
    if (ft == "TXT" && KindOf(kind.format) != Kind::Other) {
      ms.error = L"This file is damaged or is not really a " + kind.format + L" file, so its metadata cannot be read.";
      return ms;
    }
  }
  if (A.obj.Has("ExifTool:Warning")) ms.warnings.push_back(Widen(Stringify(A.obj.Get("ExifTool:Warning"))));
  ms.toolVersion = L"ExifTool " + Widen(Stringify(A.obj.Get("ExifTool:ExifToolVersion")));

  for (auto& [key, val] : A.obj.Items()) {
    if (key == "SourceFile" || key == "ExifTool:Warning" || key == "ExifTool:Error" || key == "ExifTool:ExifToolVersion") continue;
    size_t colon = key.find(':');
    if (colon == std::string::npos) continue;
    Field f;
    f.key = key;
    f.group1 = key.substr(0, colon);
    f.tag = key.substr(colon + 1);
    // duplicate suffix " (1)" produced by ExifTool for repeated tags
    bool duplicate = false;
    size_t sp = f.tag.find(" (");
    if (sp != std::string::npos) { duplicate = true; f.tag = f.tag.substr(0, sp); }
    Classified c = Classify(f.group1, f.tag, kind.format, val);
    f.group = c.group; f.label = c.label; f.type = c.type;
    f.value = Display(val);
    const Json& rv = B.ok ? B.obj.Get(key) : Json();
    if (!rv.IsNull()) {
      std::wstring r = Display(rv);
      if (r != f.value && f.type != FieldType::Binary) f.raw = r;
    }
    f.editable = !duplicate && ms.canWrite && f.type != FieldType::Binary && KeyEditable(f.group1, f.tag, kind.format);
    f.removable = f.editable;
    ms.fields.push_back(std::move(f));
  }

  // media properties from ffprobe (audio / video containers)
  if (media) {
    auto add = [&](const char* tag, const std::wstring& label, const std::wstring& value, const std::wstring& rawv, Group g, FieldType t) {
      Field f;
      f.key = std::string("FFprobe:") + tag; f.tag = tag; f.group1 = "FFprobe"; f.group = g; f.label = label; f.value = value; f.raw = rawv; f.type = t;
      ms.fields.push_back(std::move(f));
    };
    Group g = k == Kind::Audio ? Group::Audio : Group::Media;
    if (media->duration > 0) add("Duration", L"Duration", FormatTime(media->duration, false), Widen(NumToString(media->duration)), g, FieldType::Number);
    if (media->bitRate > 0) add("Bitrate", L"Overall bitrate", std::to_wstring((media->bitRate + 500) / 1000) + L" kb/s", std::to_wstring(media->bitRate), g, FieldType::Number);
    if (media->formatName.size()) add("Container", L"Container", Widen(media->formatName), L"", g, FieldType::Text);
    for (auto& s : media->streams) {
      if (s.type == "audio") {
        add("AudioCodec", L"Audio codec", Widen(s.codec), L"", g, FieldType::Text);
        if (s.sampleRate) add("SampleRate", L"Sample rate", std::to_wstring(s.sampleRate) + L" Hz", std::to_wstring(s.sampleRate), g, FieldType::Number);
        if (s.channels) add("Channels", L"Channels", std::to_wstring(s.channels), std::to_wstring(s.channels), g, FieldType::Number);
        if (s.bitRate > 0) add("AudioBitrate", L"Audio bitrate", std::to_wstring((s.bitRate + 500) / 1000) + L" kb/s", std::to_wstring(s.bitRate), g, FieldType::Number);
        break;
      }
    }
    for (auto& s : media->streams) {
      if (s.type == "video" && s.attachedPic) { ms.hasCoverArt = true; continue; }
      if (s.type == "video" && k == Kind::Video) {
        add("VideoCodec", L"Video codec", Widen(s.codec), L"", g, FieldType::Text);
        if (s.width && s.height) add("Resolution", L"Resolution", std::to_wstring(s.width) + L" x " + std::to_wstring(s.height), L"", g, FieldType::Text);
        if (s.fps > 0) { wchar_t b[32]; swprintf_s(b, L"%.3g fps", s.fps); add("FrameRate", L"Frame rate", b, Widen(NumToString(s.fps)), g, FieldType::Number); }
        if (!s.pixFmt.empty()) add("PixelFormat", L"Pixel format", Widen(s.pixFmt), L"", g, FieldType::Text);
        break;
      }
    }
  }
  if (k == Kind::Audio && !ms.hasCoverArt) {
    for (auto& f : ms.fields) if (f.tag == "Picture" || f.tag == "CoverArt" || f.tag == "PictureType" || f.tag == "AttachedPicture") { ms.hasCoverArt = true; break; }
  }

  // GPS summary (prefer ExifTool's Composite, which merges EXIF / XMP / QuickTime sources)
  if (B.ok) {
    double lat = 0, lon = 0, alt = 0;
    bool got = false, hasAlt = false;
    auto num = [&](const char* key, double& out) { const Json& v = B.obj.Get(key); if (v.IsNumber()) { out = v.AsNumber(); return true; } return false; };
    if (num("Composite:GPSLatitude", lat) && num("Composite:GPSLongitude", lon)) got = true;
    else {
      for (auto& [key, val] : B.obj.Items()) {
        size_t c = key.find(':');
        std::string tag = c == std::string::npos ? key : key.substr(c + 1);
        if (tag.size() >= 14 && tag.compare(tag.size() - 14, 14, "GPSCoordinates") == 0) {
          if (ParseCoordinates(Stringify(val), lat, lon, &alt, hasAlt)) { got = true; break; }
        }
      }
      if (!got) {
        double la = 0, lo = 0;
        for (const char* g : {"GPS", "XMP-exif"}) {
          if (num((std::string(g) + ":GPSLatitude").c_str(), la) && num((std::string(g) + ":GPSLongitude").c_str(), lo)) {
            std::string lr = Stringify(B.obj.Get(std::string(g) + ":GPSLatitudeRef")), orr = Stringify(B.obj.Get(std::string(g) + ":GPSLongitudeRef"));
            lat = (lr == "S" ? -1 : 1) * std::fabs(la); lon = (orr == "W" ? -1 : 1) * std::fabs(lo);
            got = true;
            break;
          }
        }
      }
    }
    if (got && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0 && !B.obj.Has("Composite:GPSLatitude"))) {
      ms.gps.valid = true; ms.gps.lat = lat; ms.gps.lon = lon;
      if (num("Composite:GPSAltitude", alt) || num("GPS:GPSAltitude", alt) || hasAlt) { ms.gps.alt = alt; ms.gps.hasAlt = true; }
      wchar_t b[96]; swprintf_s(b, L"%.6f, %.6f", lat, lon);
      ms.gps.decimal = b;
      std::wstring la = Widen(Stringify(A.obj.Get("Composite:GPSLatitude"))), lo = Widen(Stringify(A.obj.Get("Composite:GPSLongitude")));
      ms.gps.formatted = (la.empty() || lo.empty()) ? ms.gps.decimal : la + L", " + lo;
      ms.hasGps = true;
    }
  }
  // stable, human-friendly order: group, then the usual suspects first, then alphabetical
  static const char* kFirst[] = {"Make", "Model", "LensModel", "Lens", "Title", "Artist", "Author", "Creator", "Album", "AlbumArtist", "Track", "Year", "Genre", "Description",
                                 "ImageDescription", "Copyright", "Keywords", "Subject", "DateTimeOriginal", "CreateDate", "ModifyDate", "ISO", "ExposureTime", "FNumber",
                                 "FocalLength", "Flash", "GPSLatitude", "GPSLongitude", "GPSAltitude", "GPSPosition", "ImageWidth", "ImageHeight", "Orientation", "ColorSpace",
                                 "Duration", "Resolution", "VideoCodec", "FrameRate", "AudioCodec", "SampleRate", "Channels", "AudioBitrate", "Bitrate"};
  auto rank = [&](const Field& f) { for (size_t i = 0; i < std::size(kFirst); ++i) if (f.tag == kFirst[i]) return (int)i; return 1000; };
  std::stable_sort(ms.fields.begin(), ms.fields.end(), [&](const Field& a, const Field& b) {
    if (a.group != b.group) return a.group < b.group;
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a.key < b.key;
  });
  ms.ok = true;
  return ms;
}

} // namespace filo::meta
