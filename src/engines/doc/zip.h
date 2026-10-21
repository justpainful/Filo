// ZIP container support for OOXML / ODF / EPUB. Writer: own implementation (UTF-8 names with the language-encoding flag, stored or
// raw-deflate entries via miniz's tdefl, no extra fields => EPUB-compliant "mimetype" entry). Reader: miniz, with hard limits
// against decompression bombs and a zip-slip-safe extractor.
#pragma once
#include "engines/doc/docutil.h"
#include <memory>
#include <optional>

namespace filo::doc {

class ZipWriter {
 public:
  ZipWriter() = default;
  ~ZipWriter();
  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;

  bool Open(const fs::path& path, std::wstring* err = nullptr);
  // `deflate=false` stores the entry (used for "mimetype" and already-compressed images).
  bool Add(const std::string& name, const void* data, size_t n, bool deflate = true);
  bool Add(const std::string& name, const std::string& s, bool deflate = true) { return Add(name, s.data(), s.size(), deflate); }
  bool Add(const std::string& name, const Bytes& b, bool deflate = true) { return Add(name, b.data(), b.size(), deflate); }
  bool Close();                          // writes the central directory; false when any earlier write failed
  bool ok() const { return !failed_; }
  size_t EntryCount() const { return entries_.size(); }

 private:
  struct Entry { std::string name; uint32_t crc, csize, usize, offset; uint16_t method, flags; };
  bool Write(const void* d, size_t n);
  void* h_ = nullptr;                    // HANDLE
  uint64_t pos_ = 0;
  std::vector<Entry> entries_;
  bool failed_ = false, closed_ = false;
  uint16_t dosTime_ = 0, dosDate_ = 0;
};

struct ZipLimits {
  uint64_t maxEntryBytes = 256ull << 20;     // one decompressed entry
  uint64_t maxTotalBytes = 2048ull << 20;    // sum of declared decompressed sizes
  size_t maxEntries = 100000;
  uint64_t bombRatio = 1000;                 // declared/compressed ratio above which an entry larger than 16 MiB is refused
};
ZipLimits& DefaultZipLimits();               // process-wide defaults (tests lower them)

class ZipReader {
 public:
  struct Entry { std::string name; uint64_t size = 0, csize = 0; bool isDir = false; bool encrypted = false; uint32_t crc = 0; };
  ZipReader();
  ~ZipReader();
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool Open(const fs::path& path, std::wstring* err = nullptr, const ZipLimits* lim = nullptr);
  bool OpenMemory(const uint8_t* data, size_t n, std::wstring* err = nullptr, const ZipLimits* lim = nullptr);   // data must outlive the reader
  void Close();
  bool IsOpen() const;
  const std::vector<Entry>& Entries() const { return entries_; }
  int Find(std::string_view name) const;                 // exact, then case-insensitive, leading '/' ignored; -1 when missing
  bool Has(std::string_view name) const { return Find(name) >= 0; }
  bool Read(std::string_view name, Bytes& out, std::wstring* err = nullptr);
  bool Read(int index, Bytes& out, std::wstring* err = nullptr);
  std::optional<std::string> ReadString(std::string_view name);     // empty optional when missing/unreadable
  // Extracts every entry under `dir`; unsafe names (zip-slip: "..", absolute, drive letters, ':' streams, reserved device names)
  // are skipped and counted in *skipped.
  bool ExtractAll(const fs::path& dir, std::wstring* err = nullptr, int* skipped = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<Entry> entries_;
  ZipLimits lim_;
  bool Setup(std::wstring* err);
};

// True when `name` (a ZIP entry name) may be extracted below a base directory.
bool IsSafeZipEntryName(std::string_view name);

} // namespace filo::doc
