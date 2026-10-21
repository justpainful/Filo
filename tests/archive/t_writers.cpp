// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// Writers (ZIP / TAR / TAR.GZ / GZIP): format properties checked with independent readers, ZIP64, big and many-file stress.
#include "t_util.h"
#include <set>

using namespace filo;
using namespace filo::archive;
using namespace tu;

static void MakeTree(const fs::path& root) {
  WriteBytes(root / L"notes.txt", Text(50000, 1));
  WriteBytes(root / L"photo.jpg", Pseudo(40000, 2));
  WriteBytes(root / L"bundle.zip", Pseudo(30000, 3));
  WriteBytes(root / L"\u0645\u0631\u062d\u0628\u0627" / L"\u0645\u0644\u0641 \U0001F600.txt", "arabic + emoji name\n");
  WriteBytes(root / L"empty.dat", "");
  std::error_code ec;
  fs::create_directories(root / L"blank folder", ec);
}

TEST(writer_zip_format_properties) {
  fs::path dir = Fresh(L"w_zip");
  fs::path src = dir / L"src";
  MakeTree(src);
  // give a file a known old timestamp
  {
    HANDLE h = CreateFileW((src / L"notes.txt").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYSTEMTIME st{}; st.wYear = 2018; st.wMonth = 3; st.wDay = 9; st.wHour = 10; st.wMinute = 20; st.wSecond = 30;
    FILETIME lt, ft; SystemTimeToFileTime(&st, &lt); LocalFileTimeToFileTime(&lt, &ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
  }
  CreateResult r = CreateZip({src}, dir / L"out.zip");
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  CHECK(r.ok);
  CHECK(r.path.filename() == L"out.zip");
  CHECK(r.entries >= 7);
  Out o = Py(R"PY(
import zipfile, sys, zlib
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
res = {}
for i in z.infolist():
    res[i.filename] = (i.compress_type, bool(i.flag_bits & 0x800), i.date_time, (i.external_attr >> 16) & 0o170000, i.file_size, i.is_dir())
names = sorted(res)
bad = []
for n, (ct, utf8, dt, kind, size, isdir) in res.items():
    nonascii = any(ord(c) > 127 for c in n)
    if utf8 != nonascii: bad.append('utf8flag:' + ascii(n))
    if n.endswith('.jpg') or n.endswith('.zip'):
        if ct != 0: bad.append('should be stored:' + n)
    if n.endswith('notes.txt'):
        if ct != 8: bad.append('should be deflated:' + n)
        if dt != (2018, 3, 9, 10, 20, 30): bad.append('mtime:' + str(dt))
    if isdir and kind != 0o040000: bad.append('dir mode')
    if n.endswith('blank folder/') and not isdir: bad.append('blank folder not a dir')
print('BAD', bad)
print('HAS_BLANK', 'src/blank folder/' in res)
)PY", {r.path.wstring()});
  CHECK_EQ(o.code, 0);
  if (o.out.find("BAD []") == std::string::npos) std::printf("%s\n%s\n", o.out.c_str(), o.err.c_str());
  CHECK(o.out.find("BAD []") != std::string::npos);
  CHECK(o.out.find("HAS_BLANK True") != std::string::npos);
  Out t = Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", r.path.wstring()});
  CHECK_EQ(t.code, 0);
  // extended timestamp (UT) is present so that readers get the exact second, not the 2 s DOS resolution
  std::string bytes = ReadBytes(r.path);
  CHECK(bytes.find("UT\x05") != std::string::npos);
  // includeRoot = false puts the contents at the top level
  CreateOptions flat; flat.includeRoot = false;
  CreateResult r2 = CreateZip({src}, dir / L"flat.zip", flat);
  CHECK(r2.ok);
  Manifest m = PyManifest("zip", r2.path);
  CHECK(m.count(L"notes.txt") == 1);
  CHECK(m.count(L"src/notes.txt") == 0);
  // two inputs with the same leaf name do not collide
  fs::path a = dir / L"a" / L"same.txt", b = dir / L"b" / L"same.txt";
  WriteBytes(a, "AAA"); WriteBytes(b, "BBBB");
  CreateResult r3 = CreateZip({a, b}, dir / L"dup.zip");
  CHECK(r3.ok);
  Manifest m3 = PyManifest("zip", r3.path);
  CHECK_EQ(m3.size(), (size_t)2);
  CHECK(!r3.warnings.empty());
  // nothing to archive / missing input
  CHECK(!CreateZip({dir / L"missing.txt"}, dir / L"x.zip").ok);
  CHECK(!CreateZip({}, dir / L"y.zip").ok);
  Record("folder>zip", r.ok, "format properties: UTF-8 flag, stored/deflate choice, dirs, mtimes, UT extra");
}

TEST(writer_tar_format_properties) {
  fs::path dir = Fresh(L"w_tar");
  fs::path src = dir / L"src";
  MakeTree(src);
  std::wstring deep = L"d1234567890123456789012345678901234567890";
  fs::path lp = src / deep / deep / deep / deep;
  WriteBytes(lp / L"file with a long long long long long long long long long long long long long name.txt", "long path content");
  CreateResult r = CreateTar({src}, dir / L"out.tar");
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  CHECK(r.ok);
  CHECK(DetectFile(r.path).format == L"tar");
  Out o = Py(R"PY(
import tarfile, sys
t = tarfile.open(sys.argv[1])
names = [m.name for m in t]
longs = [n for n in names if len(n) > 100]
print('LONG', len(longs))
print('PAX', sum(1 for m in t if m.pax_headers))
print('UNI', any('\u0645\u0644\u0641' in n for n in names))
mt = {m.name: m.mtime for m in t}
print('FMT', t.format == tarfile.PAX_FORMAT or t.format == tarfile.USTAR_FORMAT)
for m in t:
    if m.isfile() and m.name.endswith('notes.txt'): print('MODE', oct(m.mode), m.uid, m.gid, m.size)
    if m.isdir(): assert m.mode & 0o700 == 0o700
)PY", {r.path.wstring()});
  CHECK_EQ(o.code, 0);
  if (o.code != 0) std::printf("%s\n", o.err.c_str());
  CHECK(o.out.find("LONG 0") == std::string::npos && o.out.find("LONG ") != std::string::npos);
  CHECK(o.out.find("UNI True") != std::string::npos);
  CHECK(o.out.find("MODE 0o644 0 0 50000") != std::string::npos);
  CHECK(SameManifest(WithRoot(DirManifest(src), L"src"), PyManifest("tar", r.path), "tar writer"));
  Out b = Run(TarExe(), {L"-tf", r.path.wstring()});
  CHECK_EQ(b.code, 0);
  Out s = Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", r.path.wstring()});
  CHECK_EQ(s.code, 0);
  // gnu tar semantic check through bsdtar extraction of the long path
  fs::path ex = dir / L"bsdtar_ex";
  fs::create_directories(ex);
  Out x = Run(TarExe(), {L"-xf", r.path.wstring(), L"-C", ex.wstring()});
  CHECK_EQ(x.code, 0);
  CHECK(Exists(ex / L"src" / deep / deep / deep / deep / L"file with a long long long long long long long long long long long long long name.txt"));
  Record("folder>tar", r.ok, "ustar+pax: long names, unicode, modes; read by python tarfile, bsdtar, 7-Zip");
}

TEST(writer_tar_pax_size_record_for_huge_files) {
  struct Capture : detail::ByteSink {
    std::string bytes;
    void Write(const void* d, size_t n) override { bytes.append((const char*)d, n); }
  };
  auto cap = std::make_unique<Capture>();
  Capture* raw = cap.get();
  auto w = detail::NewTarWriter(std::move(cap));
  detail::NewEntry e;
  e.name = L"huge.bin";
  e.size = 9663676416ull;           // 9 GiB: does not fit the 11 octal digits of ustar
  e.mtime = 133000000000000000ull;
  w->Add(e);
  CHECK(raw->bytes.size() == 512 * 3);                       // pax header + record block + ustar header
  CHECK(raw->bytes.find("size=9663676416") != std::string::npos);
  CHECK(raw->bytes[156] == 'x');
  CHECK(raw->bytes[512 * 2 + 156] == '0');
}

TEST(writer_gzip_stream_properties) {
  fs::path dir = Fresh(L"w_gz");
  fs::path src = dir / L"data.bin";
  WriteBytes(src, Text(2000000, 7) + Pseudo(500000, 8));
  CreateResult r = GzipFile(src, dir / L"data.bin.gz", CreateOptions{}, nullptr);
  CHECK(r.ok);
  Out o = Py("import gzip,sys\nd=gzip.open(sys.argv[1]).read()\nprint(len(d))\nsys.exit(0 if d==open(sys.argv[2],'rb').read() else 1)\n", {(dir / L"data.bin.gz").wstring(), src.wstring()});
  CHECK_EQ(o.code, 0);
  Out s = Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", (dir / L"data.bin.gz").wstring()});
  CHECK_EQ(s.code, 0);
  CHECK(FileSize(dir / L"data.bin.gz") < FileSize(src));
  // empty input is a valid (tiny) gzip too
  WriteBytes(dir / L"empty.bin", "");
  CreateResult e = GzipFile(dir / L"empty.bin", dir / L"empty.bin.gz", CreateOptions{}, nullptr);
  CHECK(e.ok);
  Out o2 = Py("import gzip,sys\nsys.exit(0 if gzip.open(sys.argv[1]).read()==b'' else 1)\n", {(dir / L"empty.bin.gz").wstring()});
  CHECK_EQ(o2.code, 0);
}

TEST(writer_zip64_paths_and_many_entries) {
  fs::path dir = Fresh(L"w_zip64");
  // 1) entry that is *declared* huge (size hint) forces ZIP64 headers; data is small
  {
    auto w = detail::NewZipWriter(dir / L"z64.zip", 6, ZipMethod::Deflate);
    detail::NewEntry e;
    e.name = L"declared-huge.txt"; e.size = 0xF0000000ull; e.sizeKnown = false;
    w->Add(e);
    std::string d = Text(100000, 5);
    w->Data(d.data(), d.size());
    w->EndEntry();
    detail::NewEntry f; f.name = L"normal.txt"; f.size = 5;
    w->Add(f); w->Data("hello", 5); w->EndEntry();
    w->Finish();
  }
  Manifest m = PyManifest("zip", dir / L"z64.zip");
  CHECK_EQ(m.size(), (size_t)2);
  CHECK(m[L"declared-huge.txt"].size == 100000);
  CHECK(m[L"declared-huge.txt"].crc == Crc(Text(100000, 5)));
  CHECK_EQ(Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", (dir / L"z64.zip").wstring()}).code, 0);
  CHECK_EQ(Run(TarExe(), {L"-xOf", (dir / L"z64.zip").wstring()}).code, 0);
  {
    ListResult l = ListArchive(dir / L"z64.zip");
    CHECK(l.ok);
    CHECK_EQ(l.entries.size(), (size_t)2);
    fs::path out = dir / L"o1"; fs::create_directories(out);
    CHECK(ExtractArchive(dir / L"z64.zip", out).ok);
    CHECK_EQ(ReadBytes(out / L"declared-huge.txt"), Text(100000, 5));
  }
  // 2) more than 65535 entries needs the ZIP64 end-of-directory records
  const int N = 70000;
  {
    auto w = detail::NewZipWriter(dir / L"many.zip", 6, ZipMethod::Store);
    for (int i = 0; i < N; ++i) {
      detail::NewEntry e;
      wchar_t nm[64]; swprintf_s(nm, L"d%03d/f%06d.txt", i % 200, i);
      e.name = nm; e.size = 8;
      w->Add(e);
      w->Data("12345678", 8);
      w->EndEntry();
    }
    w->Finish();
  }
  Out o = Py("import zipfile,sys\nz=zipfile.ZipFile(sys.argv[1])\nprint(len(z.namelist()), z.testzip())\n", {(dir / L"many.zip").wstring()});
  CHECK(o.out.find("70000 None") != std::string::npos);
  ListResult l = ListArchive(dir / L"many.zip");
  CHECK(l.ok);
  CHECK_EQ(l.entries.size(), (size_t)N);
  CHECK_EQ(Run(TarExe(), {L"-tf", (dir / L"many.zip").wstring()}).code, 0);
  CHECK_EQ(Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", (dir / L"many.zip").wstring()}).code, 0);
}

TEST(writer_many_small_files_roundtrip) {
  fs::path dir = Fresh(L"w_many");
  fs::path src = dir / L"src";
  const int N = 6000;
  for (int i = 0; i < N; ++i) {
    wchar_t p[96]; swprintf_s(p, L"d%02d/e%02d/file%05d.txt", i % 37, (i / 37) % 11, i);
    WriteBytes(src / p, "file number " + std::to_string(i) + "\n" + std::string((size_t)(i % 50), 'x'));
  }
  double t0 = NowSeconds();
  JobOut jz = RunCap("folder>zip", src);
  JobOut jt = RunCap("folder>gz", src);
  std::printf("  %d files: zip+tgz in %.1fs\n", N, NowSeconds() - t0);
  CHECK(jz.r.ok && jt.r.ok);
  Manifest want = WithRoot(DirManifest(src), L"src");
  CHECK(SameManifest(want, PyManifest("zip", jz.r.outputs[0]), "many files zip"));
  CHECK(SameManifest(want, PyManifest("tar", jt.r.outputs[0]), "many files tar.gz"));
  // and back through our own extractor
  fs::path out = dir / L"back";
  fs::create_directories(out);
  ExtractResult er = ExtractArchive(jz.r.outputs[0], out);
  CHECK(er.ok);
  CHECK(SameManifest(DirManifest(src), DirManifest(out / L"src"), "many files extract back"));
}

TEST(writer_big_file_roundtrip) {
  fs::path dir = Fresh(L"w_big");
  fs::path src = dir / L"src";
  fs::create_directories(src);
  // 160 MiB: 24 MiB of incompressible data followed by zeros (written in chunks, never held in memory twice)
  {
    HANDLE h = CreateFileW((src / L"big.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string rnd = Pseudo(24u << 20, 11), zero(1u << 20, '\0');
    DWORD w;
    WriteFile(h, rnd.data(), (DWORD)rnd.size(), &w, nullptr);
    for (int i = 0; i < 136; ++i) WriteFile(h, zero.data(), (DWORD)zero.size(), &w, nullptr);
    CloseHandle(h);
  }
  uint32_t want = 0;
  {
    std::string all = ReadBytes(src / L"big.bin");
    want = Crc(all);
  }
  double t0 = NowSeconds();
  JobOut jz = RunCap("folder>zip", src);
  double t1 = NowSeconds();
  JobOut jt = RunCap("folder>gz", src);
  double t2 = NowSeconds();
  std::printf("  160 MiB: zip %.1fs (%llu bytes), tar.gz %.1fs (%llu bytes)\n", t1 - t0, (unsigned long long)FileSize(jz.r.outputs[0]), t2 - t1, (unsigned long long)FileSize(jt.r.outputs[0]));
  CHECK(jz.r.ok && jt.r.ok);
  Manifest mz = PyManifest("zip", jz.r.outputs[0]);
  Manifest mt = PyManifest("tar", jt.r.outputs[0]);
  CHECK_EQ(mz[L"src/big.bin"].size, (uint64_t)(160u << 20));
  CHECK_EQ(mz[L"src/big.bin"].crc, want);
  CHECK_EQ(mt[L"src/big.bin"].crc, want);
  // extract with our reader and compare
  fs::path out = dir / L"out";
  fs::create_directories(out);
  CHECK(ExtractArchive(jz.r.outputs[0], out).ok);
  CHECK_EQ(Crc(ReadBytes(out / L"src" / L"big.bin")), want);
  // repack zip -> tar -> zip stays identical
  JobOut j2 = RunCap("zip>tar", jz.r.outputs[0]);
  CHECK(j2.r.ok);
  CHECK_EQ(PyManifest("tar", j2.r.outputs[0])[L"src/big.bin"].crc, want);
}

TEST(writer_long_paths_roundtrip) {
  fs::path dir = Fresh(L"w_longp");
  fs::path src = dir / L"src";
  std::wstring seg(60, L's');
  fs::path deep = src;
  for (int i = 0; i < 6; ++i) deep /= seg + std::to_wstring(i);
  WriteBytes(deep / L"deep.txt", "very deep");
  CHECK(LongPath(deep / L"deep.txt").size() > 360);
  JobOut jz = RunCap("folder>zip", src);
  JobOut jt = RunCap("folder>tar", src);
  CHECK(jz.r.ok);
  CHECK(jt.r.ok);
  Manifest want = WithRoot(DirManifest(src), L"src");
  CHECK(SameManifest(want, PyManifest("zip", jz.r.outputs[0]), "long path zip"));
  CHECK(SameManifest(want, PyManifest("tar", jt.r.outputs[0]), "long path tar"));
  fs::path out = dir / L"back";
  fs::create_directories(out);
  ExtractResult er = ExtractArchive(jz.r.outputs[0], out);
  CHECK(er.ok);
  CHECK(SameManifest(DirManifest(src), DirManifest(out / L"src"), "long path extract back"));
}

TEST(writer_skips_junctions_and_the_output_itself) {
  fs::path dir = Fresh(L"w_junction");
  fs::path outside = dir / L"outside";
  fs::path src = dir / L"src";
  WriteBytes(outside / L"secret.txt", "must not be archived");
  WriteBytes(src / L"inside.txt", "inside");
  Out j = Run(L"C:\\Windows\\System32\\cmd.exe", {L"/c", L"mklink", L"/J", (src / L"jlink").wstring(), outside.wstring()});
  bool haveJunction = j.code == 0 && Exists(src / L"jlink");
  CreateResult r = CreateZip({src}, dir / L"j.zip");
  CHECK(r.ok);
  Manifest m = PyManifest("zip", r.path);
  CHECK(m.count(L"src/inside.txt") == 1);
  CHECK(m.count(L"src/jlink/secret.txt") == 0);
  if (haveJunction) {
    bool warned = false;
    for (auto& w : r.warnings) warned |= w.find(L"junction") != std::wstring::npos;
    CHECK(warned);
    RemoveDirectoryW((src / L"jlink").c_str());       // removes the junction, not the target
  } else std::printf("  (could not create a junction on this machine; junction part unverified)\n");
  CHECK(Exists(outside / L"secret.txt"));
  // output written inside the folder being archived is not archived into itself
  CreateResult r2 = CreateZip({src}, src / L"self.zip");
  CHECK(r2.ok);
  Manifest m2 = PyManifest("zip", r2.path);
  CHECK(m2.count(L"src/self.zip") == 0);
  CHECK(m2.count(L"src/inside.txt") == 1);
}

TEST(writer_locked_input_fails_cleanly) {
  fs::path dir = Fresh(L"w_locked");
  fs::path src = dir / L"src";
  WriteBytes(src / L"a.txt", "fine");
  WriteBytes(src / L"locked.txt", "exclusive");
  HANDLE h = CreateFileW((src / L"locked.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  JobOut jo = RunCap("folder>zip", src);
  CloseHandle(h);
  CHECK(!jo.r.ok);
  CHECK(jo.r.error.find(L"locked.txt") != std::wstring::npos);
  CHECK(!Exists(dir / L"src.zip"));
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) if (e.path().filename().wstring().rfind(L".filo-", 0) == 0) ++leftovers;
  CHECK_EQ(leftovers, (size_t)0);
}

TEST(writer_already_compressed_extension_list) {
  CHECK(detail::LooksAlreadyCompressed(L"a/b/photo.JPG"));
  CHECK(detail::LooksAlreadyCompressed(L"movie.mp4"));
  CHECK(detail::LooksAlreadyCompressed(L"x.docx"));
  CHECK(!detail::LooksAlreadyCompressed(L"notes.txt"));
  CHECK(!detail::LooksAlreadyCompressed(L"noext"));
}
