// Format sniffing and reader construction.
#include "engines/archive/internal.h"
#include <algorithm>
#include <cstring>

namespace filo::archive {

const wchar_t* FormatName(Format f) {
  switch (f) {
    case Format::Zip: return L"zip"; case Format::Tar: return L"tar"; case Format::TarGz: return L"tar.gz"; case Format::Gz: return L"gz";
    case Format::SevenZ: return L"7z"; case Format::Rar: return L"rar"; case Format::OtherLibarchive: return L"archive";
    default: return L"unknown";
  }
}

namespace detail {

static bool ValidTarHeader(const uint8_t* h) {
  if (std::all_of(h, h + 512, [](uint8_t c) { return c == 0; })) return false;
  uint32_t sum = 0;
  for (size_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
  size_t i = 148;
  while (i < 156 && (h[i] == ' ' || h[i] == 0)) ++i;
  uint32_t stored = 0;
  bool any = false;
  for (; i < 156 && h[i] >= '0' && h[i] <= '7'; ++i) { stored = stored * 8 + (h[i] - '0'); any = true; }
  return any && stored == sum;
}

Format SniffFormat(const fs::path& file) {
  FileReader f(file);
  uint8_t h[512] = {};
  size_t n = f.ReadAt(0, h, sizeof h);
  if (n >= 4 && h[0] == 'P' && h[1] == 'K' && ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6) || (h[2] == 7 && h[3] == 8))) return Format::Zip;
  if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B) {
    try {
      GunzipSource g(std::make_unique<FileSource>(file));
      uint8_t blk[512];
      size_t got = 0;
      while (got < 512) { size_t r = g.Read(blk + got, 512 - got); if (!r) break; got += r; }
      if (got == 512 && ValidTarHeader(blk)) return Format::TarGz;
    } catch (const ArcError&) {
      return Format::Gz;       // damaged gzip: the real reader reports the problem
    }
    return Format::Gz;
  }
  if (n >= 6 && std::memcmp(h, "7z\xBC\xAF\x27\x1C", 6) == 0) return Format::SevenZ;
  if (n >= 7 && std::memcmp(h, "Rar!\x1a\x07", 6) == 0) return Format::Rar;
  if (n >= 3 && std::memcmp(h, "BZh", 3) == 0) return Format::OtherLibarchive;
  if (n >= 6 && std::memcmp(h, "\xFD" "7zXZ\0", 6) == 0) return Format::OtherLibarchive;
  if (n >= 4 && std::memcmp(h, "\x28\xB5\x2F\xFD", 4) == 0) return Format::OtherLibarchive;
  if (n >= 4 && std::memcmp(h, "\x04\x22\x4D\x18", 4) == 0) return Format::OtherLibarchive;
  if (n >= 512 && ValidTarHeader(h)) return Format::Tar;
  return Format::Unknown;
}

