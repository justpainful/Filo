// WriteMetadata / StripMetadata / HasGps / CanWrite. Always writes a NEW file: copy -> edit the copy -> verify -> commit.
#include "core/output.h"
#include "engines/meta/internal.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace filo::meta {

using namespace detail;

bool CanWrite(const std::wstring& format) {
  std::wstring f = CanonicalExt(format);
  return ExifToolWritable(f) || FfmpegTaggable(f) || OfficeFormat(f);
}

namespace {

bool ValidKey(const std::string& k) {
  if (k.empty() || k.size() > 90) return false;
  if (!(isalnum((unsigned char)k[0]) || k[0] == '_')) return false;
  for (char c : k) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '#' || c == '*')) return false;
  return k.find("::") == std::string::npos;
}

std::wstring Arg(const std::string& key, const std::string& escapedValue) { return L"-" + Widen(key) + L"=" + Widen(escapedValue); }

bool ParseNum(const std::wstring& s, double& out) {
  std::string u = Narrow(Trim(s));
  if (u.empty()) return false;
  char* end = nullptr;
  out = strtod(u.c_str(), &end);
  return end && *end == 0 && std::isfinite(out);
}

std::string Fixed(double v, int dec) { char b[48]; snprintf(b, sizeof b, "%.*f", dec, v); return b; }

// "lat, lon[, alt]" decimal degrees
bool ParsePosition(const std::wstring& v, double& lat, double& lon, double& alt, bool& hasAlt, std::wstring* err) {
  std::vector<std::wstring> parts;
  std::wstring cur;
  for (wchar_t c : v) { if (c == L',' || c == L';' || c == L' ') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur.push_back(c); }
  if (!cur.empty()) parts.push_back(cur);
  hasAlt = false;
  if (parts.size() < 2 || parts.size() > 3 || !ParseNum(parts[0], lat) || !ParseNum(parts[1], lon)) { if (err) *err = L"The position must be written as \"latitude, longitude\" in decimal degrees."; return false; }
  if (parts.size() == 3) { if (!ParseNum(parts[2], alt)) { if (err) *err = L"The altitude must be a number."; return false; } hasAlt = true; }
  if (std::fabs(lat) > 90 || std::fabs(lon) > 180) { if (err) *err = L"The position is outside the valid range (latitude -90..90, longitude -180..180)."; return false; }
  return true;
}

struct Plan {
  std::vector<std::wstring> args;          // exiftool options (before the file)
  std::vector<std::wstring> warnings;
  std::wstring error;
};

void AddGpsRemoval(Plan& p, const std::wstring& format) {
  Kind k = KindOf(format);
  p.args.push_back(L"-gps:all=");
  p.args.push_back(L"-XMP-exif:GPS*=");
  p.args.push_back(L"-GPSLatitude=");
  p.args.push_back(L"-GPSLongitude=");
  p.args.push_back(L"-GPSAltitude=");
  p.args.push_back(L"-GPSPosition=");
  if (k == Kind::Video || k == Kind::Audio) {
    p.args.push_back(L"-Keys:GPSCoordinates=");
    p.args.push_back(L"-UserData:GPSCoordinates=");
    p.args.push_back(L"-ItemList:GPSCoordinates=");
    p.args.push_back(L"-QuickTime:GPSCoordinates=");
    p.args.push_back(L"-Keys:Location=");
    p.args.push_back(L"-UserData:LocationInformation=");
    p.args.push_back(L"-ItemList:Location=");
  }
}

void AddPersonalRemoval(Plan& p, const std::wstring& format) {
  static const char* tags[] = {"Artist", "Author", "Creator", "XMP-dc:Creator", "By-line", "By-lineTitle", "Credit", "Source", "Contact", "Writer-Editor", "OwnerName",
                               "CameraOwnerName", "SerialNumber", "InternalSerialNumber", "BodySerialNumber", "LensSerialNumber", "CameraSerialNumber", "ImageUniqueID",
                               "Software", "ProcessingSoftware", "HostComputer", "CreatorTool", "LastModifiedBy", "Company", "Manager", "Producer", "XMP-aux:SerialNumber",
                               "XMP-aux:LensSerialNumber", "XMP-aux:OwnerName", "Keys:Author", "Keys:Make", "Keys:Model", "Keys:Software", "UserData:Make", "UserData:Model"};
  for (auto* t : tags) p.args.push_back(L"-" + Widen(t) + L"=");
  AddGpsRemoval(p, format);
}

