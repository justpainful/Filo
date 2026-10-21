// ZIP reader (central directory, ZIP64, UTF-8 / legacy names, CRC verification, overlap guard) and ZIP writer
// (deflate / store, ZIP64, UTF-8 flag, extended timestamps; patches CRC + sizes in place so no data descriptors are needed).
#include "engines/archive/internal.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>

namespace filo::archive::detail {

namespace {

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t U64(const uint8_t* p) { return (uint64_t)U32(p) | ((uint64_t)U32(p + 4) << 32); }
void P16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void P32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void P64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((uint8_t)(x >> (8 * i))); }

uint64_t DosToFiletime(uint16_t t, uint16_t d) {
  SYSTEMTIME st{};
  st.wYear = (WORD)(1980 + (d >> 9)); st.wMonth = (WORD)((d >> 5) & 15); st.wDay = (WORD)(d & 31);
  st.wHour = (WORD)(t >> 11); st.wMinute = (WORD)((t >> 5) & 63); st.wSecond = (WORD)((t & 31) * 2);
  if (st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1) return 0;
  FILETIME local{}, utc{};
  if (!SystemTimeToFileTime(&st, &local)) return 0;
  if (!LocalFileTimeToFileTime(&local, &utc)) utc = local;
  return ((uint64_t)utc.dwHighDateTime << 32) | utc.dwLowDateTime;
}
void FiletimeToDos(uint64_t ft, uint16_t& t, uint16_t& d) {
  t = 0; d = (1 << 5) | 1;      // 1980-01-01
  if (!ft) return;
  FILETIME utc{(DWORD)(ft & 0xFFFFFFFFu), (DWORD)(ft >> 32)}, local{};
  SYSTEMTIME st{};
  if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return;
  if (st.wYear < 1980) return;
  if (st.wYear > 2107) { st.wYear = 2107; }
  t = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
  d = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

struct ZEntry {
  Entry e;
  uint16_t method = 0, flags = 0;
  uint64_t csize = 0, usize = 0, localOff = 0;
  bool unsupported = false;
};

class ZipReader : public EntryReader {
 public:
  ZipReader(const fs::path& p, const ReaderConfig& cfg, bool* needsBsdtar) : f_(p), cfg_(cfg) {
    if (needsBsdtar) *needsBsdtar = false;
    ParseDirectory();
    for (auto& z : zs_) if (z.unsupported && !z.e.encrypted) { if (needsBsdtar) *needsBsdtar = true; }
  }
  bool NeedsBsdtar() const { for (auto& z : zs_) if (z.unsupported && !z.e.encrypted) return true; return false; }
  Format format() const override { return Format::Zip; }

  bool KnownTotals(uint64_t& files, uint64_t& bytes) const override {
    files = 0; bytes = 0;
    for (auto& z : zs_) if (z.e.type == EntryType::File) { ++files; bytes += z.usize; }
    return true;
  }

  bool Next(Entry& out) override {
    active_ = false;
    if (cur_) { rawDone_ += cur_->csize; cur_ = nullptr; }
    if (idx_ >= zs_.size()) return false;
    ZEntry& z = zs_[idx_++];
    out = z.e;
    if (z.e.type == EntryType::File || z.e.type == EntryType::Symlink) {
      if (z.e.encrypted) {
        // the caller sees the entry; reading it reports the password problem
        cur_ = &z; active_ = true; encryptedCur_ = true; return true;
      }
      encryptedCur_ = false;
      OpenData(z);
    }
    return true;
  }

  size_t Read(void* buf, size_t n) override {
    if (!active_ || !n) return 0;
    if (encryptedCur_) Fail(L"This archive is password-protected, which is not supported.");
    ZEntry& z = *cur_;
    size_t g = 0;
    if (z.method == 0) {
      uint64_t left = z.csize - inDone_;
      if (left) {
        g = f_.ReadAt(dataOff_ + inDone_, buf, (size_t)std::min<uint64_t>(n, left));
        if (g == 0) Fail(L"The archive is truncated (it ends before all of its data).");
        inDone_ += g;
      }
    } else {
      g = inf_->Read(buf, n);
    }
    if (g) {
      produced_ += g; doneBytes_ += g;
      if (produced_ > z.usize) Fail(L"The archive is damaged (an entry holds more data than declared).");
      crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)buf, g);
      return g;
    }
    active_ = false;
    if (produced_ != z.usize) Fail(L"The archive is damaged (size mismatch in '" + z.e.name + L"').");
    if (crc_ != z.e.crc32) Fail(L"The archive is damaged (checksum mismatch in '" + z.e.name + L"').");
    return 0;
  }

