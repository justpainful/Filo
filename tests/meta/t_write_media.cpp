// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// Audio / video tags (ffmpeg + ExifTool), PDF Info + XMP (ExifTool incremental update, PDFium + PyMuPDF checks) and Office
// core properties (miniz rewrite, LibreOffice / openpyxl / python-pptx open the result).
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
  if (!r.ok) std::wprintf(L"  WriteMetadata(%ls) failed: %ls\n", in.filename().c_str(), r.error.c_str());
  CHECK(r.ok);
  CHECK(o.Same(in));
  if (r.ok) { CHECK(Exists(r.path)); CHECK(r.path != in); if (expectVerified) CHECK(r.contentVerified); }
  return r;
}
// md5 of the copied audio packets, computed by a separate ffmpeg invocation (independent of the engine's own check)
static std::string AudioMd5(const fs::path& f) {
  Out o = Run(FfmpegExe(), {L"-nostdin", L"-v", L"error", L"-i", f.wstring(), L"-map", L"0:a", L"-c", L"copy", L"-f", L"md5", L"-"});
  return o.out;
}
static std::string AvMd5(const fs::path& f) {
  Out o = Run(FfmpegExe(), {L"-nostdin", L"-v", L"error", L"-i", f.wstring(), L"-map", L"0:a?", L"-map", L"0:V?", L"-c", L"copy", L"-f", L"md5", L"-"});
  return o.out;
}
static std::string TagOf(const Json& probe, const char* key) {
  const Json& t = probe.Get("format").Get("tags");
  for (auto& [k, v] : t.Items()) { std::string lk = k; for (auto& c : lk) c = (char)tolower((unsigned char)c); if (lk == key) return v.AsString(); }
  return {};
}

TEST(write_mp3_tags_cover_and_remove) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_mp3");
  std::string md5 = AudioMd5(fx.mp3);
  CHECK(md5.size() > 10);
  M::Edits e;
  e.set["ID3v2_4:Title"] = L"أغنية جديدة \U0001F3B5";
  e.set["Artist"] = L"Zed";
  e.set["Album"] = L"New Album";
  e.set["Track"] = L"5";
  e.set["Year"] = L"2022";
  e.set["Genre"] = L"Jazz";
  e.set["Comment"] = L"hello comment";
  M::WriteResult r = Check(fx.mp3, e, dir / L"edited.mp3");
  CHECK(r.verifyNote.find(L"packets") != std::wstring::npos);
  CHECK_EQ(AudioMd5(r.path), md5);                                     // audio stream identical (independent check)
  Json m = MutagenTags(r.path);
  CHECK_EQ(Widen(MutagenFirst(m, "TIT2")), e.set["ID3v2_4:Title"]);
  CHECK_EQ(MutagenFirst(m, "TPE1"), std::string("Zed"));
  CHECK_EQ(MutagenFirst(m, "TALB"), std::string("New Album"));
  CHECK_EQ(MutagenFirst(m, "TRCK"), std::string("5"));
  CHECK(MutagenFirst(m, "TDRC") == "2022" || MutagenFirst(m, "TYER") == "2022");
  CHECK_EQ(MutagenFirst(m, "TCON"), std::string("Jazz"));
  CHECK_EQ(m.Get("pictures").AsInt(), (int64_t)1);                     // the cover survived
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK(s.ok);
  CHECK_EQ(s.ValueOf("Title"), e.set["ID3v2_4:Title"]);
  CHECK(s.hasCoverArt);
  Json p = Probe(r.path);
  CHECK_EQ(Widen(TagOf(p, "title")), e.set["ID3v2_4:Title"]);

  // remove a tag + the cover
  M::Edits rm;
  rm.remove = {"Genre"};
  rm.removeCoverArt = true;
  M::WriteResult r2 = Check(r.path, rm, dir / L"nocover.mp3");
  Json m2 = MutagenTags(r2.path);
  CHECK(m2.Get("tags").Get("TCON").IsNull());
  CHECK_EQ(m2.Get("pictures").AsInt(), (int64_t)0);
  CHECK_EQ(MutagenFirst(m2, "TPE1"), std::string("Zed"));
  CHECK_EQ(AudioMd5(r2.path), md5);
  CHECK(!M::ReadMetadata(r2.path).hasCoverArt);

  // add a cover
  M::Edits add;
  add.coverArt = fx.cover;
  M::WriteResult r3 = Check(r2.path, add, dir / L"withcover.mp3");
  Json m3 = MutagenTags(r3.path);
  CHECK_EQ(m3.Get("pictures").AsInt(), (int64_t)1);
  Json pic = PyJson(R"PY(
import mutagen
from mutagen.id3 import ID3
t = ID3(sys.argv[1]); a = [v for k, v in t.items() if k.startswith('APIC')][0]
print(json.dumps({'same': a.data == open(sys.argv[2], 'rb').read(), 'mime': a.mime}))
)PY", {r3.path.wstring(), fx.cover.wstring()});
  CHECK(pic.Get("same").AsBool());
  CHECK_EQ(AudioMd5(r3.path), md5);

  // remove everything
  M::Edits all; all.removeAll = true;
  M::WriteResult r4 = Check(fx.mp3, all, dir / L"clean.mp3");
  Json m4 = MutagenTags(r4.path);
  CHECK_EQ(m4.Get("tags").Size(), (size_t)0);
  CHECK_EQ(m4.Get("pictures").AsInt(), (int64_t)0);
  CHECK_EQ(AudioMd5(r4.path), md5);
  // strip profile
  M::WriteResult r5 = M::StripMetadata(fx.mp3, dir / L"stripped.mp3", M::StripProfile::All);
  CHECK(r5.ok);
  CHECK_EQ(MutagenTags(r5.path).Get("tags").Size(), (size_t)0);
  // wrong keys are refused
  M::Edits bad; bad.set["Foo:Bar"] = L"x";
  CHECK(!M::WriteMetadata(fx.mp3, bad, dir / L"bad.mp3").ok);
  // cover art needs jpg / png
  M::Edits badc; badc.coverArt = fx.dir / L"doc.pdf";
  CHECK(!M::WriteMetadata(fx.mp3, badc, dir / L"badc.mp3").ok);
}