// Builds the ExifTool argument plan for an edit. Returns false (plan.error set) when the request cannot be honoured.
bool BuildPlan(const fs::path& input, const std::wstring& format, const Edits& e, Plan& plan, JobContext* ctx) {
  Kind kind = KindOf(format);
  bool iptc = false;
  // 1) removeAll first, so explicit set/remove entries can re-add things afterwards
  if (e.removeAll) {
    plan.args.push_back(L"-all=");
    if (e.keepOrientationAndIcc) {
      for (const wchar_t* t : {L"-tagsfromfile", L"@", L"-Orientation", L"-ICC_Profile", L"-ColorSpace", L"-SRGBRendering", L"-Gamma", L"-PrimaryChromaticities", L"-WhitePoint"})
        plan.args.push_back(t);
    }
  }
  // 2) group removal: delete every editable field of the group that the file actually has
  if (!e.removeGroups.empty()) {
    MetadataSet cur = ReadMetadata(input, ctx);
    if (!cur.ok) { plan.error = cur.error; return false; }
    std::set<std::string> done;
    for (Group g : e.removeGroups) {
      if (g == Group::Gps) AddGpsRemoval(plan, format);
      for (auto& f : cur.fields)
        if (f.group == g && f.editable && ValidKey(f.key) && done.insert(f.key).second) plan.args.push_back(L"-" + Widen(f.key) + L"=");
    }
  }
  // 3) explicit removals
  for (auto& k : e.remove) {
    if (!ValidKey(k)) { plan.error = L"Invalid field name: " + Widen(k).substr(0, 60); return false; }
    plan.args.push_back(L"-" + Widen(k) + L"=");
  }
  // 4) values
  for (auto& [key, value] : e.set) {
    if (!ValidKey(key)) { plan.error = L"Invalid field name: " + Widen(key).substr(0, 60); return false; }
    if (key == "GPS:Position" || key == "Composite:GPSPosition") {
      double lat, lon, alt = 0; bool hasAlt;
      if (Trim(value).empty()) { AddGpsRemoval(plan, format); continue; }
      if (!ParsePosition(value, lat, lon, alt, hasAlt, &plan.error)) return false;
      if (kind == Kind::Video) {
        std::string c = Fixed(lat, 6) + "," + Fixed(lon, 6) + (hasAlt ? "," + Fixed(alt, 2) : "");
        plan.args.push_back(Arg("Keys:GPSCoordinates", c));
        plan.args.push_back(Arg("UserData:GPSCoordinates", c));
      } else {
        plan.args.push_back(Arg("GPSLatitude", Fixed(std::fabs(lat), 8)));
        plan.args.push_back(Arg("GPSLatitudeRef", lat < 0 ? "S" : "N"));
        plan.args.push_back(Arg("GPSLongitude", Fixed(std::fabs(lon), 8)));
        plan.args.push_back(Arg("GPSLongitudeRef", lon < 0 ? "W" : "E"));
        if (hasAlt) {
          plan.args.push_back(Arg("GPSAltitude", Fixed(std::fabs(alt), 2)));
          plan.args.push_back(Arg("GPSAltitudeRef", alt < 0 ? "1" : "0"));
        }
      }
      continue;
    }
    if (key.rfind("IPTC:", 0) == 0) iptc = true;
    // list tags (IPTC / XMP keywords, subjects, creators...) take one entry per line; everything else keeps its line breaks
    std::string grp = key.find(':') == std::string::npos ? std::string() : key.substr(0, key.find(':'));
    std::string tag = key.substr(key.find(':') == std::string::npos ? 0 : key.find(':') + 1);
    static const std::set<std::string> listTags = {"Keywords", "Subject", "Creator", "Contributor", "Language", "SupplementalCategories", "By-line", "ContentLocationName",
                                                   "ContentLocationCode", "Publisher", "Relation", "Type", "Format", "Identifier", "CatalogSets", "Category"};
    bool listish = listTags.count(tag) > 0 && (grp.empty() || grp.rfind("IPTC", 0) == 0 || grp.rfind("XMP", 0) == 0);
    if (!listish || value.find(L'\n') == std::wstring::npos) {
      plan.args.push_back(Arg(key, EscapeValue(value)));
      continue;
    }
    std::vector<std::wstring> lines;
    {
      std::wstring cur;
      for (wchar_t c : value) { if (c == L'\n') { lines.push_back(cur); cur.clear(); } else if (c != L'\r') cur.push_back(c); }
      lines.push_back(cur);
    }
    plan.args.push_back(L"-" + Widen(key) + L"=");                       // clear, then add each entry
    for (auto& ln : lines) if (!ln.empty()) plan.args.push_back(Arg(key, EscapeValue(ln)));
  }
  if (iptc) plan.args.insert(plan.args.begin(), L"-IPTC:CodedCharacterSet=UTF8");
  // 5) cover art (M4A / MP4 via ExifTool)
  if (e.removeCoverArt) plan.args.push_back(L"-CoverArt=");
  if (e.coverArt) {
    if (kind != Kind::Audio && kind != Kind::Video) { plan.error = L"Cover art can only be set on audio files."; return false; }
    plan.args.push_back(L"-CoverArt<=" + e.coverArt->wstring());
  }
  // PDF: removals / edits are incremental updates, the old values stay inside the file's history
  if (kind == Kind::Pdf && (e.removeAll || !e.remove.empty() || !e.removeGroups.empty()))
    plan.warnings.push_back(L"PDF metadata is changed with an incremental update: the previous values can still be found inside the file's earlier revision.");
  return true;
}

