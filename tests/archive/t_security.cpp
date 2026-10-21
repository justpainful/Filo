// Malicious / damaged archives: zip-slip, absolute paths, link escapes, bombs, CRC errors, truncation, cancellation.
#include "t_util.h"
#include <set>
#include <thread>

using namespace filo;
using namespace filo::archive;
using namespace tu;

bool PyMakeZip(const fs::path& out, const std::string& body);   // t_formats.cpp

static ExtractResult Ex(const fs::path& arc, const fs::path& dest, ExtractOptions o = {}) {
  std::error_code ec;
  fs::create_directories(dest, ec);
  return ExtractArchive(arc, dest, o);
}
static size_t CountChildren(const fs::path& d) {
  size_t n = 0;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(d, ec)) { (void)e; ++n; }
  return n;
}
static bool ErrHas(const ExtractResult& r, const wchar_t* s) { return r.error.find(s) != std::wstring::npos; }

TEST(sec_zip_slip_rejected_and_nothing_escapes) {
  const char* names[] = {"..\\\\..\\\\evil.txt", "../../evil.txt", "a/../../evil.txt", "dir\\\\..\\\\..\\\\evil.txt", "./../evil.txt", "..\\\\evil.txt", "good/../../../evil.txt"};
  int i = 0;
  for (const char* nm : names) {
    fs::path dir = Fresh(L"slip" + std::to_wstring(i++));
    fs::path zip = dir / L"slip.zip";
    CHECK(PyMakeZip(zip, std::string("add(z, 'ok.txt', b'fine')\nadd(z, '") + nm + "', b'EVIL')\nadd(z, 'after.txt', b'later')\n"));
    ListResult l = ListArchive(zip);
    CHECK(l.ok);
    CHECK(l.hasUnsafe);
    fs::path out = dir / L"x" / L"out";
    ExtractResult r = Ex(zip, out);
    CHECK(!r.ok);
    CHECK(ErrHas(r, L"unsafe"));
    CHECK(!Exists(dir / L"evil.txt"));
    CHECK(!Exists(dir / L"x" / L"evil.txt"));
    CHECK(!Exists(out / L"evil.txt"));
    CHECK_EQ(CountChildren(dir / L"x"), (size_t)1);       // only "out"
    CHECK_EQ(CountChildren(out), (size_t)0);              // rolled back: nothing half extracted
    CHECK(!r.error.empty() && r.error.find(L"Nothing was written") != std::wstring::npos);
  }
}

TEST(sec_skip_unsafe_option_extracts_the_rest) {
  fs::path dir = Fresh(L"slip_skip");
  fs::path zip = dir / L"s.zip";
  CHECK(PyMakeZip(zip, "add(z, 'ok.txt', b'fine')\nadd(z, '../evil.txt', b'EVIL')\nadd(z, 'C:\\\\evil2.txt', b'EVIL')\nadd(z, 'sub/after.txt', b'later')\n"));
  ExtractOptions eo; eo.skipUnsafe = true;
  fs::path out = dir / L"x" / L"out";
  ExtractResult r = Ex(zip, out, eo);
  CHECK(r.ok);
  CHECK_EQ(r.skipped, (uint64_t)2);
  CHECK(Exists(out / L"ok.txt"));
  CHECK(Exists(out / L"sub" / L"after.txt"));
  CHECK(!Exists(dir / L"x" / L"evil.txt"));
  CHECK(!r.warnings.empty());
}