  double Progress() const override { return totalU_ ? std::min(1.0, (double)doneBytes_ / (double)totalU_) : (idx_ >= zs_.size() ? 1.0 : 0.0); }
  uint64_t RawConsumed() const override {
    if (!cur_) return rawDone_;
    if (!active_) return rawDone_ + cur_->csize;
    return rawDone_ + (encryptedCur_ ? 0 : (cur_->method == 0 ? inDone_ : inf_->InputConsumed()));
  }
  uint64_t RawSize() const override { return f_.Size(); }

 private:
  void ParseDirectory() {
    uint64_t fsz = f_.Size();
    if (fsz < 22) Fail(L"This file is not a valid zip archive.");
    size_t tailLen = (size_t)std::min<uint64_t>(fsz, 65557);
    std::vector<uint8_t> tail(tailLen);
    if (f_.ReadAt(fsz - tailLen, tail.data(), tailLen) != tailLen) Fail(L"The archive could not be read.");
    int64_t pos = -1;
    for (int64_t i = (int64_t)tailLen - 22; i >= 0; --i) {
      if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
        size_t cl = U16(&tail[i + 20]);
        if ((size_t)i + 22 + cl <= tailLen) { pos = i; break; }
        if (pos < 0) pos = i;       // keep the last candidate with an inconsistent comment length as a fallback
        break;
      }
    }
    if (pos < 0) Fail(L"This file is not a valid zip archive (or it is truncated: no end-of-archive record).");
    const uint8_t* ec = &tail[(size_t)pos];
    uint64_t eocdPos = fsz - tailLen + (uint64_t)pos;
    uint64_t total = U16(ec + 10), cdSize = U32(ec + 12), cdOff = U32(ec + 16);
    uint64_t anchor = eocdPos;
    bool z64 = total == 0xFFFF || cdSize == 0xFFFFFFFFull || cdOff == 0xFFFFFFFFull;
    if (eocdPos >= 20) {
      uint8_t loc[20];
      if (f_.ReadAt(eocdPos - 20, loc, 20) == 20 && U32(loc) == 0x07064b50) {
        uint64_t zpos = U64(loc + 8);
        uint8_t z[56];
        if (zpos + 56 <= fsz && f_.ReadAt(zpos, z, 56) == 56 && U32(z) == 0x06064b50) {
          total = U64(z + 32); cdSize = U64(z + 40); cdOff = U64(z + 48);
          anchor = zpos; z64 = true;
        } else if (z64) Fail(L"The archive is damaged (bad ZIP64 directory).");
      } else if (z64) Fail(L"The archive is damaged (missing ZIP64 directory).");
    }
    if (cdSize > fsz || cdOff > fsz || cdSize + cdOff > anchor) Fail(L"The archive is damaged (the central directory is out of range).");
    base_ = anchor - cdSize - cdOff;
    if (total > cfg_.maxEntries) Fail(L"The archive has too many entries (" + std::to_wstring(total) + L").");
    if (cdSize > (512ull << 20)) Fail(L"The archive's directory is too large.");
    if (total * 46 > cdSize) Fail(L"The archive is damaged (the entry count does not match its directory).");
    std::vector<uint8_t> cd((size_t)cdSize);
    if (cdSize && f_.ReadAt(base_ + cdOff, cd.data(), cd.size()) != cd.size()) Fail(L"The archive is truncated.");
    zs_.reserve((size_t)total);
    size_t p = 0;
    for (uint64_t i = 0; i < total; ++i) {
      if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) Fail(L"The archive is damaged (bad central directory entry).");
      const uint8_t* h = &cd[p];
      ZEntry z;
      uint16_t madeBy = U16(h + 4);
      z.flags = U16(h + 8); z.method = U16(h + 10);
      uint16_t dt = U16(h + 12), dd = U16(h + 14);
      z.e.crc32 = U32(h + 16); z.e.hasCrc = true;
      z.csize = U32(h + 20); z.usize = U32(h + 24);
      size_t nl = U16(h + 28), xl = U16(h + 30), cl = U16(h + 32);
      uint32_t ext = U32(h + 38);
      z.localOff = U32(h + 42);
      if (p + 46 + nl + xl + cl > cd.size()) Fail(L"The archive is damaged (a directory entry is cut off).");
      std::string rawName((const char*)h + 46, nl);
      const uint8_t* x = h + 46 + nl;
      uint64_t mt = 0;
      std::string uniName;
      bool haveUni = false;
      for (size_t o = 0; o + 4 <= xl;) {
        uint16_t id = U16(x + o), sz = U16(x + o + 2);
        if (o + 4 + sz > xl) break;
        const uint8_t* d = x + o + 4;
        if (id == 0x0001) {
          size_t q = 0;
          if (z.usize == 0xFFFFFFFFull && q + 8 <= sz) { z.usize = U64(d + q); q += 8; }
          if (z.csize == 0xFFFFFFFFull && q + 8 <= sz) { z.csize = U64(d + q); q += 8; }
          if (z.localOff == 0xFFFFFFFFull && q + 8 <= sz) { z.localOff = U64(d + q); q += 8; }
        } else if (id == 0x5455 && sz >= 5 && (d[0] & 1)) {
          mt = UnixToFiletime((int64_t)U32(d + 1));
        } else if (id == 0x000a && sz >= 32) {
          size_t q = 4;
          while (q + 4 <= sz) {
            uint16_t tag = U16(d + q), ts = U16(d + q + 2);
            if (tag == 1 && ts >= 8 && q + 4 + 8 <= sz) { uint64_t v = U64(d + q + 4); if (v) mt = v; }
            q += 4 + ts;
          }
        } else if (id == 0x7075 && sz >= 5 && d[0] == 1) {
          uint32_t crc = U32(d + 1);
          if (crc == (uint32_t)mz_crc32(0, (const unsigned char*)rawName.data(), rawName.size())) { uniName.assign((const char*)d + 5, sz - 5); haveUni = true; }
        }
        o += 4 + sz;
      }
      if (haveUni) z.e.name = Widen(uniName);
      else if (z.flags & 0x800) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rawName.data(), (int)rawName.size(), nullptr, 0);
        if (n > 0 || rawName.empty()) z.e.name = Widen(rawName);
        else z.e.name = DecodeLegacyName(rawName, cfg_.nameEncoding, nullptr, &z.e.legacyName);
      } else {
        int cp = 0;
        z.e.name = DecodeLegacyName(rawName, cfg_.nameEncoding, &cp, &z.e.legacyName);
        if (z.e.legacyName && !warnedLegacy_) {
          warnedLegacy_ = true;
          warnings.push_back(L"Some file names were not stored as Unicode; they were decoded as code page " + std::to_wstring(cp) +
                             L". If they look wrong, choose another name encoding.");
        }
      }
      z.e.mtime = mt ? mt : DosToFiletime(dt, dd);
      z.e.size = z.usize; z.e.packedSize = z.csize;
      z.e.encrypted = (z.flags & 1) || z.method == 99;
      if (z.e.encrypted) anyEncrypted = true;
      if (z.method != 0 && z.method != 8) z.unsupported = true;
      uint8_t host = (uint8_t)(madeBy >> 8);
      uint32_t unixMode = host == 3 ? (ext >> 16) : 0;
      z.e.mode = unixMode & 07777;
      bool dir = !z.e.name.empty() && (z.e.name.back() == L'/' || z.e.name.back() == L'\\');
      if (!dir && (host == 0 || host == 10) && (ext & 0x10) && z.usize == 0) dir = true;
      if (!dir && host == 3 && (unixMode & 0170000) == 0040000) dir = true;
      z.e.type = dir ? EntryType::Dir : EntryType::File;
      if (!dir && host == 3 && (unixMode & 0170000) == 0120000) z.e.type = EntryType::Symlink;
      if (!dir && host == 3 && (unixMode & 0170000) != 0 && (unixMode & 0170000) != 0100000 && (unixMode & 0170000) != 0120000) z.e.type = EntryType::Other;
      if (dir) { z.usize = 0; z.e.size = 0; }
      if (z.e.type == EntryType::File) totalU_ += z.usize;
      zs_.push_back(std::move(z));
      p += 46 + nl + xl + cl;
    }
    // overlapping local entries = classic "zip quine / overlapped" bombs, or corruption
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(zs_.size());
    for (auto& z : zs_) {
      uint64_t start = base_ + z.localOff;
      if (start + 30 > fsz) Fail(L"The archive is damaged (an entry points past the end of the file).");
      ranges.emplace_back(start, start + 30 + z.csize);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
      if (ranges[i].first < ranges[i - 1].second) Fail(L"The archive is damaged or malicious (its entries overlap).");
  }

  void OpenData(ZEntry& z) {
    cur_ = &z; active_ = true;
    uint8_t lh[30];
    uint64_t at = base_ + z.localOff;
    if (f_.ReadAt(at, lh, 30) != 30 || U32(lh) != 0x04034b50) Fail(L"The archive is damaged (bad local file header).");
    dataOff_ = at + 30 + U16(lh + 26) + U16(lh + 28);
    if (dataOff_ + z.csize > f_.Size()) Fail(L"The archive is truncated (an entry's data is cut off).");
    inDone_ = 0; produced_ = 0;
    crc_ = (uint32_t)mz_crc32(0, nullptr, 0);
    if (z.method == 8) {
      uint64_t off = dataOff_, left = z.csize;
      FileReader* fr = &f_;
      auto feed = [fr, off, left](void* b, size_t n) mutable -> size_t {
        size_t want = (size_t)std::min<uint64_t>(n, left);
        if (!want) return 0;
        size_t g = fr->ReadAt(off, b, want);
        off += g; left -= g;
        return g;
      };
      inf_ = std::make_unique<Inflater>(feed);
    } else if (z.method != 0) {
      Fail(L"This zip uses a compression method that is not supported.");
    }
  }

  FileReader f_;
  ReaderConfig cfg_;
  std::vector<ZEntry> zs_;
  size_t idx_ = 0;
  uint64_t base_ = 0, totalU_ = 0, doneBytes_ = 0, rawDone_ = 0;
  ZEntry* cur_ = nullptr;
  bool active_ = false, encryptedCur_ = false, warnedLegacy_ = false;
  uint64_t dataOff_ = 0, inDone_ = 0, produced_ = 0;
  uint32_t crc_ = 0;
  std::unique_ptr<Inflater> inf_;
};

