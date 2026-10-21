// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// ReadMetadata: grouping, labels, raw + display values, GPS, editability, Unicode names.
#include "m_util.h"

using namespace mt;
using M::Group;

static std::wstring Raw(const M::MetadataSet& s, const char* k) { auto* f = s.Find(k); return f ? f->raw : L"<missing>"; }
static std::wstring Label(const M::MetadataSet& s, const char* k) { auto* f = s.Find(k); return f ? f->label : L"<missing>"; }
static bool Editable(const M::MetadataSet& s, const char* k) { auto* f = s.Find(k); return f && f->editable; }
static bool Removable(const M::MetadataSet& s, const char* k) { auto* f = s.Find(k); return f && f->removable; }
static bool IsType(const M::MetadataSet& s, const char* k, M::FieldType t) { auto* f = s.Find(k); return f && f->type == t; }
static void Dump(const M::MetadataSet& s) { for (auto& f : s.fields) std::wprintf(L"    %hs [%hs] = %ls\n", f.key.c_str(), M::GroupId(f.group), f.value.substr(0, 50).c_str()); }
static bool InGroup(const M::MetadataSet& s, const char* key, Group g) { auto* f = s.Find(key); return f && f->group == g; }

TEST(read_exiftool_is_available_and_version) {
  CHECK(M::ExifToolAvailable());
  CHECK(!M::ExifToolPath().empty());
  M::MetadataSet s = M::ReadMetadata(Fixtures().jpg);
  CHECK(s.ok);
  CHECK(s.toolVersion.find(L"ExifTool") == 0);
}

TEST(read_jpeg_groups_values_and_raw) {
  const Fx& fx = Fixtures();
  M::MetadataSet s = M::ReadMetadata(fx.jpg);
  if (!s.ok) std::wprintf(L"  error: %ls\n", s.error.c_str());
  CHECK(s.ok);
  if (std::getenv("META_DUMP")) Dump(s);
  CHECK_EQ(s.format, std::wstring(L"jpg"));
  CHECK(s.canWrite);
  // camera
  CHECK(InGroup(s, "IFD0:Make", Group::Camera));
  CHECK_EQ(s.ValueOf("IFD0:Make"), std::wstring(L"Canon"));
  CHECK_EQ(s.ValueOf("IFD0:Model"), std::wstring(L"EOS R5"));
  CHECK_EQ(s.ValueOf("ExifIFD:LensModel"), std::wstring(L"RF 50mm F1.8 STM"));
  CHECK(InGroup(s, "ExifIFD:SerialNumber", Group::Camera));
  // exposure: display value and raw value
  CHECK(InGroup(s, "ExifIFD:ExposureTime", Group::Exposure));
  CHECK_EQ(s.ValueOf("ExifIFD:ExposureTime"), std::wstring(L"1/250"));
  CHECK(Raw(s, "ExifIFD:ExposureTime") == L"0.004");
  CHECK_EQ(s.ValueOf("ExifIFD:FNumber"), std::wstring(L"2.8"));
  CHECK_EQ(s.ValueOf("ExifIFD:ISO"), std::wstring(L"400"));
  CHECK(InGroup(s, "ExifIFD:FocalLength", Group::Exposure));
  CHECK(InGroup(s, "ExifIFD:Flash", Group::Exposure));
  // dates
  CHECK(InGroup(s, "ExifIFD:DateTimeOriginal", Group::DateTime));
  CHECK_EQ(s.ValueOf("ExifIFD:DateTimeOriginal"), std::wstring(L"2020:05:06 07:08:09"));
  CHECK(IsType(s, "ExifIFD:DateTimeOriginal", M::FieldType::DateTime));
  // image
  CHECK(InGroup(s, "IFD0:Orientation", Group::Image));
  CHECK(s.ValueOf("IFD0:Orientation").find(L"90") != std::wstring::npos);
  CHECK(Raw(s, "IFD0:Orientation") == L"6");
  CHECK(InGroup(s, "File:ImageWidth", Group::Image));
  CHECK_EQ(s.ValueOf("File:ImageWidth"), std::wstring(L"320"));
  bool icc = false;
  for (auto* f : s.InGroup(Group::Image)) icc |= f->group1 == "ICC_Profile";
  CHECK(icc);
  // author
  CHECK(InGroup(s, "IFD0:Artist", Group::Author));
  CHECK_EQ(s.ValueOf("IFD0:Artist"), std::wstring(L"John Q Public"));
  CHECK(InGroup(s, "IFD0:Copyright", Group::Author));
  CHECK(InGroup(s, "IFD0:ImageDescription", Group::Author));
  // file group
  CHECK(InGroup(s, "System:FileSize", Group::File));
  CHECK(InGroup(s, "File:FileType", Group::File));
  // labels
  CHECK_EQ(Label(s, "IFD0:Make"), std::wstring(L"Camera maker"));
  CHECK_EQ(Label(s, "ExifIFD:ExposureTime"), std::wstring(L"Shutter speed"));
  // editability
  CHECK(Editable(s, "IFD0:Artist") && Removable(s, "IFD0:Artist"));
  CHECK(!Editable(s, "System:FileSize"));
  CHECK(!Editable(s, "File:ImageWidth"));
  CHECK(!Editable(s, "ExifIFD:ExifVersion"));
  CHECK(!Editable(s, "Composite:ImageSize"));
}

