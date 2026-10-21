// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// Helpers for the metadata engine tests: scratch dirs, fixture generation with independent tools (Pillow + piexif, ffmpeg,
// mutagen, PyMuPDF, openpyxl, python-pptx), independent readers and hashing.
#pragma once
#include "test.h"
#include "core/job.h"
#include "core/json.h"
#include "core/process.h"
#include "core/util.h"
#include "engines/meta/metadata.h"
#include "miniz.h"
#include <cstdio>
#include <set>

namespace mt {

using namespace filo;
namespace M = filo::meta;

inline void RemoveAll(const fs::path& p) {
  std::error_code ec;
  std::wstring s = fs::absolute(p).wstring();
  if (s.rfind(L"\\\\?\\", 0) != 0) s = L"\\\\?\\" + s;
  fs::remove_all(fs::path(s), ec);
}
inline fs::path RepoRoot() {
  fs::path p = ExeDir();
  for (int i = 0; i < 6; ++i) { if (fs::exists(p / L"docs" / L"ENGINE-CONTRACT.md")) return p; p = p.parent_path(); }
  return ExeDir();
}
inline fs::path GenRoot() {
  static fs::path r = [] {
    fs::path d = AppDataDir() / L"test-meta";
    RemoveAll(d);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
  }();
  return r;
}
inline fs::path Fresh(const std::wstring& name) {
  fs::path d = GenRoot() / name;
  RemoveAll(d);
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}
inline bool Exists(const fs::path& p) { return GetFileAttributesW(LongPath(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
inline std::string ReadBytes(const fs::path& p) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
  std::string s((size_t)sz.QuadPart, '\0');
  size_t done = 0;
  while (done < s.size()) { DWORD g = 0; if (!ReadFile(h, s.data() + done, (DWORD)std::min<size_t>(s.size() - done, 1u << 28), &g, nullptr) || !g) break; done += g; }
  CloseHandle(h);
  s.resize(done);
  return s;
}
inline void MkDirs(const fs::path& d) {
  std::error_code ec;
  std::wstring s = fs::absolute(d).wstring();
  if (s.rfind(L"\\\\?\\", 0) != 0) s = L"\\\\?\\" + s;
  fs::create_directories(fs::path(s), ec);
}
inline void WriteBytes(const fs::path& p, const std::string& d) {
  MkDirs(p.parent_path());
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  size_t done = 0;
  while (done < d.size()) { DWORD w = 0; if (!WriteFile(h, d.data() + done, (DWORD)std::min<size_t>(d.size() - done, 1u << 28), &w, nullptr)) break; done += w; }
  CloseHandle(h);
}
inline uint32_t Crc(const std::string& s) { return (uint32_t)mz_crc32(mz_crc32(0, nullptr, 0), (const unsigned char*)s.data(), s.size()); }
inline uint64_t MTime(const fs::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA ad{};
  GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &ad);
  return ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
}

inline fs::path Where(const wchar_t* exe) { wchar_t b[MAX_PATH * 2]; return SearchPathW(nullptr, exe, nullptr, (DWORD)std::size(b), b, nullptr) ? fs::path(b) : fs::path(); }
inline fs::path PythonExe() { static fs::path p = Where(L"python.exe"); return p; }
inline fs::path FfmpegExe() { static fs::path p = Where(L"ffmpeg.exe"); return p; }
inline fs::path FfprobeExe() { static fs::path p = Where(L"ffprobe.exe"); return p; }
inline fs::path SofficeExe() { fs::path p = L"C:\\Program Files\\LibreOffice\\program\\soffice.exe"; return fs::exists(p) ? p : fs::path(); }

struct Out { int code = -1; std::string out, err; };
inline Out Run(const fs::path& exe, std::vector<std::wstring> args, DWORD timeout = 600000) {
  ProcessOptions po; po.exe = exe; po.args = std::move(args); po.timeoutMs = timeout; po.lowPriority = false; po.captureLimit = 64u << 20;
  ProcessResult r = RunProcess(po);
  Out o; o.code = r.started && !r.timedOut ? (int)r.exitCode : -1; o.out = r.out; o.err = r.err;
  return o;
}
inline Out Py(const std::string& code, std::vector<std::wstring> args = {}, DWORD timeout = 600000) {
  std::vector<std::wstring> a = {L"-I", L"-c", Widen(code)};
  for (auto& x : args) a.push_back(x);
  return Run(PythonExe(), a, timeout);
}
// Runs python and parses the JSON it prints (always UTF-8)
inline Json PyJson(const std::string& code, std::vector<std::wstring> args = {}) {
  Out o = Py("import sys, json\nsys.stdout.reconfigure(encoding='utf-8')\n" + code, std::move(args));
  if (o.code != 0) { std::printf("  python failed: %s\n", o.err.substr(0, 500).c_str()); return Json(); }
  auto j = Json::Parse(o.out);
  return j ? *j : Json();
}
inline Out Ff(std::vector<std::wstring> args) {
  std::vector<std::wstring> a = {L"-nostdin", L"-hide_banner", L"-v", L"error", L"-y"};
  for (auto& x : args) a.push_back(x);
  return Run(FfmpegExe(), a);
}
inline Json Probe(const fs::path& f) {
  Out o = Run(FfprobeExe(), {L"-v", L"error", L"-print_format", L"json", L"-show_format", L"-show_streams", f.wstring()});
  auto j = Json::Parse(o.out);
  return j ? *j : Json();
}

// ---- fixtures ----------------------------------------------------------------------------------------------------------------

struct Fx {
  fs::path dir;
  fs::path jpg, jpgUnicode, jpgNoGps, png, webp, tiff, gif, avif;
  fs::path mp3, flac, ogg, opus, wav, m4a, mp4, mkv, webm;
  fs::path pdf, docx, xlsx, pptx, cover;
};

inline const char* kFixturePy = R"PY(
import sys, os, io, zipfile, subprocess, struct
from PIL import Image, ImageCms, PngImagePlugin, ImageDraw
import piexif
out = sys.argv[1]
def P(n): return os.path.join(out, n)
# picture with structure (gradient + shapes), so pixel hashes are meaningful
im = Image.new('RGB', (320, 240))
px = im.load()
for y in range(240):
    for x in range(320):
        px[x, y] = ((x * 255) // 319, (y * 255) // 239, ((x ^ y) * 3) & 255)
d = ImageDraw.Draw(im); d.ellipse((60, 40, 260, 200), outline=(255, 255, 0), width=5); d.rectangle((10, 10, 80, 50), fill=(0, 0, 0))
icc = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
def rat(a, b=1): return (a, b)
exif = {
 '0th': {piexif.ImageIFD.Make: b'Canon', piexif.ImageIFD.Model: b'EOS R5', piexif.ImageIFD.Software: b'Photoshop 25.0', piexif.ImageIFD.Artist: b'John Q Public',
         piexif.ImageIFD.Copyright: b'(c) 2020 ACME Corp', piexif.ImageIFD.ImageDescription: b'A sunny day in the park', piexif.ImageIFD.Orientation: 6,
         piexif.ImageIFD.DateTime: b'2020:05:06 07:08:10'},
 'Exif': {piexif.ExifIFD.DateTimeOriginal: b'2020:05:06 07:08:09', piexif.ExifIFD.ExposureTime: rat(1, 250), piexif.ExifIFD.FNumber: rat(28, 10),
          piexif.ExifIFD.ISOSpeedRatings: 400, piexif.ExifIFD.FocalLength: rat(50, 1), piexif.ExifIFD.LensModel: b'RF 50mm F1.8 STM',
          piexif.ExifIFD.BodySerialNumber: b'1234567', piexif.ExifIFD.Flash: 16, piexif.ExifIFD.WhiteBalance: 0, piexif.ExifIFD.ColorSpace: 1},
 'GPS': {piexif.GPSIFD.GPSVersionID: (2, 3, 0, 0), piexif.GPSIFD.GPSLatitudeRef: b'N', piexif.GPSIFD.GPSLatitude: (rat(24), rat(42), rat(4896, 100)),
         piexif.GPSIFD.GPSLongitudeRef: b'E', piexif.GPSIFD.GPSLongitude: (rat(46), rat(40), rat(3108, 100)), piexif.GPSIFD.GPSAltitudeRef: 0,
         piexif.GPSIFD.GPSAltitude: rat(6150, 10)},
 '1st': {}, 'thumbnail': None}
eb = piexif.dump(exif)
im.save(P('photo.jpg'), 'JPEG', quality=92, exif=eb, icc_profile=icc)
im.save('\\\\?\\' + P('\u0635\u0648\u0631\u0629 \U0001F600 \u65e5\u672c.jpg'), 'JPEG', quality=92, exif=eb, icc_profile=icc)
exif2 = {'0th': {piexif.ImageIFD.Make: b'Acme', piexif.ImageIFD.Model: b'NoGps 1', piexif.ImageIFD.Orientation: 1}, 'Exif': {}, 'GPS': {}, '1st': {}, 'thumbnail': None}
im.save(P('nogps.jpg'), 'JPEG', quality=90, exif=piexif.dump(exif2))
info = PngImagePlugin.PngInfo(); info.add_text('Title', 'PNG Title'); info.add_text('Author', 'PNG Author'); info.add_text('Description', 'PNG description')
im.save(P('photo.png'), 'PNG', pnginfo=info, icc_profile=icc)
im.save(P('photo.webp'), 'WEBP', quality=90, exif=eb, icc_profile=icc)
ifd = {315: 'TIFF Artist', 305: 'TIFF Software', 270: 'TIFF description'}
im.save(P('photo.tiff'), 'TIFF', tiffinfo=ifd, icc_profile=icc)
frames = [Image.new('P', (48, 32), c) for c in (1, 100, 200)]
for i, f in enumerate(frames):
    f.putpalette([v for k in range(256) for v in (k, 255 - k, (k * 7) & 255)])
frames[0].save(P('anim.gif'), save_all=True, append_images=frames[1:], duration=100, loop=0, comment=b'gif comment')
try:
    im.save(P('photo.avif'), 'AVIF', quality=80)
except Exception as e:
    sys.stderr.write('avif unavailable: %s\n' % e)
im.save(P('cover.jpg'), 'JPEG', quality=85)
# documents
import pymupdf as fitz
doc = fitz.open()
for i in range(3):
    pg = doc.new_page(); pg.insert_text((72, 100), 'Page %d: the quick brown fox jumps over the lazy dog' % (i + 1))
doc.set_metadata({'title': 'PDF Title', 'author': 'PDF Author', 'subject': 'PDF Subject', 'keywords': 'one, two', 'creator': 'Fixture Creator', 'producer': 'Fixture Producer'})
doc.save(P('doc.pdf')); doc.close()
core = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:dcmitype="http://purl.org/dc/dcmitype/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>My Title</dc:title><dc:creator>Alice</dc:creator><cp:lastModifiedBy>Bob</cp:lastModifiedBy><dcterms:created xsi:type="dcterms:W3CDTF">2020-01-02T03:04:05Z</dcterms:created><dcterms:modified xsi:type="dcterms:W3CDTF">2021-02-03T04:05:06Z</dcterms:modified></cp:coreProperties>'
app = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>Microsoft Office Word</Application><Company>ACME Corp</Company><Manager>The Boss</Manager></Properties>'
ct = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Default Extension="jpeg" ContentType="image/jpeg"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/><Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/><Override PartName="/docProps/custom.xml" ContentType="application/vnd.openxmlformats-officedocument.custom-properties+xml"/></Types>'
rels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/><Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/custom-properties" Target="docProps/custom.xml"/><Relationship Id="rId5" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail" Target="docProps/thumbnail.jpeg"/></Relationships>'
custom = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/custom-properties" xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"><property fmtid="{D5CDD505-2E9C-101B-9397-08002B2CF9AE}" pid="2" name="Secret"><vt:lpwstr>classified</vt:lpwstr></property></Properties>'
docx = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Hello metadata world</w:t></w:r></w:p></w:body></w:document>'
thumb = io.BytesIO(); im.resize((32, 24)).save(thumb, 'JPEG')
with zipfile.ZipFile(P('doc.docx'), 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('[Content_Types].xml', ct); z.writestr('_rels/.rels', rels); z.writestr('word/document.xml', docx)
    z.writestr('docProps/core.xml', core); z.writestr('docProps/app.xml', app); z.writestr('docProps/custom.xml', custom); z.writestr('docProps/thumbnail.jpeg', thumb.getvalue())
import openpyxl
wb = openpyxl.Workbook(); wb.active['A1'] = 'cell'; wb.properties.creator = 'XLSX Author'; wb.properties.title = 'XLSX Title'; wb.properties.lastModifiedBy = 'XLSX Editor'; wb.save(P('sheet.xlsx'))
from pptx import Presentation
pr = Presentation(); pr.slides.add_slide(pr.slide_layouts[0]); pr.core_properties.author = 'PPTX Author'; pr.core_properties.title = 'PPTX Title'; pr.save(P('deck.pptx'))
)PY";

inline const Fx& Fixtures() {
  static Fx f;
  static bool built = false;
  if (built) return f;
  built = true;
  f.dir = Fresh(L"fixtures");
  Out o = Py(kFixturePy, {f.dir.wstring()});
  if (o.code != 0) std::printf("  fixture script failed: %s\n", o.err.substr(0, 800).c_str());
  else if (!o.err.empty()) std::printf("  fixture note: %s\n", o.err.substr(0, 200).c_str());
  auto P = [&](const wchar_t* n) { return f.dir / n; };
  f.jpg = P(L"photo.jpg"); f.jpgNoGps = P(L"nogps.jpg"); f.png = P(L"photo.png"); f.webp = P(L"photo.webp"); f.tiff = P(L"photo.tiff"); f.gif = P(L"anim.gif");
  f.avif = P(L"photo.avif"); f.cover = P(L"cover.jpg"); f.pdf = P(L"doc.pdf"); f.docx = P(L"doc.docx"); f.xlsx = P(L"sheet.xlsx"); f.pptx = P(L"deck.pptx");
  f.jpgUnicode = f.dir / L"\u0635\u0648\u0631\u0629 \U0001F600 \u65e5\u0646.jpg";
  f.jpgUnicode = f.dir / L"\u0635\u0648\u0631\u0629 \U0001F600 \u65e5\u672c.jpg";
  // audio / video with ffmpeg
  std::wstring sine = L"sine=frequency=440:duration=3";
  f.mp3 = P(L"song.mp3"); f.flac = P(L"song.flac"); f.ogg = P(L"song.ogg"); f.opus = P(L"song.opus"); f.wav = P(L"song.wav"); f.m4a = P(L"song.m4a");
  auto tags = [](std::vector<std::wstring> v) { return v; };
  Ff({L"-f", L"lavfi", L"-i", sine, L"-i", f.cover.wstring(), L"-map", L"0:a", L"-map", L"1:v", L"-c:a", L"libmp3lame", L"-b:a", L"96k", L"-c:v", L"copy", L"-id3v2_version", L"3",
      L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", L"-metadata", L"album=Album X", L"-metadata", L"track=3", L"-metadata", L"date=2019", L"-metadata", L"genre=Rock",
      L"-metadata:s:v", L"title=Album cover", L"-disposition:v", L"attached_pic", f.mp3.wstring()});
  Ff({L"-f", L"lavfi", L"-i", sine, L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", L"-metadata", L"album=Album X", f.flac.wstring()});
  Ff({L"-f", L"lavfi", L"-i", sine, L"-c:a", L"libvorbis", L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", f.ogg.wstring()});
  Ff({L"-f", L"lavfi", L"-i", sine, L"-c:a", L"libopus", L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", f.opus.wstring()});
  Ff({L"-f", L"lavfi", L"-i", sine, L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", f.wav.wstring()});
  Ff({L"-f", L"lavfi", L"-i", sine, L"-c:a", L"aac", L"-metadata", L"title=Song Title", L"-metadata", L"artist=Some Artist", L"-metadata", L"album=Album X", f.m4a.wstring()});
  f.mp4 = P(L"clip.mp4"); f.mkv = P(L"clip.mkv"); f.webm = P(L"clip.webm");
  Ff({L"-f", L"lavfi", L"-i", L"testsrc=size=160x120:rate=10:duration=2", L"-f", L"lavfi", L"-i", L"sine=frequency=330:duration=2", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac",
      L"-metadata", L"title=Clip Title", L"-metadata", L"comment=a comment", L"-metadata", L"com.apple.quicktime.location.ISO6709=+24.7136+046.6753+000.000/", L"-movflags", L"use_metadata_tags", f.mp4.wstring()});
  Ff({L"-f", L"lavfi", L"-i", L"testsrc=size=160x120:rate=10:duration=2", L"-f", L"lavfi", L"-i", L"sine=frequency=330:duration=2", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac",
      L"-metadata", L"title=Clip Title", f.mkv.wstring()});
  Ff({L"-f", L"lavfi", L"-i", L"testsrc=size=160x120:rate=10:duration=2", L"-c:v", L"libvpx-vp9", L"-metadata", L"title=Clip Title", f.webm.wstring()});
  (void)tags;
  return f;
}

// ---- independent readers --------------------------------------------------------------------------------------------------------

// Hash of the decoded pixels computed by Pillow (all frames)
inline std::string PilHash(const fs::path& f) {
  Json j = PyJson(R"PY(
import hashlib
from PIL import Image, ImageSequence
im = Image.open(sys.argv[1]); h = hashlib.sha256()
for fr in ImageSequence.Iterator(im): h.update(fr.convert('RGBA').tobytes()); h.update(str(fr.size).encode())
print(json.dumps({'h': h.hexdigest()}))
)PY", {f.wstring()});
  return j.Get("h").AsString();
}
inline Json PiexifDump(const fs::path& f) {
  return PyJson(R"PY(
import piexif
d = piexif.load(sys.argv[1])
def conv(v):
    if isinstance(v, bytes): return v.decode('utf-8', 'replace')
    if isinstance(v, tuple): return [conv(x) for x in v]
    return v
res = {}
for ifd in ('0th', 'Exif', 'GPS', '1st'):
    res[ifd] = {piexif.TAGS[ifd][k]['name'] if ifd in piexif.TAGS and k in piexif.TAGS[ifd] else str(k): conv(v) for k, v in d[ifd].items()}
print(json.dumps(res))
)PY", {f.wstring()});
}
inline Json MutagenTags(const fs::path& f) {
  return PyJson(R"PY(
import mutagen
m = mutagen.File(sys.argv[1])
tags = {}
if m is not None and m.tags is not None:
    for k in m.tags.keys():
        v = m.tags[k]
        if hasattr(v, 'text'): tags[k] = [str(x) for x in v.text]
        elif isinstance(v, list): tags[k] = [str(x) if not isinstance(x, bytes) else 'bytes' for x in v]
        else: tags[k] = [str(v)[:80]]
        if k.lower() != k and not (len(k) == 4 and k[:3].isupper()): tags.setdefault(k.lower(), tags[k])
pics = 0
if m is not None:
    if hasattr(m, 'pictures'): pics += len(m.pictures)
    if m.tags is not None and any(str(k).startswith('APIC') for k in m.tags.keys()): pics += 1
    if m.tags is not None and 'covr' in m.tags.keys(): pics += 1
print(json.dumps({'tags': tags, 'pictures': pics, 'length': getattr(m.info, 'length', 0) if m is not None else 0}))
)PY", {f.wstring()});
}
inline std::string MutagenFirst(const Json& t, const char* key) {
  const Json& v = t.Get("tags").Get(key);
  return v.IsArray() && v.Size() ? v.At(0).AsString() : std::string();
}

} // namespace mt
