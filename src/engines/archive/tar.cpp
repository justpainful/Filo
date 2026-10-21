// TAR reader (ustar, pax, GNU long names; plain or through gzip) and TAR writer (ustar + pax for long / non-ASCII names).
#include "engines/archive/internal.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>
#include <map>

namespace filo::archive::detail {

namespace {

constexpr size_t kBlock = 512;

uint64_t ParseNum(const uint8_t* p, size_t n) {
  if (n && (p[0] & 0x80)) {            // GNU base-256
    uint64_t v = p[0] & 0x3F;
    for (size_t i = 1; i < n; ++i) v = (v << 8) | p[i];
    return v;
  }
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
  uint64_t v = 0;
  for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + (p[i] - '0');
  return v;
}

std::string CStr(const uint8_t* p, size_t n) {
  size_t l = 0;
  while (l < n && p[l]) ++l;
  return std::string((const char*)p, l);
}

std::wstring DecodeTarName(const std::string& b, int forced) {
  bool ascii = std::all_of(b.begin(), b.end(), [](char c) { return (unsigned char)c < 0x80; });
  if (ascii) return Widen(b);
  if (forced > 0) return DecodeLegacyName(b, forced);
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), nullptr, 0);
  if (n > 0) return Widen(b);
  n = MultiByteToWideChar(SystemAnsiCodePage(), 0, b.data(), (int)b.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(SystemAnsiCodePage(), 0, b.data(), (int)b.size(), w.data(), n);
  return w;
}

uint64_t ParseMtime(const std::string& v) {
  // pax mtime "1700000000.123456"
  double d = 0;
  try { d = std::stod(v); } catch (...) { return 0; }
  if (d <= 0) return 0;
  return (uint64_t)(d * 1e7) + kUnixToFiletime;
}

class TarReader : public EntryReader {
 public:
  TarReader(std::unique_ptr<ByteSource> src, Format fmt, const ReaderConfig& cfg, std::function<void()> keep)
      : src_(std::move(src)), fmt_(fmt), cfg_(cfg), keep_(std::move(keep)) {}
  Format format() const override { return fmt_; }