TEST(sec_absolute_drive_unc_device_and_stream_names) {
  fs::path dir = Fresh(L"abs");
  fs::path target = dir / L"abs_target.txt";
  std::string t = Narrow(target.wstring());
  std::string esc;
  for (char c : t) { if (c == '\\') esc += "\\\\"; else esc.push_back(c); }
  const std::string bad[] = {
      "/abs_target.txt", "\\\\\\\\abs_target.txt", esc,                           // absolute with the real target path (C:\\...)
      "C:\\\\Windows\\\\evil.dll", "d:evil.txt", "\\\\\\\\server\\\\share\\\\x.txt", "\\\\\\\\?\\\\C:\\\\evil.txt", "//./pipe/evil",
      "file.txt:hidden.exe", "doc.docx::$DATA"};
  int i = 0;
  for (auto& nm : bad) {
    fs::path d2 = Fresh(L"abs_" + std::to_wstring(i++));
    fs::path zip = d2 / L"a.zip";
    CHECK(PyMakeZip(zip, "add(z, 'ok.txt', b'fine')\nadd(z, '" + nm + "', b'EVIL')\n"));
    fs::path out = d2 / L"out";
    ExtractResult r = Ex(zip, out);
    if (r.ok) std::printf("  accepted unsafe name: %s\n", nm.c_str());
    CHECK(!r.ok);
    CHECK_EQ(CountChildren(out), (size_t)0);
  }
  CHECK(!Exists(target));
}

TEST(sec_reserved_names_are_renamed_not_created_as_devices) {
  fs::path dir = Fresh(L"reserved");
  fs::path zip = dir / L"r.zip";
  CHECK(PyMakeZip(zip, "add(z, 'CON', b'a')\nadd(z, 'nul.txt', b'b')\nadd(z, 'dir/AUX', b'c')\nadd(z, 'COM1.log', b'd')\nadd(z, 'trailing. ', b'e')\nadd(z, 'what?.txt', b'f')\n"));
  fs::path out = dir / L"out";
  ExtractResult r = Ex(zip, out);
  CHECK(r.ok);
  CHECK(Exists(out / L"_CON"));
  CHECK(Exists(out / L"_nul.txt"));
  CHECK(Exists(out / L"dir" / L"_AUX"));
  CHECK(Exists(out / L"_COM1.log"));
  CHECK(Exists(out / L"trailing"));
  CHECK(Exists(out / L"what_.txt"));
  CHECK(r.warnings.size() >= 5);
}

TEST(sec_tar_link_escapes) {
  fs::path dir = Fresh(L"tarlink");
  fs::path tar = dir / L"l.tar";
  Out o = Py(R"PY(
import tarfile, sys, io
with tarfile.open(sys.argv[1], 'w', format=tarfile.PAX_FORMAT) as t:
    def f(name, data=b'x'):
        ti = tarfile.TarInfo(name); ti.size = len(data); t.addfile(ti, io.BytesIO(data))
    def l(name, target, hard=False):
        ti = tarfile.TarInfo(name); ti.type = tarfile.LNKTYPE if hard else tarfile.SYMTYPE; ti.linkname = target; t.addfile(ti)
    f('real.txt', b'real content')
    l('safe-link', 'real.txt')
    l('sub/safe-hard', 'real.txt', hard=True)
    l('escape-link', '../../etc/passwd')
)PY", {tar.wstring()});
  CHECK(o.code == 0);
  ListResult lr = ListArchive(tar);
  CHECK(lr.ok);
  CHECK(lr.hasUnsafe);
  fs::path out = dir / L"x" / L"out";
  ExtractResult r = Ex(tar, out);
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"unsafe"));
  CHECK_EQ(CountChildren(out), (size_t)0);
  // with the unsafe entry skipped: safe symlink skipped with a warning, safe hardlink copied
  ExtractOptions eo; eo.skipUnsafe = true;
  fs::path out2 = dir / L"y" / L"out";
  r = Ex(tar, out2, eo);
  CHECK(r.ok);
  CHECK(Exists(out2 / L"real.txt"));
  CHECK(!Exists(out2 / L"safe-link"));
  CHECK(!Exists(out2 / L"escape-link"));
  CHECK(Exists(out2 / L"sub" / L"safe-hard"));
  CHECK_EQ(ReadBytes(out2 / L"sub" / L"safe-hard"), std::string("real content"));
  bool symWarn = false;
  for (auto& w : r.warnings) symWarn |= w.find(L"symbolic link") != std::wstring::npos;
  CHECK(symWarn);
}

