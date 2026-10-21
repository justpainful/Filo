// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// WriteMetadata / StripMetadata / HasGps on images: read -> edit -> read back with ExifTool AND independent readers
// (piexif, Pillow), pixel hashes unchanged, original untouched.
#include "m_util.h"

using namespace mt;
using M::Group;

struct Orig {
  uint32_t crc; uint64_t mtime;
  explicit Orig(const fs::path& p) : crc(Crc(ReadBytes(p))), mtime(MTime(p)) {}
  bool Same(const fs::path& p) const { return Crc(ReadBytes(p)) == crc && MTime(p) == mtime; }
};

static M::WriteResult Check(const fs::path& in, const M::Edits& e, const fs::path& out, bool expectVerified = true) {
  Orig o(in);
  M::WriteResult r = M::WriteMetadata(in, e, out);
  if (!r.ok) std::wprintf(L"  WriteMetadata failed: %ls\n", r.error.c_str());
  CHECK(r.ok);
  CHECK(o.Same(in));                                  // the original is never touched
  if (r.ok) {
    CHECK(Exists(r.path));
    CHECK(r.path != in);
    if (expectVerified) CHECK(r.contentVerified);
  }
  return r;
}

TEST(write_jpeg_edit_roundtrip_with_independent_readers) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_jpg");
  M::Edits e;
  e.set["IFD0:Artist"] = L"Jane Doe";
  e.set["IFD0:Copyright"] = L"(c) 2024 Jane";
  e.set["IFD0:ImageDescription"] = L"Line one\nLine two & <more>";
  e.set["IFD0:Make"] = L"Nikon";
  e.set["ExifIFD:DateTimeOriginal"] = L"2021:07:08 09:10:11";
  e.set["GPS:Position"] = L"40.7128, -74.0060, 10";
  e.set["XMP-dc:Title"] = L"\u0639\u0646\u0648\u0627\u0646 \u0639\u0631\u0628\u064a \U0001F600 \u65e5\u672c\u8a9e";
  e.set["IPTC:Keywords"] = L"\u0643\u0644\u0645\u0629\nword2\nword3";
  e.remove = {"IFD0:Model", "IFD0:Software"};
  M::WriteResult r = Check(fx.jpg, e, dir / L"edited.jpg");
  CHECK(r.path.filename() == L"edited.jpg");
  CHECK(r.verifyNote.find(L"pixels") != std::wstring::npos);

  // 1) ExifTool read-back
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK(s.ok);
  CHECK_EQ(s.ValueOf("IFD0:Artist"), std::wstring(L"Jane Doe"));
  CHECK_EQ(s.ValueOf("IFD0:Make"), std::wstring(L"Nikon"));
  CHECK_EQ(s.ValueOf("IFD0:ImageDescription"), std::wstring(L"Line one\nLine two & <more>"));
  CHECK_EQ(s.ValueOf("XMP-dc:Title"), e.set["XMP-dc:Title"]);
  CHECK(s.Find("IFD0:Model") == nullptr);
  CHECK(s.Find("IFD0:Software") == nullptr);
  CHECK(s.hasGps);
  CHECK_NEAR(s.gps.lat, 40.7128, 1e-5);
  CHECK_NEAR(s.gps.lon, -74.0060, 1e-5);
  CHECK_NEAR(s.gps.alt, 10.0, 0.01);
  // 2) piexif (independent EXIF parser)
  Json px = PiexifDump(r.path);
  CHECK_EQ(px.Get("0th").Get("Artist").AsString(), std::string("Jane Doe"));
  CHECK_EQ(px.Get("0th").Get("Make").AsString(), std::string("Nikon"));
  CHECK_EQ(px.Get("0th").Get("ImageDescription").AsString(), std::string("Line one\nLine two & <more>"));
  CHECK(!px.Get("0th").Has("Model"));
  CHECK(!px.Get("0th").Has("Software"));
  CHECK_EQ(px.Get("Exif").Get("DateTimeOriginal").AsString(), std::string("2021:07:08 09:10:11"));
  CHECK_EQ(px.Get("GPS").Get("GPSLatitudeRef").AsString(), std::string("N"));
  CHECK_EQ(px.Get("GPS").Get("GPSLongitudeRef").AsString(), std::string("W"));
  CHECK_EQ(px.Get("0th").Get("Orientation").AsInt(), (int64_t)6);          // untouched fields survive
  CHECK_EQ(px.Get("Exif").Get("ISOSpeedRatings").AsInt(), (int64_t)400);
  // 3) Pillow: pixels, ICC profile, XMP title and IPTC keywords
  CHECK_EQ(PilHash(r.path), PilHash(fx.jpg));
  Json pil = PyJson(R"PY(
from PIL import Image, IptcImagePlugin
im = Image.open(sys.argv[1]); a = Image.open(sys.argv[2])
xmp = im.getxmp()
def find(d, key):
    if isinstance(d, dict):
        for k, v in d.items():
            if k.endswith(key): return v
            r = find(v, key)
            if r is not None: return r
    return None
title = find(xmp, 'title')
txt = json.dumps(title, ensure_ascii=False)
iptc = IptcImagePlugin.getiptcinfo(im) or {}
kw = iptc.get((2, 25))
if kw is not None and not isinstance(kw, list): kw = [kw]
print(json.dumps({'icc_same': im.info.get('icc_profile') == a.info.get('icc_profile'), 'icc': im.info.get('icc_profile') is not None, 'title': txt,
                  'kw': [k.decode('utf8') for k in kw] if kw else None, 'size': list(im.size)}, ensure_ascii=False))
)PY", {r.path.wstring(), fx.jpg.wstring()});
  CHECK(pil.Get("icc").AsBool());
  CHECK(pil.Get("icc_same").AsBool());
  CHECK(pil.Get("title").AsString().find(Narrow(L"\u0639\u0646\u0648\u0627\u0646")) != std::string::npos);
  CHECK(pil.Get("kw").IsArray() && pil.Get("kw").Size() == 3);
  if (pil.Get("kw").Size() == 3) CHECK_EQ(Widen(pil.Get("kw").At(0).AsString()), std::wstring(L"\u0643\u0644\u0645\u0629"));

  // running again with the same desired name never overwrites
  M::WriteResult r2 = M::WriteMetadata(fx.jpg, e, dir / L"edited.jpg");
  CHECK(r2.ok);
  CHECK(r2.path != r.path);
  CHECK(r2.path.filename() == L"edited (1).jpg");
}

