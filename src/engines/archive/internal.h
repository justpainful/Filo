// Internal building blocks of the archive engine (not part of the public API; unit tests include it directly).
#pragma once
#include "engines/archive/archive.h"
#include <functional>
#include <exception>
#include <memory>

namespace filo::archive::detail {

// Thrown by readers / writers; caught at the public API boundary and turned into a user-facing sentence.
struct ArcError { std::wstring msg; };
struct ArcCancelled {};
[[noreturn]] void Fail(std::wstring msg);

constexpr uint64_t kUnixToFiletime = 116444736000000000ull;   // 1601 -> 1970 in 100 ns ticks
inline uint64_t UnixToFiletime(int64_t unixSec) { return unixSec <= 0 ? 0 : (uint64_t)unixSec * 10000000ull + kUnixToFiletime; }
inline int64_t FiletimeToUnix(uint64_t ft) { return ft < kUnixToFiletime ? 0 : (int64_t)((ft - kUnixToFiletime) / 10000000ull); }

// ---- raw file access --------------------------------------------------------------------------------------------------
class FileReader {
 public:
  explicit FileReader(const fs::path& p);       // throws ArcError when it cannot be opened
  ~FileReader();
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;
  uint64_t Size() const { return size_; }
  size_t ReadAt(uint64_t off, void* buf, size_t n);   // short only at end of file
  size_t Read(void* buf, size_t n);                    // sequential
  void Skip(uint64_t n) { pos_ += n; }
  uint64_t Pos() const { return pos_; }
  void Seek(uint64_t p) { pos_ = p; }
 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0, pos_ = 0;
};

class FileWriter {
 public:
  explicit FileWriter(const fs::path& p, bool createNew = false);  // CREATE_ALWAYS (the caller owns a temp path)
  ~FileWriter();
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;
  void Write(const void* d, size_t n);
  uint64_t Tell() const { return pos_ + buf_.size(); }
  void WriteAt(uint64_t off, const void* d, size_t n);   // flushes, writes at `off`, restores the position
  void Flush();
  void Close();
  void SetTime(uint64_t filetime);
  HANDLE Handle() const { return h_; }
 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  uint64_t pos_ = 0;            // file offset of the start of buf_
  std::vector<uint8_t> buf_;
};

// ---- sources / sinks ---------------------------------------------------------------------------------------------------
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual size_t Read(void* buf, size_t n) = 0;            // 0 = end of data
  virtual void Skip(uint64_t n);                            // default: read and discard
  virtual uint64_t RawConsumed() const = 0;                 // bytes consumed from the underlying file (progress / ratio)
  virtual uint64_t RawSize() const = 0;                     // size of the underlying file
};
class FileSource : public ByteSource {
 public:
  explicit FileSource(const fs::path& p) : f_(p) {}
  size_t Read(void* buf, size_t n) override { return f_.Read(buf, n); }
  void Skip(uint64_t n) override { f_.Skip(n); }
  uint64_t RawConsumed() const override { return f_.Pos(); }
  uint64_t RawSize() const override { return f_.Size(); }
  FileReader& file() { return f_; }
 private:
  FileReader f_;
};

// Streaming raw-deflate decoder (miniz tinfl). Input comes from a feed callback.
class Inflater {
 public:
  using Feed = std::function<size_t(void*, size_t)>;
  explicit Inflater(Feed feed);
  size_t Read(void* out, size_t n);       // 0 only at the end of the deflate stream; throws on corrupt / truncated input
  bool Done() const { return done_ && pendRd_ == pendWr_; }
  size_t ReadRaw(void* out, size_t n);    // after Done(): the bytes that follow the stream (gzip trailer, next member)
  void Reset();                           // start another deflate stream on the same input
  uint64_t InputConsumed() const { return consumed_; }
 private:
  std::shared_ptr<void> st_;     // tinfl_decompressor (opaque here)
  Feed feed_;
  std::vector<uint8_t> in_, dict_;
  size_t inPos_ = 0, inEnd_ = 0, dictOfs_ = 0, pendRd_ = 0, pendWr_ = 0;
  bool eof_ = false, done_ = false;
  uint64_t consumed_ = 0;
};

struct GzipInfo { std::string name; uint32_t mtime = 0; bool hasName = false; };

// gzip file (multi-member) as a byte stream; verifies CRC32 + ISIZE of every member.
class GunzipSource : public ByteSource {
 public:
  explicit GunzipSource(std::unique_ptr<FileSource> src);
  size_t Read(void* buf, size_t n) override;
  uint64_t RawConsumed() const override { return src_->RawConsumed(); }
  uint64_t RawSize() const override { return src_->RawSize(); }
  const GzipInfo& Info() { if (members_ == 0 && !finished_) StartMember(); return info_; }   // parses the first header on demand
  bool trailingGarbage = false;
 private:
  bool StartMember();
  std::unique_ptr<FileSource> src_;
  std::unique_ptr<Inflater> inf_;
  GzipInfo info_;
  int members_ = 0;
  bool inMember_ = false, finished_ = false;
  uint32_t crc_ = 0;
  uint64_t isize_ = 0;
};

class ByteSink {
 public:
  virtual ~ByteSink() = default;
  virtual void Write(const void* d, size_t n) = 0;
  virtual void Finish() {}
};
class FileSink : public ByteSink {
 public:
  explicit FileSink(const fs::path& p) : f_(p) {}
  void Write(const void* d, size_t n) override { f_.Write(d, n); }
  void Finish() override { f_.Flush(); f_.Close(); }
  FileWriter& file() { return f_; }
 private:
  FileWriter f_;
};

