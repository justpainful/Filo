// no-em-dash:allow (test data uses Arabic / CJK code points written as \u escapes)
// Every registered capability, run through RunConversion() inside a JobManager job, verified with independent readers.
#include "t_util.h"
#include <set>

using namespace filo;
using namespace filo::archive;
using namespace tu;

static const char* kZipFromDirPy = R"PY(
import zipfile, sys, os
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        pre = '' if rel == '.' else rel.replace(os.sep, '/') + '/'
        if pre: z.write(root, pre)
        for f in files: z.write(os.path.join(root, f), pre + f)
)PY";

struct Inputs {
  fs::path dir, src, zip, tar, tgz, gz1, z7, rar;
  Manifest expect;          // manifest of src
  std::string gz1Payload;
};

// Builds one input of every kind from the same source tree with independent tools.
static const Inputs& Build() {
  static Inputs in;
  static bool done = false;
  if (done) return in;
  done = true;
  in.dir = Fresh(L"caps_in");
  in.src = in.dir / L"src";
  MakeSourceTree(in.src);
  in.expect = DirManifest(in.src);
  in.zip = in.dir / L"tree.zip";
  Out o = Py(kZipFromDirPy, {in.src.wstring(), in.zip.wstring()});
  if (o.code != 0) std::printf("  zip build failed: %s\n", o.err.c_str());
  in.tar = in.dir / L"tree.tar";
  o = Run(TarExe(), {L"-cf", in.tar.wstring(), L"-C", in.src.wstring(), L"."});
  if (o.code != 0) std::printf("  tar build failed\n");
  in.tgz = in.dir / L"tree.tar.gz";
  o = Run(TarExe(), {L"-czf", in.tgz.wstring(), L"-C", in.src.wstring(), L"."});
  in.gz1 = in.dir / L"notes.txt.gz";
  in.gz1Payload = Text(300000, 9);
  fs::path payload = in.dir / L"notes.txt";
  WriteBytes(payload, in.gz1Payload);
  o = Py("import gzip,sys,shutil\nwith open(sys.argv[1],'rb') as f, open(sys.argv[2],'wb') as g:\n  z=gzip.GzipFile(filename='notes.txt',mode='wb',fileobj=g,mtime=1600000000); shutil.copyfileobj(f,z); z.close()\n", {payload.wstring(), in.gz1.wstring()});
  in.z7 = in.dir / L"tree.7z";
  o = Run(SevenZipExe(), {L"a", L"-t7z", L"-bso0", L"-bsp0", in.z7.wstring(), (in.src / L"*").wstring()});
  in.rar = in.dir / L"tree.rar";
  o = Py(R"PY(
import zlib, struct, sys, os
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
def file_hdr(name, data, is_dir, mtime):
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
src, outp = sys.argv[1], sys.argv[2]
out = b'Rar!\x1a\x07\x01\x00' + block(bytes([1, 0, 0]))
for root, dirs, files in os.walk(src):
    rel = os.path.relpath(root, src).replace(os.sep, '/')
    if rel != '.': out += file_hdr(rel, b'', True, 1700000000)
    for f in files:
        d = open(os.path.join(root, f), 'rb').read()
        out += file_hdr((rel + '/' if rel != '.' else '') + f, d, False, 1700000000)
out += block(bytes([5, 0, 0]))
open(outp, 'wb').write(out)
)PY", {in.src.wstring(), in.rar.wstring()});
  if (o.code != 0) std::printf("  rar build failed: %s\n", o.err.c_str());
  return in;
}

static uint64_t H(const fs::path& p) { return ((uint64_t)Crc(ReadBytes(p)) << 32) | FileSize(p); }

// Runs `id` on `input`, checks the job result and returns the produced path.
static fs::path Convert(const std::string& id, const fs::path& input, Json opts = Json::MakeObject(), JobOut* outJob = nullptr) {
  uint64_t before = H(input);
  uint64_t mt = MTime(input);
  JobOut jo = RunCap(id, input, opts);
  if (!jo.r.ok) std::wprintf(L"  %hs failed: %ls\n", id.c_str(), jo.r.error.c_str());
  CHECK(jo.r.ok);
  CHECK(jo.state == JobState::Completed);
  CHECK_EQ(H(input), before);            // the input is never modified
  CHECK_EQ(MTime(input), mt);
  if (outJob) *outJob = jo;
  return jo.r.outputs.empty() ? fs::path() : jo.r.outputs[0];
}