std::wstring Sanitize(std::string s) {
  // drop " - <path>" suffixes ExifTool appends to messages and cap the length
  size_t p = s.find(" - ");
  if (p != std::string::npos && s.find(":\\", p) != std::string::npos) s.resize(p);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  std::wstring w = Widen(s);
  if (w.size() > 200) w.resize(200);
  return w;
}

// Parses ExifTool's write output. Returns true when the write succeeded.
bool ParseWrite(const EtRun& r, WriteResult& wr, bool lenient) {
  if (!r.started) { wr.error = L"ExifTool could not be started."; return false; }
  if (r.cancelled) { wr.error = L"Cancelled"; return false; }
  if (r.timedOut) { wr.error = L"ExifTool did not finish in time."; return false; }
  std::string all = r.err + "\n" + r.out;
  std::vector<std::wstring> errors;
  bool notUpdated = false, updated = false;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t e = all.find('\n', pos);
    std::string line = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    while (!line.empty() && (line.back() == '\r')) line.pop_back();
    pos = e == std::string::npos ? all.size() : e + 1;
    if (line.rfind("Error: ", 0) == 0) errors.push_back(Sanitize(line.substr(7)));
    else if (line.rfind("Warning: ", 0) == 0) {
      std::string w = line.substr(9);
      size_t d = w.find("doesn't exist or isn't writable");
      if (d != std::string::npos) {
        if (!lenient) {
          size_t s = w.find("Sorry, ");
          errors.push_back(L"'" + Widen(w.substr(s == std::string::npos ? 0 : s + 7, d - (s == std::string::npos ? 0 : s + 7))) + L"' cannot be written to this file type");
        }
      } else if (w.find("is not defined") != std::string::npos) {
        if (!lenient) errors.push_back(L"Unknown field: " + Widen(w.substr(0, w.find(" is not defined"))));
      } else if (w.find("Nothing to do") == std::string::npos) wr.warnings.push_back(Sanitize(w));
    }
    else if (line.find("weren't updated due to errors") != std::string::npos) notUpdated = true;
    else if (line.find("image files updated") != std::string::npos || line.find("files created") != std::string::npos) updated = true;
  }
  if (!errors.empty()) { wr.error = L"The metadata could not be written: " + errors[0]; return false; }
  if (notUpdated) { wr.error = L"The metadata could not be written to this file."; return false; }
  if (!updated) wr.warnings.push_back(L"No change was needed (the file already had these values).");
  return true;
}