// Streaming raw-deflate encoder (miniz tdefl).
class Deflater {
 public:
  using Out = std::function<void(const void*, size_t)>;
  Deflater(int level, Out out);
  void Write(const void* d, size_t n);
  void Finish();
 private:
  std::shared_ptr<void> d_;      // tdefl_compressor (opaque here)
  Out out_;
  std::exception_ptr err_;
  friend struct DeflaterAccess;
};

class GzipSink : public ByteSink {
 public:
  GzipSink(ByteSink& out, int level, const std::string& utf8Name, int64_t mtimeUnix);
  void Write(const void* d, size_t n) override;
  void Finish() override;
 private:
  ByteSink& out_;
  Deflater def_;
  uint32_t crc_ = 0;
  uint64_t isize_ = 0;
  bool finished_ = false;
};

// ---- entry readers -----------------------------------------------------------------------------------------------------
struct ReaderConfig {
  int nameEncoding = 0;
  uint64_t maxEntries = 1000000;
  uint64_t maxTotalBytes = 8ull << 30;     // also caps the temporary tar written for bsdtar-backed formats
};

class EntryReader {
 public:
  virtual ~EntryReader() = default;
  virtual Format format() const = 0;
  // Advances to the next entry (skipping unread data of the current one). false = clean end. Throws ArcError on corruption.
  virtual bool Next(Entry& e) = 0;
  // Data of the current file entry; 0 at its end. The checksum (zip / gzip) is verified when the end is reached.
  virtual size_t Read(void* buf, size_t n) = 0;
  virtual double Progress() const = 0;
  virtual uint64_t RawConsumed() const = 0;
  virtual uint64_t RawSize() const = 0;
  // Totals known in advance (ZIP central directory); false when the format must be streamed.
  virtual bool KnownTotals(uint64_t& files, uint64_t& bytes) const { (void)files; (void)bytes; return false; }
  std::vector<std::wstring> warnings;
  bool anyEncrypted = false;
};

Format SniffFormat(const fs::path& file);
// Opens any supported archive. `ctx` is used for bsdtar conversions (cancel + scratch dir). Throws ArcError.
std::unique_ptr<EntryReader> OpenReader(const fs::path& file, const ReaderConfig& cfg, JobContext* ctx);
std::unique_ptr<EntryReader> OpenZipReader(const fs::path& file, const ReaderConfig& cfg, bool* needsBsdtar);
std::unique_ptr<EntryReader> OpenTarReader(std::unique_ptr<ByteSource> src, Format fmt, const ReaderConfig& cfg, std::function<void()> keepAlive = {});
std::unique_ptr<EntryReader> OpenGzSingleReader(std::unique_ptr<GunzipSource> src, const fs::path& archivePath);

// Converts any libarchive-readable archive to a temporary pax tar next to the job scratch dir; returns the tar path.
// The caller deletes it (see TempFileGuard). Throws ArcError with a user-facing message (password, unsupported, corrupt).
fs::path BsdtarToTar(const fs::path& archive, const ReaderConfig& cfg, JobContext* ctx);
struct TempFileGuard {
  fs::path dir;
  ~TempFileGuard();
};

// ---- entry writers -----------------------------------------------------------------------------------------------------
struct NewEntry {
  std::wstring name;               // archive name (UTF-16, '/' separators, directories without trailing slash)
  bool isDir = false;
  uint64_t size = 0;               // for files; used for the ZIP64 decision. sizeKnown=false => treat as potentially huge
  bool sizeKnown = true;
  uint64_t mtime = 0;              // FILETIME
  uint32_t mode = 0;               // 0 => 0644 / 0755
  bool compressible = true;
};

class EntryWriter {
 public:
  virtual ~EntryWriter() = default;
  virtual void Add(const NewEntry& e) = 0;           // starts an entry (directories complete immediately)
  virtual void Data(const void* d, size_t n) = 0;
  virtual void EndEntry() = 0;
  virtual void Finish() = 0;                         // writes the directory / end blocks, closes the file
  std::vector<WrittenEntry> written;
  std::vector<std::wstring> warnings;
};
std::unique_ptr<EntryWriter> NewZipWriter(const fs::path& out, int level, ZipMethod method);
std::unique_ptr<EntryWriter> NewTarWriter(std::unique_ptr<ByteSink> sink);
bool LooksAlreadyCompressed(const std::wstring& name);

// Name helpers
std::wstring NormalizeSeparators(std::wstring_view s);   // '\' -> '/'
std::string ToUtf8(std::wstring_view w);

// Destination name bookkeeping (case-insensitive collisions, file/dir conflicts). Pure; unit tested.
class NameResolver {
 public:
  // Resolves a safe relative path ('\' separated, validated) to the path to create. Adds " (n)" on collisions and records `note`.
  std::wstring Resolve(const std::wstring& safeRel, bool isDir, std::wstring* note);
  // The path an earlier entry was created at (for hard links); empty when unknown.
  std::wstring Lookup(const std::wstring& safeRel) const;
 private:
  struct Map;
  std::shared_ptr<Map> m_;
 public:
  NameResolver();
};

// The system default ANSI code page (what non-UTF-8-manifested tools such as tar.exe use), not this process's active one.
UINT SystemAnsiCodePage();

// Free space of the volume holding `dir` (UINT64_MAX when unknown).
uint64_t FreeSpace(const fs::path& dir);
std::wstring FormatSize(uint64_t n);

} // namespace filo::archive::detail