static bool Expect(const std::string& id, bool ok, const std::string& note) { Record(id, ok, note); return ok; }

static void CheckZipOut(const fs::path& out, const Manifest& want, const std::string& id, const char* what) {
  bool good = true;
  good &= Exists(out);
  good &= DetectFile(out).format == L"zip";
  good &= SameManifest(want, PyManifest("zip", out), what);
  Out t = Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", out.wstring()});
  good &= t.code == 0;
  Out b = Run(TarExe(), {L"-tf", out.wstring()});
  good &= b.code == 0;
  CHECK(good);
  Expect(id, good, what);
}
static void CheckTarOut(const fs::path& out, const Manifest& want, const std::string& id, const char* what, bool gz) {
  bool good = Exists(out);
  std::string head = ReadBytes(out).substr(0, 2);
  good &= gz ? (head == std::string("\x1f\x8b", 2)) : (DetectFile(out).format == L"tar");
  good &= SameManifest(want, PyManifest("tar", out), what);
  Out t = Run(SevenZipExe(), {L"t", L"-bso0", L"-bsp0", out.wstring()});
  good &= t.code == 0;
  Out b = Run(TarExe(), {L"-tf", out.wstring()});
  good &= b.code == 0;
  CHECK(good);
  Expect(id, good, what);
}

// ---- repack matrix -----------------------------------------------------------------------------------------------------------

TEST(cap_registry_lists_expected_paths) {
  filo::RegisterArchiveEngine();
  const char* expected[] = {"zip>extract", "tar>extract", "gz>extract", "rar>extract", "7z>extract", "zip>tar", "zip>gz", "tar>zip", "tar>gz", "gz>zip", "gz>tar",
                            "rar>zip", "rar>tar", "rar>gz", "7z>zip", "7z>tar", "7z>gz", "folder>zip", "folder>tar", "folder>gz"};
  for (auto id : expected) {
    const Capability* c = Cap(id);
    if (!c) std::printf("  missing capability %s\n", id);
    CHECK(c != nullptr);
    if (c) { CHECK(c->run != nullptr); CHECK(c->Runnable()); CHECK(!c->limitation.empty()); }
  }
  CHECK(Cap("zip>extract")->producesFolder);
  CHECK(Cap("folder>zip")->group == Group::Folder);
  CHECK(!Cap("folder>zip")->producesFolder);
  CHECK(Cap("zip>tar")->group == Group::Archive);
  CHECK_EQ(CapabilityRegistry::Get().Find(L"zip", L"tar") != nullptr, true);
  CHECK(!CapabilityRegistry::Get().OutputsFor(L"zip").empty());
}

TEST(cap_extract_all_five_formats_via_jobs) {
  const Inputs& in = Build();
  struct C { const char* id; fs::path file; bool tarLike; };
  for (auto& c : {C{"zip>extract", in.zip}, C{"tar>extract", in.tar}, C{"gz>extract", in.tgz}, C{"7z>extract", in.z7}, C{"rar>extract", in.rar}}) {
    JobOut jo;
    fs::path out = Convert(c.id, c.file, Json::MakeObject(), &jo);
    bool good = jo.r.ok && Exists(out) && fs::is_directory(out);
    if (good) good &= SameManifest(FilesOnly(in.expect), FilesOnly(DirManifest(out)), c.id);
    if (good) good &= Exists(out / L"empty dir" / L"nested empty") || std::string(c.id) == "zip>extract";    // empty dirs kept (python zip has them too)
    CHECK(good);
    Record(c.id, good, "RunConversion in a JobManager job; extracted tree equals the source tree (python manifest)");
    CHECK(out.filename().wstring().find(L".filo-") == std::wstring::npos);
  }
  // single-file gz
  JobOut jo;
  fs::path out = Convert("gz>extract", in.gz1, Json::MakeObject(), &jo);
  CHECK_EQ(ReadBytes(out / L"notes.txt"), in.gz1Payload);
}