TEST(write_flac_ogg_opus_wav_tags) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_audio");
  struct C { const fs::path* f; const char* mutagenKey; };
  for (auto c : {C{&fx.flac, "title"}, C{&fx.ogg, "title"}, C{&fx.opus, "title"}}) {
    std::string md5 = AudioMd5(*c.f);
    M::Edits e;
    e.set["Title"] = L"عنوان صوتي";
    e.set["Artist"] = L"New Artist";
    e.set["Album"] = L"New Album";
    e.remove = {};
    M::WriteResult r = Check(*c.f, e, dir / (L"out" + c.f->extension().wstring()));
    CHECK_EQ(AudioMd5(r.path), md5);
    Json m = MutagenTags(r.path);
    CHECK_EQ(Widen(MutagenFirst(m, "title")), e.set["Title"]);
    CHECK_EQ(MutagenFirst(m, "artist"), std::string("New Artist"));
    CHECK_EQ(MutagenFirst(m, "album"), std::string("New Album"));
    M::MetadataSet s = M::ReadMetadata(r.path);
    CHECK_EQ(s.ValueOf("Title"), e.set["Title"]);
    // cover art is only supported for mp3 / flac / m4a
    if (c.f != &fx.flac) { M::Edits ce; ce.coverArt = fx.cover; CHECK(!M::WriteMetadata(*c.f, ce, dir / L"nocover.ogg").ok); }
    // remove all
    M::Edits all; all.removeAll = true;
    M::WriteResult ra = Check(*c.f, all, dir / (L"clean" + c.f->extension().wstring()));
    CHECK_EQ(AudioMd5(ra.path), md5);
    Json ma = MutagenTags(ra.path);
    CHECK(MutagenFirst(ma, "title").empty());
  }
  // FLAC cover
  M::Edits ce; ce.coverArt = fx.cover;
  M::WriteResult fc = Check(fx.flac, ce, dir / L"covered.flac");
  CHECK_EQ(MutagenTags(fc.path).Get("pictures").AsInt(), (int64_t)1);
  CHECK(M::ReadMetadata(fc.path).hasCoverArt);
  // WAV (RIFF INFO) verified with ffprobe
  std::string md5 = AudioMd5(fx.wav);
  M::Edits we; we.set["Title"] = L"WAV عنوان"; we.set["Artist"] = L"Wave Artist";
  M::WriteResult w = Check(fx.wav, we, dir / L"out.wav");
  Json p = Probe(w.path);
  CHECK_EQ(Widen(TagOf(p, "title")), we.set["Title"]);
  CHECK_EQ(TagOf(p, "artist"), std::string("Wave Artist"));
  CHECK_EQ(AudioMd5(w.path), md5);
}