  bool Next(Entry& e) override {
    if (ended_) return false;
    if (remaining_ + padding_) { src_->Skip(remaining_ + padding_); remaining_ = padding_ = 0; }
    std::string longName, longLink;
    std::map<std::string, std::string> local;
    for (;;) {
      uint8_t h[kBlock];
      size_t got = ReadFull(h, kBlock);
      if (got == 0) {
        if (first_) Fail(L"The archive is empty.");
        ended_ = true; return false;
      }
      if (got < kBlock) Fail(L"The archive is truncated (it ends in the middle of a header).");
      if (std::all_of(h, h + kBlock, [](uint8_t c) { return c == 0; })) {
        ended_ = true;
        if (first_) Fail(L"The archive is empty.");
        Drain();       // reads the rest so that a gzip trailer (CRC) is verified too
        return false;
      }
      // checksum
      uint32_t sum = 0, sumS = 0;
      for (size_t i = 0; i < kBlock; ++i) {
        uint8_t c = (i >= 148 && i < 156) ? ' ' : h[i];
        sum += c; sumS += (uint32_t)(int)(int8_t)c;
      }
      uint64_t stored = ParseNum(h + 148, 8);
      if (stored != sum && stored != (uint32_t)sumS) {
        if (first_) Fail(L"This file is not a valid tar archive.");
        Fail(L"The archive is damaged (a tar header checksum does not match).");
      }
      first_ = false;
      char type = (char)h[156];
      uint64_t size = ParseNum(h + 124, 12);
      uint64_t padded = (size + kBlock - 1) / kBlock * kBlock;
      if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
        if (size > (1u << 22)) Fail(L"The archive is damaged (an extended tar header is too large).");
        std::string payload((size_t)size, '\0');
        if (size && ReadFull(payload.data(), (size_t)size) != size) Fail(L"The archive is truncated.");
        if (padded > size) src_->Skip(padded - size);
        if (type == 'L') longName = std::string(payload.c_str());
        else if (type == 'K') longLink = std::string(payload.c_str());
        else ParsePax(payload, type == 'g' ? global_ : local);
        continue;
      }
      if (cfg_.maxEntries && ++count_ > cfg_.maxEntries) Fail(L"The archive has too many entries.");
      e = Entry{};
      std::string name = CStr(h, 100), link = CStr(h + 157, 100);
      bool ustar = std::memcmp(h + 257, "ustar", 5) == 0 && h[262] == 0;
      if (ustar) { std::string pre = CStr(h + 345, 155); if (!pre.empty()) name = pre + "/" + name; }
      if (!longName.empty()) name = longName;
      if (!longLink.empty()) link = longLink;
      auto pick = [&](const char* k) -> const std::string* {
        auto it = local.find(k); if (it != local.end()) return &it->second;
        it = global_.find(k); if (it != global_.end()) return &it->second;
        return nullptr;
      };
      for (auto* m : {&local, &global_}) for (auto& [k, v] : *m) if (k.rfind("GNU.sparse", 0) == 0) Fail(L"This tar archive contains sparse files, which are not supported.");
      if (type == 'S') Fail(L"This tar archive contains sparse files, which are not supported.");
      if (auto* v = pick("path")) name = *v;
      if (auto* v = pick("linkpath")) link = *v;
      if (auto* v = pick("size")) { try { size = std::stoull(*v); } catch (...) {} }
      e.mtime = ParseNum(h + 136, 12) * 10000000ull + kUnixToFiletime;
      if (ParseNum(h + 136, 12) == 0) e.mtime = 0;
      if (auto* v = pick("mtime")) { uint64_t t = ParseMtime(*v); if (t) e.mtime = t; }
      e.mode = (uint32_t)ParseNum(h + 100, 8) & 07777;
      e.name = DecodeTarName(name, cfg_.nameEncoding);
      e.linkTarget = DecodeTarName(link, cfg_.nameEncoding);
      switch (type) {
        case '5': e.type = EntryType::Dir; break;
        case '2': e.type = EntryType::Symlink; break;
        case '1': e.type = EntryType::Hardlink; break;
        case '0': case 0: case '7': e.type = EntryType::File; break;
        default: e.type = EntryType::Other; break;
      }
      if (e.type == EntryType::Dir || e.type == EntryType::Symlink || e.type == EntryType::Hardlink) {
        // GNU tar stores directory names with a trailing '/', and old writers give links a size; neither has data we use.
        remaining_ = e.type == EntryType::Dir ? size : 0;
        e.size = 0;
      } else {
        e.size = size; remaining_ = size;
      }
      if (e.type == EntryType::File && !name.empty() && name.back() == '/') e.type = EntryType::Dir;
      padding_ = remaining_ ? (kBlock - remaining_ % kBlock) % kBlock : 0;
      return true;
    }
  }

  size_t Read(void* buf, size_t n) override {
    if (!remaining_ || !n) return 0;
    size_t want = (size_t)std::min<uint64_t>(n, remaining_);
    size_t g = src_->Read(buf, want);
    if (g == 0) Fail(L"The archive is truncated (it ends before all of its data).");
    remaining_ -= g;
    return g;
  }
  double Progress() const override { return src_->RawSize() ? std::min(1.0, (double)src_->RawConsumed() / (double)src_->RawSize()) : 0.0; }
  uint64_t RawConsumed() const override { return src_->RawConsumed(); }
  uint64_t RawSize() const override { return src_->RawSize(); }