bool CopyToTemp(const fs::path& in, const fs::path& tmp, std::wstring* err) {
  if (!CopyFileW(LongPath(in).c_str(), LongPath(tmp).c_str(), FALSE)) { *err = L"The working copy could not be created: " + LastErrorMessage(); return false; }
  SetFileAttributesW(LongPath(tmp).c_str(), FILE_ATTRIBUTE_NORMAL);
  return true;
}

WriteResult Run(const fs::path& input, const Edits& edits, const fs::path& desiredOut, JobContext* ctx, bool lenient, const Plan* preset) {
  WriteResult r;
  FileKind fk = DetectFile(input);
  if (!fk.exists) { r.error = L"The file is no longer available."; return r; }
  if (fk.isDir) { r.error = L"This is a folder, not a file."; return r; }
  if (!fk.readable) { r.error = L"The file cannot be opened (access denied or in use)."; return r; }
  if (fk.cloudPlaceholder) { r.error = L"This file is stored online only. Download it first, then try again."; return r; }
  if (fk.size == 0) { r.error = L"The file is empty."; return r; }
  std::wstring fmt = fk.format;
  if (!CanWrite(fmt)) { r.error = L"Editing metadata is not supported for " + (fmt.empty() ? std::wstring(L"this kind of") : fmt) + L" files."; return r; }
  if (!preset && edits.Empty()) { r.error = L"There is nothing to change."; return r; }
  if (fk.extensionMismatch) r.warnings.push_back(L"The file extension does not match its content (detected: " + fmt + L"); it was edited as a " + fmt + L" file.");

  std::wstring how;
  std::optional<std::string> before = ContentFingerprint(input, fmt, &how, ctx);
  if (ctx && ctx->Cancelled()) { r.error = L"Cancelled"; return r; }
  if (ctx) ctx->SetProgress(0.15);

  OutputTransaction tx(desiredOut);
  const fs::path tmp = tx.TempPath();
  std::error_code ec;
  fs::create_directories(tmp.parent_path(), ec);
  // the working copy carries the REAL extension (ExifTool / ffmpeg choose the writer from it), whatever name the caller asked for
  fs::path work = tmp;
  work.replace_extension(L"." + fmt);
  auto dropWork = OnExit([&] { if (work != tmp) { std::error_code e2; fs::remove(work, e2); } });

  if (OfficeFormat(fmt)) {
    WriteResult o = WriteOffice(input, edits, work, edits.removeAll, false);
    if (!o.ok) { r.error = o.error; return r; }
    for (auto& w : o.warnings) r.warnings.push_back(w);
  } else if (FfmpegTaggable(fmt)) {
    WriteResult o = WriteViaFfmpeg(input, fmt, edits, work, ctx);
    if (!o.ok) { r.error = o.error; return r; }
    for (auto& w : o.warnings) r.warnings.push_back(w);
  } else {
    if (!ExifToolAvailable()) { r.error = L"ExifTool is not installed, so metadata cannot be edited."; return r; }
    Plan plan;
    if (preset) plan = *preset;
    else if (!BuildPlan(input, fmt, edits, plan, ctx)) { r.error = plan.error; return r; }
    for (auto& w : plan.warnings) r.warnings.push_back(w);
    std::wstring err;
    if (!CopyToTemp(input, work, &err)) { r.error = err; return r; }
    std::vector<std::wstring> args = {L"-overwrite_original", L"-m", L"-E"};
    args.insert(args.end(), plan.args.begin(), plan.args.end());
    args.push_back(work.wstring());
    EtRun run = RunExifTool(args, ctx, 15 * 60 * 1000);
    if (!ParseWrite(run, r, lenient)) return r;
  }
  if (ctx) { if (ctx->Cancelled()) { r.error = L"Cancelled"; return r; } ctx->SetProgress(0.7); }

  // strip operations promise that no location is left: check instead of assuming
  if (lenient && (preset || edits.removeAll || !edits.removeGroups.empty()) && HasGps(work)) {
    r.error = L"Some location tags could not be removed (they were written by a tool that ExifTool cannot edit). Use \"Remove all metadata\" for this file, or remove them with the tool that created them.";
    return r;
  }
  // the output must exist, still be the same kind of file, and carry identical content
  FileKind ok = DetectFile(work);
  if (!ok.exists || ok.size == 0) { r.error = L"The edited file was not created."; return r; }
  if (ok.format != fmt && std::getenv("META_DEBUG_KEEP")) CopyFileW(LongPath(work).c_str(), L"C:/Users/kuroi/AppData/Local/Temp/meta_debug.bin", FALSE);
  if (ok.format != fmt) { r.error = L"The edited file is no longer recognised as " + fmt + L"; nothing was saved."; return r; }
  std::wstring how2;
  std::optional<std::string> after = ContentFingerprint(work, fmt, &how2, ctx);
  if (before) {
    if (!after) { r.error = L"The edited file could not be opened again to verify it; nothing was saved."; return r; }
    if (*before != *after) { r.error = L"Verification failed: the picture / audio / document content changed while editing, so nothing was saved."; return r; }
    r.contentVerified = true;
    r.verifyNote = how.empty() ? how2 : how;
  } else {
    r.warnings.push_back(L"The content could not be compared automatically for this format.");
  }
  if (work != tmp && !MoveFileExW(LongPath(work).c_str(), LongPath(tmp).c_str(), MOVEFILE_REPLACE_EXISTING)) { r.error = L"The edited file could not be saved: " + LastErrorMessage(); return r; }
  std::wstring cerr;
  if (!tx.Commit(&cerr)) { r.error = cerr; return r; }
  r.path = tx.FinalPath();
  r.ok = true;
  if (ctx) ctx->SetProgress(1.0);
  return r;
}

} // namespace