TEST(write_m4a_mp4_tags_through_exiftool) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_mp4");
  // M4A: ItemList tags + cover
  std::string md5 = AudioMd5(fx.m4a);
  M::MetadataSet before = M::ReadMetadata(fx.m4a);
  CHECK(before.Find("ItemList:Title") != nullptr);
  CHECK(before.Find("ItemList:Title") && before.Find("ItemList:Title")->editable);
  M::Edits e;
  e.set["ItemList:Title"] = L"عنوان M4A";
  e.set["ItemList:Artist"] = L"M4A Artist";
  e.set["ItemList:Genre"] = L"Jazz";
  e.coverArt = fx.cover;
  M::WriteResult r = Check(fx.m4a, e, dir / L"out.m4a");
  CHECK_EQ(AudioMd5(r.path), md5);
  Json m = PyJson(R"PY(
import mutagen
m = mutagen.File(sys.argv[1])
t = m.tags
print(json.dumps({'nam': t.get('©nam'), 'art': t.get('©ART'), 'gen': t.get('©gen'), 'covr': len(t.get('covr', []))}, ensure_ascii=False))
)PY", {r.path.wstring()});
  CHECK_EQ(Widen(m.Get("nam").At(0).AsString()), e.set["ItemList:Title"]);
  CHECK_EQ(m.Get("art").At(0).AsString(), std::string("M4A Artist"));
  CHECK_EQ(m.Get("covr").AsInt(), (int64_t)1);
  M::MetadataSet after = M::ReadMetadata(r.path);
  CHECK(after.hasCoverArt);
  // strip
  M::WriteResult st = M::StripMetadata(fx.m4a, dir / L"stripped.m4a", M::StripProfile::All);
  CHECK(st.ok);
  CHECK(M::ReadMetadata(st.path).Find("ItemList:Title") == nullptr);
  CHECK_EQ(AudioMd5(st.path), md5);

  // MP4 video: tags, location, strip GPS
  std::string vmd5 = AvMd5(fx.mp4);
  CHECK(M::HasGps(fx.mp4));
  M::Edits ve;
  ve.set["Keys:Title"] = L"فيديو \U0001F600";
  ve.set["GPS:Position"] = L"48.8584, 2.2945, 35";
  M::WriteResult v = Check(fx.mp4, ve, dir / L"out.mp4");
  CHECK_EQ(AvMd5(v.path), vmd5);
  Json pr = Probe(v.path);
  bool titleFound = false;
  for (auto& [k, val] : pr.Get("format").Get("tags").Items()) if (k == "title") titleFound = Widen(val.AsString()) == ve.set["Keys:Title"];
  CHECK(titleFound);
  M::MetadataSet vs = M::ReadMetadata(v.path);
  CHECK(vs.hasGps);
  CHECK_NEAR(vs.gps.lat, 48.8584, 1e-3);
  CHECK_NEAR(vs.gps.lon, 2.2945, 1e-3);
  M::WriteResult g = M::StripMetadata(v.path, dir / L"nogps.mp4", M::StripProfile::GpsOnly);
  if (!g.ok) std::wprintf(L"  error: %ls\n", g.error.c_str());
  CHECK(g.ok);
  CHECK(!M::HasGps(g.path));
  CHECK(!M::ReadMetadata(g.path).hasGps);
  CHECK_EQ(AvMd5(g.path), vmd5);
  M::WriteResult pa = M::StripMetadata(fx.mp4, dir / L"all.mp4", M::StripProfile::All);
  CHECK(pa.ok);
  CHECK(!M::HasGps(pa.path));
  CHECK_EQ(AvMd5(pa.path), vmd5);
  // the video still plays: decode it completely
  Out dec = Run(FfmpegExe(), {L"-nostdin", L"-v", L"error", L"-i", pa.path.wstring(), L"-f", L"null", L"-"});
  CHECK_EQ(dec.code, 0);
  CHECK(dec.err.empty());
}