TEST(write_jpeg_unicode_input_and_output_names) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_jpg_uni");
  M::Edits e;
  e.set["IFD0:Artist"] = L"\u0645\u0647\u0646\u062f\u0633 \u0639\u0631\u0628\u064a";
  fs::path out = dir / L"\u0646\u062a\u064a\u062c\u0629 \U0001F600 \u65e5\u672c.jpg";
  M::WriteResult r = Check(fx.jpgUnicode, e, out);
  CHECK(r.path.filename() == out.filename());
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK(s.ok);
  CHECK_EQ(s.ValueOf("IFD0:Artist"), e.set["IFD0:Artist"]);
  Json px = PiexifDump(r.path);
  CHECK_EQ(px.Get("0th").Get("Artist").AsString(), Narrow(e.set["IFD0:Artist"]));
}

TEST(write_remove_groups_and_all) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_groups");
  M::Edits g;
  g.removeGroups = {Group::Camera};
  M::WriteResult r = Check(fx.jpg, g, dir / L"nocamera.jpg");
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK(s.ok);
  CHECK(s.Find("IFD0:Make") == nullptr);
  CHECK(s.Find("IFD0:Model") == nullptr);
  CHECK(s.Find("ExifIFD:LensModel") == nullptr);
  CHECK_EQ(s.ValueOf("IFD0:Artist"), std::wstring(L"John Q Public"));     // other groups untouched
  CHECK(s.hasGps);
  M::Edits a;
  a.removeAll = true;                                                         // default: orientation + ICC kept
  M::WriteResult r2 = Check(fx.jpg, a, dir / L"stripped.jpg");
  M::MetadataSet s2 = M::ReadMetadata(r2.path);
  CHECK(s2.ok);
  CHECK(s2.Find("IFD0:Artist") == nullptr);
  CHECK(!s2.hasGps);
  CHECK_EQ(s2.ValueOf("IFD0:Orientation").find(L"90") != std::wstring::npos, true);
  bool icc = false;
  for (auto& f : s2.fields) icc |= f.group1 == "ICC_Profile";
  CHECK(icc);
}

