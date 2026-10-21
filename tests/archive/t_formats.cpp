// no-em-dash:allow (test data uses CJK / Arabic code points written as \u escapes)
// Reading real-world archives made by independent tools: listing, extraction, names, timestamps, empty dirs.
#include "t_util.h"
#include <set>

using namespace filo;
using namespace filo::archive;
using namespace tu;

static const char* kZipPrelude = R"PY(
import zipfile, sys, struct
def add(z, name, data=b'', dt=(2021,5,17,13,45,30), mode=0o100644, comp=zipfile.ZIP_DEFLATED, isdir=False):
    zi = zipfile.ZipInfo('x', dt)
    zi.filename = name
    zi.compress_type = comp
    zi.create_system = 3
    zi.external_attr = (mode << 16) | (0x10 if isdir else 0)
    z.writestr(zi, data)
def patch(path, mapping):
    b = open(path, 'rb').read()
    for ph, raw in mapping.items():
        assert len(ph) == len(raw)
        b = b.replace(ph, raw)
    open(path, 'wb').write(b)
z = zipfile.ZipFile(sys.argv[1], 'w')
)PY";

bool PyMakeZip(const fs::path& out, const std::string& body) {
  Out o = Py(std::string(kZipPrelude) + body + "\nz.close()\n", {out.wstring()});
  if (o.code != 0) std::printf("  python zip failed: %s\n", o.err.substr(0, 400).c_str());
  return o.code == 0;
}

static uint64_t UnixToFiletimeForTest(int64_t s) { return (uint64_t)s * 10000000ull + 116444736000000000ull; }

static ExtractResult DoExtract(const fs::path& arc, const fs::path& dest, ExtractOptions o = {}) {
  std::error_code ec;
  fs::create_directories(dest, ec);
  return ExtractArchive(arc, dest, o);
}

static std::vector<std::wstring> Names(const ListResult& l) {
  std::vector<std::wstring> v;
  for (auto& e : l.entries) v.push_back(e.name);
  return v;
}
static bool Has(const std::vector<std::wstring>& v, const std::wstring& n) { return std::find(v.begin(), v.end(), n) != v.end(); }

// FILETIME for a local DOS date / time (what ZIP stores)
static uint64_t LocalFt(int y, int mo, int d, int h, int mi, int s) {
  SYSTEMTIME st{}; st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d; st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)s;
  FILETIME l{}, u{};
  SystemTimeToFileTime(&st, &l);
  LocalFileTimeToFileTime(&l, &u);
  return ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
}
static bool NearFt(uint64_t a, uint64_t b, double secs) { double d = (double)(a > b ? a - b : b - a) / 1e7; return d <= secs; }