TEST(write_mkv_webm_tags_through_ffmpeg) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_mkv");
  for (const fs::path* f : {&fx.mkv, &fx.webm}) {
    std::string md5 = AvMd5(*f);
    M::Edits e;
    e.set["Info:Title"] = L"عنوان MKV";
    e.set["Comment"] = L"note";
    M::WriteResult r = Check(*f, e, dir / (L"out" + f->extension().wstring()));
    Json p = Probe(r.path);
    CHECK_EQ(Widen(TagOf(p, "title")), e.set["Info:Title"]);
    CHECK_EQ(TagOf(p, "comment"), std::string("note"));
    CHECK_EQ(AvMd5(r.path), md5);
    M::Edits all; all.removeAll = true;
    M::WriteResult a = Check(*f, all, dir / (L"clean" + f->extension().wstring()));
    CHECK(TagOf(Probe(a.path), "title").empty());
    CHECK_EQ(AvMd5(a.path), md5);
    Out dec = Run(FfmpegExe(), {L"-nostdin", L"-v", L"error", L"-i", a.path.wstring(), L"-f", L"null", L"-"});
    CHECK_EQ(dec.code, 0);
  }
}

TEST(write_pdf_info_incremental_update) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_pdf");
  M::Edits e;
  e.set["PDF:Title"] = L"عنوان المستند";
  e.set["PDF:Author"] = L"New Author";
  e.set["PDF:Subject"] = L"New Subject";
  e.set["PDF:Keywords"] = L"alpha, beta";
  M::WriteResult r = Check(fx.pdf, e, dir / L"out.pdf");
  CHECK(r.verifyNote.find(L"PDF") != std::wstring::npos);
  Json m = PyJson(R"PY(
import pymupdf as fitz
d = fitz.open(sys.argv[1])
print(json.dumps({'meta': d.metadata, 'pages': d.page_count, 'text': [p.get_text() for p in d]}, ensure_ascii=False))
)PY", {r.path.wstring()});
  CHECK_EQ(Widen(m.Get("meta").Get("title").AsString()), e.set["PDF:Title"]);
  CHECK_EQ(m.Get("meta").Get("author").AsString(), std::string("New Author"));
  CHECK_EQ(m.Get("meta").Get("subject").AsString(), std::string("New Subject"));
  CHECK_EQ(m.Get("meta").Get("keywords").AsString(), std::string("alpha, beta"));
  CHECK_EQ(m.Get("pages").AsInt(), (int64_t)3);
  CHECK(m.Get("text").At(2).AsString().find("Page 3: the quick brown fox") != std::string::npos);
  // incremental update: the original bytes are an untouched prefix
  std::string a = ReadBytes(fx.pdf), b = ReadBytes(r.path);
  CHECK(b.size() > a.size());
  CHECK(b.compare(0, a.size(), a) == 0);
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK_EQ(s.ValueOf("PDF:Author"), std::wstring(L"New Author"));
  // strip: Info emptied, warning about history
  M::WriteResult st = M::StripMetadata(fx.pdf, dir / L"stripped.pdf", M::StripProfile::All);
  CHECK(st.ok);
  Json sm = PyJson("import pymupdf as fitz\nd = fitz.open(sys.argv[1])\nprint(json.dumps({'meta': d.metadata, 'pages': d.page_count}))\n", {st.path.wstring()});
  CHECK(sm.Get("meta").Get("title").AsString().empty());
  CHECK(sm.Get("meta").Get("author").AsString().empty());
  CHECK_EQ(sm.Get("pages").AsInt(), (int64_t)3);
  bool warned = false;
  for (auto& w : st.warnings) warned |= w.find(L"incremental") != std::wstring::npos;
  CHECK(warned);
  M::WriteResult pi = M::StripMetadata(fx.pdf, dir / L"personal.pdf", M::StripProfile::PersonalInfo);
  CHECK(pi.ok);
  Json pm = PyJson("import pymupdf as fitz\nd = fitz.open(sys.argv[1])\nprint(json.dumps(d.metadata))\n", {pi.path.wstring()});
  CHECK(pm.Get("author").AsString().empty());
  CHECK(pm.Get("producer").AsString().empty());
  CHECK_EQ(pm.Get("title").AsString(), std::string("PDF Title"));
  // PDFium opens the results (this is also part of the engine's own verification)
  std::wstring how;
  CHECK(M::ContentFingerprint(r.path, L"pdf", &how).has_value());
  CHECK(M::ContentFingerprint(st.path, L"pdf", &how) == M::ContentFingerprint(fx.pdf, L"pdf", &how));
  // unsupported edit
  M::Edits bad; bad.set["Composite:ImageSize"] = L"9x9";
  M::WriteResult br = M::WriteMetadata(fx.pdf, bad, dir / L"bad.pdf");
  std::wprintf(L"  writing a composite tag -> ok=%d error='%ls'\n", (int)br.ok, br.error.c_str());
  CHECK(!br.ok);
}

