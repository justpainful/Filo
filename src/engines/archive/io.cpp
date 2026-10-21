// Raw file access, streaming deflate / inflate (miniz tinfl + tdefl), gzip source / sink.
#include "engines/archive/internal.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>

namespace filo::archive::detail {

void Fail(std::wstring msg) { throw ArcError{std::move(msg)}; }

std::wstring FormatSize(uint64_t n) { return FormatBytes(n); }

UINT SystemAnsiCodePage() {
  static UINT cp = [] {
    wchar_t b[16] = {};
    if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE, b, 16)) {
      UINT v = (UINT)_wtoi(b);
      if (v) return v;
    }
    return (UINT)1252;
  }();
  return cp;
}

uint64_t FreeSpace(const fs::path& dir) {
  ULARGE_INTEGER avail{}, total{}, freeb{};
  std::wstring p = dir.wstring();
  if (!GetDiskFreeSpaceExW(p.c_str(), &avail, &total, &freeb)) return UINT64_MAX;
  return avail.QuadPart;
}

// ---- FileReader / FileWriter ---------------------------------------------------------------------------------------------

FileReader::FileReader(const fs::path& p) {
  h_ = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_ == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) Fail(L"The file is no longer available.");
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) Fail(L"The file cannot be opened (access denied or in use).");
    Fail(L"The file cannot be opened: " + LastErrorMessage(e));
  }
  LARGE_INTEGER sz{};
  GetFileSizeEx(h_, &sz);
  size_ = (uint64_t)sz.QuadPart;
}
FileReader::~FileReader() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

size_t FileReader::ReadAt(uint64_t off, void* buf, size_t n) {
  size_t total = 0;
  auto* b = (uint8_t*)buf;
  while (total < n) {
    OVERLAPPED ov{};
    uint64_t o = off + total;
    ov.Offset = (DWORD)(o & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(o >> 32);
    DWORD want = (DWORD)std::min<size_t>(n - total, 1u << 30), got = 0;
    if (!ReadFile(h_, b + total, want, &got, &ov)) {
      DWORD e = GetLastError();
      if (e == ERROR_HANDLE_EOF) break;
      Fail(L"The archive could not be read: " + LastErrorMessage(e));
    }
    if (got == 0) break;
    total += got;
  }
  return total;
}
size_t FileReader::Read(void* buf, size_t n) {
  size_t g = ReadAt(pos_, buf, n);
  pos_ += g;
  return g;
}

FileWriter::FileWriter(const fs::path& p, bool createNew) {
  h_ = CreateFileW(LongPath(p).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, createNew ? CREATE_NEW : CREATE_ALWAYS,
                   FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_ == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) Fail(L"There is not enough free disk space.");
    Fail(L"The output file could not be created: " + LastErrorMessage(e));
  }
  buf_.reserve(1u << 20);
}
FileWriter::~FileWriter() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

void FileWriter::Flush() {
  if (buf_.empty()) return;
  size_t done = 0;
  while (done < buf_.size()) {
    DWORD w = 0;
    if (!WriteFile(h_, buf_.data() + done, (DWORD)std::min<size_t>(buf_.size() - done, 1u << 30), &w, nullptr)) {
      DWORD e = GetLastError();
      if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) Fail(L"There is not enough free disk space.");
      Fail(L"Writing the output failed: " + LastErrorMessage(e));
    }
    done += w;
  }
  pos_ += buf_.size();
  buf_.clear();
}
void FileWriter::Write(const void* d, size_t n) {
  if (n >= (1u << 20)) { Flush(); buf_.assign((const uint8_t*)d, (const uint8_t*)d + n); Flush(); return; }
  if (buf_.size() + n > (1u << 20)) Flush();
  buf_.insert(buf_.end(), (const uint8_t*)d, (const uint8_t*)d + n);
}
void FileWriter::WriteAt(uint64_t off, const void* d, size_t n) {
  Flush();
  OVERLAPPED ov{};
  ov.Offset = (DWORD)(off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(off >> 32);
  DWORD w = 0;
  if (!WriteFile(h_, d, (DWORD)n, &w, &ov) || w != n) Fail(L"Writing the output failed: " + LastErrorMessage());
  LARGE_INTEGER p; p.QuadPart = (LONGLONG)pos_;       // overlapped writes move the file pointer: restore it
  SetFilePointerEx(h_, p, nullptr, FILE_BEGIN);
}
void FileWriter::Close() {
  Flush();
  if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
}
void FileWriter::SetTime(uint64_t ft) {
  if (!ft || h_ == INVALID_HANDLE_VALUE) return;
  FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu); f.dwHighDateTime = (DWORD)(ft >> 32);
  SetFileTime(h_, nullptr, nullptr, &f);
}

void ByteSource::Skip(uint64_t n) {
  std::vector<uint8_t> tmp((size_t)std::min<uint64_t>(n, 1u << 16));
  while (n) {
    size_t g = Read(tmp.data(), (size_t)std::min<uint64_t>(n, tmp.size()));
    if (!g) Fail(L"The archive is truncated (it ends before all of its data).");
    n -= g;
  }
}