TEST(fmt_zip_python_unicode_list_and_extract) {
  fs::path dir = Fresh(L"zip_py");
  fs::path zip = dir / L"u.zip";
  CHECK(PyMakeZip(zip, R"PY(
add(z, 'plain.txt', b'hello world')
add(z, 'dir/', b'', mode=0o40755, isdir=True)
add(z, 'dir/sub/ملف عربي.txt', b'arabic\n')
add(z, 'emoji \U0001F600 face.txt', b'emoji')
add(z, '日本語/ファイル.txt', b'jp', dt=(2019,12,31,23,59,58))
add(z, 'empty dir/', b'', mode=0o40755, isdir=True, dt=(2020,1,2,3,4,6))
add(z, 'big.bin', bytes(range(256)) * 9000)
add(z, 'stored.txt', b'stored data' * 50, comp=zipfile.ZIP_STORED)
add(z, 'zero.txt', b'')
)PY"));
  ListResult l = ListArchive(zip);
  CHECK(l.ok);
  CHECK(l.format == Format::Zip);
  CHECK_EQ(l.entries.size(), (size_t)9);
  auto n = Names(l);
  CHECK(Has(n, L"plain.txt"));
  CHECK(Has(n, L"dir/sub/ملف عربي.txt"));
  CHECK(Has(n, L"emoji \U0001F600 face.txt"));
  CHECK(Has(n, L"日本語/ファイル.txt"));
  CHECK(Has(n, L"empty dir/"));
  for (auto& e : l.entries) if (e.name == L"big.bin") { CHECK_EQ(e.size, (uint64_t)2304000); CHECK(e.hasCrc); CHECK(e.packedSize > 0); }
  for (auto& e : l.entries) if (e.name == L"empty dir/") CHECK(e.type == EntryType::Dir);
  CHECK(!l.hasUnsafe);

  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  CHECK(SameManifest(FilesOnly(PyManifest("zip", zip)), FilesOnly(DirManifest(out)), "zip_py vs extracted"));
  CHECK(Exists(out / L"empty dir"));
  CHECK(Exists(out / L"dir" / L"sub" / L"ملف عربي.txt"));
  // preserved modification times (DOS time is local, 2 s resolution)
  CHECK(NearFt(MTime(out / L"plain.txt"), LocalFt(2021, 5, 17, 13, 45, 30), 3.0));
  CHECK(NearFt(MTime(out / L"日本語" / L"ファイル.txt"), LocalFt(2019, 12, 31, 23, 59, 58), 3.0));
  CHECK(NearFt(MTime(out / L"empty dir"), LocalFt(2020, 1, 2, 3, 4, 6), 3.0));
  Record("zip>extract", r.ok, "python zipfile fixture: unicode names, empty dirs, mtimes");
}

TEST(fmt_zip_powershell_compress_archive) {
  fs::path dir = Fresh(L"zip_ps");
  fs::path src = dir / L"src";
  MakeSourceTree(src);
  fs::path zip = dir / L"ps.zip";
  fs::path pwsh = L"pwsh.exe";
  wchar_t b[MAX_PATH * 2];
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, (DWORD)std::size(b), b, nullptr)) pwsh = b;
  Out o = Run(pwsh, {L"-NoProfile", L"-NonInteractive", L"-Command", L"Compress-Archive -LiteralPath '" + src.wstring() + L"' -DestinationPath '" + zip.wstring() + L"' -Force"});
  CHECK(o.code == 0);
  CHECK(Exists(zip));
  ListResult l = ListArchive(zip);
  CHECK(l.ok);
  CHECK(!l.entries.empty());
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  Manifest srcM = WithRoot(DirManifest(src), L"src");
  Manifest got = DirManifest(out);
  // Compress-Archive may omit empty directories; compare files only
  for (auto it = srcM.begin(); it != srcM.end();) { if (it->second.kind == 'd') it = srcM.erase(it); else ++it; }
  for (auto it = got.begin(); it != got.end();) { if (it->second.kind == 'd') it = got.erase(it); else ++it; }
  CHECK(SameManifest(srcM, got, "powershell zip vs source"));
}

TEST(fmt_zip_backslash_names_become_folders) {
  fs::path dir = Fresh(L"zip_bs");
  fs::path zip = dir / L"bs.zip";
  CHECK(PyMakeZip(zip, R"PY(
add(z, 'win\\sub\\file.txt', b'from an old windows zipper')
add(z, 'win\\top.txt', b'top')
)PY"));
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  CHECK(Exists(out / L"win" / L"sub" / L"file.txt"));
  CHECK(Exists(out / L"win" / L"top.txt"));
}

TEST(fmt_zip_legacy_code_pages) {
  fs::path dir = Fresh(L"zip_legacy");
  fs::path zip = dir / L"legacy.zip";
  // raw name bytes are patched into the file so that the UTF-8 flag stays clear (what DOS / old Japanese / Chinese tools write)
  std::string body = R"PY(
import os
raws = [bytes.fromhex('6361668220946c2e747874'), bytes.fromhex('83658358836 72e747874'.replace(' ','')), bytes.fromhex('d6d0cec4cec42e747874')]
mapping = {}
for i, raw in enumerate(raws):
    ph = ('@%d@' % i).ljust(len(raw), '#')
    add(z, ph, b'data %d' % i)
    mapping[ph.encode()] = raw
z.close()
patch(sys.argv[1], mapping)
z = open(os.devnull, 'wb')
)PY";
  CHECK(PyMakeZip(zip, body));
  // The second and third names are valid in more than one DBCS code page, so use the explicit override like `unzip -O`.
  ListOptions lo;
  lo.nameEncoding = 437;
  ListResult l437 = ListArchive(zip, lo);
  CHECK(l437.ok);
  CHECK(Has(Names(l437), L"café öl.txt"));
  lo.nameEncoding = 932;
  ListResult l932 = ListArchive(zip, lo);
  CHECK(l932.ok);
  CHECK(Has(Names(l932), L"テスト.txt"));
  lo.nameEncoding = 936;
  ListResult l936 = ListArchive(zip, lo);
  CHECK(l936.ok);
  CHECK(Has(Names(l936), L"中文文.txt"));
  // automatic mode: flagged as legacy + a warning, never a failure
  ListResult la = ListArchive(zip);
  CHECK(la.ok);
  bool anyLegacy = false;
  for (auto& e : la.entries) anyLegacy |= e.legacyName;
  CHECK(anyLegacy);
  CHECK(!la.warnings.empty());
  ExtractOptions eo; eo.nameEncoding = 932;
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out, eo);
  CHECK(r.ok);
  CHECK(Exists(out / L"テスト.txt"));
}