TEST(read_jpeg_gps_formatted_and_raw) {
  M::MetadataSet s = M::ReadMetadata(Fixtures().jpg);
  CHECK(s.ok);
  CHECK(s.hasGps);
  CHECK(s.gps.valid);
  CHECK_NEAR(s.gps.lat, 24.0 + 42.0 / 60 + 48.96 / 3600, 1e-6);
  CHECK_NEAR(s.gps.lon, 46.0 + 40.0 / 60 + 31.08 / 3600, 1e-6);
  CHECK(s.gps.hasAlt);
  CHECK_NEAR(s.gps.alt, 615.0, 0.01);
  CHECK(s.gps.formatted.find(L"deg") != std::wstring::npos);
  CHECK(s.gps.formatted.find(L"N") != std::wstring::npos);
  CHECK(s.gps.decimal.find(L"24.7136") == 0);
  CHECK(InGroup(s, "GPS:GPSLatitude", Group::Gps));
  CHECK(IsType(s, "GPS:GPSLatitude", M::FieldType::Gps));
  CHECK(Raw(s, "GPS:GPSLatitude").find(L"24.7136") == 0);                 // raw decimal
  CHECK(s.ValueOf("GPS:GPSLatitude").find(L"deg") != std::wstring::npos);  // formatted
  M::MetadataSet n = M::ReadMetadata(Fixtures().jpgNoGps);
  CHECK(n.ok);
  CHECK(!n.hasGps);
  CHECK(!n.gps.valid);
}

TEST(read_other_image_formats) {
  const Fx& fx = Fixtures();
  M::MetadataSet p = M::ReadMetadata(fx.png);
  CHECK(p.ok);
  CHECK_EQ(p.format, std::wstring(L"png"));
  CHECK_EQ(p.ValueOf("PNG:Title"), std::wstring(L"PNG Title"));
  CHECK(InGroup(p, "PNG:Author", Group::Author));
  CHECK(Editable(p, "PNG:Title"));
  CHECK(!Editable(p, "PNG:ImageWidth"));
  M::MetadataSet w = M::ReadMetadata(fx.webp);
  CHECK(w.ok);
  CHECK_EQ(w.ValueOf("IFD0:Make"), std::wstring(L"Canon"));
  CHECK(w.hasGps);
  M::MetadataSet t = M::ReadMetadata(fx.tiff);
  CHECK(t.ok);
  CHECK_EQ(t.ValueOf("IFD0:Artist"), std::wstring(L"TIFF Artist"));
  M::MetadataSet g = M::ReadMetadata(fx.gif);
  CHECK(g.ok);
  CHECK_EQ(g.format, std::wstring(L"gif"));
  CHECK(g.canWrite);
  if (Exists(fx.avif)) {
    M::MetadataSet a = M::ReadMetadata(fx.avif);
    CHECK(a.ok);
    CHECK_EQ(a.format, std::wstring(L"avif"));
  } else std::printf("  (AVIF fixture could not be created on this machine)\n");
}