// ---- Inflater ---------------------------------------------------------------------------------------------------------------

Inflater::Inflater(Feed feed) : feed_(std::move(feed)), in_(1 << 16), dict_(TINFL_LZ_DICT_SIZE) {
  auto* d = new tinfl_decompressor;
  tinfl_init(d);
  st_ = std::shared_ptr<void>(d, [](void* p) { delete (tinfl_decompressor*)p; });
}
void Inflater::Reset() {
  tinfl_init((tinfl_decompressor*)st_.get());
  dictOfs_ = pendRd_ = pendWr_ = 0;
  done_ = false;
}

size_t Inflater::Read(void* out, size_t n) {
  size_t total = 0;
  auto* o = (uint8_t*)out;
  auto* d = (tinfl_decompressor*)st_.get();
  while (total < n) {
    if (pendRd_ < pendWr_) {
      size_t c = std::min(n - total, pendWr_ - pendRd_);
      std::memcpy(o + total, dict_.data() + pendRd_, c);
      pendRd_ += c; total += c;
      continue;
    }
    if (done_) break;
    if (inPos_ == inEnd_ && !eof_) {
      size_t g = feed_(in_.data(), in_.size());
      if (g == 0) eof_ = true;
      inPos_ = 0; inEnd_ = g;
    }
    size_t inBytes = inEnd_ - inPos_;
    size_t outBytes = dict_.size() - dictOfs_;
    mz_uint32 flags = eof_ ? 0 : TINFL_FLAG_HAS_MORE_INPUT;
    tinfl_status st = tinfl_decompress(d, in_.data() + inPos_, &inBytes, dict_.data(), dict_.data() + dictOfs_, &outBytes, flags);
    inPos_ += inBytes; consumed_ += inBytes;
    pendRd_ = dictOfs_; pendWr_ = dictOfs_ + outBytes;
    dictOfs_ = (dictOfs_ + outBytes) & (dict_.size() - 1);
    if (st == TINFL_STATUS_DONE) { done_ = true; continue; }
    if (st == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS || (st < 0 && eof_ && inBytes == 0))
      Fail(L"The archive is truncated (its compressed data ends early).");
    if (st < 0) Fail(L"The archive is damaged (its compressed data is corrupt).");
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && eof_ && inBytes == 0 && outBytes == 0)
      Fail(L"The archive is truncated (its compressed data ends early).");
    if (eof_ && inBytes == 0 && outBytes == 0 && st != TINFL_STATUS_HAS_MORE_OUTPUT)
      Fail(L"The archive is truncated (its compressed data ends early).");
  }
  return total;
}

size_t Inflater::ReadRaw(void* out, size_t n) {
  size_t total = 0;
  auto* o = (uint8_t*)out;
  while (total < n) {
    if (inPos_ < inEnd_) {
      size_t c = std::min(n - total, inEnd_ - inPos_);
      std::memcpy(o + total, in_.data() + inPos_, c);
      inPos_ += c; total += c; consumed_ += c;
      continue;
    }
    if (eof_) break;
    size_t g = feed_(in_.data(), in_.size());
    if (g == 0) { eof_ = true; break; }
    inPos_ = 0; inEnd_ = g;
  }
  return total;
}

// ---- GunzipSource -------------------------------------------------------------------------------------------------------------

GunzipSource::GunzipSource(std::unique_ptr<FileSource> src) : src_(std::move(src)) {
  FileSource* s = src_.get();
  inf_ = std::make_unique<Inflater>([s](void* b, size_t n) { return s->Read(b, n); });
}

bool GunzipSource::StartMember() {
  uint8_t h[10];
  size_t g = inf_->ReadRaw(h, 10);
  if (g == 0) return false;
  if (g < 10 || h[0] != 0x1F || h[1] != 0x8B) {
    if (members_ == 0) Fail(L"This is not a valid gzip file.");
    trailingGarbage = !(g >= 1 && std::all_of(h, h + g, [](uint8_t c) { return c == 0; }));
    return false;
  }
  if (h[2] != 8) Fail(L"This gzip file uses an unsupported compression method.");
  uint8_t flg = h[3];
  uint32_t mt = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
  if (flg & 4) {     // FEXTRA
    uint8_t l[2];
    if (inf_->ReadRaw(l, 2) != 2) Fail(L"The gzip header is truncated.");
    size_t xl = l[0] | (l[1] << 8);
    std::vector<uint8_t> x(xl);
    if (inf_->ReadRaw(x.data(), xl) != xl) Fail(L"The gzip header is truncated.");
  }
  auto cstr = [&](std::string* dst) {
    for (int i = 0;; ++i) {
      uint8_t c;
      if (inf_->ReadRaw(&c, 1) != 1) Fail(L"The gzip header is truncated.");
      if (c == 0) break;
      if (dst && dst->size() < 4096) dst->push_back((char)c);
      if (i > (1 << 20)) Fail(L"The gzip header is corrupt.");
    }
  };
  std::string fname;
  if (flg & 8) cstr(&fname);
  if (flg & 16) cstr(nullptr);
  if (flg & 2) { uint8_t c[2]; if (inf_->ReadRaw(c, 2) != 2) Fail(L"The gzip header is truncated."); }
  if (members_ == 0) { info_.name = fname; info_.hasName = (flg & 8) != 0; info_.mtime = mt; }
  inf_->Reset();
  crc_ = (uint32_t)mz_crc32(0, nullptr, 0);
  isize_ = 0;
  inMember_ = true;
  ++members_;
  return true;
}

