// Capability registration for the archive engine.
#include "engines/archive/archive.h"
#include "registry/capability.h"
#include <algorithm>

namespace filo {

using namespace archive;

namespace {

bool OptBool(const Json& o, const char* k, bool def = false) {
  const Json& v = o.Get(k);
  if (v.IsBool()) return v.AsBool();
  if (v.IsNumber()) return v.AsNumber() != 0;
  if (v.IsString()) return v.AsString() == "1" || v.AsString() == "true";
  return def;
}
int OptInt(const Json& o, const char* k, int def) {
  const Json& v = o.Get(k);
  return (v.IsNumber() || v.IsString()) ? (int)v.AsNumber((double)def) : def;
}
ZipMethod OptMethod(const Json& o) {
  std::string m = o.Get("method").AsString();
  if (m == "store") return ZipMethod::Store;
  if (m == "deflate") return ZipMethod::Deflate;
  return ZipMethod::Auto;
}

ExtractOptions EOpts(const Json& o) {
  ExtractOptions e;
  e.skipUnsafe = OptBool(o, "skipUnsafe");
  e.nameEncoding = OptInt(o, "nameEncoding", 0);
  return e;
}
CreateOptions COpts(const Json& o) {
  CreateOptions c;
  c.method = OptMethod(o);
  c.level = std::clamp(OptInt(o, "level", 6), 1, 9);
  return c;
}

EngineResult ToEngine(const CreateResult& r) {
  EngineResult er;
  er.ok = r.ok; er.error = r.error; er.warnings = r.warnings;
  return er;
}

OptionSpec SkipUnsafeOpt() {
  OptionSpec o;
  o.id = "skipUnsafe"; o.kind = OptionSpec::Kind::Toggle; o.def = 0;
  o.label = L"Skip unsafe entries instead of refusing the whole archive";
  return o;
}
OptionSpec EncodingOpt() {
  OptionSpec o;
  o.id = "nameEncoding"; o.kind = OptionSpec::Kind::Choice; o.label = L"File name encoding (old zip files)";
  o.choices = {{"0", L"Automatic"}, {"65001", L"Unicode (UTF-8)"}, {"437", L"DOS Western (CP437)"}, {"1252", L"Windows Western"},
               {"936", L"Chinese (GBK)"}, {"950", L"Chinese (Big5)"}, {"932", L"Japanese (Shift-JIS)"}, {"949", L"Korean"}};
  return o;
}
OptionSpec MethodOpt() {
  OptionSpec o;
  o.id = "method"; o.kind = OptionSpec::Kind::Choice; o.label = L"Compression";
  o.choices = {{"auto", L"Automatic (store already-compressed files)"}, {"deflate", L"Always compress"}, {"store", L"Store only (fastest)"}};
  return o;
}
OptionSpec LevelOpt() {
  OptionSpec o;
  o.id = "level"; o.kind = OptionSpec::Kind::Slider; o.label = L"Compression level"; o.min = 1; o.max = 9; o.step = 1; o.def = 6;
  return o;
}

VerifyFn VerifyArchive() {
  return [](const fs::path& p, std::wstring* why) { return VerifyReadable(p, why); };
}

void AddExtract(const wchar_t* in, std::function<bool()> avail, const wchar_t* limitation) {
  Capability c;
  c.id = Narrow(in) + ">extract"; c.group = Group::Archive; c.in = in; c.out = L"extract"; c.engine = "native";
  c.producesFolder = true; c.lossy = false; c.limitation = limitation; c.available = avail;
  c.options = {SkipUnsafeOpt()};
  if (std::wstring(in) == L"zip") c.options.push_back(EncodingOpt());
  c.run = [](const ConvertTask& t, JobContext& ctx) {
    ExtractResult r = ExtractArchive(t.input, t.output, EOpts(t.options), &ctx);
    EngineResult er;
    er.ok = r.ok; er.error = r.error; er.warnings = r.warnings;
    return er;
  };
  CapabilityRegistry::Get().Add(std::move(c));
}

enum class Kind { Repack, GzipTar, GunzipTar, Folder };

void AddConv(const wchar_t* in, const wchar_t* out, Group g, Kind kind, OutKind ok, std::function<bool()> avail, const wchar_t* limitation) {
  Capability c;
  c.id = Narrow(in) + ">" + Narrow(out); c.group = g; c.in = in; c.out = out; c.engine = "native";
  c.lossy = false; c.limitation = limitation; c.available = avail;
  bool toZip = std::wstring(out) == L"zip";
  bool toGz = std::wstring(out) == L"gz";
  if (toZip) c.options = {MethodOpt(), LevelOpt()};
  else if (toGz) c.options = {LevelOpt()};
  if (kind == Kind::Repack && std::wstring(in) != L"folder") { c.options.push_back(SkipUnsafeOpt()); if (std::wstring(in) == L"zip") c.options.push_back(EncodingOpt()); }
  c.verify = VerifyArchive();
  c.run = [kind, ok](const ConvertTask& t, JobContext& ctx) -> EngineResult {
    CreateOptions co = COpts(t.options);
    switch (kind) {
      case Kind::Folder: return ToEngine(WriteFromPaths({t.input}, ok, t.output, co, &ctx));
      case Kind::GzipTar: return ToEngine(GzipFile(t.input, t.output, co, &ctx));
      case Kind::GunzipTar: return ToEngine(GunzipToTar(t.input, t.output, co, &ctx));
      default: return ToEngine(Repack(t.input, ok, t.output, co, EOpts(t.options), &ctx));
    }
  };
  CapabilityRegistry::Get().Add(std::move(c));
}

} // namespace

void RegisterArchiveEngine() {
  auto none = std::function<bool()>();
  auto rar = std::function<bool()>([] { return BsdtarCanReadRar(); });
  auto sevenz = std::function<bool()>([] { return BsdtarCanRead7z(); });
  const wchar_t* linkNote = L"Symbolic links are never created. Unsafe entries (.. paths, absolute paths, drive letters, alternate data streams) make the whole archive be refused unless 'skip unsafe entries' is on.";
  const wchar_t* repackNote = L"Entry by entry; symbolic links, owners and permissions are not carried over. Unsafe entries are refused. Password-protected archives are not supported.";

  AddExtract(L"zip", none, linkNote);
  AddExtract(L"tar", none, linkNote);
  AddExtract(L"gz", none, L"Handles .tar.gz / .tgz (extracts the files inside) and single compressed files. Unsafe entries refuse the archive unless 'skip unsafe entries' is on.");
  AddExtract(L"rar", rar, L"Read through Windows' built-in tar.exe (libarchive): RAR4/RAR5 without a password. The archive is converted to a temporary tar first, so it needs scratch disk space.");
  AddExtract(L"7z", sevenz, L"Read through Windows' built-in tar.exe (libarchive): 7z without a password. The archive is converted to a temporary tar first, so it needs scratch disk space.");

  AddConv(L"zip", L"tar", Group::Archive, Kind::Repack, OutKind::Tar, none, repackNote);
  AddConv(L"zip", L"gz", Group::Archive, Kind::Repack, OutKind::TarGz, none, L"Produces a .tar.gz (a gzip-compressed tar of the zip's contents). Same limits as zip>tar.");
  AddConv(L"tar", L"zip", Group::Archive, Kind::Repack, OutKind::Zip, none, repackNote);
  AddConv(L"tar", L"gz", Group::Archive, Kind::GzipTar, OutKind::TarGz, none, L"Compresses the tar file byte for byte into a .tar.gz.");
  AddConv(L"gz", L"zip", Group::Archive, Kind::Repack, OutKind::Zip, none, repackNote);
  AddConv(L"gz", L"tar", Group::Archive, Kind::GunzipTar, OutKind::Tar, none, L"Decompresses one gzip layer. If the .gz holds a single file (not a tar) it is wrapped into a new tar and a warning says so.");
  AddConv(L"rar", L"zip", Group::Archive, Kind::Repack, OutKind::Zip, rar, repackNote);
  AddConv(L"rar", L"tar", Group::Archive, Kind::Repack, OutKind::Tar, rar, repackNote);
  AddConv(L"rar", L"gz", Group::Archive, Kind::Repack, OutKind::TarGz, rar, L"Produces a .tar.gz. Same limits as rar>zip.");
  AddConv(L"7z", L"zip", Group::Archive, Kind::Repack, OutKind::Zip, sevenz, repackNote);
  AddConv(L"7z", L"tar", Group::Archive, Kind::Repack, OutKind::Tar, sevenz, repackNote);
  AddConv(L"7z", L"gz", Group::Archive, Kind::Repack, OutKind::TarGz, sevenz, L"Produces a .tar.gz. Same limits as 7z>zip.");

  const wchar_t* folderNote = L"The folder (with its name as the top-level entry) is archived with names, empty folders and modified times; links / junctions are skipped.";
  AddConv(L"folder", L"zip", Group::Folder, Kind::Folder, OutKind::Zip, none, folderNote);
  AddConv(L"folder", L"tar", Group::Folder, Kind::Folder, OutKind::Tar, none, folderNote);
  AddConv(L"folder", L"gz", Group::Folder, Kind::Folder, OutKind::TarGz, none, L"Produces a .tar.gz of the folder.");
}

} // namespace filo