TEST(sec_tar_traversal_names) {
  fs::path dir = Fresh(L"tarslip");
  fs::path tar = dir / L"t.tar";
  Out o = Py(R"PY(
import tarfile, sys, io
with tarfile.open(sys.argv[1], 'w', format=tarfile.GNU_FORMAT) as t:
    for name in ('fine.txt', '../../tar-evil.txt', '/tmp/abs-evil.txt', 'sub\\..\\..\\evil2.txt'):
        ti = tarfile.TarInfo(name); ti.size = 4; t.addfile(ti, io.BytesIO(b'evil'))
)PY", {tar.wstring()});
  CHECK(o.code == 0);
  fs::path out = dir / L"x" / L"out";
  ExtractResult r = Ex(tar, out);
  CHECK(!r.ok);
  CHECK(!Exists(dir / L"tar-evil.txt"));
  CHECK(!Exists(dir / L"x" / L"tar-evil.txt"));
  CHECK_EQ(CountChildren(dir / L"x"), (size_t)1);
}

TEST(sec_zip_symlink_entry_with_escaping_target) {
  fs::path dir = Fresh(L"ziplink");
  fs::path zip = dir / L"l.zip";
  CHECK(PyMakeZip(zip, "add(z, 'ok.txt', b'fine')\nadd(z, 'link', b'../../outside', mode=0o120777)\n"));
  ListResult l = ListArchive(zip);
  CHECK(l.ok);
  bool sym = false;
  for (auto& e : l.entries) sym |= e.name == L"link" && e.type == EntryType::Symlink;
  CHECK(sym);
  ExtractResult r = Ex(zip, dir / L"x" / L"out");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"unsafe"));
  CHECK(CountChildren(dir / L"x" / L"out") == 0);
  // a harmless symlink is just skipped (never created)
  fs::path zip2 = dir / L"l2.zip";
  CHECK(PyMakeZip(zip2, "add(z, 'ok.txt', b'fine')\nadd(z, 'link', b'ok.txt', mode=0o120777)\n"));
  fs::path out2 = dir / L"out2";
  r = Ex(zip2, out2);
  CHECK(r.ok);
  CHECK(Exists(out2 / L"ok.txt"));
  CHECK(!Exists(out2 / L"link"));
}

TEST(sec_ratio_and_size_caps) {
  fs::path dir = Fresh(L"bomb");
  fs::path zip = dir / L"zeros.zip";
  Out o = Py(R"PY(
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    with z.open('zeros.bin', 'w') as h:
        for _ in range(50): h.write(bytes(1 << 20))
    z.writestr('small.txt', b'small')
)PY", {zip.wstring()});
  CHECK(o.code == 0);
  // ratio guard (limits lowered so the test stays small)
  ExtractOptions eo; eo.ratioFloorBytes = 1 << 20; eo.maxRatio = 100;
  ExtractResult r = Ex(zip, dir / L"o1", eo);
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"bomb"));
  CHECK_EQ(CountChildren(dir / L"o1"), (size_t)0);
  // total size cap
  ExtractOptions e2; e2.maxTotalBytes = 10u << 20;
  r = Ex(zip, dir / L"o2", e2);
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"safety limit"));
  CHECK_EQ(CountChildren(dir / L"o2"), (size_t)0);
  // same archive passes with default limits (50 MiB, ratio floor 1 GiB)
  r = Ex(zip, dir / L"o3");
  CHECK(r.ok);
  CHECK_EQ(FileSize(dir / L"o3" / L"zeros.bin"), (uint64_t)(50u << 20));
  // entry count cap
  fs::path many = dir / L"many.zip";
  CHECK(PyMakeZip(many, "for i in range(300): add(z, 'f%d.txt' % i, b'x')\n"));
  ExtractOptions e3; e3.maxEntries = 100;
  r = Ex(many, dir / L"o4", e3);
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"too many"));
  ListOptions lo; lo.maxEntries = 100;
  CHECK(!ListArchive(many, lo).ok);
  // impossible free-space requirement
  ExtractOptions e4; e4.reserveFreeBytes = 1ull << 60;
  r = Ex(many, dir / L"o5", e4);
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"disk space"));
}