TEST(cap_zip_to_tar_and_gz) {
  const Inputs& in = Build();
  fs::path out = Convert("zip>tar", in.zip);
  CheckTarOut(out, in.expect, "zip>tar", "zip>tar: tar manifest equals source tree", false);
  fs::path out2 = Convert("zip>gz", in.zip);
  CheckTarOut(out2, in.expect, "zip>gz", "zip>gz: produces tar.gz, manifest equals source tree", true);
}

TEST(cap_tar_to_zip_and_gz) {
  const Inputs& in = Build();
  fs::path out = Convert("tar>zip", in.tar);
  CheckZipOut(out, in.expect, "tar>zip", "tar>zip: zip manifest equals source tree");
  fs::path out2 = Convert("tar>gz", in.tar);
  CheckTarOut(out2, in.expect, "tar>gz", "tar>gz: gzip of the tar", true);
  // byte exact: gunzip(out2) == the original tar
  Out o = Py("import gzip,sys\nsys.exit(0 if gzip.open(sys.argv[1]).read()==open(sys.argv[2],'rb').read() else 1)\n", {out2.wstring(), in.tar.wstring()});
  CHECK_EQ(o.code, 0);
}

TEST(cap_gz_to_zip_and_tar) {
  const Inputs& in = Build();
  fs::path out = Convert("gz>zip", in.tgz);
  CheckZipOut(out, in.expect, "gz>zip", "gz>zip from tar.gz: zip manifest equals source tree");
  // single-file gz -> zip with the stored name
  JobOut jo;
  fs::path z1 = Convert("gz>zip", in.gz1, Json::MakeObject(), &jo);
  Manifest want;
  want[L"notes.txt"] = Item{'f', in.gz1Payload.size(), Crc(in.gz1Payload)};
  CHECK(SameManifest(want, PyManifest("zip", z1), "gz>zip single file"));
  // tar.gz -> tar : exact decompression
  fs::path t = Convert("gz>tar", in.tgz, Json::MakeObject(), &jo);
  bool good = Exists(t) && DetectFile(t).format == L"tar";
  Out o = Py("import gzip,sys\nsys.exit(0 if gzip.open(sys.argv[1]).read()==open(sys.argv[2],'rb').read() else 1)\n", {in.tgz.wstring(), t.wstring()});
  good &= o.code == 0;
  good &= SameManifest(in.expect, PyManifest("tar", t), "gz>tar from tar.gz");
  CHECK(good);
  // single-file gz -> tar wraps with a warning
  fs::path t2 = Convert("gz>tar", in.gz1, Json::MakeObject(), &jo);
  Manifest w2; w2[L"notes.txt"] = Item{'f', in.gz1Payload.size(), Crc(in.gz1Payload)};
  good &= SameManifest(w2, PyManifest("tar", t2), "gz>tar single file");
  bool warned = false;
  for (auto& w : jo.r.warnings) warned |= w.find(L"single") != std::wstring::npos;
  good &= warned;
  CHECK(warned);
  Record("gz>tar", good, "tar.gz -> exact tar; single-file gz is wrapped with a warning");
  Record("gz>zip", good, "tar.gz and single-file gz -> zip");
}

TEST(cap_rar_to_zip_tar_gz) {
  const Inputs& in = Build();
  CHECK(Cap("rar>zip")->Runnable());
  fs::path z = Convert("rar>zip", in.rar);
  CheckZipOut(z, in.expect, "rar>zip", "rar>zip: zip manifest equals the tree the (hand-assembled) RAR5 was built from");
  fs::path t = Convert("rar>tar", in.rar);
  CheckTarOut(t, in.expect, "rar>tar", "rar>tar", false);
  fs::path g = Convert("rar>gz", in.rar);
  CheckTarOut(g, in.expect, "rar>gz", "rar>gz (tar.gz)", true);
}

TEST(cap_7z_to_zip_tar_gz) {
  const Inputs& in = Build();
  CHECK(Cap("7z>zip")->Runnable());
  fs::path z = Convert("7z>zip", in.z7);
  CheckZipOut(z, in.expect, "7z>zip", "7z>zip: zip manifest equals the source tree");
  fs::path t = Convert("7z>tar", in.z7);
  CheckTarOut(t, in.expect, "7z>tar", "7z>tar", false);
  fs::path g = Convert("7z>gz", in.z7);
  CheckTarOut(g, in.expect, "7z>gz", "7z>gz (tar.gz)", true);
}