TEST(strip_profiles_on_jpeg) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_strip");
  // GPS only
  M::WriteResult g = M::StripMetadata(fx.jpg, dir / L"gps.jpg", M::StripProfile::GpsOnly);
  if (!g.ok) std::wprintf(L"  error: %ls\n", g.error.c_str());
  CHECK(g.ok);
  CHECK(g.contentVerified);
  Json px = PiexifDump(g.path);
  CHECK_EQ(px.Get("GPS").Size(), (size_t)0);
  CHECK_EQ(px.Get("0th").Get("Make").AsString(), std::string("Canon"));
  CHECK_EQ(px.Get("0th").Get("Artist").AsString(), std::string("John Q Public"));
  CHECK(!M::HasGps(g.path));
  CHECK(!M::ReadMetadata(g.path).hasGps);
  CHECK_EQ(PilHash(g.path), PilHash(fx.jpg));
  // personal info
  M::WriteResult p = M::StripMetadata(fx.jpg, dir / L"personal.jpg", M::StripProfile::PersonalInfo);
  if (!p.ok) std::wprintf(L"  error: %ls\n", p.error.c_str());
  CHECK(p.ok);
  Json pp = PiexifDump(p.path);
  CHECK(!pp.Get("0th").Has("Artist"));
  CHECK(!pp.Get("0th").Has("Software"));
  CHECK(!pp.Get("Exif").Has("BodySerialNumber"));
  CHECK_EQ(pp.Get("GPS").Size(), (size_t)0);
  CHECK_EQ(pp.Get("0th").Get("Make").AsString(), std::string("Canon"));
  CHECK_EQ(pp.Get("0th").Get("Copyright").AsString(), std::string("(c) 2020 ACME Corp"));
  CHECK_EQ(pp.Get("Exif").Get("DateTimeOriginal").AsString(), std::string("2020:05:06 07:08:09"));
  CHECK_EQ(PilHash(p.path), PilHash(fx.jpg));
  // everything except orientation + ICC
  M::WriteResult k = M::StripMetadata(fx.jpg, dir / L"keep.jpg", M::StripProfile::KeepOrientationAndICC);
  CHECK(k.ok);
  Json kp = PiexifDump(k.path);
  CHECK_EQ(kp.Get("0th").Get("Orientation").AsInt(), (int64_t)6);
  CHECK(!kp.Get("0th").Has("Make"));
  CHECK(!kp.Get("0th").Has("Artist"));
  CHECK_EQ(kp.Get("GPS").Size(), (size_t)0);
  Json pil = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nprint(json.dumps({'icc': im.info.get('icc_profile') is not None}))\n", {k.path.wstring()});
  CHECK(pil.Get("icc").AsBool());
  CHECK_EQ(PilHash(k.path), PilHash(fx.jpg));
  // all, including orientation and ICC
  M::WriteResult a = M::StripMetadata(fx.jpg, dir / L"all.jpg", M::StripProfile::All);
  CHECK(a.ok);
  Json ap = PiexifDump(a.path);
  CHECK_EQ(ap.Get("0th").Size(), (size_t)0);
  CHECK_EQ(ap.Get("Exif").Size(), (size_t)0);
  CHECK_EQ(ap.Get("GPS").Size(), (size_t)0);
  Json pil2 = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nprint(json.dumps({'icc': im.info.get('icc_profile') is not None}))\n", {a.path.wstring()});
  CHECK(!pil2.Get("icc").AsBool());
  bool rotateWarning = false;
  for (auto& w : a.warnings) rotateWarning |= w.find(L"orientation") != std::wstring::npos;
  CHECK(rotateWarning);
  CHECK_EQ(PilHash(a.path), PilHash(fx.jpg));
  CHECK_EQ(Crc(ReadBytes(fx.jpg)) != 0, true);
  // stripped files are smaller
  CHECK(ReadBytes(a.path).size() < ReadBytes(fx.jpg).size());
}