TEST(sec_real_default_ratio_guard_on_4gib_of_zeros) {
  fs::path dir = Fresh(L"bomb_real");
  fs::path zip = dir / L"bomb.zip";
  double t0 = NowSeconds();
  Out o = Py(R"PY(
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    with z.open('zeros.bin', 'w', force_zip64=True) as h:
        blk = bytes(64 << 20)
        for _ in range(64): h.write(blk)
)PY", {zip.wstring()}, 900000);
  CHECK(o.code == 0);
  std::printf("  (bomb built in %.1fs, %llu bytes)\n", NowSeconds() - t0, (unsigned long long)FileSize(zip));
  ExtractResult r = Ex(zip, dir / L"out");     // default options
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"bomb"));
  CHECK(!Exists(dir / L"out" / L"zeros.bin"));
  Record("zip>extract", !r.ok, "default-limit decompression bomb (4 GiB of zeros) refused before writing");
}

TEST(sec_tar_header_claiming_a_huge_size) {
  fs::path dir = Fresh(L"tarhuge");
  fs::path tar = dir / L"h.tar";
  Out o = Py(R"PY(
import tarfile, sys
with tarfile.open(sys.argv[1], 'w') as t:
    ti = tarfile.TarInfo('claims-a-terabyte.bin'); ti.size = 1 << 40
    t.fileobj.write(ti.tobuf(tarfile.GNU_FORMAT))
)PY", {tar.wstring()});
  CHECK(o.code == 0);
  ExtractResult r = Ex(tar, dir / L"out");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"safety limit") || ErrHas(r, L"truncated"));
}

static std::string Flip(std::string b, size_t pos) { b[pos] = (char)(b[pos] ^ 0x5A); return b; }

TEST(sec_crc_mismatch_detected) {
  fs::path dir = Fresh(L"crc");
  // stored entry: flip a payload byte
  fs::path zip = dir / L"s.zip";
  CHECK(PyMakeZip(zip, "add(z, 'a.txt', b'ABCDEFGHIJKLMNOPQRSTUVWXYZ' * 40, comp=zipfile.ZIP_STORED)\nadd(z, 'b.txt', b'second')\n"));
  std::string b = ReadBytes(zip);
  size_t at = b.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  CHECK(at != std::string::npos);
  fs::path bad = dir / L"stored-bad.zip";
  WriteBytes(bad, Flip(b, at + 100));
  ExtractResult r = Ex(bad, dir / L"o1");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"checksum") || ErrHas(r, L"damaged"));
  CHECK_EQ(CountChildren(dir / L"o1"), (size_t)0);
  // deflate entry: corrupt compressed bytes
  fs::path zip2 = dir / L"d.zip";
  CHECK(PyMakeZip(zip2, "import random\nrandom.seed(1)\nadd(z, 'r.bin', bytes(random.getrandbits(8) for _ in range(5000)) + b'x' * 20000)\n"));
  std::string b2 = ReadBytes(zip2);
  fs::path bad2 = dir / L"deflate-bad.zip";
  WriteBytes(bad2, Flip(b2, 200));
  r = Ex(bad2, dir / L"o2");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"damaged") || ErrHas(r, L"checksum") || ErrHas(r, L"corrupt"));
  // only the CRC field in the central directory is wrong
  fs::path zip3 = dir / L"c.zip";
  CHECK(PyMakeZip(zip3, "add(z, 'a.txt', b'hello crc')\n"));
  std::string b3 = ReadBytes(zip3);
  size_t cd = b3.rfind("PK\x01\x02");
  CHECK(cd != std::string::npos);
  fs::path bad3 = dir / L"cdcrc.zip";
  WriteBytes(bad3, Flip(b3, cd + 16));
  r = Ex(bad3, dir / L"o3");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"checksum"));
  // gzip trailer
  fs::path gz = dir / L"t.tar.gz";
  Out o = Py(R"PY(
import tarfile, sys, io
with tarfile.open(sys.argv[1], 'w:gz') as t:
    ti = tarfile.TarInfo('a.txt'); ti.size = 3000; t.addfile(ti, io.BytesIO(b'abc' * 1000))
)PY", {gz.wstring()});
  CHECK(o.code == 0);
  std::string g = ReadBytes(gz);
  fs::path badgz = dir / L"badtrailer.tar.gz";
  WriteBytes(badgz, Flip(g, g.size() - 8));
  r = Ex(badgz, dir / L"o4");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"checksum") || ErrHas(r, L"damaged"));
}