TEST(read_audio_tags_cover_and_properties) {
  const Fx& fx = Fixtures();
  M::MetadataSet s = M::ReadMetadata(fx.mp3);
  if (!s.ok) std::wprintf(L"  error: %ls\n", s.error.c_str());
  CHECK(s.ok);
  CHECK_EQ(s.format, std::wstring(L"mp3"));
  CHECK(s.canWrite);
  CHECK(s.hasCoverArt);
  const M::Field* title = s.FindTag("Title");
  CHECK(title && title->group == Group::Audio && title->value == L"Song Title" && title->editable);
  CHECK_EQ(s.ValueOf("Artist"), std::wstring(L"Some Artist"));
  CHECK_EQ(s.ValueOf("Album"), std::wstring(L"Album X"));
  CHECK_EQ(s.ValueOf("Genre"), std::wstring(L"Rock"));
  CHECK(s.FindTag("Track") != nullptr);
  CHECK(s.ValueOf("FFprobe:Duration").find(L"0:03") == 0);
  CHECK(s.ValueOf("FFprobe:SampleRate").find(L"Hz") != std::wstring::npos);
  CHECK(s.ValueOf("FFprobe:Channels") == L"1");
  CHECK(s.ValueOf("FFprobe:AudioBitrate").find(L"kb/s") != std::wstring::npos);
  CHECK(InGroup(s, "FFprobe:Duration", Group::Audio));
  CHECK(!Editable(s, "FFprobe:Duration"));
  for (auto* f : {&fx.flac, &fx.ogg, &fx.opus, &fx.wav, &fx.m4a}) {
    M::MetadataSet a = M::ReadMetadata(*f);
    if (!a.ok) std::wprintf(L"  %ls: %ls\n", f->filename().c_str(), a.error.c_str());
    CHECK(a.ok);
    CHECK_EQ(a.ValueOf("Title"), std::wstring(L"Song Title"));
    CHECK(a.canWrite);
    CHECK(!a.hasCoverArt);
  }
}

TEST(read_video_properties_and_location) {
  const Fx& fx = Fixtures();
  M::MetadataSet s = M::ReadMetadata(fx.mp4);
  CHECK(s.ok);
  CHECK_EQ(s.format, std::wstring(L"mp4"));
  CHECK(s.canWrite);
  CHECK(InGroup(s, "FFprobe:Duration", Group::Media));
  CHECK(s.ValueOf("FFprobe:Resolution").find(L"160 x 120") == 0);
  CHECK(s.ValueOf("FFprobe:VideoCodec") == L"h264");
  CHECK(s.ValueOf("FFprobe:FrameRate").find(L"10") == 0);
  CHECK(s.FindTag("Title") != nullptr);
  CHECK(s.FindTag("Title") && s.FindTag("Title")->group == Group::Author);
  CHECK_EQ(s.ValueOf("Title"), std::wstring(L"Clip Title"));
  M::MetadataSet k = M::ReadMetadata(fx.mkv);
  CHECK(k.ok);
  CHECK(k.canWrite);
  CHECK_EQ(k.ValueOf("Title"), std::wstring(L"Clip Title"));
  M::MetadataSet w = M::ReadMetadata(fx.webm);
  CHECK(w.ok);
  CHECK(w.canWrite);
  std::printf("  mp4 has location tags: %s\n", s.hasGps ? "yes" : "no");
}

TEST(read_documents) {
  const Fx& fx = Fixtures();
  M::MetadataSet p = M::ReadMetadata(fx.pdf);
  if (!p.ok) std::wprintf(L"  error: %ls\n", p.error.c_str());
  CHECK(p.ok);
  CHECK_EQ(p.format, std::wstring(L"pdf"));
  CHECK(p.canWrite);
  CHECK(InGroup(p, "PDF:Title", Group::Document));
  CHECK_EQ(p.ValueOf("PDF:Title"), std::wstring(L"PDF Title"));
  CHECK_EQ(p.ValueOf("PDF:Author"), std::wstring(L"PDF Author"));
  CHECK_EQ(p.ValueOf("PDF:Producer"), std::wstring(L"Fixture Producer"));
  CHECK_EQ(p.ValueOf("PDF:PageCount"), std::wstring(L"3"));
  CHECK(Editable(p, "PDF:Title"));
  CHECK(!Editable(p, "PDF:PageCount"));
  M::MetadataSet d = M::ReadMetadata(fx.docx);
  CHECK(d.ok);
  CHECK_EQ(d.format, std::wstring(L"docx"));
  CHECK(d.canWrite);
  CHECK_EQ(d.ValueOf("Title"), std::wstring(L"My Title"));
  CHECK_EQ(d.ValueOf("Creator"), std::wstring(L"Alice"));
  CHECK_EQ(d.ValueOf("LastModifiedBy"), std::wstring(L"Bob"));
  CHECK_EQ(d.ValueOf("Company"), std::wstring(L"ACME Corp"));
  CHECK(d.FindTag("Title") && d.FindTag("Title")->group == Group::Document);
  CHECK(d.FindTag("Title") && d.FindTag("Title")->editable);
  CHECK(M::ReadMetadata(fx.xlsx).ok);
  CHECK(M::ReadMetadata(fx.pptx).ok);
  CHECK_EQ(M::ReadMetadata(fx.xlsx).ValueOf("Creator"), std::wstring(L"XLSX Author"));
  CHECK_EQ(M::ReadMetadata(fx.pptx).ValueOf("Creator"), std::wstring(L"PPTX Author"));
}