TEST(has_gps_fast_check) {
  const Fx& fx = Fixtures();
  CHECK(M::HasGps(fx.jpg));
  CHECK(M::HasGps(fx.jpgUnicode));
  CHECK(!M::HasGps(fx.jpgNoGps));
  CHECK(M::HasGps(fx.webp));
  CHECK(!M::HasGps(fx.png));
  CHECK(!M::HasGps(fx.mp3));
  CHECK(M::HasGps(fx.mp4));
  CHECK(!M::HasGps(fx.mkv));
  CHECK(!M::HasGps(fx.pdf));
  CHECK(!M::HasGps(Fresh(L"hg") / L"missing.jpg"));
  // speed: the JPEG path does not start a process
  double t0 = NowSeconds();
  for (int i = 0; i < 50; ++i) M::HasGps(fx.jpg);
  double per = (NowSeconds() - t0) / 50;
  std::printf("  HasGps(jpg): %.2f ms per call\n", per * 1000);
  CHECK(per < 0.02);
  // a GPS-carrying PNG made by us is detected through ExifTool
  fs::path dir = Fresh(L"hg_png");
  M::Edits e; e.set["GPS:Position"] = L"51.5, -0.12";
  M::WriteResult r = M::WriteMetadata(fx.png, e, dir / L"gps.png");
  CHECK(r.ok);
  CHECK(M::HasGps(r.path));
}

TEST(write_png_webp_tiff_gif) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_others");
  // PNG: text chunks + GPS
  M::Edits pe;
  pe.set["PNG:Title"] = L"\u0639\u0646\u0648\u0627\u0646 PNG";
  pe.set["PNG:Author"] = L"Someone";
  pe.set["GPS:Position"] = L"35.6762, 139.6503";
  M::WriteResult p = Check(fx.png, pe, dir / L"out.png");
  M::MetadataSet ps = M::ReadMetadata(p.path);
  CHECK_EQ(ps.ValueOf("PNG:Author"), std::wstring(L"Someone"));
  CHECK_EQ(ps.ValueOf("PNG:Title"), pe.set["PNG:Title"]);
  CHECK(ps.hasGps);
  Json pj = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nim.load()\nprint(json.dumps({'t': im.text.get('Title') if hasattr(im,'text') else None, 'a': im.text.get('Author') if hasattr(im,'text') else None, 'icc': im.info.get('icc_profile') is not None}, ensure_ascii=False))\n", {p.path.wstring()});
  CHECK_EQ(pj.Get("a").AsString(), std::string("Someone"));
  CHECK_EQ(Widen(pj.Get("t").AsString()), pe.set["PNG:Title"]);
  CHECK(pj.Get("icc").AsBool());
  CHECK_EQ(PilHash(p.path), PilHash(fx.png));
  // PNG: strip everything but ICC
  M::WriteResult ps2 = M::StripMetadata(p.path, dir / L"png-stripped.png", M::StripProfile::KeepOrientationAndICC);
  CHECK(ps2.ok);
  CHECK(!M::ReadMetadata(ps2.path).hasGps);
  CHECK(M::ReadMetadata(ps2.path).Find("PNG:Author") == nullptr);
  CHECK_EQ(PilHash(ps2.path), PilHash(fx.png));

  // WebP (EXIF + ICC)
  M::Edits we;
  we.set["IFD0:Artist"] = L"WebP Artist";
  we.remove = {"IFD0:Model"};
  M::WriteResult w = Check(fx.webp, we, dir / L"out.webp");
  Json wj = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nex = im.getexif()\nprint(json.dumps({'artist': ex.get(315), 'model': ex.get(272), 'make': ex.get(271)}))\n", {w.path.wstring()});
  CHECK_EQ(wj.Get("artist").AsString(), std::string("WebP Artist"));
  CHECK(wj.Get("model").IsNull());
  CHECK_EQ(wj.Get("make").AsString(), std::string("Canon"));
  CHECK_EQ(PilHash(w.path), PilHash(fx.webp));
  M::WriteResult ws = M::StripMetadata(fx.webp, dir / L"gps.webp", M::StripProfile::GpsOnly);
  CHECK(ws.ok);
  CHECK(!M::HasGps(ws.path));

  // TIFF
  M::Edits te;
  te.set["IFD0:Artist"] = L"TIFF New";
  te.set["IFD0:Copyright"] = L"TIFF copyright";
  M::WriteResult t = Check(fx.tiff, te, dir / L"out.tiff");
  Json tj = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nprint(json.dumps({'a': im.tag_v2.get(315), 'c': im.tag_v2.get(33432), 's': im.tag_v2.get(305)}))\n", {t.path.wstring()});
  CHECK_EQ(tj.Get("a").AsString(), std::string("TIFF New"));
  CHECK_EQ(tj.Get("c").AsString(), std::string("TIFF copyright"));
  CHECK_EQ(tj.Get("s").AsString(), std::string("TIFF Software"));
  CHECK_EQ(PilHash(t.path), PilHash(fx.tiff));

  // animated GIF: all frames identical after editing
  M::Edits ge;
  ge.set["XMP-dc:Title"] = L"Animated";
  ge.set["XMP-dc:Creator"] = L"GIF Author";
  M::WriteResult g = Check(fx.gif, ge, dir / L"out.gif");
  M::MetadataSet gs = M::ReadMetadata(g.path);
  CHECK_EQ(gs.ValueOf("XMP-dc:Title"), std::wstring(L"Animated"));
  CHECK_EQ(PilHash(g.path), PilHash(fx.gif));
  Json gj = PyJson("from PIL import Image\nim = Image.open(sys.argv[1])\nprint(json.dumps({'frames': getattr(im, 'n_frames', 1)}))\n", {g.path.wstring()});
  CHECK_EQ(gj.Get("frames").AsInt(), (int64_t)3);
}

