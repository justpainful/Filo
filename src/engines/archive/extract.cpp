// ListArchive + ExtractArchive: validation of every entry, size / ratio / disk guards, canonical-path containment,
// collision-safe names, preserved timestamps.
#include "core/output.h"
#include "engines/archive/internal.h"
#include <algorithm>
#include <unordered_set>

namespace filo::archive {

using namespace detail;

namespace {

std::wstring ShortName(const std::wstring& n) {
  std::wstring s = n.substr(0, 80);
  for (auto& c : s) if (c < 0x20) c = L'?';
  if (n.size() > 80) s += L"...";
  return s;
}

// Canonical final path of an existing file / directory (handle based, so junctions and short names are resolved).
std::wstring CanonicalOf(const fs::path& p, bool isDir) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::wstring buf(1024, L'\0');
  DWORD n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(), FILE_NAME_NORMALIZED);
  if (n >= buf.size()) { buf.resize(n + 1); n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(), FILE_NAME_NORMALIZED); }
  CloseHandle(h);
  (void)isDir;
  buf.resize(n);
  return buf;
}

bool IsInside(const std::wstring& rootCanon, const std::wstring& candidate) {
  if (candidate.size() <= rootCanon.size()) return false;
  return CompareStringOrdinal(candidate.data(), (int)rootCanon.size(), rootCanon.data(), (int)rootCanon.size(), TRUE) == CSTR_EQUAL;
}

// Deletes everything inside `dir` (keeps `dir`). Uses \\?\ paths so trees deeper than MAX_PATH go too.
void RemoveChildren(const fs::path& dir) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    std::wstring p = e.path().wstring();
    if (p.rfind(L"\\\\?\\", 0) != 0) p = L"\\\\?\\" + fs::absolute(e.path()).wstring();
    std::error_code e2;
    fs::remove_all(fs::path(p), e2);
  }
}

struct WarnSink {
  std::vector<std::wstring>* v;
  size_t count = 0, extra = 0;
  void Add(std::wstring w) { if (++count <= 40) v->push_back(std::move(w)); else ++extra; }
  void Flush() { if (extra) v->push_back(L"... and " + std::to_wstring(extra) + L" more notices."); extra = 0; }
};

} // namespace

ListResult ListArchive(const fs::path& file, const ListOptions& opt, JobContext* ctx) {
  ListResult res;
  try {
    ReaderConfig cfg;
    cfg.nameEncoding = opt.nameEncoding; cfg.maxEntries = opt.maxEntries;
    auto rd = OpenReader(file, cfg, ctx);
    res.format = rd->format();
    res.archiveSize = rd->RawSize();
    PathPolicy pol;
    Entry e;
    while (rd->Next(e)) {
      if (ctx && ctx->Cancelled()) throw ArcCancelled{};
      if (res.entries.size() >= opt.maxEntries) Fail(L"The archive has too many entries.");
      PathCheck pc = ValidateEntryPath(e.name, pol);
      if (!pc.Usable()) { e.unsafe = true; e.problem = L"Refused: " + pc.reason; res.hasUnsafe = true; }
      else { e.safeName = pc.path; if (pc.verdict == PathVerdict::Fixed) e.problem = L"Renamed: " + pc.reason; }
      if (!e.unsafe && (e.type == EntryType::Symlink || e.type == EntryType::Hardlink)) {
        if (!e.linkTarget.empty()) {
          PathCheck lc = ValidateLinkTarget(e.name, e.linkTarget, e.type == EntryType::Hardlink);
          if (!lc.Usable()) { e.unsafe = true; e.problem = L"Refused: " + lc.reason; res.hasUnsafe = true; }
        }
        if (!e.unsafe) e.problem = e.type == EntryType::Symlink ? L"Symbolic links are not created" : e.problem;
      }
      if (e.type == EntryType::File) res.totalSize += e.size;
      res.entries.push_back(e);
    }
    res.warnings = rd->warnings;
    res.encrypted = rd->anyEncrypted;
    res.ok = true;
  } catch (const ArcError& e) {
    res.error = e.msg;
  } catch (const ArcCancelled&) {
    res.error = L"Cancelled";
  } catch (const std::bad_alloc&) {
    res.error = L"The archive is too large to list (out of memory).";
  } catch (const std::exception&) {
    res.error = L"The archive could not be read.";
  }
  return res;
}