TEST(sec_truncated_archives) {
  fs::path dir = Fresh(L"trunc");
  fs::path src = dir / L"src";
  MakeSourceTree(src);
  // zip
  CreateResult cz = WriteFromPaths({src}, OutKind::Zip, dir / L"full.zip", CreateOptions{}, nullptr);
  if (!cz.ok) std::wprintf(L"  zip create error: %ls\n", cz.error.c_str());
  CHECK(cz.ok);
  std::string z = ReadBytes(dir / L"full.zip");
  CHECK(z.size() > 1000);
  int idx = 0;
  for (double frac : {0.1, 0.5, 0.9, 0.999}) {
    fs::path t = dir / (L"cut" + std::to_wstring(idx++) + L".zip");
    WriteBytes(t, z.substr(0, (size_t)(z.size() * frac)));
    ListResult l = ListArchive(t);
    ExtractResult r = Ex(t, dir / (L"o" + std::to_wstring(idx)));
    if (r.ok || l.ok) std::printf("  truncated zip at %.3f accepted (list=%d extract=%d)\n", frac, (int)l.ok, (int)r.ok);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
  }
  // tar and tar.gz
  CreateResult ct = WriteFromPaths({src}, OutKind::Tar, dir / L"full.tar", CreateOptions{}, nullptr);
  CreateResult cg = WriteFromPaths({src}, OutKind::TarGz, dir / L"full.tar.gz", CreateOptions{}, nullptr);
  if (!ct.ok) std::wprintf(L"  tar create error: %ls\n", ct.error.c_str());
  if (!cg.ok) std::wprintf(L"  tgz create error: %ls\n", cg.error.c_str());
  CHECK(ct.ok && cg.ok);
  for (auto name : {L"full.tar", L"full.tar.gz"}) {
    std::string b = ReadBytes(dir / name);
    for (double frac : {0.3, 0.7, 0.95}) {
      fs::path t = dir / (std::wstring(L"cut_") + std::to_wstring((int)(frac * 100)) + name);
      WriteBytes(t, b.substr(0, (size_t)(b.size() * frac)));
      ExtractResult r = Ex(t, dir / (L"ot_" + std::to_wstring((int)(frac * 100)) + name));
      if (r.ok) std::printf("  truncated %ls at %.2f accepted\n", name, frac);
      CHECK(!r.ok);
      if (!r.ok) CHECK(r.error.find(L"truncated") != std::wstring::npos || r.error.find(L"damaged") != std::wstring::npos || r.error.find(L"valid") != std::wstring::npos);
    }
  }
}