// ---- writer ------------------------------------------------------------------------------------------------------------------

struct CdRec {
  std::string name;
  uint16_t flags = 0, method = 0, dosT = 0, dosD = 0;
  uint32_t crc = 0, extAttr = 0;
  uint64_t csize = 0, usize = 0, localOff = 0;
  int64_t unixT = 0;
  bool local64 = false;
};

class ZipWriter : public EntryWriter {
 public:
  ZipWriter(const fs::path& out, int level, ZipMethod method) : f_(out), level_(level), method_(method) {}

  void Add(const NewEntry& e) override {
    if (inFile_) EndEntry();
    if (cds_.size() >= 0xFFFFFFF0ull) Fail(L"Too many entries for a zip archive.");
    CdRec r;
    std::string name = ToUtf8(NormalizeSeparators(e.name));
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (e.isDir) name.push_back('/');
    if (name.size() > 65000) Fail(L"A file name is too long to store in a zip archive.");
    bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return (unsigned char)c < 0x80; });
    r.name = name;
    r.flags = ascii ? 0 : 0x800;
    bool store = e.isDir || method_ == ZipMethod::Store || (method_ == ZipMethod::Auto && (!e.compressible || LooksAlreadyCompressed(e.name))) ||
                 (e.sizeKnown && e.size == 0);
    r.method = store ? 0 : 8;
    FiletimeToDos(e.mtime, r.dosT, r.dosD);
    r.unixT = e.mtime ? FiletimeToUnix(e.mtime) : 0;
    uint32_t mode = e.mode ? (e.mode & 07777) : (e.isDir ? 0755 : 0644);
    r.extAttr = ((e.isDir ? 040000u : 0100000u) | mode) << 16 | (e.isDir ? 0x10u : 0x20u);
    r.local64 = !e.isDir && (!e.sizeKnown ? e.size >= 0xF0000000ull : e.size >= 0xF0000000ull);
    r.localOff = f_.Tell();
    // local header
    std::vector<uint8_t> h;
    P32(h, 0x04034b50);
    P16(h, r.local64 ? 45 : 20);
    P16(h, r.flags); P16(h, r.method); P16(h, r.dosT); P16(h, r.dosD);
    P32(h, 0);
    P32(h, r.local64 ? 0xFFFFFFFFu : 0); P32(h, r.local64 ? 0xFFFFFFFFu : 0);
    std::vector<uint8_t> ex;
    if (r.unixT > 0 && r.unixT < 0xFFFFFFFFll) { P16(ex, 0x5455); P16(ex, 5); ex.push_back(1); P32(ex, (uint32_t)r.unixT); }
    zip64DataOff_ = r.localOff + 30 + r.name.size() + ex.size() + 4;
    if (r.local64) { P16(ex, 0x0001); P16(ex, 16); P64(ex, 0); P64(ex, 0); }
    P16(h, (uint16_t)r.name.size()); P16(h, (uint16_t)ex.size());
    h.insert(h.end(), r.name.begin(), r.name.end());
    h.insert(h.end(), ex.begin(), ex.end());
    f_.Write(h.data(), h.size());

    cur_ = std::move(r);
    curW_ = WrittenEntry{};
    curW_.name = e.name;
    while (!curW_.name.empty() && (curW_.name.back() == L'/' || curW_.name.back() == L'\\')) curW_.name.pop_back();
    curW_.name = NormalizeSeparators(curW_.name);
    curW_.isDir = e.isDir;
    inFile_ = true;
    crc_ = (uint32_t)mz_crc32(0, nullptr, 0);
    usize_ = 0; csize_ = 0;
    dataStart_ = f_.Tell();
    if (cur_.method == 8) def_ = std::make_unique<Deflater>(level_, [this](const void* b, size_t n) { f_.Write(b, n); });
    else def_.reset();
    if (e.isDir) EndEntry();
  }

  void Data(const void* d, size_t n) override {
    if (!inFile_) Fail(L"Internal error: data written outside of a file entry.");
    crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)d, n);
    usize_ += n;
    if (def_) def_->Write(d, n); else f_.Write(d, n);
  }

  void EndEntry() override {
    if (!inFile_) return;
    inFile_ = false;
    if (def_) { def_->Finish(); def_.reset(); }
    csize_ = f_.Tell() - dataStart_;
    cur_.crc = crc_; cur_.usize = usize_; cur_.csize = csize_;
    if (cur_.local64) {
      std::vector<uint8_t> z;
      P64(z, usize_); P64(z, csize_);
      f_.WriteAt(zip64DataOff_, z.data(), z.size());
      uint8_t c[4]; for (int i = 0; i < 4; ++i) c[i] = (uint8_t)(crc_ >> (8 * i));
      f_.WriteAt(cur_.localOff + 14, c, 4);
    } else {
      if (usize_ >= 0xFFFFFFFFull || csize_ >= 0xFFFFFFFFull) Fail(L"A file grew beyond 4 GiB while it was being archived; please try again.");
      std::vector<uint8_t> z;
      P32(z, crc_); P32(z, (uint32_t)csize_); P32(z, (uint32_t)usize_);
      f_.WriteAt(cur_.localOff + 14, z.data(), z.size());
    }
    curW_.size = usize_; curW_.crc32 = crc_;
    written.push_back(curW_);
    cds_.push_back(std::move(cur_));
  }

  void Finish() override {
    if (inFile_) EndEntry();
    uint64_t cdStart = f_.Tell();
    std::vector<uint8_t> b;
    for (auto& r : cds_) {
      bool needU = r.usize >= 0xFFFFFFFFull, needC = r.csize >= 0xFFFFFFFFull, needO = r.localOff >= 0xFFFFFFFFull;
      bool z64 = needU || needC || needO;
      std::vector<uint8_t> ex;
      if (r.unixT > 0 && r.unixT < 0xFFFFFFFFll) { P16(ex, 0x5455); P16(ex, 5); ex.push_back(1); P32(ex, (uint32_t)r.unixT); }
      if (z64) {
        P16(ex, 0x0001); P16(ex, (uint16_t)((needU + needC + needO) * 8));
        if (needU) P64(ex, r.usize);
        if (needC) P64(ex, r.csize);
        if (needO) P64(ex, r.localOff);
      }
      P32(b, 0x02014b50);
      P16(b, (uint16_t)((3 << 8) | (z64 || r.local64 ? 45 : 20)));
      P16(b, z64 || r.local64 ? 45 : 20);
      P16(b, r.flags); P16(b, r.method); P16(b, r.dosT); P16(b, r.dosD);
      P32(b, r.crc);
      P32(b, needC ? 0xFFFFFFFFu : (uint32_t)r.csize);
      P32(b, needU ? 0xFFFFFFFFu : (uint32_t)r.usize);
      P16(b, (uint16_t)r.name.size()); P16(b, (uint16_t)ex.size()); P16(b, 0);
      P16(b, 0); P16(b, 0);
      P32(b, r.extAttr);
      P32(b, needO ? 0xFFFFFFFFu : (uint32_t)r.localOff);
      b.insert(b.end(), r.name.begin(), r.name.end());
      b.insert(b.end(), ex.begin(), ex.end());
      if (b.size() > (1u << 20)) { f_.Write(b.data(), b.size()); b.clear(); }
    }
    if (!b.empty()) f_.Write(b.data(), b.size());
    uint64_t cdSize = f_.Tell() - cdStart, count = cds_.size();
    b.clear();
    bool z64 = count >= 0xFFFF || cdStart >= 0xFFFFFFFFull || cdSize >= 0xFFFFFFFFull;
    if (z64) {
      uint64_t z64pos = f_.Tell();
      P32(b, 0x06064b50); P64(b, 44); P16(b, 45); P16(b, 45); P32(b, 0); P32(b, 0);
      P64(b, count); P64(b, count); P64(b, cdSize); P64(b, cdStart);
      P32(b, 0x07064b50); P32(b, 0); P64(b, z64pos); P32(b, 1);
    }
    P32(b, 0x06054b50); P16(b, 0); P16(b, 0);
    P16(b, z64 ? 0xFFFF : (uint16_t)count); P16(b, z64 ? 0xFFFF : (uint16_t)count);
    P32(b, z64 ? 0xFFFFFFFFu : (uint32_t)cdSize); P32(b, z64 ? 0xFFFFFFFFu : (uint32_t)cdStart);
    P16(b, 0);
    f_.Write(b.data(), b.size());
    f_.Close();
  }

 private:
  FileWriter f_;
  int level_;
  ZipMethod method_;
  std::vector<CdRec> cds_;
  CdRec cur_;
  WrittenEntry curW_;
  std::unique_ptr<Deflater> def_;
  uint32_t crc_ = 0;
  uint64_t usize_ = 0, csize_ = 0, dataStart_ = 0, zip64DataOff_ = 0;
  bool inFile_ = false;
};

} // namespace