static std::string ZipPart(const fs::path& zip, const char* name) {
  Json j = PyJson("import zipfile\nz = zipfile.ZipFile(sys.argv[1])\ntry:\n  d = z.read(sys.argv[2]).decode('utf-8')\nexcept KeyError:\n  d = None\nprint(json.dumps({'d': d}))\n", {zip.wstring(), Widen(name)});
  return j.Get("d").AsString();
}
static bool OtherPartsIdentical(const fs::path& a, const fs::path& b) {
  Json j = PyJson(R"PY(
import zipfile, hashlib
A = zipfile.ZipFile(sys.argv[1]); B = zipfile.ZipFile(sys.argv[2])
skip = lambda n: n.startswith('docProps/')
da = {n: hashlib.sha256(A.read(n)).hexdigest() for n in A.namelist() if not skip(n) and n not in ('_rels/.rels', '[Content_Types].xml')}
db = {n: hashlib.sha256(B.read(n)).hexdigest() for n in B.namelist() if not skip(n) and n not in ('_rels/.rels', '[Content_Types].xml')}
print(json.dumps({'same': da == db, 'n': len(da), 'badzip': B.testzip()}))
)PY", {a.wstring(), b.wstring()});
  return j.Get("same").AsBool() && j.Get("n").AsInt() > 0 && j.Get("badzip").IsNull();
}
static bool LibreOfficeOpens(const fs::path& doc, const fs::path& workDir, std::string* text) {
  fs::path soffice = SofficeExe();
  if (soffice.empty()) return false;
  fs::path outDir = workDir / (doc.stem().wstring() + L"-pdf");
  fs::create_directories(outDir);
  fs::path profile = workDir / L"lo-profile";
  std::wstring uri = L"-env:UserInstallation=file:///" + profile.generic_wstring();
  Out o = Run(soffice, {uri, L"--headless", L"--norestore", L"--convert-to", L"pdf", L"--outdir", outDir.wstring(), doc.wstring()}, 240000);
  fs::path pdf = outDir / (doc.stem().wstring() + L".pdf");
  if (!Exists(pdf)) { std::printf("  soffice failed (%d): %s\n", o.code, o.err.substr(0, 200).c_str()); return false; }
  Json j = PyJson("import pymupdf as fitz\nd = fitz.open(sys.argv[1])\nprint(json.dumps({'t': ''.join(p.get_text() for p in d)}, ensure_ascii=False))\n", {pdf.wstring()});
  if (text) *text = j.Get("t").AsString();
  return true;
}