TEST(sec_damaged_tar_header_checksum) {
  fs::path dir = Fresh(L"tarbad");
  fs::path src = Fresh(L"tarbad_src");
  WriteBytes(src / L"a.txt", Text(5000, 1));
  WriteBytes(src / L"b.txt", Text(5000, 2));
  fs::path tar = dir / L"ok.tar";
  CHECK(WriteFromPaths({src}, OutKind::Tar, tar, CreateOptions{}, nullptr).ok);
  std::string b = ReadBytes(tar);
  // second header sits after: dir header (512) + a.txt header + 5000 data padded (5120) ...
  size_t second = 512 + 512 + 5120;
  fs::path bad = dir / L"bad.tar";
  WriteBytes(bad, Flip(b, second + 10));
  ExtractResult r = Ex(bad, dir / L"o");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"damaged") || ErrHas(r, L"valid"));
}

TEST(sec_overlapping_zip_entries_rejected) {
  fs::path dir = Fresh(L"overlap");
  fs::path zip = dir / L"o.zip";
  CHECK(PyMakeZip(zip, "add(z, 'a.txt', b'A' * 1000)\nadd(z, 'b.txt', b'B' * 1000)\n"));
  std::string b = ReadBytes(zip);
  // make the second central-directory record point at the first local header (offset 0)
  size_t first = b.find("PK\x01\x02");
  size_t second = b.find("PK\x01\x02", first + 4);
  CHECK(second != std::string::npos);
  for (int i = 0; i < 4; ++i) b[second + 42 + i] = 0;
  fs::path bad = dir / L"overlap.zip";
  WriteBytes(bad, b);
  ExtractResult r = Ex(bad, dir / L"out");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"overlap"));
}

TEST(sec_encrypted_zip_reports_password) {
  fs::path dir = Fresh(L"enc");
  fs::path zip = dir / L"e.zip";
  CHECK(PyMakeZip(zip, "add(z, 'secret.txt', b'top secret')\n"));
  std::string b = ReadBytes(zip);
  b[6] = (char)(b[6] | 1);                                  // local header: encrypted flag
  size_t cd = b.find("PK\x01\x02");
  b[cd + 8] = (char)(b[cd + 8] | 1);                        // central header
  fs::path enc = dir / L"flagged.zip";
  WriteBytes(enc, b);
  ListResult l = ListArchive(enc);
  CHECK(l.ok);
  CHECK(l.encrypted);
  ExtractResult r = Ex(enc, dir / L"out");
  CHECK(!r.ok);
  CHECK(ErrHas(r, L"password"));
}

TEST(sec_wrong_signature_and_garbage) {
  fs::path dir = Fresh(L"garbage");
  WriteBytes(dir / L"g.zip", "PK\x03\x04 and then random garbage that is not a zip file at all");
  CHECK(!ListArchive(dir / L"g.zip").ok);
  WriteBytes(dir / L"g.gz", std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10) + "not deflate data!!");
  ExtractResult r = Ex(dir / L"g.gz", dir / L"o");
  CHECK(!r.ok);
  WriteBytes(dir / L"g.7z", std::string("7z\xBC\xAF\x27\x1C", 6) + std::string(100, 'x'));
  r = Ex(dir / L"g.7z", dir / L"o7");
  CHECK(!r.ok);
  WriteBytes(dir / L"g.rar", std::string("Rar!\x1a\x07\x01\x00", 8) + std::string(100, 'x'));
  r = Ex(dir / L"g.rar", dir / L"orar");
  CHECK(!r.ok);
  CHECK(!r.error.empty());
}