TEST(write_avif_if_available) {
  const Fx& fx = Fixtures();
  if (!Exists(fx.avif)) { std::printf("  (no AVIF fixture on this machine: AVIF writing unverified)\n"); return; }
  fs::path dir = Fresh(L"w_avif");
  M::Edits e;
  e.set["XMP-dc:Title"] = L"AVIF title";
  M::WriteResult r = M::WriteMetadata(fx.avif, e, dir / L"out.avif");
  std::wprintf(L"  AVIF write -> ok=%d error='%ls' verified=%d note=%ls\n", (int)r.ok, r.error.c_str(), (int)r.contentVerified, r.verifyNote.c_str());
  if (r.ok) {
    CHECK_EQ(M::ReadMetadata(r.path).ValueOf("XMP-dc:Title"), std::wstring(L"AVIF title"));
    CHECK(M::CanWrite(L"avif"));
  }
}

TEST(write_errors_and_edge_cases) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_err");
  M::Edits ok; ok.set["IFD0:Artist"] = L"x";
  // missing file, folder, empty file
  CHECK(!M::WriteMetadata(dir / L"missing.jpg", ok, dir / L"o1.jpg").ok);
  CHECK(!M::WriteMetadata(dir, ok, dir / L"o2.jpg").ok);
  WriteBytes(dir / L"empty.jpg", "");
  CHECK(!M::WriteMetadata(dir / L"empty.jpg", ok, dir / L"o3.jpg").ok);
  // nothing to do
  M::WriteResult n = M::WriteMetadata(fx.jpg, M::Edits{}, dir / L"o4.jpg");
  CHECK(!n.ok);
  CHECK(n.error.find(L"nothing") != std::wstring::npos);
  // not writable format
  fs::path bmp = dir / L"a.bmp";
  Py("import sys\nfrom PIL import Image\nImage.new('RGB',(8,8),(1,2,3)).save(sys.argv[1])\n", {bmp.wstring()});
  M::WriteResult b = M::WriteMetadata(bmp, ok, dir / L"o5.bmp");
  CHECK(!b.ok);
  CHECK(b.error.find(L"not supported") != std::wstring::npos);
  CHECK(!M::CanWrite(L"bmp"));
  CHECK(M::CanWrite(L"jpg") && M::CanWrite(L"mp3") && M::CanWrite(L"pdf") && M::CanWrite(L"docx") && M::CanWrite(L"mkv") && M::CanWrite(L"heic"));
  // invalid / dangerous keys never reach the command line
  for (const char* k : {"IFD0:Artist=evil", "-all", "Artist -overwrite_original", "a b", "../x", "", "-execute", "@file", "IFD0:Artist\nfoo"}) {
    M::Edits bad; bad.set[k] = L"x";
    M::WriteResult r = M::WriteMetadata(fx.jpg, bad, dir / L"o6.jpg");
    if (r.ok) std::printf("  accepted dangerous key '%s'\n", k);
    CHECK(!r.ok);
  }
  // values that look like options or paths are plain data
  M::Edits weird;
  weird.set["IFD0:Artist"] = L"-all= -overwrite_original C:\\Windows\\win.ini <file >out \"quoted\" 'single' & more";
  M::WriteResult w = Check(fx.jpg, weird, dir / L"weird.jpg");
  CHECK_EQ(M::ReadMetadata(w.path).ValueOf("IFD0:Artist"), weird.set["IFD0:Artist"]);
  CHECK(M::ReadMetadata(w.path).hasGps);                                   // "-all=" inside a value did nothing
  // invalid GPS
  M::Edits badgps; badgps.set["GPS:Position"] = L"999, 5";
  CHECK(!M::WriteMetadata(fx.jpg, badgps, dir / L"o7.jpg").ok);
  M::Edits badgps2; badgps2.set["GPS:Position"] = L"hello";
  CHECK(!M::WriteMetadata(fx.jpg, badgps2, dir / L"o8.jpg").ok);
  // unknown tag
  M::Edits unk; unk.set["IFD0:NoSuchTagAtAll"] = L"x";
  M::WriteResult u = M::WriteMetadata(fx.jpg, unk, dir / L"o9.jpg");
  std::wprintf(L"  unknown tag -> ok=%d error='%ls'\n", (int)u.ok, u.error.c_str());
  CHECK(!u.ok);
  // read-only source file works and stays read-only
  fs::path ro = dir / L"ro.jpg";
  CopyFileW(fx.jpg.c_str(), ro.c_str(), FALSE);
  SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
  M::WriteResult r = Check(ro, ok, dir / L"from-ro.jpg");
  CHECK(r.ok);
  CHECK((GetFileAttributesW(ro.c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
  CHECK((GetFileAttributesW(r.path.c_str()) & FILE_ATTRIBUTE_READONLY) == 0);
  SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
  // corrupt JPEG: clear failure, nothing left behind
  std::string whole = ReadBytes(fx.jpg);
  WriteBytes(dir / L"cut.jpg", whole.substr(0, 3000));
  M::WriteResult c = M::WriteMetadata(dir / L"cut.jpg", ok, dir / L"o10.jpg");
  std::wprintf(L"  truncated jpeg -> ok=%d error='%ls'\n", (int)c.ok, c.error.c_str());
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) { std::wstring nm = e.path().filename().wstring(); if (nm.find(L".filo-") != std::wstring::npos || nm.find(L"_exiftool_tmp") != std::wstring::npos) ++leftovers; }
  CHECK_EQ(leftovers, (size_t)0);
}

TEST(write_extension_mismatch_is_reported) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_mismatch");
  fs::path liar = dir / L"actually-a-jpeg.png";
  CopyFileW(fx.jpg.c_str(), liar.c_str(), FALSE);
  M::Edits e; e.set["IFD0:Artist"] = L"Mismatch";
  M::WriteResult r = M::WriteMetadata(liar, e, dir / L"out.png");
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  CHECK(r.ok);
  bool warned = false;
  for (auto& w : r.warnings) warned |= w.find(L"does not match") != std::wstring::npos;
  CHECK(warned);
  CHECK_EQ(M::ReadMetadata(r.path).format, std::wstring(L"jpg"));
}