ExtractResult ExtractArchive(const fs::path& file, const fs::path& destDir, const ExtractOptions& opt, JobContext* ctx) {
  ExtractResult res;
  WarnSink warn{&res.warnings};
  try {
    std::error_code ec;
    if (!fs::is_directory(destDir, ec)) Fail(L"The destination folder does not exist.");
    std::wstring rootCanon = CanonicalOf(destDir, true);
    if (rootCanon.empty()) Fail(L"The destination folder cannot be opened.");
    if (rootCanon.back() != L'\\') rootCanon.push_back(L'\\');
    size_t destLen = LongPath(destDir).size();

    ReaderConfig cfg;
    cfg.nameEncoding = opt.nameEncoding; cfg.maxEntries = opt.maxEntries; cfg.maxTotalBytes = opt.maxTotalBytes;
    auto rd = OpenReader(file, cfg, ctx);
    res.format = rd->format();
    uint64_t archSize = std::max<uint64_t>(1, rd->RawSize());

    uint64_t kFiles = 0, kBytes = 0;
    uint64_t freeSpace = FreeSpace(destDir);
    if (rd->KnownTotals(kFiles, kBytes)) {
      if (kBytes > opt.maxTotalBytes)
        Fail(L"The archive expands to " + FormatSize(kBytes) + L", which is more than the safety limit of " + FormatSize(opt.maxTotalBytes) + L".");
      if (kBytes > opt.ratioFloorBytes && (double)kBytes / (double)archSize > opt.maxRatio)
        Fail(L"The archive looks like a decompression bomb (it would expand " + std::to_wstring((uint64_t)((double)kBytes / (double)archSize)) + L" times) and was not extracted.");
      if (freeSpace != UINT64_MAX && freeSpace < kBytes + opt.reserveFreeBytes)
        Fail(L"There is not enough free disk space: the archive needs " + FormatSize(kBytes) + L" but only " + FormatSize(freeSpace) + L" is free.");
    } else if (freeSpace != UINT64_MAX && freeSpace < opt.reserveFreeBytes) {
      Fail(L"There is not enough free disk space.");
    }

    PathPolicy pol = opt.path;
    pol.maxRelative = std::min<size_t>(pol.maxRelative, destLen + 8 < 32000 ? 32000 - destLen - 8 : 64);
    NameResolver resolver;
    std::unordered_set<std::wstring> madeDirs;
    std::vector<std::pair<std::wstring, uint64_t>> dirTimes;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t total = 0, entries = 0, sinceFreeCheck = 0, skippedLinks = 0, skippedOther = 0;
    double lastProgress = 0;
    bool longPathNoted = false;

    auto unsafe = [&](const std::wstring& name, const std::wstring& reason) {
      if (!opt.skipUnsafe)
        Fail(L"The archive contains an unsafe entry ('" + ShortName(name) + L"': " + reason + L") and was not extracted. Nothing was written.");
      ++res.skipped;
      warn.Add(L"Skipped an unsafe entry '" + ShortName(name) + L"': " + reason + L".");
    };

    auto ensureDir = [&](const std::wstring& rel) {
      // create every prefix of `rel` (a '\' separated safe path)
      size_t pos = 0;
      while (pos <= rel.size()) {
        size_t nx = rel.find(L'\\', pos);
        std::wstring prefix = rel.substr(0, nx == std::wstring::npos ? rel.size() : nx);
        if (!prefix.empty() && madeDirs.insert(ToLower(prefix)).second) {
          fs::path p = destDir / prefix;
          if (!CreateDirectoryW(LongPath(p).c_str(), nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
              if (err == ERROR_DISK_FULL) Fail(L"There is not enough free disk space.");
              Fail(L"A folder could not be created: " + LastErrorMessage(err));
            }
            DWORD a = GetFileAttributesW(LongPath(p).c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) Fail(L"A folder could not be created because a file with that name exists.");
          }
          std::wstring canon = CanonicalOf(p, true);
          if (!IsInside(rootCanon, canon + L"\\")) Fail(L"An entry would have been created outside of the destination folder; extraction was stopped.");
        }
        if (nx == std::wstring::npos) break;
        pos = nx + 1;
      }
    };

    Entry e;
    while (rd->Next(e)) {
      if (ctx && ctx->Cancelled()) throw ArcCancelled{};
      if (++entries > opt.maxEntries) Fail(L"The archive has too many entries.");
      PathCheck pc = ValidateEntryPath(e.name, pol);
      if (!pc.Usable()) { unsafe(e.name, pc.reason); continue; }
      for (auto& n : pc.notes) warn.Add(L"'" + ShortName(e.name) + L"': " + n + L".");

      switch (e.type) {
        case EntryType::Dir: {
          if (pc.path.empty()) break;
          std::wstring note;
          std::wstring rel = resolver.Resolve(pc.path, true, &note);
          if (!note.empty()) warn.Add(note + L".");
          ensureDir(rel);
          dirTimes.emplace_back(rel, e.mtime);
          ++res.dirs;
          break;
        }
        case EntryType::File: {
          if (pc.path.empty()) { unsafe(e.name, L"a file cannot be named '.'"); break; }
          if (total + e.size > opt.maxTotalBytes) Fail(L"The archive expands to more than the safety limit of " + FormatSize(opt.maxTotalBytes) + L" and was not fully extracted.");
          if (e.size >= (64ull << 20) && freeSpace != UINT64_MAX && FreeSpace(destDir) < e.size + opt.reserveFreeBytes) Fail(L"There is not enough free disk space.");
          std::wstring note;
          std::wstring rel = resolver.Resolve(pc.path, false, &note);
          if (!note.empty()) warn.Add(note + L".");
          size_t sl = rel.rfind(L'\\');
          if (sl != std::wstring::npos) ensureDir(rel.substr(0, sl));
          fs::path out = destDir / rel;
          if (!longPathNoted && LongPath(out).size() > 259) { longPathNoted = true; warn.Add(L"Some extracted paths are longer than 259 characters; some programs cannot open them."); }
          FileWriter w(out, true);
          std::wstring canon = CanonicalOf(out, false);
          if (!IsInside(rootCanon, canon)) Fail(L"An entry would have been created outside of the destination folder; extraction was stopped.");
          uint64_t got = 0;
          for (;;) {
            size_t g = rd->Read(buf.data(), buf.size());
            if (!g) break;
            w.Write(buf.data(), g);
            got += g; total += g; sinceFreeCheck += g;
            if (total > opt.maxTotalBytes) Fail(L"The archive expands to more than the safety limit of " + FormatSize(opt.maxTotalBytes) + L" and was not fully extracted.");
            uint64_t raw = std::max<uint64_t>(1, rd->RawConsumed());
            if (total > opt.ratioFloorBytes && (double)total / (double)raw > opt.maxRatio)
              Fail(L"The archive looks like a decompression bomb (its data expands more than " + std::to_wstring((uint64_t)opt.maxRatio) + L" times) and extraction was stopped.");
            if (sinceFreeCheck > (256ull << 20)) {
              sinceFreeCheck = 0;
              if (FreeSpace(destDir) < opt.reserveFreeBytes) Fail(L"There is not enough free disk space.");
            }
            if (ctx) {
              if (ctx->Cancelled()) throw ArcCancelled{};
              double p = rd->Progress();
              if (p - lastProgress > 0.002) { lastProgress = p; ctx->SetProgress(p); }
            }
          }
          if (e.size && got != e.size && res.format != Format::Gz) Fail(L"The archive is damaged (the size of '" + ShortName(e.name) + L"' does not match).");
          w.Flush();
          w.SetTime(e.mtime);
          w.Close();
          ++res.files; res.bytes += got;
          res.written.push_back(rel);
          break;
        }
        case EntryType::Symlink: {
          std::wstring target = e.linkTarget;
          if (target.empty()) {      // ZIP stores the target as the entry data
            std::string t; size_t g;
            while ((g = rd->Read(buf.data(), std::min<size_t>(buf.size(), 8192))) > 0 && t.size() < 8192) t.append((const char*)buf.data(), g);
            target = Widen(t);
          }
          PathCheck lc = ValidateLinkTarget(e.name, target, false);
          if (!lc.Usable()) { unsafe(e.name, L"symbolic link: " + lc.reason); break; }
          ++skippedLinks; ++res.skipped;
          break;
        }
        case EntryType::Hardlink: {
          PathCheck lc = ValidateLinkTarget(e.name, e.linkTarget, true);
          if (!lc.Usable()) { unsafe(e.name, L"hard link: " + lc.reason); break; }
          std::wstring src = resolver.Lookup(lc.path);
          if (src.empty() || pc.path.empty()) { warn.Add(L"A hard link to '" + ShortName(e.linkTarget) + L"' was skipped (its target was not extracted).") ; ++res.skipped; break; }
          std::wstring note;
          std::wstring rel = resolver.Resolve(pc.path, false, &note);
          if (!note.empty()) warn.Add(note + L".");
          size_t sl = rel.rfind(L'\\');
          if (sl != std::wstring::npos) ensureDir(rel.substr(0, sl));
          if (!CopyFileW(LongPath(destDir / src).c_str(), LongPath(destDir / rel).c_str(), TRUE)) Fail(L"A hard link could not be copied: " + LastErrorMessage());
          ++res.files; res.written.push_back(rel);
          break;
        }
        default: ++skippedOther; ++res.skipped; break;
      }
    }

    if (skippedLinks) warn.Add(std::to_wstring(skippedLinks) + L" symbolic link(s) were not created (Windows needs special privileges for them).");
    if (skippedOther) warn.Add(std::to_wstring(skippedOther) + L" special file(s) (devices, pipes) were skipped.");
    for (auto& w : rd->warnings) warn.Add(w);
    // directory timestamps last (writing files changes them); deepest first
    std::sort(dirTimes.begin(), dirTimes.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    for (auto& [rel, t] : dirTimes) {
      if (!t) continue;
      HANDLE h = CreateFileW(LongPath(destDir / rel).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      FILETIME f{(DWORD)(t & 0xFFFFFFFFu), (DWORD)(t >> 32)};
      SetFileTime(h, nullptr, nullptr, &f);
      CloseHandle(h);
    }
    warn.Flush();
    if (res.files + res.dirs == 0) {
      res.error = res.skipped ? L"Nothing could be extracted: every entry was skipped." : L"The archive is empty.";
      return res;
    }
    if (ctx) ctx->SetProgress(1.0);
    res.ok = true;
  } catch (const ArcError& e) {
    res.error = e.msg;
  } catch (const ArcCancelled&) {
    res.error = L"Cancelled";
  } catch (const std::bad_alloc&) {
    res.error = L"The archive is too large to process (out of memory).";
  } catch (const std::exception&) {
    res.error = L"The archive could not be extracted.";
  }
  warn.Flush();
  if (!res.ok) RemoveChildren(destDir);     // never leave a half-extracted tree behind (long-path safe)
  return res;
}

ExtractResult ExtractArchiveToFolder(const fs::path& file, const fs::path& desiredDir, fs::path* finalDir, const ExtractOptions& opt, JobContext* ctx) {
  OutputDirTransaction tx(desiredDir);
  ExtractResult r = ExtractArchive(file, tx.TempDir(), opt, ctx);
  if (!r.ok) return r;
  std::wstring err;
  if (!tx.Commit(&err)) { r.ok = false; r.error = err; return r; }
  if (finalDir) *finalDir = tx.FinalDir();
  return r;
}

} // namespace filo::archive