 private:
  void Drain() {
    std::vector<uint8_t> tmp(1 << 16);
    while (src_->Read(tmp.data(), tmp.size()) > 0) {}
  }
  size_t ReadFull(void* buf, size_t n) {
    size_t t = 0;
    while (t < n) { size_t g = src_->Read((uint8_t*)buf + t, n - t); if (!g) break; t += g; }
    return t;
  }
  static void ParsePax(const std::string& p, std::map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < p.size()) {
      size_t sp = p.find(' ', i);
      if (sp == std::string::npos) break;
      size_t len = 0;
      try { len = (size_t)std::stoull(p.substr(i, sp - i)); } catch (...) { break; }
      if (len < sp - i + 3 || i + len > p.size()) break;
      std::string rec = p.substr(sp + 1, i + len - sp - 2);   // without trailing '\n'
      size_t eq = rec.find('=');
      if (eq != std::string::npos) out[rec.substr(0, eq)] = rec.substr(eq + 1);
      i += len;
    }
  }
  std::unique_ptr<ByteSource> src_;
  Format fmt_;
  ReaderConfig cfg_;
  std::function<void()> keep_;
  uint64_t remaining_ = 0, padding_ = 0, count_ = 0;
  bool ended_ = false, first_ = true;
  std::map<std::string, std::string> global_;
};

// ---- writer ---------------------------------------------------------------------------------------------------------------------

void PutOctal(uint8_t* dst, size_t width, uint64_t v) {
  // width includes the terminating NUL
  for (size_t i = 0; i + 1 < width; ++i) dst[width - 2 - i] = (uint8_t)('0' + ((v >> (3 * i)) & 7));
  dst[width - 1] = 0;
}

std::string PaxRecord(const std::string& key, const std::string& val) {
  size_t base = 1 + key.size() + 1 + val.size() + 1;    // " key=val\n"
  size_t len = base + 1;
  for (int i = 0; i < 4; ++i) len = base + std::to_string(len).size();
  return std::to_string(len) + " " + key + "=" + val + "\n";
}

class TarWriter : public EntryWriter {
 public:
  explicit TarWriter(std::unique_ptr<ByteSink> sink) : sink_(std::move(sink)) {}