size_t GunzipSource::Read(void* buf, size_t n) {
  if (n == 0 || finished_) return 0;
  for (;;) {
    if (!inMember_) {
      if (!StartMember()) { finished_ = true; return 0; }
    }
    size_t g = inf_->Read(buf, n);
    if (g > 0) {
      crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)buf, g);
      isize_ += g;
      return g;
    }
    // member complete: trailer
    uint8_t t[8];
    if (inf_->ReadRaw(t, 8) != 8) Fail(L"The archive is truncated (the gzip trailer is missing).");
    uint32_t crc = t[0] | (t[1] << 8) | (t[2] << 16) | ((uint32_t)t[3] << 24);
    uint32_t isz = t[4] | (t[5] << 8) | (t[6] << 16) | ((uint32_t)t[7] << 24);
    if (crc != crc_ || isz != (uint32_t)(isize_ & 0xFFFFFFFFu)) Fail(L"The archive is damaged (gzip checksum mismatch).");
    inMember_ = false;
  }
}

// ---- Deflater / GzipSink ---------------------------------------------------------------------------------------------------

struct DeflaterAccess {
  static mz_bool Put(const void* b, int len, void* user) {
    auto* self = (Deflater*)user;
    try { self->out_(b, (size_t)len); } catch (...) { self->err_ = std::current_exception(); return MZ_FALSE; }
    return MZ_TRUE;
  }
  static void Init(Deflater* self, int level) {
    auto* d = new tdefl_compressor;
    int flags = (int)tdefl_create_comp_flags_from_zip_params(std::clamp(level, 0, 9), -15, MZ_DEFAULT_STRATEGY);
    if (tdefl_init(d, &Put, self, flags) != TDEFL_STATUS_OKAY) { delete d; Fail(L"The compressor could not be initialised."); }
    self->d_ = std::shared_ptr<void>(d, [](void* p) { delete (tdefl_compressor*)p; });
  }
};

Deflater::Deflater(int level, Out out) : out_(std::move(out)) { DeflaterAccess::Init(this, level); }

void Deflater::Write(const void* d, size_t n) {
  if (!n) return;
  tdefl_status st = tdefl_compress_buffer((tdefl_compressor*)d_.get(), d, n, TDEFL_NO_FLUSH);
  if (err_) std::rethrow_exception(err_);
  if (st < 0) Fail(L"Compression failed.");
}
void Deflater::Finish() {
  tdefl_status st = tdefl_compress_buffer((tdefl_compressor*)d_.get(), nullptr, 0, TDEFL_FINISH);
  if (err_) std::rethrow_exception(err_);
  if (st != TDEFL_STATUS_DONE) Fail(L"Compression failed.");
}

GzipSink::GzipSink(ByteSink& out, int level, const std::string& name, int64_t mtimeUnix)
    : out_(out), def_(level, [&out](const void* b, size_t n) { out.Write(b, n); }) {
  std::vector<uint8_t> h = {0x1F, 0x8B, 8, (uint8_t)(name.empty() ? 0 : 8), 0, 0, 0, 0, 0, 0xFF};
  uint32_t mt = mtimeUnix > 0 && mtimeUnix < 0xFFFFFFFFll ? (uint32_t)mtimeUnix : 0;
  h[4] = (uint8_t)mt; h[5] = (uint8_t)(mt >> 8); h[6] = (uint8_t)(mt >> 16); h[7] = (uint8_t)(mt >> 24);
  if (level >= 9) h[8] = 2; else if (level <= 1) h[8] = 4;
  if (!name.empty()) { h.insert(h.end(), name.begin(), name.end()); h.push_back(0); }
  out_.Write(h.data(), h.size());
  crc_ = (uint32_t)mz_crc32(0, nullptr, 0);
}
void GzipSink::Write(const void* d, size_t n) {
  crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)d, n);
  isize_ += n;
  def_.Write(d, n);
}
void GzipSink::Finish() {
  if (finished_) return;
  finished_ = true;
  def_.Finish();
  uint8_t t[8];
  uint32_t sz = (uint32_t)(isize_ & 0xFFFFFFFFu);
  for (int i = 0; i < 4; ++i) { t[i] = (uint8_t)(crc_ >> (8 * i)); t[4 + i] = (uint8_t)(sz >> (8 * i)); }
  out_.Write(t, 8);
  out_.Finish();
}

} // namespace filo::archive::detail