TEST(fmt_zip_streaming_zip64_and_data_descriptors) {
  fs::path dir = Fresh(L"zip_stream");
  fs::path zip = dir / L"stream.zip";
  Out o = Py(R"PY(
import zipfile, sys
class W:                     # non-seekable sink: forces data descriptors
    def __init__(s, f): s.f = f; s.n = 0
    def write(s, b): s.n += len(b); return s.f.write(b)
    def flush(s): s.f.flush()
    def tell(s): return s.n
with open(sys.argv[1], 'wb') as f:
    with zipfile.ZipFile(W(f), 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('a/descriptor.txt', b'x' * 100000)
        with z.open('zip64 forced.bin', 'w', force_zip64=True) as h: h.write(b'0123456789' * 50000)
        z.writestr('b.txt', b'tail')
)PY", {zip.wstring()});
  CHECK(o.code == 0);
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  CHECK(SameManifest(FilesOnly(PyManifest("zip", zip)), FilesOnly(DirManifest(out)), "streaming zip"));
}

TEST(fmt_zip_bzip2_and_lzma_members_via_bsdtar) {
  fs::path dir = Fresh(L"zip_bz");
  fs::path zip = dir / L"bz.zip";
  Out o = Py(R"PY(
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr(zipfile.ZipInfo('bz.txt', (2022,1,1,0,0,0)), b'bzip2 member ' * 1000, compress_type=zipfile.ZIP_BZIP2)
    z.writestr(zipfile.ZipInfo('lz.txt', (2022,1,1,0,0,0)), b'lzma member ' * 1000, compress_type=zipfile.ZIP_LZMA)
    z.writestr(zipfile.ZipInfo('def.txt', (2022,1,1,0,0,0)), b'deflate member ' * 1000, compress_type=zipfile.ZIP_DEFLATED)
)PY", {zip.wstring()});
  CHECK(o.code == 0);
  ListResult l = ListArchive(zip);
  CHECK(l.ok);
  CHECK_EQ(l.entries.size(), (size_t)3);
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  std::string expect;
  for (int i = 0; i < 1000; ++i) expect += "bzip2 member ";
  CHECK_EQ(ReadBytes(out / L"bz.txt"), expect);
  CHECK_EQ(ReadBytes(out / L"lz.txt").size(), (size_t)12000);
  CHECK_EQ(ReadBytes(out / L"def.txt").size(), (size_t)15000);
}

TEST(fmt_tar_variants_from_python_and_bsdtar) {
  fs::path dir = Fresh(L"tar_var");
  fs::path src = dir / L"src";
  MakeSourceTree(src);
  for (const char* fmt : {"USTAR_FORMAT", "GNU_FORMAT", "PAX_FORMAT"}) {
    fs::path tar = dir / (std::wstring(L"py_") + Widen(fmt) + L".tar");
    Out o = Py(R"PY(
import tarfile, sys, io, time
fmt = getattr(tarfile, sys.argv[2])
longdir = 'd' * 40 + '/' + 'e' * 40 + '/' + 'f' * 40
with tarfile.open(sys.argv[1], 'w', format=fmt) as t:
    def add(name, data, mtime=1600000000):
        ti = tarfile.TarInfo(name); ti.size = len(data); ti.mtime = mtime; ti.mode = 0o644
        t.addfile(ti, io.BytesIO(data))
    add('short.txt', b'short')
    add(longdir + '/file with a quite long name ' + 'x' * 40 + '.txt', b'long path')
    if sys.argv[2] != 'USTAR_FORMAT': add('n' * 150 + '.txt', b'long name only')
    ti = tarfile.TarInfo('folder'); ti.type = tarfile.DIRTYPE; ti.mode = 0o755; ti.mtime = 1500000000; t.addfile(ti)
    if sys.argv[2] != 'USTAR_FORMAT':
        add('uni/ملف عربي \U0001F600.txt', b'unicode')
)PY", {tar.wstring(), Widen(fmt)});
    if (o.code != 0) std::printf("  python: %s\n", o.err.c_str());
    CHECK(o.code == 0);
    ListResult l = ListArchive(tar);
    CHECK(l.ok);
    CHECK(l.format == Format::Tar);
    fs::path out = dir / (L"out_" + Widen(fmt));
    ExtractResult r = DoExtract(tar, out);
    CHECK(r.ok);
    if (!r.ok) std::wprintf(L"  %hs error: %ls\n", fmt, r.error.c_str());
    CHECK(SameManifest(FilesOnly(PyManifest("tar", tar)), FilesOnly(DirManifest(out)), fmt));
    CHECK(Exists(out / L"folder"));
    CHECK(NearFt(MTime(out / L"short.txt"), UnixToFiletimeForTest(1600000000), 1.0));
  }
  // bsdtar (tar.exe) writes ./ prefixed pax archives
  fs::path btar = dir / L"bsd.tar";
  Out o = Run(TarExe(), {L"-cf", btar.wstring(), L"-C", src.wstring(), L"."});
  CHECK(o.code == 0);
  fs::path out = dir / L"out_bsd";
  ExtractResult r = DoExtract(btar, out);
  CHECK(r.ok);
  CHECK(SameManifest(DirManifest(src), DirManifest(out), "bsdtar tar"));
  Record("tar>extract", r.ok, "python tarfile (ustar/gnu/pax) + bsdtar fixtures");
}

TEST(fmt_tar_gz_tgz_and_bsdtar_czf) {
  fs::path dir = Fresh(L"tgz");
  fs::path src = dir / L"src";
  MakeSourceTree(src);
  fs::path tgz = dir / L"bsd.tar.gz";
  Out o = Run(TarExe(), {L"-czf", tgz.wstring(), L"-C", src.wstring(), L"."});
  CHECK(o.code == 0);
  ListResult l = ListArchive(tgz);
  CHECK(l.ok);
  CHECK(l.format == Format::TarGz);
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(tgz, out);
  CHECK(r.ok);
  CHECK(SameManifest(DirManifest(src), DirManifest(out), "tar.gz"));
  fs::path py = dir / L"py.tgz";
  o = Py(R"PY(
import tarfile, sys
with tarfile.open(sys.argv[1], 'w:gz') as t: t.add(sys.argv[2], arcname='root')
)PY", {py.wstring(), src.wstring()});
  CHECK(o.code == 0);
  fs::path out2 = dir / L"out2";
  r = DoExtract(py, out2);
  CHECK(r.ok);
  CHECK(SameManifest(WithRoot(DirManifest(src), L"root"), DirManifest(out2), "python tgz"));
  Record("gz>extract", r.ok, "tar.gz from bsdtar and python; single-file and multi-member gz");
}

TEST(fmt_gz_single_file_and_multimember) {
  fs::path dir = Fresh(L"gz1");
  fs::path gz = dir / L"anything.gz";
  Out o = Py(R"PY(
import gzip, sys, io
buf = open(sys.argv[1], 'wb')
g = gzip.GzipFile(filename='stored name.txt', mode='wb', fileobj=buf, mtime=1600000000)
g.write(b'single file payload\n' * 5000); g.close(); buf.close()
with gzip.GzipFile(filename='', mode='wb', fileobj=open(sys.argv[2], 'wb'), mtime=0) as g2: g2.write(b'nameless')
with open(sys.argv[3], 'wb') as f:
    for part in (b'first member ', b'second member'):
        f.write(gzip.compress(part))
)PY", {gz.wstring(), (dir / L"report.csv.gz").wstring(), (dir / L"multi.gz").wstring()});
  CHECK(o.code == 0);
  ListResult l = ListArchive(gz);
  CHECK(l.ok);
  CHECK(l.format == Format::Gz);
  CHECK_EQ(l.entries.size(), (size_t)1);
  CHECK(l.entries[0].name == L"stored name.txt");
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(gz, out);
  CHECK(r.ok);
  CHECK_EQ(FileSize(out / L"stored name.txt"), (uint64_t)100000);
  CHECK(NearFt(MTime(out / L"stored name.txt"), UnixToFiletimeForTest(1600000000), 1.0));
  fs::path out2 = dir / L"out2";
  r = DoExtract(dir / L"report.csv.gz", out2);
  CHECK(r.ok);
  CHECK_EQ(ReadBytes(out2 / L"report.csv"), std::string("nameless"));
  fs::path out3 = dir / L"out3";
  r = DoExtract(dir / L"multi.gz", out3);
  CHECK(r.ok);
  CHECK_EQ(ReadBytes(out3 / L"multi"), std::string("first member second member"));
}

TEST(fmt_gz_header_name_cannot_escape) {
  fs::path dir = Fresh(L"gz_evil");
  fs::path gz = dir / L"e.gz";
  Out o = Py(R"PY(
import gzip, sys
with open(sys.argv[1], 'wb') as f:
    g = gzip.GzipFile(filename='..\\..\\evil.txt', mode='wb', fileobj=f, mtime=0); g.write(b'x'); g.close()
)PY", {gz.wstring()});
  CHECK(o.code == 0);
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(gz, out);
  CHECK(r.ok);
  CHECK(Exists(out / L"evil.txt"));                    // only the final component is kept
  CHECK(!Exists(dir / L"evil.txt"));
}

TEST(fmt_7z_unicode_list_extract) {
  CHECK(BsdtarCanRead7z());
  fs::path dir = Fresh(L"sevenz");
  fs::path src = dir / L"src";
  MakeSourceTree(src);
  fs::path z7 = dir / L"a.7z";
  Out o = Run(SevenZipExe(), {L"a", L"-t7z", L"-bso0", L"-bsp0", z7.wstring(), (src / L"*").wstring()});
  CHECK(o.code == 0);
  ListResult l = ListArchive(z7);
  CHECK(l.ok);
  if (!l.ok) std::wprintf(L"  error: %ls\n", l.error.c_str());
  CHECK(l.format == Format::SevenZ);
  CHECK(Has(Names(l), L"مرحبا/ملف عربي.txt"));
  CHECK(Has(Names(l), L"日本語/ファイル \U0001F600.txt"));
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(z7, out);
  CHECK(r.ok);
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  CHECK(SameManifest(DirManifest(src), DirManifest(out), "7z"));
  Record("7z>extract", r.ok, "7-Zip 'a -t7z' fixture, unicode names, empty dirs; read via bsdtar -> pax tar");
}

// ---- hand-assembled RAR5 (stored) -------------------------------------------------------------------------------------------
const char* kRarPy = R"PY(
import zlib, struct, sys
def vint(n):
    out = bytearray()
    while True:
        b = n & 0x7f; n >>= 7
        if n: out.append(b | 0x80)
        else: out.append(b); break
    return bytes(out)
def block(body):
    size = vint(len(body))
    return struct.pack('<I', zlib.crc32(size + body) & 0xffffffff) + size + body
def file_hdr(name, data, is_dir=False, mtime=1700000000):
    nb = name.encode('utf8')
    flags = 0x2 if not is_dir else 0
    ff = 0x2 | (0x4 if not is_dir else 0) | (0x1 if is_dir else 0)
    body = vint(2) + vint(flags)
    if not is_dir: body += vint(len(data))
    body += vint(ff) + vint(len(data)) + vint(0x10 if is_dir else 0x20)
    body += struct.pack('<I', mtime)
    if not is_dir: body += struct.pack('<I', zlib.crc32(data) & 0xffffffff)
    body += vint(0) + vint(0) + vint(len(nb)) + nb
    return block(body) + data
entries = [
    ('docs', b'', True),
    ('docs/readme.txt', b'rar readme\n' * 20, False),
    ('docs/ملف عربي.txt', 'arabic in rar\n'.encode('utf8'), False),
    ('日本語 \U0001F600.bin', bytes(range(256)) * 40, False),
    ('empty', b'', True),
    ('zero.txt', b'', False),
]
out = b'Rar!\x1a\x07\x01\x00' + block(bytes([1, 0, 0]))
for n, d, isdir in entries: out += file_hdr(n, d, isdir)
out += block(bytes([5, 0, 0]))
open(sys.argv[1], 'wb').write(out)
)PY";