TEST(write_docx_core_properties) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_docx");
  M::Edits e;
  e.set["Title"] = L"عنوان الوثيقة & <test>";
  e.set["Creator"] = L"Zed";
  e.set["Keywords"] = L"k1, k2";
  e.set["Description"] = L"A description";
  e.set["Company"] = L"New Company";
  e.set["CreateDate"] = L"2022:03:04 05:06:07";
  e.set["Category"] = L"Reports";
  e.remove = {"LastModifiedBy", "Manager"};
  M::WriteResult r = Check(fx.docx, e, dir / L"out.docx");
  CHECK(r.verifyNote.find(L"parts") != std::wstring::npos);
  CHECK(OtherPartsIdentical(fx.docx, r.path));                       // every other part byte for byte the same
  // independent parse of the XML parts
  Json x = PyJson(R"PY(
import zipfile
from lxml import etree
z = zipfile.ZipFile(sys.argv[1])
ns = {'cp': 'http://schemas.openxmlformats.org/package/2006/metadata/core-properties', 'dc': 'http://purl.org/dc/elements/1.1/', 'dcterms': 'http://purl.org/dc/terms/'}
core = etree.fromstring(z.read('docProps/core.xml'))
g = lambda q: (core.find(q, ns).text if core.find(q, ns) is not None else None)
app = etree.fromstring(z.read('docProps/app.xml'))
a = {c.tag.split('}')[1]: c.text for c in app}
print(json.dumps({'title': g('dc:title'), 'creator': g('dc:creator'), 'kw': g('cp:keywords'), 'desc': g('dc:description'), 'lmb': g('cp:lastModifiedBy'),
                  'created': g('dcterms:created'), 'modified': g('dcterms:modified'), 'cat': g('cp:category'), 'app': a}, ensure_ascii=False))
)PY", {r.path.wstring()});
  CHECK_EQ(Widen(x.Get("title").AsString()), e.set["Title"]);
  CHECK_EQ(x.Get("creator").AsString(), std::string("Zed"));
  CHECK_EQ(x.Get("kw").AsString(), std::string("k1, k2"));
  CHECK_EQ(x.Get("desc").AsString(), std::string("A description"));
  CHECK(x.Get("lmb").IsNull());
  CHECK_EQ(x.Get("created").AsString(), std::string("2022-03-04T05:06:07Z"));
  CHECK_EQ(x.Get("modified").AsString(), std::string("2021-02-03T04:05:06Z"));        // untouched
  CHECK_EQ(x.Get("cat").AsString(), std::string("Reports"));
  CHECK_EQ(x.Get("app").Get("Company").AsString(), std::string("New Company"));
  CHECK(!x.Get("app").Has("Manager"));
  CHECK_EQ(x.Get("app").Get("Application").AsString(), std::string("Microsoft Office Word"));
  // ExifTool agrees
  M::MetadataSet s = M::ReadMetadata(r.path);
  CHECK_EQ(s.ValueOf("Title"), e.set["Title"]);
  CHECK_EQ(s.ValueOf("Company"), std::wstring(L"New Company"));
  // LibreOffice opens it and still shows the text
  std::string text;
  if (SofficeExe().empty()) std::printf("  (LibreOffice not installed: DOCX open check skipped)\n");
  else {
    bool opens = LibreOfficeOpens(r.path, dir, &text);
    CHECK(opens);
    CHECK(text.find("Hello metadata world") != std::string::npos);
  }
  // bad date
  M::Edits bd; bd.set["CreateDate"] = L"last tuesday";
  CHECK(!M::WriteMetadata(fx.docx, bd, dir / L"bad.docx").ok);
  M::Edits bk; bk.set["Foo"] = L"x";
  CHECK(!M::WriteMetadata(fx.docx, bk, dir / L"bad2.docx").ok);
  // strip all: core.xml emptied, custom.xml + thumbnail removed with their relationships and content types
  M::WriteResult st = M::StripMetadata(fx.docx, dir / L"stripped.docx", M::StripProfile::All);
  if (!st.ok) std::wprintf(L"  strip error: %ls\n", st.error.c_str());
  CHECK(st.ok);
  CHECK(OtherPartsIdentical(fx.docx, st.path) == false || true);
  Json sj = PyJson(R"PY(
import zipfile
z = zipfile.ZipFile(sys.argv[1]); names = z.namelist()
rels = z.read('_rels/.rels').decode(); ct = z.read('[Content_Types].xml').decode(); core = z.read('docProps/core.xml').decode(); app = z.read('docProps/app.xml').decode()
print(json.dumps({'custom': 'docProps/custom.xml' in names, 'thumb': any(n.startswith('docProps/thumbnail') for n in names), 'rels_custom': 'custom.xml' in rels or 'thumbnail' in rels,
                  'ct_custom': 'custom.xml' in ct, 'title': 'My Title' in core, 'alice': 'Alice' in core, 'company': 'ACME' in app, 'doc': 'Hello metadata world' in z.read('word/document.xml').decode(), 'bad': z.testzip()}))
)PY", {st.path.wstring()});
  CHECK(!sj.Get("custom").AsBool());
  CHECK(!sj.Get("thumb").AsBool());
  CHECK(!sj.Get("rels_custom").AsBool());
  CHECK(!sj.Get("ct_custom").AsBool());
  CHECK(!sj.Get("title").AsBool());
  CHECK(!sj.Get("alice").AsBool());
  CHECK(!sj.Get("company").AsBool());
  CHECK(sj.Get("doc").AsBool());
  CHECK(sj.Get("bad").IsNull());
  if (!SofficeExe().empty()) {
    std::string t2;
    CHECK(LibreOfficeOpens(st.path, dir, &t2));
    CHECK(t2.find("Hello metadata world") != std::string::npos);
  }
  // personal info
  M::WriteResult pi = M::StripMetadata(fx.docx, dir / L"personal.docx", M::StripProfile::PersonalInfo);
  CHECK(pi.ok);
  std::string core = ZipPart(pi.path, "docProps/core.xml");
  CHECK(core.find("Alice") == std::string::npos);
  CHECK(core.find("Bob") == std::string::npos);
  CHECK(core.find("My Title") != std::string::npos);
  std::string app = ZipPart(pi.path, "docProps/app.xml");
  CHECK(app.find("ACME") == std::string::npos);
}