TEST(cap_folder_to_zip_tar_gz) {
  const Inputs& in = Build();
  Manifest want = WithRoot(in.expect, L"src");
  fs::path z = Convert("folder>zip", in.src);
  CHECK_EQ(ExtOf(z), std::wstring(L"zip"));
  CheckZipOut(z, want, "folder>zip", "folder>zip: unicode names, empty dirs, root folder kept");
  fs::path t = Convert("folder>tar", in.src);
  CheckTarOut(t, want, "folder>tar", "folder>tar", false);
  fs::path g = Convert("folder>gz", in.src);
  CheckTarOut(g, want, "folder>gz", "folder>gz (tar.gz)", true);
  // modification times survive in the zip (python reads DOS time as local)
  Out o = Py(R"PY(
import zipfile, sys, os, time
z = zipfile.ZipFile(sys.argv[1])
bad = 0
for i in z.infolist():
    if i.is_dir(): continue
    p = os.path.join(sys.argv[2], *i.filename.split('/')[1:])
    mt = os.path.getmtime(p)
    dt = time.mktime(tuple(i.date_time) + (0, 0, -1))
    if abs(dt - mt) > 3: bad += 1
print(bad)
)PY", {z.wstring(), in.src.wstring()});
  CHECK_EQ(o.out.substr(0, 1), std::string("0"));
}

TEST(cap_options_are_honoured) {
  const Inputs& in = Build();
  // store vs deflate: stored zip is larger than the deflated one for text
  Json store = Json::MakeObject(); store["method"] = "store";
  Json defl = Json::MakeObject(); defl["method"] = "deflate"; defl["level"] = 9;
  fs::path a = Convert("tar>zip", in.tar, store);
  fs::path b = Convert("tar>zip", in.tar, defl);
  CHECK(FileSize(a) > FileSize(b));
  Out o = Py("import zipfile,sys\nz=zipfile.ZipFile(sys.argv[1])\nprint(sorted(set(i.compress_type for i in z.infolist() if not i.is_dir())))\n", {a.wstring()});
  CHECK(o.out.find("[0]") != std::string::npos);
  // skipUnsafe option reaches the engine
  fs::path dir = Fresh(L"opt_unsafe");
  fs::path zip = dir / L"s.zip";
  Out m = Py(R"PY(
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('ok.txt', b'fine')
    zi = zipfile.ZipInfo('x'); zi.filename = '../evil.txt'; z.writestr(zi, b'EVIL')
)PY", {zip.wstring()});
  CHECK(m.code == 0);
  JobOut refused = RunCap("zip>extract", zip);
  CHECK(!refused.r.ok);
  CHECK(refused.r.error.find(L"unsafe") != std::wstring::npos);
  Json skip = Json::MakeObject(); skip["skipUnsafe"] = true;
  JobOut okJob = RunCap("zip>extract", zip, skip);
  CHECK(okJob.r.ok);
  CHECK(!okJob.r.warnings.empty());
  CHECK(!okJob.r.outputs.empty() && Exists(okJob.r.outputs[0] / L"ok.txt"));
  CHECK(!Exists(dir / L"evil.txt"));
}

TEST(cap_unsafe_repack_is_refused) {
  fs::path dir = Fresh(L"repack_unsafe");
  fs::path zip = dir / L"s.zip";
  Out m = Py(R"PY(
import zipfile, sys
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('ok.txt', b'fine')
    zi = zipfile.ZipInfo('x'); zi.filename = '..\\..\\evil.txt'; z.writestr(zi, b'EVIL')
)PY", {zip.wstring()});
  CHECK(m.code == 0);
  JobOut jo = RunCap("zip>tar", zip);
  CHECK(!jo.r.ok);
  CHECK(jo.r.error.find(L"unsafe") != std::wstring::npos);
  CHECK(!Exists(dir / L"s.tar"));
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) if (e.path().filename().wstring().rfind(L".filo-", 0) == 0) ++leftovers;
  CHECK_EQ(leftovers, (size_t)0);
  Json skip = Json::MakeObject(); skip["skipUnsafe"] = true;
  JobOut ok = RunCap("zip>tar", zip, skip);
  CHECK(ok.r.ok);
  Manifest m2 = PyManifest("tar", ok.r.outputs[0]);
  CHECK_EQ(m2.size(), (size_t)1);
  CHECK(m2.count(L"ok.txt") == 1);
}