TEST(read_unicode_file_name_and_long_path) {
  const Fx& fx = Fixtures();
  CHECK(Exists(fx.jpgUnicode));
  M::MetadataSet s = M::ReadMetadata(fx.jpgUnicode);
  if (!s.ok) std::wprintf(L"  error: %ls\n", s.error.c_str());
  CHECK(s.ok);
  CHECK_EQ(s.ValueOf("IFD0:Make"), std::wstring(L"Canon"));
  CHECK_EQ(s.ValueOf("System:FileName"), fx.jpgUnicode.filename().wstring());
  // path longer than MAX_PATH
  fs::path deep = Fresh(L"read_long");
  for (int i = 0; i < 8; ++i) deep /= std::wstring(L"segment_") + std::to_wstring(i) + std::wstring(30, L'x');
  MkDirs(deep);
  WriteBytes(deep / L"long file.jpg", ReadBytes(fx.jpg));
  CHECK(LongPath(deep / L"long file.jpg").size() > 260);
  M::MetadataSet l = M::ReadMetadata(deep / L"long file.jpg");
  if (!l.ok) std::wprintf(L"  error: %ls\n", l.error.c_str());
  CHECK(l.ok);
  CHECK_EQ(l.ValueOf("IFD0:Model"), std::wstring(L"EOS R5"));
}

TEST(read_errors_are_clean) {
  fs::path dir = Fresh(L"read_err");
  M::MetadataSet a = M::ReadMetadata(dir / L"missing.jpg");
  CHECK(!a.ok);
  CHECK(!a.error.empty());
  M::MetadataSet b = M::ReadMetadata(dir);
  CHECK(!b.ok);
  WriteBytes(dir / L"empty.jpg", "");
  M::MetadataSet c = M::ReadMetadata(dir / L"empty.jpg");
  std::wprintf(L"  empty file -> ok=%d error='%ls'\n", (int)c.ok, c.error.c_str());
  WriteBytes(dir / L"garbage.jpg", "this is definitely not a jpeg");
  M::MetadataSet d = M::ReadMetadata(dir / L"garbage.jpg");
  std::wprintf(L"  garbage -> ok=%d error='%ls'\n", (int)d.ok, d.error.c_str());
  CHECK(!d.ok);
  // truncated JPEG still reads what it can or fails cleanly
  std::string whole = ReadBytes(Fixtures().jpg);
  WriteBytes(dir / L"trunc.jpg", whole.substr(0, whole.size() / 3));
  M::MetadataSet e = M::ReadMetadata(dir / L"trunc.jpg");
  CHECK(e.ok || !e.error.empty());
  // not a supported type
  WriteBytes(dir / L"notes.xyz", "some plain text");
  M::MetadataSet f = M::ReadMetadata(dir / L"notes.xyz");
  CHECK(f.ok || !f.error.empty());
}

TEST(read_does_not_touch_the_file) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"read_ro");
  fs::path copy = dir / L"ro.jpg";
  CopyFileW(fx.jpg.c_str(), copy.c_str(), FALSE);
  SetFileAttributesW(copy.c_str(), FILE_ATTRIBUTE_READONLY);
  uint32_t before = Crc(ReadBytes(copy));
  uint64_t mt = MTime(copy);
  M::MetadataSet s = M::ReadMetadata(copy);
  CHECK(s.ok);
  CHECK_EQ(Crc(ReadBytes(copy)), before);
  CHECK_EQ(MTime(copy), mt);
  SetFileAttributesW(copy.c_str(), FILE_ATTRIBUTE_NORMAL);
}
