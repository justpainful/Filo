// Archive writers driven from the file system (files / folders) or from another archive (re-pack), plus gz helpers.
#include "core/output.h"
#include "engines/archive/internal.h"
#include <algorithm>
#include <cstring>

namespace filo::archive {

using namespace detail;

namespace {

struct FsItem {
  fs::path path;
  std::wstring arc;
  bool isDir = false;
  uint64_t size = 0, mtime = 0;
};

uint64_t FtOf(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

std::wstring LowerPath(const fs::path& p) { return ToLower(LongPath(p)); }

void Walk(const fs::path& dir, const std::wstring& prefix, std::vector<FsItem>& out, uint64_t& bytes, std::vector<std::wstring>& warnings,
          const std::wstring& skipLower, JobContext* ctx) {
  struct Ent { std::wstring name; bool dir; uint64_t size, mtime; };
  std::vector<Ent> ents;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((LongPath(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return;     // empty
    Fail(L"The folder '" + dir.filename().wstring() + L"' could not be read: " + LastErrorMessage(e));
  }
  do {
    std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
      warnings.push_back(L"'" + n + L"' is a link / junction and was skipped.");
      continue;
    }
    ents.push_back({n, isDir, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, FtOf(fd.ftLastWriteTime)});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
  for (auto& e : ents) {
    if (ctx && ctx->Cancelled()) throw ArcCancelled{};
    FsItem it;
    it.path = dir / e.name;
    it.arc = prefix.empty() ? e.name : prefix + L"/" + e.name;
    it.isDir = e.dir; it.size = e.dir ? 0 : e.size; it.mtime = e.mtime;
    if (!e.dir && !skipLower.empty() && LowerPath(it.path) == skipLower) continue;
    out.push_back(it);
    if (e.dir) Walk(it.path, it.arc, out, bytes, warnings, skipLower, ctx);
    else bytes += e.size;
  }
}

void Collect(const std::vector<fs::path>& inputs, bool includeRoot, const fs::path& outFile, std::vector<FsItem>& items, uint64_t& bytes,
             std::vector<std::wstring>& warnings, JobContext* ctx) {
  NameResolver nr;
  std::wstring skip = LowerPath(outFile);
  for (auto& in : inputs) {
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (!GetFileAttributesExW(LongPath(in).c_str(), GetFileExInfoStandard, &ad)) Fail(L"'" + in.filename().wstring() + L"' is no longer available.");
    bool isDir = (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring leaf = in.filename().wstring();
    if (leaf.empty()) leaf = in.parent_path().filename().wstring();
    if (leaf.empty()) { leaf = in.root_name().wstring(); leaf.erase(std::remove(leaf.begin(), leaf.end(), L':'), leaf.end()); }
    if (leaf.empty()) leaf = L"folder";
    std::wstring note;
    std::wstring name = nr.Resolve(leaf, isDir, &note);
    if (!note.empty()) warnings.push_back(note + L".");
    if (isDir) {
      FsItem root;
      root.path = in; root.arc = name; root.isDir = true; root.mtime = FtOf(ad.ftLastWriteTime);
      if (includeRoot) { items.push_back(root); Walk(in, name, items, bytes, warnings, skip, ctx); }
      else Walk(in, L"", items, bytes, warnings, skip, ctx);
    } else {
      if (LowerPath(in) == skip) continue;
      FsItem f;
      f.path = in; f.arc = name; f.size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow; f.mtime = FtOf(ad.ftLastWriteTime);
      items.push_back(f);
      bytes += f.size;
    }
  }
}

// Sink that gzips into a file (used for tar.gz).
class GzFileSink : public ByteSink {
 public:
  GzFileSink(const fs::path& out, int level, const std::string& name, int64_t mtime) : file_(out), gz_(file_, level, name, mtime) {}
  void Write(const void* d, size_t n) override { gz_.Write(d, n); }
  void Finish() override { gz_.Finish(); }
 private:
  FileSink file_;
  GzipSink gz_;
};

std::unique_ptr<EntryWriter> MakeWriter(OutKind k, const fs::path& out, const CreateOptions& o) {
  switch (k) {
    case OutKind::Zip: return NewZipWriter(out, o.level, o.method);
    case OutKind::Tar: return NewTarWriter(std::make_unique<FileSink>(out));
    default: {
      FILETIME now; GetSystemTimeAsFileTime(&now);
      return NewTarWriter(std::make_unique<GzFileSink>(out, o.level, std::string(), FiletimeToUnix(FtOf(now))));
    }
  }
}

bool FinishAndVerify(EntryWriter& w, const fs::path& out, const CreateOptions& opt, JobContext* ctx, CreateResult& res) {
  w.Finish();
  for (auto& x : w.warnings) res.warnings.push_back(x);
  res.entries = w.written.size();
  for (auto& x : w.written) res.bytes += x.size;
  if (res.entries == 0) { res.error = L"There was nothing to add to the archive."; return false; }
  if (opt.verify) {
    if (BsdtarPath().empty()) { res.warnings.push_back(L"The result could not be double-checked (Windows' tar.exe was not found)."); }
    else {
      std::wstring why;
      if (!VerifyWithBsdtar(out, w.written, &why, ctx)) { res.error = L"The new archive failed verification: " + why; return false; }
    }
  }
  return true;
}

template <class F>
CreateResult Guarded(F&& f) {
  CreateResult res;
  try { return f(res); }
  catch (const ArcError& e) { res.ok = false; res.error = e.msg; }
  catch (const ArcCancelled&) { res.ok = false; res.error = L"Cancelled"; }
  catch (const std::bad_alloc&) { res.ok = false; res.error = L"Out of memory."; }
  catch (const std::exception&) { res.ok = false; res.error = L"The archive could not be created."; }
  return res;
}

struct Limiter {
  const ExtractOptions& o;
  uint64_t total = 0;
  void Add(uint64_t n, uint64_t raw) {
    total += n;
    if (total > o.maxTotalBytes) Fail(L"The archive expands to more than the safety limit of " + FormatSize(o.maxTotalBytes) + L".");
    if (total > o.ratioFloorBytes && (double)total / (double)std::max<uint64_t>(1, raw) > o.maxRatio)
      Fail(L"The archive looks like a decompression bomb and was not converted.");
  }
};

} // namespace

CreateResult WriteFromPaths(const std::vector<fs::path>& inputs, OutKind kind, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx) {
  return Guarded([&](CreateResult& res) {
    std::vector<FsItem> items;
    uint64_t totalBytes = 0;
    if (inputs.empty()) Fail(L"There is nothing to archive.");
    Collect(inputs, opt.includeRoot, outFile, items, totalBytes, res.warnings, ctx);
    if (items.empty()) Fail(L"There is nothing to archive (the folder is empty).");
    auto w = MakeWriter(kind, outFile, opt);
    std::vector<uint8_t> buf(1 << 20);
    uint64_t done = 0;
    double lastP = 0;
    for (auto& it : items) {
      if (ctx && ctx->Cancelled()) throw ArcCancelled{};
      NewEntry ne;
      ne.name = it.arc; ne.isDir = it.isDir; ne.size = it.size; ne.mtime = it.mtime;
      w->Add(ne);
      if (it.isDir) continue;
      try {
        FileReader f(it.path);
        for (;;) {
          size_t g = f.Read(buf.data(), buf.size());
          if (!g) break;
          w->Data(buf.data(), g);
          done += g;
          if (ctx) {
            if (ctx->Cancelled()) throw ArcCancelled{};
            double p = totalBytes ? (double)done / (double)totalBytes : 0;
            if (p - lastP > 0.002) { lastP = p; ctx->SetProgress(p * 0.95); }
          }
        }
      } catch (const ArcError& e) {
        if (e.msg.rfind(L"A file ", 0) == 0) throw;       // changed / grew while archiving
        Fail(L"'" + it.path.filename().wstring() + L"' could not be read: " + e.msg);
      }
      w->EndEntry();
    }
    if (!FinishAndVerify(*w, outFile, opt, ctx, res)) { res.ok = false; return res; }
    if (ctx) ctx->SetProgress(1.0);
    res.ok = true;
    return res;
  });
}

CreateResult Repack(const fs::path& inputArchive, OutKind kind, const fs::path& outFile, const CreateOptions& opt, const ExtractOptions& eopt, JobContext* ctx) {
  return Guarded([&](CreateResult& res) {
    ReaderConfig cfg;
    cfg.nameEncoding = eopt.nameEncoding; cfg.maxEntries = eopt.maxEntries; cfg.maxTotalBytes = eopt.maxTotalBytes;
    auto rd = OpenReader(inputArchive, cfg, ctx);
    uint64_t kFiles = 0, kBytes = 0;
    uint64_t archSize = std::max<uint64_t>(1, rd->RawSize());
    if (rd->KnownTotals(kFiles, kBytes)) {
      if (kBytes > eopt.maxTotalBytes) Fail(L"The archive expands to " + FormatSize(kBytes) + L", which is more than the safety limit of " + FormatSize(eopt.maxTotalBytes) + L".");
      if (kBytes > eopt.ratioFloorBytes && (double)kBytes / (double)archSize > eopt.maxRatio) Fail(L"The archive looks like a decompression bomb and was not converted.");
    }
    bool gzSingle = rd->format() == Format::Gz;
    uint64_t gzCount = 0;
    if (gzSingle && kind != OutKind::Zip) {       // tar headers need the size up front: count it first
      auto g = GunzipSource(std::make_unique<FileSource>(inputArchive));
      std::vector<uint8_t> tmp(1 << 20);
      size_t n;
      while ((n = g.Read(tmp.data(), tmp.size())) > 0) {
        gzCount += n;
        if (gzCount > eopt.maxTotalBytes) Fail(L"The archive expands to more than the safety limit of " + FormatSize(eopt.maxTotalBytes) + L".");
        if (ctx && ctx->Cancelled()) throw ArcCancelled{};
        if (ctx) ctx->SetProgress(0.4 * g.RawConsumed() / (double)std::max<uint64_t>(1, g.RawSize()));
      }
    }
    auto w = MakeWriter(kind, outFile, opt);
    PathPolicy pol = eopt.path;
    Limiter lim{eopt};
    std::vector<uint8_t> buf(1 << 20);
    Entry e;
    uint64_t skippedLinks = 0, skippedOther = 0, unsafeSkipped = 0;
    double lastP = 0;
    while (rd->Next(e)) {
      if (ctx && ctx->Cancelled()) throw ArcCancelled{};
      PathCheck pc = ValidateEntryPath(e.name, pol);
      auto unsafe = [&](const std::wstring& what) {
        if (!eopt.skipUnsafe) Fail(L"The archive contains an unsafe entry ('" + e.name.substr(0, 80) + L"': " + what + L") and was not converted.");
        ++unsafeSkipped;
        if (res.warnings.size() < 30) res.warnings.push_back(L"Skipped an unsafe entry '" + e.name.substr(0, 80) + L"': " + what + L".");
      };
      if (!pc.Usable()) { unsafe(pc.reason); continue; }
      if (pc.verdict == PathVerdict::Fixed && res.warnings.size() < 30) res.warnings.push_back(L"'" + e.name.substr(0, 80) + L"': " + pc.reason + L".");
      std::wstring name = pc.path;
      std::replace(name.begin(), name.end(), L'\\', L'/');
      switch (e.type) {
        case EntryType::Dir: {
          if (name.empty()) break;
          NewEntry ne; ne.name = name; ne.isDir = true; ne.mtime = e.mtime; ne.mode = e.mode;
          w->Add(ne);
          break;
        }
        case EntryType::File: {
          if (name.empty()) { unsafe(L"a file cannot be named '.'"); break; }
          NewEntry ne; ne.name = name; ne.mtime = e.mtime; ne.mode = e.mode; ne.size = e.size;
          if (gzSingle) {
            if (kind == OutKind::Zip) { ne.sizeKnown = false; ne.size = rd->RawSize() >= 4000000 ? 0xF0000000ull : 0; }
            else ne.size = gzCount;
          }
          w->Add(ne);
          for (;;) {
            size_t g = rd->Read(buf.data(), buf.size());
            if (!g) break;
            w->Data(buf.data(), g);
            lim.Add(g, rd->RawConsumed());
            if (ctx) {
              if (ctx->Cancelled()) throw ArcCancelled{};
              double p = rd->Progress();
              if (gzSingle && kind != OutKind::Zip) p = 0.4 + 0.6 * p;
              if (p - lastP > 0.002) { lastP = p; ctx->SetProgress(p * 0.97); }
            }
          }
          w->EndEntry();
          break;
        }
        case EntryType::Symlink: case EntryType::Hardlink: {
          if (!e.linkTarget.empty()) {
            PathCheck lc = ValidateLinkTarget(e.name, e.linkTarget, e.type == EntryType::Hardlink);
            if (!lc.Usable()) { unsafe(L"link: " + lc.reason); break; }
          }
          ++skippedLinks;
          break;
        }
        default: ++skippedOther; break;
      }
    }
    if (skippedLinks) res.warnings.push_back(std::to_wstring(skippedLinks) + L" link(s) were not carried over (the target format here stores plain files and folders only).");
    if (skippedOther) res.warnings.push_back(std::to_wstring(skippedOther) + L" special file(s) were skipped.");
    for (auto& x : rd->warnings) res.warnings.push_back(x);
    if (!FinishAndVerify(*w, outFile, opt, ctx, res)) { res.ok = false; return res; }
    if (ctx) ctx->SetProgress(1.0);
    res.ok = true;
    return res;
  });
}

CreateResult GunzipToTar(const fs::path& gzFile, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx) {
  return Guarded([&](CreateResult& res) {
    Format f = SniffFormat(gzFile);
    if (f == Format::Gz) {
      res.warnings.push_back(L"This .gz holds a single compressed file, not a tar archive; the file was wrapped into a new tar.");
      ExtractOptions eo;
      CreateResult r2 = Repack(gzFile, OutKind::Tar, outFile, opt, eo, ctx);
      r2.warnings.insert(r2.warnings.begin(), res.warnings.begin(), res.warnings.end());
      return r2;
    }
    if (f != Format::TarGz) Fail(L"This file is not a gzip-compressed tar archive.");
    {
      GunzipSource g(std::make_unique<FileSource>(gzFile));
      FileWriter out(outFile);
      std::vector<uint8_t> buf(1 << 20);
      double lastP = 0;
      size_t n;
      while ((n = g.Read(buf.data(), buf.size())) > 0) {
        out.Write(buf.data(), n);
        if (ctx) {
          if (ctx->Cancelled()) throw ArcCancelled{};
          double p = (double)g.RawConsumed() / (double)std::max<uint64_t>(1, g.RawSize());
          if (p - lastP > 0.002) { lastP = p; ctx->SetProgress(p * 0.9); }
        }
      }
      out.Close();
      if (g.trailingGarbage) res.warnings.push_back(L"Extra data after the gzip stream was ignored.");
    }
    // verify: list the produced tar with our reader, then compare with an independent reader
    if (opt.verify && !BsdtarPath().empty()) {
      ReaderConfig cfg;
      auto rd = OpenTarReader(std::make_unique<FileSource>(outFile), Format::Tar, cfg);
      std::vector<WrittenEntry> exp;
      Entry e;
      while (rd->Next(e)) {
        if (e.type != EntryType::File && e.type != EntryType::Dir) {
          // links / special entries: bsdtar lists them too, with size 0
        }
        WrittenEntry w;
        w.name = e.name;
        while (!w.name.empty() && (w.name.back() == L'/' || w.name.back() == L'\\')) w.name.pop_back();
        if (w.name.rfind(L"./", 0) == 0) { /* bsdtar prints names as stored */ }
        w.isDir = e.type == EntryType::Dir; w.size = e.size;
        if (e.type == EntryType::Symlink || e.type == EntryType::Hardlink) w.size = 0;
        exp.push_back(w);
      }
      std::wstring why;
      if (!VerifyWithBsdtar(outFile, exp, &why, ctx)) Fail(L"The produced tar failed verification: " + why);
      res.entries = exp.size();
    }
    if (ctx) ctx->SetProgress(1.0);
    res.ok = true;
    return res;
  });
}

CreateResult GzipFile(const fs::path& in, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx) {
  return Guarded([&](CreateResult& res) {
    FileReader src(in);
    {
      FileSink fs_(outFile);
      WIN32_FILE_ATTRIBUTE_DATA ad{};
      int64_t mt = 0;
      if (GetFileAttributesExW(LongPath(in).c_str(), GetFileExInfoStandard, &ad)) mt = FiletimeToUnix(FtOf(ad.ftLastWriteTime));
      GzipSink gz(fs_, opt.level, std::string(), mt);
      std::vector<uint8_t> buf(1 << 20);
      double lastP = 0;
      for (;;) {
        size_t g = src.Read(buf.data(), buf.size());
        if (!g) break;
        gz.Write(buf.data(), g);
        if (ctx) {
          if (ctx->Cancelled()) throw ArcCancelled{};
          double p = (double)src.Pos() / (double)std::max<uint64_t>(1, src.Size());
          if (p - lastP > 0.002) { lastP = p; ctx->SetProgress(p * 0.9); }
        }
      }
      gz.Finish();
    }
    // verify by decompressing and comparing the CRC against the source
    {
      GunzipSource g(std::make_unique<FileSource>(outFile));
      FileReader again(in);
      std::vector<uint8_t> a(1 << 20), b(1 << 20);
      for (;;) {
        size_t n = g.Read(a.data(), a.size());
        if (!n) break;
        size_t m = 0;
        while (m < n) { size_t k = again.Read(b.data() + m, n - m); if (!k) break; m += k; }
        if (m != n || std::memcmp(a.data(), b.data(), n) != 0) Fail(L"The compressed file does not match the original (the source changed while compressing?).");
      }
      uint8_t extra;
      if (again.Read(&extra, 1) != 0) Fail(L"The compressed file is shorter than the original.");
    }
    if (ctx) ctx->SetProgress(1.0);
    res.ok = true;
    return res;
  });
}

static CreateResult ViaTransaction(const fs::path& desiredOut, JobContext* ctx, const std::function<CreateResult(const fs::path&)>& body) {
  OutputTransaction tx(desiredOut);
  CreateResult r = body(tx.TempPath());
  if (!r.ok) return r;
  std::wstring err;
  if (!tx.Commit(&err)) { r.ok = false; r.error = err; return r; }
  r.path = tx.FinalPath();
  return r;
}

CreateResult CreateZip(const std::vector<fs::path>& inputs, const fs::path& out, const CreateOptions& opt, JobContext* ctx) {
  return ViaTransaction(out, ctx, [&](const fs::path& t) { return WriteFromPaths(inputs, OutKind::Zip, t, opt, ctx); });
}
CreateResult CreateTar(const std::vector<fs::path>& inputs, const fs::path& out, const CreateOptions& opt, JobContext* ctx) {
  return ViaTransaction(out, ctx, [&](const fs::path& t) { return WriteFromPaths(inputs, OutKind::Tar, t, opt, ctx); });
}
CreateResult CreateTarGz(const std::vector<fs::path>& inputs, const fs::path& out, const CreateOptions& opt, JobContext* ctx) {
  return ViaTransaction(out, ctx, [&](const fs::path& t) { return WriteFromPaths(inputs, OutKind::TarGz, t, opt, ctx); });
}

} // namespace filo::archive