bool LooksAlreadyCompressed(const std::wstring& name) {
  size_t dot = name.rfind(L'.');
  if (dot == std::wstring::npos) return false;
  std::wstring e = ToLower(name.substr(dot + 1));
  static const wchar_t* k[] = {L"zip", L"gz", L"tgz", L"bz2", L"xz", L"7z", L"rar", L"zst", L"jpg", L"jpeg", L"png", L"gif", L"webp", L"heic", L"avif",
                               L"mp3", L"mp4", L"m4a", L"m4v", L"mov", L"mkv", L"webm", L"avi", L"flac", L"ogg", L"opus", L"aac", L"docx", L"xlsx",
                               L"pptx", L"odt", L"ods", L"odp", L"epub", L"jar", L"apk", L"woff2", L"wmv", L"wma", L"cab", L"lz4", L"br"};
  for (auto* s : k) if (e == s) return true;
  return false;
}

std::unique_ptr<EntryReader> OpenZipReader(const fs::path& file, const ReaderConfig& cfg, bool* needsBsdtar) {
  auto r = std::make_unique<ZipReader>(file, cfg, needsBsdtar);
  if (r->NeedsBsdtar()) return nullptr;
  return r;
}
std::unique_ptr<EntryWriter> NewZipWriter(const fs::path& out, int level, ZipMethod method) {
  return std::make_unique<ZipWriter>(out, level, method);
}

} // namespace filo::archive::detail
