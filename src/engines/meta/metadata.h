// Filo metadata engine: read / edit / strip file metadata (EXIF, GPS, IPTC, XMP, ID3 / Vorbis / MP4 tags, PDF Info,
// Office core properties) without ever touching the original file and without changing the actual content.
// ExifTool (Artistic / GPL, bundled in tools\exiftool\) runs as a separate process; ffmpeg (separate process) writes
// audio / MKV tags; Office documents are rewritten with miniz. Headless: no UI, no shell.
#pragma once
#include "core/job.h"
#include "core/util.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace filo::meta {

enum class Group {
  File,        // name, size, type, file system times
  Camera,      // make, model, lens, serial numbers, owner
  Exposure,    // ISO, shutter, aperture, focal length, flash...
  Image,       // size, orientation, colour space, ICC profile, resolution
  DateTime,    // created / modified / taken
  Gps,         // latitude, longitude, altitude, direction
  Author,      // author, copyright, title, description, keywords
  IptcXmp,     // remaining IPTC / XMP / Photoshop fields
  Audio,       // title, artist, album, track, year, genre, cover art, bitrate, sample rate, channels
  Media,       // duration, codecs, resolution, frame rate (video container)
  Document,    // PDF / Office: author, creator, producer, pages, created / modified
  Other,       // maker notes and everything unclassified
};
const wchar_t* GroupLabel(Group g);
const char* GroupId(Group g);               // "File", "Camera", ... (stable ids for removeGroups / settings)
std::optional<Group> GroupFromId(const std::string& id);

enum class FieldType { Text, Number, DateTime, Gps, Binary, List, Boolean };

struct Field {
  std::string key;                // ExifTool tag name including the family-1 group: "IFD0:Make", "XMP-dc:Title", "GPS:GPSLatitude"
  std::string tag;                // "Make"
  std::string group1;             // "IFD0"
  Group group = Group::Other;
  std::wstring label;             // "Camera maker"
  std::wstring value;             // display value (ExifTool's print conversion: "1/250", "24 deg 42' 48.96\" N")
  std::wstring raw;               // unconverted value ("0.004", "24.7136"); empty when identical / not applicable
  FieldType type = FieldType::Text;
  bool editable = false;          // WriteMetadata accepts `set` for this key
  bool removable = false;         // WriteMetadata accepts `remove` for this key
};

struct GpsInfo {
  bool valid = false;
  double lat = 0, lon = 0, alt = 0;
  bool hasAlt = false;
  std::wstring formatted;         // "24 deg 42' 48.96\" N, 46 deg 40' 31.08\" E"
  std::wstring decimal;           // "24.713600, 46.675300"
};

struct MetadataSet {
  bool ok = false;
  std::wstring error;
  std::vector<std::wstring> warnings;
  std::wstring format;            // detected format id ("jpg", "mp3", "docx"...)
  bool canWrite = false;          // WriteMetadata can edit this file
  bool hasGps = false;
  bool hasCoverArt = false;       // audio files with an embedded picture
  GpsInfo gps;
  std::wstring toolVersion;       // "ExifTool 13.59"
  std::vector<Field> fields;      // all fields, in the order produced
  const Field* Find(const std::string& key) const;          // exact key "IFD0:Make"
  const Field* FindTag(const std::string& tag) const;       // first field with this tag name in any group ("Make")
  std::vector<const Field*> InGroup(Group g) const;
  std::wstring ValueOf(const std::string& tagOrKey) const;  // display value or empty
};

// Reads everything ExifTool (and ffprobe for media properties) knows. One call = a few hundred ms.
MetadataSet ReadMetadata(const fs::path& file, JobContext* ctx = nullptr);

struct Edits {
  std::map<std::string, std::wstring> set;        // key -> new value; list tags (Keywords, Subject) take one value per line
  std::vector<std::string> remove;                // keys (or bare tag names) to delete
  std::vector<Group> removeGroups;                // delete every writable field of these groups
  bool removeAll = false;                         // delete all metadata (orientation / ICC are KEPT unless keepOrientationAndIcc = false)
  bool keepOrientationAndIcc = true;
  std::optional<fs::path> coverArt;               // audio (mp3 / m4a / flac): embed this jpg / png as the cover
  bool removeCoverArt = false;
  bool Empty() const { return set.empty() && remove.empty() && removeGroups.empty() && !removeAll && !coverArt && !removeCoverArt; }
};

struct WriteResult {
  bool ok = false;
  std::wstring error;
  std::vector<std::wstring> warnings;
  fs::path path;                                  // committed output (never the input)
  bool contentVerified = false;                   // decoded pixels / audio+video packets / PDF text identical to the input
  std::wstring verifyNote;                        // how it was verified ("pixels", "packets", "pdf text", "ooxml parts")
};

// Writes a NEW file: input -> temp copy -> edit -> verify -> OutputTransaction commit at `desiredOut`. The input is
// never modified. Keys use the names ReadMetadata returns ("IFD0:Artist", "XMP-dc:Title", "ID3v2_3:Title"...). The special
// key "GPS:Position" takes "lat, lon[, alt]" in decimal degrees.
WriteResult WriteMetadata(const fs::path& input, const Edits& edits, const fs::path& desiredOut, JobContext* ctx = nullptr);

enum class StripProfile {
  All,                      // every metadata block, including orientation and ICC profile
  GpsOnly,                  // location only (EXIF GPS, XMP GPS, QuickTime location)
  PersonalInfo,             // author / owner names, GPS, serial numbers, software, document author / company
  KeepOrientationAndICC,    // everything except image orientation and the colour profile
};
WriteResult StripMetadata(const fs::path& input, const fs::path& desiredOut, StripProfile profile, JobContext* ctx = nullptr);

// Fast check for location data (native parse for JPEG, ExifTool for everything else).
bool HasGps(const fs::path& file);

// Which formats can be edited ("jpg", "png", "tiff", "webp", "heic", "avif", "gif", "mp4", "mov", "m4a", "mp3", "flac", "ogg", "opus",
// "wav", "mkv", "webm", "pdf", "docx", "xlsx", "pptx"). Canonical lower-case ids as used by DetectFile().
bool CanWrite(const std::wstring& format);

// Content fingerprint used to prove an edit did not touch the real content (decoded pixels, packet hashes, PDF text,
// OOXML parts other than docProps). Empty optional = the format cannot be fingerprinted here.
std::optional<std::string> ContentFingerprint(const fs::path& file, const std::wstring& format, std::wstring* how = nullptr, JobContext* ctx = nullptr);

// ExifTool location (tools\exiftool\exiftool.exe, tools\exiftool.exe, the dev copy under third_party) and availability.
fs::path ExifToolPath();
bool ExifToolAvailable();

} // namespace filo::meta