TEST(write_cancel_leaves_nothing_behind) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_cancel");
  // a big TIFF-like payload: a 250 MB PNG-less file is slow to copy + hash; use the largest cheap thing, a long WAV
  fs::path big = dir / L"big.wav";
  Out o = Run(FfmpegExe(), {L"-nostdin", L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"sine=frequency=300:duration=1800", L"-ar", L"48000", big.wstring()});
  CHECK_EQ(o.code, 0);
  JobSpec spec;
  spec.operation = L"meta"; spec.title = L"cancel test"; spec.inputs = {big};
  M::Edits e; e.set["Title"] = L"will be cancelled";
  spec.work = [big, e, dir](JobContext& ctx) {
    M::WriteResult r = M::WriteMetadata(big, e, dir / L"cancelled.wav", &ctx);
    return r.ok ? JobResult::Ok({r.path}) : JobResult::Fail(r.error);
  };
  auto& jm = JobManager::Get();
  uint64_t id = jm.Submit(spec);
  Sleep(400);
  jm.Cancel(id);
  jm.WaitFor(id, 120000);
  auto info = jm.Info(id);
  CHECK(info.has_value());
  if (info) {
    std::printf("  cancel test final state: %ls\n", JobStateName(info->state));
    CHECK(info->state == JobState::Cancelled || info->state == JobState::Completed);       // completed only if it won the race
    if (info->state == JobState::Cancelled) CHECK(!Exists(dir / L"cancelled.wav"));
  }
  size_t leftovers = 0;
  for (auto& e2 : fs::directory_iterator(dir)) if (e2.path().filename().wstring().find(L".filo-") != std::wstring::npos) ++leftovers;
  CHECK_EQ(leftovers, (size_t)0);
  (void)fx;
}
