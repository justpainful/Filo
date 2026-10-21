#include "engines/doc/zip.h"
#include "miniz.h"
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cstring>

namespace filo::doc {

ZipLimits& DefaultZipLimits() { static ZipLimits l; return l; }

// ---- writer ----------------------------------------------------------------------------------------------------------
ZipWriter::~ZipWriter() {
  if (h_) { CloseHandle((HANDLE)h_); h_ = nullptr; }
}

bool ZipWriter::Open(const fs::path& path, std::wstring* err) {
  h_ = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_ == INVALID_HANDLE_VALUE) { h_ = nullptr; if (err) *err = L"The output file could not be created."; failed_ = true; return false; }
  time_t t = time(nullptr);
  tm lt{};
  localtime_s(&lt, &t);
  int year = std::max(1980, lt.tm_year + 1900);
  dosTime_ = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
  dosDate_ = (uint16_t)(((year - 1980) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
  return true;
}

bool ZipWriter::Write(const void* d, size_t n) {
  if (failed_ || !h_) return false;
  const uint8_t* p = static_cast<const uint8_t*>(d);
  size_t done = 0;
  while (done < n) {
    DWORD w = 0;
    DWORD want = (DWORD)std::min<size_t>(n - done, 32u << 20);
    if (!WriteFile((HANDLE)h_, p + done, want, &w, nullptr) || w == 0) { failed_ = true; return false; }
    done += w;
  }
  pos_ += n;
  return true;
}

static void Put16(std::string& s, uint16_t v) { s.push_back((char)v); s.push_back((char)(v >> 8)); }
static void Put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back((char)(v >> (8 * i))); }

bool ZipWriter::Add(const std::string& name, const void* data, size_t n, bool deflate) {
  if (failed_ || !h_ || closed_) return false;
  if (n >= 0xFFFFFFF0ull || pos_ >= 0xFFFFFF00ull || entries_.size() >= 65000 || name.empty() || name.size() > 60000) { failed_ = true; return false; }
  Entry e;
  e.name = name;
  e.usize = (uint32_t)n;
  e.crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, static_cast<const uint8_t*>(data), n);
  bool ascii = true;
  for (unsigned char c : name) if (c >= 0x80) ascii = false;
  e.flags = ascii ? 0 : 0x0800;
  const void* payload = data;
  size_t plen = n;
  void* heap = nullptr;
  e.method = 0;
  if (deflate && n > 16) {
    size_t outLen = 0;
    int flags = (int)tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY);
    heap = tdefl_compress_mem_to_heap(data, n, &outLen, flags);
    if (heap && outLen < n) { payload = heap; plen = outLen; e.method = 8; }
    else { if (heap) mz_free(heap); heap = nullptr; }
  }
  e.csize = (uint32_t)plen;
  e.offset = (uint32_t)pos_;
  std::string hdr;
  Put32(hdr, 0x04034b50);
  Put16(hdr, 20); Put16(hdr, e.flags); Put16(hdr, e.method); Put16(hdr, dosTime_); Put16(hdr, dosDate_);
  Put32(hdr, e.crc); Put32(hdr, e.csize); Put32(hdr, e.usize);
  Put16(hdr, (uint16_t)name.size()); Put16(hdr, 0);
  hdr += name;
  bool ok = Write(hdr.data(), hdr.size()) && (plen == 0 || Write(payload, plen));
  if (heap) mz_free(heap);
  if (ok) entries_.push_back(std::move(e));
  return ok;
}

bool ZipWriter::Close() {
  if (closed_) return !failed_;
  closed_ = true;
  if (!h_) return false;
  uint32_t cdStart = (uint32_t)pos_;
  std::string cd;
  for (auto& e : entries_) {
    Put32(cd, 0x02014b50);
    Put16(cd, 20); Put16(cd, 20); Put16(cd, e.flags); Put16(cd, e.method); Put16(cd, dosTime_); Put16(cd, dosDate_);
    Put32(cd, e.crc); Put32(cd, e.csize); Put32(cd, e.usize);
    Put16(cd, (uint16_t)e.name.size()); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0);
    Put32(cd, 0); Put32(cd, e.offset);
    cd += e.name;
  }
  uint32_t cdSize = (uint32_t)cd.size();
  Put32(cd, 0x06054b50);
  Put16(cd, 0); Put16(cd, 0); Put16(cd, (uint16_t)entries_.size()); Put16(cd, (uint16_t)entries_.size());
  Put32(cd, cdSize); Put32(cd, cdStart); Put16(cd, 0);
  Write(cd.data(), cd.size());
  bool ok = !failed_;
  if (ok) ok = FlushFileBuffers((HANDLE)h_) != 0 || true;
  CloseHandle((HANDLE)h_);
  h_ = nullptr;
  return ok && !failed_;
}