TEST(sec_long_paths_and_long_names) {
  fs::path dir = Fresh(L"longp");
  fs::path zip = dir / L"long.zip";
  CHECK(PyMakeZip(zip, R"PY(
deep = '/'.join(['segment%02d_' % i + 'x' * 20 for i in range(14)])
add(z, deep + '/deep file.txt', b'deep content')
add(z, 'n' * 300 + '.txt', b'overlong name')
add(z, 'ok.txt', b'ok')
)PY"));
  fs::path out = dir / L"out";
  ExtractResult r = Ex(zip, out);
  CHECK(r.ok);
  if (!r.ok) std::wprintf(L"  error: %ls\n", r.error.c_str());
  bool deepFound = false, shortened = false;
  for (auto& w : r.written) {
    if (w.size() > 259 && w.find(L"deep file.txt") != std::wstring::npos) {
      deepFound = Exists(out / w);
      CHECK_EQ(ReadBytes(out / w), std::string("deep content"));
    }
    if (w.find(L'~') != std::wstring::npos && w.size() <= 255) shortened = true;
  }
  CHECK(deepFound);
  CHECK(shortened);
  bool warnedLong = false;
  for (auto& w : r.warnings) warnedLong |= w.find(L"259") != std::wstring::npos;
  CHECK(warnedLong);
}

TEST(sec_cancel_mid_extraction_leaves_nothing) {
  fs::path dir = Fresh(L"cancel");
  fs::path zip = dir / L"big.zip";
  Out o = Py(R"PY(
import zipfile, sys, os
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_STORED) as z:
    for i in range(6):
        with z.open('part%d.bin' % i, 'w', force_zip64=True) as h:
            for _ in range(40): h.write(os.urandom(1 << 20))
)PY", {zip.wstring()});
  CHECK(o.code == 0);
  JobOut jo = RunCap("zip>extract", zip, Json::MakeObject(), [](uint64_t id) {
    Sleep(250);
    JobManager::Get().Cancel(id);
  });
  CHECK(jo.state == JobState::Cancelled);
  CHECK(!jo.r.ok);
  CHECK(!Exists(dir / L"big"));
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) { std::wstring n = e.path().filename().wstring(); if (n.rfind(L".filo-", 0) == 0) ++leftovers; }
  CHECK_EQ(leftovers, (size_t)0);
}

TEST(sec_real_password_protected_archives) {
  fs::path dir = Fresh(L"pw");
  fs::path src = dir / L"src";
  WriteBytes(src / L"secret.txt", Text(5000, 3));
  struct C { const wchar_t* name; std::vector<std::wstring> args; };
  fs::path zc = dir / L"zipcrypto.zip", za = dir / L"aes.zip", z7 = dir / L"enc.7z", z7h = dir / L"enc-headers.7z";
  CHECK_EQ(Run(SevenZipExe(), {L"a", L"-tzip", L"-pHunter2", L"-mem=ZipCrypto", L"-bso0", L"-bsp0", zc.wstring(), (src / L"*").wstring()}).code, 0);
  CHECK_EQ(Run(SevenZipExe(), {L"a", L"-tzip", L"-pHunter2", L"-mem=AES256", L"-bso0", L"-bsp0", za.wstring(), (src / L"*").wstring()}).code, 0);
  CHECK_EQ(Run(SevenZipExe(), {L"a", L"-t7z", L"-pHunter2", L"-bso0", L"-bsp0", z7.wstring(), (src / L"*").wstring()}).code, 0);
  CHECK_EQ(Run(SevenZipExe(), {L"a", L"-t7z", L"-pHunter2", L"-mhe=on", L"-bso0", L"-bsp0", z7h.wstring(), (src / L"*").wstring()}).code, 0);
  for (auto* f : {&zc, &za, &z7, &z7h}) {
    ExtractResult r = Ex(*f, dir / (f->stem().wstring() + L"-out"));
    if (r.ok) std::wprintf(L"  %ls extracted without a password?!\n", f->filename().c_str());
    CHECK(!r.ok);
    if (!r.ok && r.error.find(L"password") == std::wstring::npos) std::wprintf(L"  %ls: unexpected message: %ls\n", f->filename().c_str(), r.error.c_str());
    CHECK(ErrHas(r, L"password"));
    CHECK(!Exists(dir / (f->stem().wstring() + L"-out") / L"secret.txt"));
  }
  JobOut jo = RunCap("zip>tar", zc);
  CHECK(!jo.r.ok);
  CHECK(jo.r.error.find(L"password") != std::wstring::npos);
}
