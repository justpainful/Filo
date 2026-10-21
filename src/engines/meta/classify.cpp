// Field classification: which UI group a tag belongs to, its label, type and whether it can be edited.
#include "engines/meta/internal.h"
#include "registry/filetype.h"
#include <algorithm>
#include <set>
#include <unordered_map>

namespace filo::meta {

const wchar_t* GroupLabel(Group g) {
  switch (g) {
    case Group::File: return L"File"; case Group::Camera: return L"Camera"; case Group::Exposure: return L"Exposure"; case Group::Image: return L"Image";
    case Group::DateTime: return L"Date and time"; case Group::Gps: return L"Location (GPS)"; case Group::Author: return L"Author and description";
    case Group::IptcXmp: return L"IPTC / XMP"; case Group::Audio: return L"Audio"; case Group::Media: return L"Video / media"; case Group::Document: return L"Document";
    default: return L"Other";
  }
}
const char* GroupId(Group g) {
  switch (g) {
    case Group::File: return "File"; case Group::Camera: return "Camera"; case Group::Exposure: return "Exposure"; case Group::Image: return "Image";
    case Group::DateTime: return "DateTime"; case Group::Gps: return "Gps"; case Group::Author: return "Author"; case Group::IptcXmp: return "IptcXmp";
    case Group::Audio: return "Audio"; case Group::Media: return "Media"; case Group::Document: return "Document"; default: return "Other";
  }
}
std::optional<Group> GroupFromId(const std::string& id) {
  for (int i = 0; i <= (int)Group::Other; ++i) if (id == GroupId((Group)i)) return (Group)i;
  return std::nullopt;
}

const Field* MetadataSet::Find(const std::string& key) const {
  for (auto& f : fields) if (f.key == key) return &f;
  return nullptr;
}
const Field* MetadataSet::FindTag(const std::string& tag) const {
  for (auto& f : fields) if (f.tag == tag) return &f;
  return nullptr;
}
std::vector<const Field*> MetadataSet::InGroup(Group g) const {
  std::vector<const Field*> v;
  for (auto& f : fields) if (f.group == g) v.push_back(&f);
  return v;
}
std::wstring MetadataSet::ValueOf(const std::string& k) const {
  if (auto* f = Find(k)) return f->value;
  if (auto* f = FindTag(k)) return f->value;
  return {};
}

namespace detail {

std::wstring FormatOf(const fs::path& f) { return DetectFile(f).format; }

Kind KindOf(const std::wstring& fm) {
  static const std::set<std::wstring> img = {L"jpg", L"png", L"gif", L"bmp", L"tiff", L"webp", L"heic", L"avif", L"svg", L"ico"};
  static const std::set<std::wstring> aud = {L"mp3", L"m4a", L"wav", L"flac", L"ogg", L"opus", L"aiff", L"wma", L"aac"};
  static const std::set<std::wstring> vid = {L"mp4", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"m4v", L"mpg", L"flv", L"3gp", L"ts"};
  static const std::set<std::wstring> off = {L"docx", L"xlsx", L"pptx", L"odt", L"ods", L"odp", L"epub", L"doc", L"xls", L"ppt", L"rtf"};
  if (img.count(fm)) return Kind::Image;
  if (aud.count(fm)) return Kind::Audio;
  if (vid.count(fm)) return Kind::Video;
  if (fm == L"pdf") return Kind::Pdf;
  if (off.count(fm)) return Kind::Office;
  return Kind::Other;
}
bool ExifToolWritable(const std::wstring& f) {
  static const std::set<std::wstring> w = {L"jpg", L"png", L"tiff", L"webp", L"heic", L"avif", L"gif", L"mp4", L"mov", L"m4v", L"m4a", L"3gp", L"pdf"};
  return w.count(f) > 0;
}
bool FfmpegTaggable(const std::wstring& f) {
  static const std::set<std::wstring> w = {L"mp3", L"flac", L"ogg", L"opus", L"wav", L"mkv", L"webm"};
  return w.count(f) > 0;
}
bool OfficeFormat(const std::wstring& f) { return f == L"docx" || f == L"xlsx" || f == L"pptx"; }

std::wstring Humanize(const std::string& tag) {
  static const std::unordered_map<std::string, const wchar_t*> names = {
      {"Make", L"Camera maker"}, {"Model", L"Camera model"}, {"LensModel", L"Lens"}, {"LensMake", L"Lens maker"}, {"LensID", L"Lens ID"},
      {"ISO", L"ISO"}, {"FNumber", L"Aperture (f-number)"}, {"ExposureTime", L"Shutter speed"}, {"FocalLength", L"Focal length"},
      {"FocalLengthIn35mmFormat", L"Focal length (35 mm equivalent)"}, {"DateTimeOriginal", L"Date taken"}, {"CreateDate", L"Date created"},
      {"ModifyDate", L"Date modified"}, {"GPSLatitude", L"Latitude"}, {"GPSLongitude", L"Longitude"}, {"GPSAltitude", L"Altitude"},
      {"GPSLatitudeRef", L"Latitude reference"}, {"GPSLongitudeRef", L"Longitude reference"}, {"GPSAltitudeRef", L"Altitude reference"},
      {"GPSPosition", L"Position"}, {"GPSVersionID", L"GPS version"}, {"GPSDateStamp", L"GPS date"}, {"GPSTimeStamp", L"GPS time"},
      {"ImageWidth", L"Width"}, {"ImageHeight", L"Height"}, {"ImageSize", L"Image size"}, {"ICC_Profile", L"ICC profile"},
      {"ProfileDescription", L"ICC profile name"}, {"ColorSpace", L"Colour space"}, {"Orientation", L"Orientation"},
      {"XResolution", L"Horizontal resolution"}, {"YResolution", L"Vertical resolution"}, {"ResolutionUnit", L"Resolution unit"},
      {"Artist", L"Artist / author"}, {"Copyright", L"Copyright"}, {"ImageDescription", L"Description"}, {"UserComment", L"Comment"},
      {"Software", L"Software"}, {"FileSize", L"File size"}, {"FileType", L"File type"}, {"FileName", L"File name"}, {"MIMEType", L"MIME type"},
      {"FileModifyDate", L"File modified"}, {"FileCreateDate", L"File created"}, {"FileAccessDate", L"File accessed"},
      {"Title", L"Title"}, {"Author", L"Author"}, {"Creator", L"Creator"}, {"Producer", L"Producer"}, {"Subject", L"Subject"},
      {"Keywords", L"Keywords"}, {"PageCount", L"Pages"}, {"Pages", L"Pages"}, {"PDFVersion", L"PDF version"}, {"Duration", L"Duration"},
      {"AudioBitrate", L"Audio bitrate"}, {"SampleRate", L"Sample rate"}, {"ChannelMode", L"Channel mode"}, {"AudioChannels", L"Channels"},
      {"Album", L"Album"}, {"Track", L"Track"}, {"Genre", L"Genre"}, {"Year", L"Year"}, {"Composer", L"Composer"}, {"AlbumArtist", L"Album artist"},
      {"SerialNumber", L"Serial number"}, {"InternalSerialNumber", L"Internal serial number"}, {"BodySerialNumber", L"Body serial number"},
      {"LensSerialNumber", L"Lens serial number"}, {"OwnerName", L"Owner"}, {"CameraOwnerName", L"Camera owner"},
      {"Flash", L"Flash"}, {"WhiteBalance", L"White balance"}, {"MeteringMode", L"Metering mode"}, {"ExposureProgram", L"Exposure program"},
      {"ExposureCompensation", L"Exposure compensation"}, {"ShutterSpeedValue", L"Shutter speed value"}, {"ApertureValue", L"Aperture value"},
      {"Rights", L"Rights"}, {"CreatorTool", L"Created with"}, {"LastModifiedBy", L"Last modified by"}};
  auto it = names.find(tag);
  if (it != names.end()) return it->second;
  std::string s;
  for (size_t i = 0; i < tag.size(); ++i) {
    char c = tag[i];
    if (c == '_') { s.push_back(' '); continue; }
    if (i > 0 && isupper((unsigned char)c)) {
      bool prevLower = islower((unsigned char)tag[i - 1]) || isdigit((unsigned char)tag[i - 1]);
      bool nextLower = i + 1 < tag.size() && islower((unsigned char)tag[i + 1]) && isupper((unsigned char)tag[i - 1]);
      if (prevLower || nextLower) s.push_back(' ');
    }
    s.push_back(c);
  }
  return Widen(s);
}

namespace {
bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool EndsWith(const std::string& s, const char* p) { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }

bool IsDateTag(const std::string& t) {
  static const std::set<std::string> exact = {"CreateDate", "ModifyDate", "DateTimeOriginal", "DateTimeDigitized", "DateCreated", "TimeCreated", "DigitalCreationDate",
                                              "DigitalCreationTime", "MetadataDate", "CreationDate", "ModDate", "Created", "Modified", "CreationTime", "SubSecTimeOriginal",
                                              "OffsetTime", "OffsetTimeOriginal", "OffsetTimeDigitized", "SubSecTime", "SubSecTimeDigitized", "ReleaseDate", "LastPrinted",
                                              "ContentCreateDate", "Year", "RecordingTime", "EncodingTime"};
  if (exact.count(t)) return true;
  if (EndsWith(t, "CreateDate") || EndsWith(t, "ModifyDate") || EndsWith(t, "CreationDate") || EndsWith(t, "ModificationDate") || EndsWith(t, "DateTime") || EndsWith(t, "DateTimeOriginal")) return true;
  if (StartsWith(t, "File") && EndsWith(t, "Date")) return false;
  return false;
}
bool IsCameraTag(const std::string& t) {
  static const std::set<std::string> s = {"Make", "Model", "LensModel", "LensMake", "LensID", "Lens", "LensInfo", "LensSerialNumber", "SerialNumber", "InternalSerialNumber",
                                          "BodySerialNumber", "CameraSerialNumber", "OwnerName", "CameraOwnerName", "Firmware", "FirmwareVersion", "CameraModelName",
                                          "LensType", "Software", "HostComputer", "CameraID", "DeviceModelName", "DeviceManufacturer", "AndroidVersion", "ProcessingSoftware"};
  return s.count(t) > 0;
}
bool IsExposureTag(const std::string& t) {
  static const std::set<std::string> s = {"ExposureTime", "FNumber", "Aperture", "ISO", "ShutterSpeed", "ShutterSpeedValue", "ApertureValue", "ExposureProgram",
                                          "ExposureCompensation", "ExposureMode", "ExposureIndex", "MeteringMode", "Flash", "FlashEnergy", "FlashpixVersion_", "FocalLength",
                                          "FocalLengthIn35mmFormat", "FocalLength35efl", "WhiteBalance", "LightSource", "SceneCaptureType", "BrightnessValue", "MaxApertureValue",
                                          "SubjectDistance", "SubjectDistanceRange", "DigitalZoomRatio", "SensingMethod", "SceneType", "CustomRendered", "GainControl", "Contrast",
                                          "Saturation", "Sharpness", "LightValue", "DOF", "HyperfocalDistance", "FOV", "ScaleFactor35efl", "CircleOfConfusion", "ISOSpeedRatings",
                                          "SensitivityType", "RecommendedExposureIndex", "FileSource", "ExposureBiasValue", "Gain", "FocusDistance", "AutoFocus"};
  return s.count(t) > 0;
}
bool IsImageTag(const std::string& g, const std::string& t) {
  static const std::set<std::string> s = {"ImageWidth", "ImageHeight", "ImageSize", "Megapixels", "Orientation", "ColorSpace", "BitsPerSample", "ColorComponents", "Compression",
                                          "PhotometricInterpretation", "XResolution", "YResolution", "ResolutionUnit", "YCbCrSubSampling", "YCbCrPositioning", "ExifImageWidth",
                                          "ExifImageHeight", "ExifVersion", "ComponentsConfiguration", "FlashpixVersion", "EncodingProcess", "BitDepth", "ColorType", "Interlace",
                                          "Filter", "Gamma", "SRGBRendering", "PrimaryChromaticities", "WhitePoint", "SamplesPerPixel", "ThumbnailImage", "ThumbnailLength",
                                          "ThumbnailOffset", "PreviewImage", "InteropIndex", "InteropVersion", "RelatedImageWidth", "RelatedImageHeight", "PixelsPerUnitX",
                                          "PixelsPerUnitY", "PixelUnits", "JFIFVersion", "AnimationIterations", "FrameCount", "Duration_", "HasColorMap", "ColorResolutionDepth",
                                          "BackgroundColor", "TransparentColor", "BlueMatrixColumn", "GreenMatrixColumn", "RedMatrixColumn", "AlphaMode"};
  return s.count(t) > 0 || g == "ICC_Profile" || StartsWith(g, "ICC") || g == "JFIF" || g == "PNG-pHYs" || g == "PNG-IHDR" || g == "GIF";
}
bool IsAuthorTag(const std::string& t) {
  static const std::set<std::string> s = {"Artist", "Author", "Creator", "By-line", "Byline", "By-lineTitle", "Copyright", "CopyrightNotice", "Rights", "UsageTerms", "Title", "ObjectName",
                                          "Headline", "Description", "ImageDescription", "Caption-Abstract", "Comment", "UserComment", "Subject", "Keywords", "Credit", "Source",
                                          "Contact", "WebStatement", "Notes", "CreatorWorkURL", "CopyrightFlag", "Label", "Rating", "Instructions", "Writer-Editor", "LastModifiedBy"};
  return s.count(t) > 0;
}
} // namespace

Classified Classify(const std::string& g, const std::string& t, const std::wstring& format, const Json& v) {
  Classified c;
  c.label = Humanize(t);
  c.type = FieldType::Text;
  if (v.IsNumber()) c.type = FieldType::Number;
  else if (v.IsBool()) c.type = FieldType::Boolean;
  else if (v.IsArray()) c.type = FieldType::List;
  else if (v.IsString() && StartsWith(v.AsString(), "(Binary data")) c.type = FieldType::Binary;
  Kind kind = KindOf(format);
  bool docFmt = kind == Kind::Pdf || kind == Kind::Office;

  if (g == "System" || g == "ExifTool" || (g == "File" && (t == "FileType" || t == "FileTypeExtension" || t == "MIMEType" || t == "FileName" || t == "Directory" || t == "FileSize" ||
                                                          t == "FilePermissions" || StartsWith(t, "FileModify") || StartsWith(t, "FileAccess") || StartsWith(t, "FileCreate") ||
                                                          t == "FileInodeChangeDate" || t == "ExifByteOrder" || t == "CurrentIPTCDigest"))) {
    c.group = Group::File;
    return c;
  }
  if (StartsWith(g, "ICC") || g == "JFIF") { c.group = Group::Image; return c; }
  if ((t == "Location" || t == "LocationInformation") && (g == "Keys" || g == "UserData" || g == "ItemList" || g == "QuickTime")) { c.group = Group::Gps; return c; }
  if (StartsWith(t, "GPS") || g == "GPS" || EndsWith(t, "GPSCoordinates") || t == "GPSPosition") {
    c.group = Group::Gps;
    if (t == "GPSLatitude" || t == "GPSLongitude" || t == "GPSAltitude" || t == "GPSPosition" || EndsWith(t, "GPSCoordinates")) c.type = FieldType::Gps;
    return c;
  }
  if (g == "MakerNotes" || StartsWith(g, "Canon") || StartsWith(g, "Nikon") || StartsWith(g, "Sony") || StartsWith(g, "Olympus") || StartsWith(g, "Pentax") ||
      StartsWith(g, "Panasonic") || StartsWith(g, "Fuji") || StartsWith(g, "Apple") || StartsWith(g, "Samsung") || StartsWith(g, "Leica") || g == "Photoshop" || g == "Adobe") {
    c.group = Group::Other;
    return c;
  }
  if (docFmt && (g == "PDF" || StartsWith(g, "XMP") || StartsWith(g, "PDF") || g == "ZIP" || g == "OOXML" || g == "XML" || g == "File" || g == "Document" || g == "XMP-x" || g == "Composite")) {
    c.group = Group::Document;
    if (IsDateTag(t) || EndsWith(t, "Date")) c.type = FieldType::DateTime;
    return c;
  }
  if (IsDateTag(t)) { c.group = Group::DateTime; c.type = FieldType::DateTime; return c; }
  if (kind == Kind::Audio) {
    c.group = Group::Audio;
    return c;
  }
  if (IsCameraTag(t)) { c.group = Group::Camera; return c; }
  if (IsExposureTag(t)) { c.group = Group::Exposure; return c; }
  if (IsAuthorTag(t) && !StartsWith(g, "Track")) { c.group = Group::Author; return c; }
  if (IsImageTag(g, t)) { c.group = Group::Image; return c; }
  if (StartsWith(g, "IPTC") || StartsWith(g, "XMP")) { c.group = Group::IptcXmp; return c; }
  if (kind == Kind::Video) { c.group = Group::Media; return c; }
  if (kind == Kind::Image && (g == "IFD0" || g == "IFD1" || g == "ExifIFD" || g == "InteropIFD" || g == "SubIFD" || StartsWith(g, "PNG") || g == "File" || g == "Composite")) { c.group = Group::Image; return c; }
  c.group = Group::Other;
  return c;
}

bool KeyEditable(const std::string& g, const std::string& t, const std::wstring& format) {
  Kind kind = KindOf(format);
  if (g == "System" || g == "ExifTool" || g == "File" || g == "Composite" || g == "MakerNotes" || g == "Photoshop" || g == "JFIF" || StartsWith(g, "ICC") ||
      g == "FFprobe" || StartsWith(g, "PNG-")) return false;
  if (ExifToolWritable(format)) {
    static const std::set<std::string> ro = {"ImageWidth", "ImageHeight", "ExifImageWidth", "ExifImageHeight", "ExifVersion", "ComponentsConfiguration", "FlashpixVersion",
                                             "ThumbnailImage", "PreviewImage", "ThumbnailOffset", "ThumbnailLength", "BitsPerSample", "Compression", "PhotometricInterpretation",
                                             "StripOffsets", "StripByteCounts", "RowsPerStrip", "SamplesPerPixel", "PlanarConfiguration", "YCbCrSubSampling", "InteropIndex",
                                             "InteropVersion", "GPSVersionID", "Duration", "MovieHeaderVersion", "ModifyDate_", "TimeScale", "MediaDuration", "TrackDuration"};
    if (ro.count(t)) return false;
    if (g == "IFD0" || g == "ExifIFD" || g == "GPS" || g == "IPTC" || StartsWith(g, "XMP") || g == "Keys" || g == "ItemList" || g == "UserData" || g == "InteropIFD" ||
        g == "PDF" || g == "IFD1" || g == "SubIFD") {
      if (g == "PDF") return t == "Title" || t == "Author" || t == "Subject" || t == "Keywords" || t == "Creator" || t == "Producer" || t == "CreateDate" || t == "ModifyDate";
      if (g == "IFD1") return false;
      return true;
    }
    if (g == "QuickTime") return t == "CreateDate" || t == "ModifyDate" || t == "Title" || t == "Artist" || t == "Comment";
    if (g == "PNG") return t == "Title" || t == "Author" || t == "Description" || t == "Copyright" || t == "Software" || t == "Comment" || t == "CreationTime" || t == "Source" || t == "Warning" || t == "Disclaimer";
    return false;
  }
  if (FfmpegTaggable(format)) {
    static const std::set<std::string> ok = {"Title", "Artist", "Album", "AlbumArtist", "Band", "Track", "TrackNumber", "Year", "Date", "RecordingTime", "Genre", "Comment", "Comments", "Composer",
                                             "Copyright", "DiscNumber", "Part", "Lyrics", "Publisher", "Description", "Language", "Conductor", "Subtitle", "Performer"};
    return ok.count(t) > 0 && g != "MPEG" && g != "Composite";
  }
  if (OfficeFormat(format)) {
    static const std::set<std::string> ok = {"Title", "Subject", "Creator", "Author", "Keywords", "Description", "LastModifiedBy", "Category", "Created", "Modified", "Company", "Manager",
                                             "ContentStatus", "Language", "Revision", "Application"};
    return ok.count(t) > 0;
  }
  (void)kind;
  return false;
}

} // namespace detail
} // namespace filo::meta