// ---- reader ----------------------------------------------------------------------------------------------------------
struct ZipReader::Impl {
  mz_zip_archive zip{};
  FILE* f = nullptr;
  bool open = false;
};

ZipReader::ZipReader() : impl_(new Impl) {}
ZipReader::~ZipReader() { Close(); }

void ZipReader::Close() {
  if (!impl_) return;
  if (impl_->open) { mz_zip_reader_end(&impl_->zip); impl_->open = false; }
  if (impl_->f) { fclose(impl_->f); impl_->f = nullptr; }
  entries_.clear();
}
bool ZipReader::IsOpen() const { return impl_ && impl_->open; }

static std::string FixName(const char* raw, bool utf8Flag) {
  std::string s = raw;
  bool ascii = true;
  for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
  if (ascii || utf8Flag || IsValidUtf8(s)) return s;
  int n = MultiByteToWideChar(437, 0, s.data(), (int)s.size(), nullptr, 0);   // legacy ZIPs: CP437
  std::wstring w(n, L'\0');
  MultiByteToWideChar(437, 0, s.data(), (int)s.size(), w.data(), n);
  return Narrow(w);
}

bool ZipReader::Setup(std::wstring* err) {
  mz_zip_archive& z = impl_->zip;
  mz_uint n = mz_zip_reader_get_num_files(&z);
  if (n > lim_.maxEntries) { if (err) *err = L"The archive contains too many entries."; return false; }
  uint64_t total = 0;
  entries_.reserve(n);
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, i, &st)) { if (err) *err = L"The archive directory is damaged."; return false; }
    Entry e;
    e.name = FixName(st.m_filename, (st.m_bit_flag & 0x800) != 0);
    e.size = st.m_uncomp_size; e.csize = st.m_comp_size; e.isDir = st.m_is_directory != 0; e.encrypted = st.m_is_encrypted != 0; e.crc = st.m_crc32;
    total += e.size;
    if (total > lim_.maxTotalBytes) { if (err) *err = L"The file expands to an unreasonable size and was not opened (possible decompression bomb)."; return false; }
    if (e.size > (16ull << 20) && e.csize > 0 && e.size / e.csize > lim_.bombRatio) { if (err) *err = L"The file contains data with an extreme compression ratio and was not opened (possible decompression bomb)."; return false; }
    entries_.push_back(std::move(e));
  }
  return true;
}

bool ZipReader::Open(const fs::path& path, std::wstring* err, const ZipLimits* lim) {
  Close();
  lim_ = lim ? *lim : DefaultZipLimits();
  impl_->f = _wfopen(LongPath(path).c_str(), L"rb");
  if (!impl_->f) { if (err) *err = L"The file could not be opened."; return false; }
  _fseeki64(impl_->f, 0, SEEK_END);
  uint64_t size = (uint64_t)_ftelli64(impl_->f);
  _fseeki64(impl_->f, 0, SEEK_SET);
  memset(&impl_->zip, 0, sizeof impl_->zip);
  if (!mz_zip_reader_init_cfile(&impl_->zip, impl_->f, size, 0)) {
    if (err) *err = L"The file is not a valid ZIP-based document (it may be corrupt or truncated).";
    fclose(impl_->f); impl_->f = nullptr;
    return false;
  }
  impl_->open = true;
  if (!Setup(err)) { Close(); return false; }
  return true;
}