WriteResult WriteMetadata(const fs::path& input, const Edits& edits, const fs::path& desiredOut, JobContext* ctx) {
  return Run(input, edits, desiredOut, ctx, false, nullptr);
}

WriteResult StripMetadata(const fs::path& input, const fs::path& desiredOut, StripProfile profile, JobContext* ctx) {
  std::wstring fmt = FormatOf(input);
  Edits e;
  switch (profile) {
    case StripProfile::All: e.removeAll = true; e.keepOrientationAndIcc = false; break;
    case StripProfile::KeepOrientationAndICC: e.removeAll = true; e.keepOrientationAndIcc = true; break;
    case StripProfile::GpsOnly: e.removeGroups = {Group::Gps}; break;
    case StripProfile::PersonalInfo:
      if (OfficeFormat(fmt)) e.remove = {"Creator", "Author", "LastModifiedBy", "Company", "Manager", "Application", "Template"};
      else if (FfmpegTaggable(fmt)) e.remove = {"Artist", "Composer", "Comment", "Encoder", "Copyright", "Performer", "Conductor", "Publisher"};
      break;
  }
  if (profile == StripProfile::PersonalInfo && ExifToolWritable(fmt)) {
    Plan plan;
    AddPersonalRemoval(plan, fmt);
    if (KindOf(fmt) == Kind::Pdf) plan.warnings.push_back(L"PDF metadata is changed with an incremental update: the previous values can still be found inside the file's earlier revision.");
    return Run(input, e, desiredOut, ctx, true, &plan);
  }
  if (profile == StripProfile::GpsOnly && ExifToolWritable(fmt)) {
    Plan plan;
    AddGpsRemoval(plan, fmt);
    return Run(input, e, desiredOut, ctx, true, &plan);
  }
  if (e.removeAll && KindOf(fmt) == Kind::Image && !e.keepOrientationAndIcc) {
    // the picture may display rotated once the orientation tag is gone: tell the user
    WriteResult r = Run(input, e, desiredOut, ctx, true, nullptr);
    if (r.ok) r.warnings.push_back(L"The orientation and colour profile were removed too; some viewers may now show the picture rotated or with different colours.");
    return r;
  }
  return Run(input, e, desiredOut, ctx, true, nullptr);
}

} // namespace filo::meta