  void Add(const NewEntry& e) override {
    if (inFile_) EndEntry();
    std::string name = ToUtf8(NormalizeSeparators(e.name));
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (e.isDir) name.push_back('/');
    uint64_t sz = e.isDir ? 0 : e.size;
    int64_t unixT = e.mtime ? FiletimeToUnix(e.mtime) : 0;
    uint32_t mode = e.mode ? (e.mode & 07777) : (e.isDir ? 0755 : 0644);

    // name placement
    std::string hdrName = name, prefix;
    std::string pax;
    bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return (unsigned char)c < 0x80; });
    if (!ascii) {
      pax += PaxRecord("path", name);
      hdrName.clear();
      for (char c : name) hdrName.push_back((unsigned char)c < 0x80 ? c : '_');
    }
    if (hdrName.size() > 100) {
      bool split = false;
      if (ascii) {
        for (size_t i = hdrName.size(); i-- > 0;) {
          if (hdrName[i] == '/' && i <= 155 && hdrName.size() - i - 1 <= 100 && i + 1 < hdrName.size()) {
            prefix = hdrName.substr(0, i); hdrName = hdrName.substr(i + 1); split = true; break;
          }
        }
      }
      if (!split) {
        if (ascii) pax += PaxRecord("path", name);
        hdrName = hdrName.substr(hdrName.size() - 100);   // placeholder, readers use the pax path
        prefix.clear();
      }
    } else if (!ascii && hdrName.size() > 100) {
      hdrName.resize(100);
    }
    if (sz >= (1ull << 33)) pax += PaxRecord("size", std::to_string(sz));
    if (unixT >= (1ll << 33)) { pax += PaxRecord("mtime", std::to_string(unixT)); unixT = 0; }
    if (unixT < 0) unixT = 0;
    if (!pax.empty()) WritePax(pax, name);

    uint8_t h[kBlock] = {};
    std::memcpy(h, hdrName.data(), std::min<size_t>(100, hdrName.size()));
    PutOctal(h + 100, 8, mode);
    PutOctal(h + 108, 8, 0);
    PutOctal(h + 116, 8, 0);
    PutOctal(h + 124, 12, sz >= (1ull << 33) ? 0 : sz);
    PutOctal(h + 136, 12, (uint64_t)unixT);
    h[156] = e.isDir ? '5' : '0';
    std::memcpy(h + 257, "ustar\0" "00", 8);
    std::memcpy(h + 345, prefix.data(), std::min<size_t>(155, prefix.size()));
    Checksum(h);
    sink_->Write(h, kBlock);

    WrittenEntry w;
    w.name = e.name; while (!w.name.empty() && (w.name.back() == L'/' || w.name.back() == L'\\')) w.name.pop_back();
    w.name = NormalizeSeparators(w.name);
    w.isDir = e.isDir; w.size = sz;
    cur_ = w;
    expect_ = sz;
    got_ = 0;
    crc_ = (uint32_t)mz_crc32(0, nullptr, 0);
    inFile_ = !e.isDir;
    if (e.isDir) written.push_back(cur_);
  }

  void Data(const void* d, size_t n) override {
    if (!inFile_) Fail(L"Internal error: data written outside of a file entry.");
    got_ += n;
    if (got_ > expect_) Fail(L"A file grew while it was being archived: '" + cur_.name + L"'.");
    crc_ = (uint32_t)mz_crc32(crc_, (const unsigned char*)d, n);
    sink_->Write(d, n);
  }

  void EndEntry() override {
    if (!inFile_) return;
    inFile_ = false;
    if (got_ != expect_) Fail(L"A file changed while it was being archived: '" + cur_.name + L"'.");
    size_t pad = (kBlock - expect_ % kBlock) % kBlock;
    if (pad) { uint8_t z[kBlock] = {}; sink_->Write(z, pad); }
    cur_.crc32 = crc_;
    written.push_back(cur_);
  }

  void Finish() override {
    if (inFile_) EndEntry();
    uint8_t z[kBlock * 2] = {};
    sink_->Write(z, sizeof z);
    sink_->Finish();
  }

 private:
  static void Checksum(uint8_t* h) {
    std::memset(h + 148, ' ', 8);
    uint32_t s = 0;
    for (size_t i = 0; i < kBlock; ++i) s += h[i];
    for (int i = 0; i < 6; ++i) h[148 + 5 - i] = (uint8_t)('0' + ((s >> (3 * i)) & 7));
    h[154] = 0; h[155] = ' ';
  }
  void WritePax(const std::string& pax, const std::string& name) {
    uint8_t h[kBlock] = {};
    std::string base = name;
    while (!base.empty() && base.back() == '/') base.pop_back();
    size_t sl = base.rfind('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    std::string pn = "PaxHeader/";
    for (char c : base) pn.push_back((unsigned char)c < 0x80 && c >= 0x20 ? c : '_');
    if (pn.size() > 99) pn.resize(99);
    std::memcpy(h, pn.data(), pn.size());
    PutOctal(h + 100, 8, 0644);
    PutOctal(h + 108, 8, 0);
    PutOctal(h + 116, 8, 0);
    PutOctal(h + 124, 12, pax.size());
    PutOctal(h + 136, 12, 0);
    h[156] = 'x';
    std::memcpy(h + 257, "ustar\0" "00", 8);
    Checksum(h);
    sink_->Write(h, kBlock);
    sink_->Write(pax.data(), pax.size());
    size_t pad = (kBlock - pax.size() % kBlock) % kBlock;
    if (pad) { uint8_t z[kBlock] = {}; sink_->Write(z, pad); }
  }
  std::unique_ptr<ByteSink> sink_;
  WrittenEntry cur_;
  uint64_t expect_ = 0, got_ = 0;
  uint32_t crc_ = 0;
  bool inFile_ = false;
};

} // namespace

std::unique_ptr<EntryReader> OpenTarReader(std::unique_ptr<ByteSource> src, Format fmt, const ReaderConfig& cfg, std::function<void()> keep) {
  return std::make_unique<TarReader>(std::move(src), fmt, cfg, std::move(keep));
}
std::unique_ptr<EntryWriter> NewTarWriter(std::unique_ptr<ByteSink> sink) { return std::make_unique<TarWriter>(std::move(sink)); }

} // namespace filo::archive::detail