bool ZipReader::OpenMemory(const uint8_t* data, size_t n, std::wstring* err, const ZipLimits* lim) {
  Close();
  lim_ = lim ? *lim : DefaultZipLimits();
  memset(&impl_->zip, 0, sizeof impl_->zip);
  if (!mz_zip_reader_init_mem(&impl_->zip, data, n, 0)) { if (err) *err = L"The data is not a valid ZIP container."; return false; }
  impl_->open = true;
  if (!Setup(err)) { Close(); return false; }
  return true;
}

int ZipReader::Find(std::string_view name) const {
  while (!name.empty() && name.front() == '/') name.remove_prefix(1);
  for (size_t i = 0; i < entries_.size(); ++i) if (entries_[i].name == name) return (int)i;
  std::string ln = LowerAscii(name);
  for (size_t i = 0; i < entries_.size(); ++i) if (LowerAscii(entries_[i].name) == ln) return (int)i;
  return -1;
}

bool ZipReader::Read(int index, Bytes& out, std::wstring* err) {
  out.clear();
  if (!IsOpen() || index < 0 || (size_t)index >= entries_.size()) { if (err) *err = L"Entry not found."; return false; }
  const Entry& e = entries_[index];
  if (e.encrypted) { if (err) *err = L"The document is password-protected, which is not supported."; return false; }
  if (e.size > lim_.maxEntryBytes) { if (err) *err = L"A part of the document is too large to process safely."; return false; }
  out.resize((size_t)e.size);
  if (e.size == 0) return true;
  if (!mz_zip_reader_extract_to_mem(&impl_->zip, (mz_uint)index, out.data(), out.size(), 0)) {
    out.clear();
    if (err) *err = L"A part of the document is damaged (it could not be decompressed).";
    return false;
  }
  return true;
}
bool ZipReader::Read(std::string_view name, Bytes& out, std::wstring* err) {
  int i = Find(name);
  if (i < 0) { out.clear(); if (err) *err = L"The document is missing a required part."; return false; }
  return Read(i, out, err);
}
std::optional<std::string> ZipReader::ReadString(std::string_view name) {
  Bytes b;
  if (!Read(name, b)) return std::nullopt;
  return std::string(b.begin(), b.end());
}

bool IsSafeZipEntryName(std::string_view name) {
  if (name.empty() || name.size() > 1000) return false;
  if (name.front() == '/' || name.front() == '\\') return false;
  if (name.find(':') != std::string_view::npos) return false;            // drive letters and NTFS alternate streams
  if (name.find('\0') != std::string_view::npos) return false;
  std::string n(name);
  std::replace(n.begin(), n.end(), '\\', '/');
  size_t st = 0;
  while (st <= n.size()) {
    size_t e = n.find('/', st);
    if (e == std::string::npos) e = n.size();
    std::string comp = n.substr(st, e - st);
    if (comp == "..") return false;
    std::string up = UpperAscii(comp);
    size_t dot = up.find('.');
    std::string stem = dot == std::string::npos ? up : up.substr(0, dot);
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || ((stem.size() == 4) && (StartsWith(stem, "COM") || StartsWith(stem, "LPT")) && stem[3] >= '0' && stem[3] <= '9')) return false;
    if (!comp.empty() && (comp.back() == '.' || comp.back() == ' ') && comp != ".") return false;
    st = e + 1;
  }
  return true;
}

bool ZipReader::ExtractAll(const fs::path& dir, std::wstring* err, int* skipped) {
  if (skipped) *skipped = 0;
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path base = fs::weakly_canonical(dir, ec);
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& e = entries_[i];
    if (!IsSafeZipEntryName(e.name)) { if (skipped) ++*skipped; continue; }
    std::string rel = e.name;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    fs::path target = base / fs::path(Widen(rel)).lexically_normal();
    // belt and braces: the normalised target must stay below `base`
    auto relp = target.lexically_relative(base);
    if (relp.empty() || *relp.begin() == L"..") { if (skipped) ++*skipped; continue; }
    if (e.isDir || (!rel.empty() && rel.back() == '/')) { fs::create_directories(target, ec); continue; }
    fs::create_directories(target.parent_path(), ec);
    Bytes data;
    if (!Read((int)i, data, err)) return false;
    if (!WriteFileBytes(target, data.data(), data.size(), err)) return false;
  }
  return true;
}

} // namespace filo::doc