fs::path MakeRar(const fs::path& dir) {
  fs::path rar = dir / L"handmade.rar";
  Out o = Py(kRarPy, {rar.wstring()});
  if (o.code != 0) std::printf("  rar builder failed: %s\n", o.err.c_str());
  return rar;
}

TEST(fmt_rar5_handmade_stored) {
  CHECK(BsdtarCanReadRar());
  fs::path dir = Fresh(L"rar5");
  fs::path rar = MakeRar(dir);
  CHECK(Exists(rar));
  CHECK(DetectFormat(rar) == Format::Rar);
  ListResult l = ListArchive(rar);
  CHECK(l.ok);
  if (!l.ok) std::wprintf(L"  error: %ls\n", l.error.c_str());
  CHECK_EQ(l.entries.size(), (size_t)6);
  CHECK(Has(Names(l), L"docs/ملف عربي.txt"));
  CHECK(Has(Names(l), L"日本語 \U0001F600.bin"));
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(rar, out);
  CHECK(r.ok);
  CHECK_EQ(ReadBytes(out / L"docs" / L"readme.txt").size(), (size_t)220);
  std::string seq;
  for (int i = 0; i < 40; ++i) for (int b = 0; b < 256; ++b) seq.push_back((char)b);
  CHECK_EQ(Crc(ReadBytes(out / L"日本語 \U0001F600.bin")), Crc(seq));
  CHECK(Exists(out / L"empty"));
  CHECK_EQ(FileSize(out / L"zero.txt"), (uint64_t)0);
  Record("rar>extract", r.ok, "hand-assembled stored RAR5 (no RAR encoder offline); read via bsdtar -> pax tar");
}