namespace {

class GuardedReader : public EntryReader {
 public:
  GuardedReader(fs::path scratch, std::unique_ptr<EntryReader> inner, Format fmt, uint64_t origSize)
      : guard_{std::move(scratch)}, inner_(std::move(inner)), fmt_(fmt), orig_(origSize) {}
  Format format() const override { return fmt_; }
  bool Next(Entry& e) override { bool r = inner_->Next(e); warnings = inner_->warnings; return r; }
  size_t Read(void* b, size_t n) override { return inner_->Read(b, n); }
  double Progress() const override { return inner_->Progress(); }
  uint64_t RawConsumed() const override { return (uint64_t)(inner_->Progress() * (double)orig_); }
  uint64_t RawSize() const override { return orig_; }
 private:
  TempFileGuard guard_;              // destroyed last: deletes the converted tar after the reader released it
  std::unique_ptr<EntryReader> inner_;
  Format fmt_;
  uint64_t orig_;
};

class GzSingleReader : public EntryReader {
 public:
  GzSingleReader(std::unique_ptr<GunzipSource> g, const fs::path& archive, uint64_t isizeHint) : g_(std::move(g)), path_(archive), hint_(isizeHint) {}
  Format format() const override { return Format::Gz; }
  bool Next(Entry& e) override {
    if (done_) return false;
    done_ = true;
    e = Entry{};
    std::string nm = g_->Info().hasName ? g_->Info().name : std::string();
    std::wstring name;
    if (!nm.empty()) {
      // RFC 1952 says ISO 8859-1, but most tools write the local / UTF-8 name
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, nm.data(), (int)nm.size(), nullptr, 0);
      if (n > 0) name = Widen(nm);
      else { n = MultiByteToWideChar(detail::SystemAnsiCodePage(), 0, nm.data(), (int)nm.size(), nullptr, 0); name.assign(n, L'\0'); MultiByteToWideChar(detail::SystemAnsiCodePage(), 0, nm.data(), (int)nm.size(), name.data(), n); }
      size_t sl = name.find_last_of(L"/\\");
      if (sl != std::wstring::npos) name = name.substr(sl + 1);       // never trust a path inside the gzip header
    }
    if (name.empty() || name == L"." || name == L"..") {
      name = path_.filename().wstring();
      std::wstring l = ToLower(name);
      if (l.size() > 3 && l.compare(l.size() - 3, 3, L".gz") == 0) name.resize(name.size() - 3);
      else if (l.size() > 4 && l.compare(l.size() - 4, 4, L".tgz") == 0) { name.resize(name.size() - 4); name += L".tar"; }
      else name += L".out";
      if (name.empty()) name = L"file";
    }
    e.name = name;
    e.type = EntryType::File;
    e.size = hint_;
    e.mtime = g_->Info().mtime ? UnixToFiletime(g_->Info().mtime) : 0;
    return true;
  }
  size_t Read(void* b, size_t n) override { return g_->Read(b, n); }
  double Progress() const override { return g_->RawSize() ? std::min(1.0, (double)g_->RawConsumed() / (double)g_->RawSize()) : 0.0; }
  uint64_t RawConsumed() const override { return g_->RawConsumed(); }
  uint64_t RawSize() const override { return g_->RawSize(); }
 private:
  std::unique_ptr<GunzipSource> g_;
  fs::path path_;
  uint64_t hint_;
  bool done_ = false;
};

} // namespace

std::unique_ptr<EntryReader> OpenGzSingleReader(std::unique_ptr<GunzipSource> src, const fs::path& archivePath) {
  uint64_t hint = 0;
  try {
    FileReader f(archivePath);
    uint8_t t[4];
    if (f.Size() >= 18 && f.ReadAt(f.Size() - 4, t, 4) == 4) hint = t[0] | (t[1] << 8) | (t[2] << 16) | ((uint64_t)t[3] << 24);
  } catch (const ArcError&) {}
  return std::make_unique<GzSingleReader>(std::move(src), archivePath, hint);
}

std::unique_ptr<EntryReader> OpenReader(const fs::path& file, const ReaderConfig& cfg, JobContext* ctx) {
  std::error_code ec;
  if (fs::is_directory(file, ec)) Fail(L"This is a folder, not an archive.");
  Format fmt = SniffFormat(file);
  bool viaBsdtar = false;
  switch (fmt) {
    case Format::Zip: {
      bool nb = false;
      auto r = OpenZipReader(file, cfg, &nb);
      if (r) return r;
      viaBsdtar = true;      // bzip2 / lzma / zstd / ppmd members
      break;
    }
    case Format::Tar: return OpenTarReader(std::make_unique<FileSource>(file), Format::Tar, cfg);
    case Format::TarGz: return OpenTarReader(std::make_unique<GunzipSource>(std::make_unique<FileSource>(file)), Format::TarGz, cfg);
    case Format::Gz: {
      auto g = std::make_unique<GunzipSource>(std::make_unique<FileSource>(file));
      return OpenGzSingleReader(std::move(g), file);
    }
    case Format::SevenZ: case Format::Rar: case Format::OtherLibarchive: viaBsdtar = true; break;
    default: Fail(L"This file is not a supported archive (zip, tar, gz, rar or 7z).");
  }
  (void)viaBsdtar;
  uint64_t orig = 0;
  { FileReader f(file); orig = f.Size(); }
  fs::path tarFile = BsdtarToTar(file, cfg, ctx);
  fs::path scratch = tarFile.parent_path();
  try {
    auto inner = OpenTarReader(std::make_unique<FileSource>(tarFile), Format::Tar, cfg);
    return std::make_unique<GuardedReader>(scratch, std::move(inner), fmt, orig);
  } catch (...) {
    std::error_code e2; fs::remove_all(scratch, e2);
    throw;
  }
}

} // namespace detail

Format DetectFormat(const fs::path& file) {
  try { return detail::SniffFormat(file); } catch (...) { return Format::Unknown; }
}

} // namespace filo::archive