TEST(cap_read_only_input_and_wrong_extension) {
  const Inputs& in = Build();
  fs::path dir = Fresh(L"ro");
  fs::path ro = dir / L"readonly.zip";
  CopyFileW(in.zip.c_str(), ro.c_str(), FALSE);
  SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
  fs::path out = Convert("zip>tar", ro);
  CHECK(Exists(out));
  CHECK_EQ(Crc(ReadBytes(ro)), Crc(ReadBytes(in.zip)));
  SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
  // a zip named .pdf: the registry works on the detected format; extension mismatch gives a warning
  fs::path liar = dir / L"liar.pdf";
  CopyFileW(in.zip.c_str(), liar.c_str(), FALSE);
  FileKind k = DetectFile(liar);
  CHECK(k.format == L"zip");
  CHECK(k.extensionMismatch);
  JobOut jo = RunCap("zip>tar", liar);
  CHECK(jo.r.ok);
  bool warned = false;
  for (auto& w : jo.r.warnings) warned |= w.find(L"does not match") != std::wstring::npos;
  CHECK(warned);
}

TEST(cap_progress_is_reported_and_cancel_works) {
  fs::path dir = Fresh(L"progress");
  fs::path src = dir / L"src";
  WriteBytes(src / L"a.bin", Pseudo(30u << 20, 4));
  WriteBytes(src / L"b.bin", Pseudo(30u << 20, 5));
  JobOut jo = RunCap("folder>tar", src);
  CHECK(jo.r.ok);
  double mx = 0; size_t distinct = 0; std::set<int> seen;
  for (double p : jo.progress) { mx = std::max(mx, p); seen.insert((int)(p * 20)); }
  distinct = seen.size();
  CHECK(mx >= 0.99);
  CHECK(distinct >= 3);
  // cancel a long zip>tar mid-way
  fs::path zip = dir / L"big.zip";
  Out o = Py(R"PY(
import zipfile, sys, os
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_STORED) as z:
    for i in range(5):
        with z.open('p%d.bin' % i, 'w', force_zip64=True) as h:
            for _ in range(40): h.write(os.urandom(1 << 20))
)PY", {zip.wstring()});
  CHECK(o.code == 0);
  JobOut c = RunCap("zip>tar", zip, Json::MakeObject(), [](uint64_t id) { Sleep(200); JobManager::Get().Cancel(id); });
  CHECK(c.state == JobState::Cancelled);
  CHECK(!Exists(dir / L"big.tar"));
  size_t leftovers = 0;
  for (auto& e : fs::directory_iterator(dir)) if (e.path().filename().wstring().rfind(L".filo-", 0) == 0) ++leftovers;
  CHECK_EQ(leftovers, (size_t)0);
}

TEST(cap_output_never_overwrites_existing_file) {
  const Inputs& in = Build();
  fs::path dir = Fresh(L"nooverwrite");
  fs::path zip = dir / L"x.zip";
  CopyFileW(in.zip.c_str(), zip.c_str(), FALSE);
  WriteBytes(dir / L"x.tar", "precious existing file");
  fs::path out = Convert("zip>tar", zip);
  CHECK(out != dir / L"x.tar");
  CHECK_EQ(ReadBytes(dir / L"x.tar"), std::string("precious existing file"));
  CHECK(Exists(out));
}

TEST(cap_all_registered_capabilities_have_a_passing_test) {
  filo::RegisterArchiveEngine();
  for (auto& c : CapabilityRegistry::Get().All()) {
    if (c.engine != "native") continue;
    bool tested = Mx().results.count(c.id) && Mx().results[c.id].first == "pass";
    if (!tested) std::printf("  capability %s has no passing test\n", c.id.c_str());
    CHECK(tested);
  }
}