TEST(fmt_detects_by_content_not_extension) {
  fs::path dir = Fresh(L"wrongext");
  fs::path zip = dir / L"real.zip";
  CHECK(PyMakeZip(zip, "add(z, 'a.txt', b'content')\n"));
  fs::path renamed = dir / L"actually_a_zip.rar";
  CopyFileW(zip.c_str(), renamed.c_str(), FALSE);
  CHECK(DetectFormat(renamed) == Format::Zip);
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(renamed, out);
  CHECK(r.ok);
  CHECK_EQ(ReadBytes(out / L"a.txt"), std::string("content"));
  fs::path junk = dir / L"junk.zip";
  WriteBytes(junk, "this is not an archive at all, just text");
  ListResult l = ListArchive(junk);
  CHECK(!l.ok);
  CHECK(!l.error.empty());
  WriteBytes(dir / L"empty.zip", "");
  CHECK(!ListArchive(dir / L"empty.zip").ok);
  CHECK(!ListArchive(dir / L"nope.zip").ok);
}

TEST(fmt_empty_zip_is_reported_empty) {
  fs::path dir = Fresh(L"emptyzip");
  fs::path zip = dir / L"e.zip";
  CHECK(PyMakeZip(zip, "pass\n"));
  ListResult l = ListArchive(zip);
  CHECK(l.ok);
  CHECK_EQ(l.entries.size(), (size_t)0);
  ExtractResult r = DoExtract(zip, dir / L"out");
  CHECK(!r.ok);
  CHECK(r.error.find(L"empty") != std::wstring::npos);
}