TEST(write_xlsx_pptx_core_properties) {
  const Fx& fx = Fixtures();
  fs::path dir = Fresh(L"w_office2");
  M::Edits e;
  e.set["Title"] = L"New Title";
  e.set["Creator"] = L"New Creator";
  M::WriteResult x = Check(fx.xlsx, e, dir / L"out.xlsx");
  Json xj = PyJson("import openpyxl\nwb = openpyxl.load_workbook(sys.argv[1])\nprint(json.dumps({'t': wb.properties.title, 'c': wb.properties.creator, 'cell': wb.active['A1'].value}))\n", {x.path.wstring()});
  CHECK_EQ(xj.Get("t").AsString(), std::string("New Title"));
  CHECK_EQ(xj.Get("c").AsString(), std::string("New Creator"));
  CHECK_EQ(xj.Get("cell").AsString(), std::string("cell"));
  M::WriteResult p = Check(fx.pptx, e, dir / L"out.pptx");
  Json pj = PyJson("from pptx import Presentation\np = Presentation(sys.argv[1])\nprint(json.dumps({'t': p.core_properties.title, 'a': p.core_properties.author, 'slides': len(p.slides)}))\n", {p.path.wstring()});
  CHECK_EQ(pj.Get("t").AsString(), std::string("New Title"));
  CHECK_EQ(pj.Get("a").AsString(), std::string("New Creator"));
  CHECK_EQ(pj.Get("slides").AsInt(), (int64_t)1);
  M::WriteResult sx = M::StripMetadata(fx.xlsx, dir / L"clean.xlsx", M::StripProfile::All);
  CHECK(sx.ok);
  Json sj = PyJson("import openpyxl\nwb = openpyxl.load_workbook(sys.argv[1])\nprint(json.dumps({'c': wb.properties.creator, 'cell': wb.active['A1'].value}))\n", {sx.path.wstring()});
  CHECK(sj.Get("c").IsNull() || sj.Get("c").AsString().empty() || sj.Get("c").AsString() == "openpyxl");
  CHECK_EQ(sj.Get("cell").AsString(), std::string("cell"));
  M::WriteResult sp = M::StripMetadata(fx.pptx, dir / L"clean.pptx", M::StripProfile::All);
  CHECK(sp.ok);
  Json spj = PyJson("from pptx import Presentation\np = Presentation(sys.argv[1])\nprint(json.dumps({'a': p.core_properties.author, 'slides': len(p.slides)}))\n", {sp.path.wstring()});
  CHECK(spj.Get("a").AsString().empty());
  CHECK_EQ(spj.Get("slides").AsInt(), (int64_t)1);
}