TEST(fmt_duplicate_and_case_colliding_names_are_renamed_not_overwritten) {
  fs::path dir = Fresh(L"collide");
  fs::path zip = dir / L"c.zip";
  CHECK(PyMakeZip(zip, R"PY(
add(z, 'Readme.txt', b'one')
add(z, 'README.TXT', b'two')
add(z, 'readme.txt', b'three')
add(z, 'Readme.txt', b'four')
add(z, 'Dir/a.txt', b'd1')
add(z, 'dir/b.txt', b'd2')
add(z, 'file', b'plain file')
add(z, 'file/inner.txt', b'inside what used to be a file')
)PY"));
  fs::path out = dir / L"out";
  ExtractResult r = DoExtract(zip, out);
  CHECK(r.ok);
  CHECK(!r.warnings.empty());
  int found = 0;
  std::set<std::string> contents;
  for (auto& de : fs::directory_iterator(out)) {
    std::wstring n = ToLower(de.path().filename().wstring());
    if (n.rfind(L"readme", 0) == 0) { ++found; contents.insert(ReadBytes(de.path())); }
  }
  CHECK_EQ(found, 4);
  CHECK_EQ(contents.size(), (size_t)4);
  int dirs = 0;
  for (auto& de : fs::directory_iterator(out)) if (de.is_directory() && ToLower(de.path().filename().wstring()).rfind(L"dir", 0) == 0) ++dirs;
  CHECK_EQ(dirs, 1);
  CHECK(Exists(out / L"file"));
  CHECK(Exists(out / L"file (1)" / L"inner.txt"));
}
