// Content fingerprints: prove that a metadata edit did not change the real content.
//   images   : decoded pixels of every frame (WIC), fallback ffmpeg decode hash
//   media    : SHA-256 of the copied packets of every audio / video / subtitle stream (ffmpeg -c copy -f streamhash)
//   PDF      : page count + page sizes + extracted text + rendered pixels of the first pages (PDFium)
//   Office   : SHA-256 of every zip part except docProps
#include "core/com.h"
#include "engines/common/ffmpeg.h"
#include "engines/meta/internal.h"
#include "miniz.h"
#include "registry/filetype.h"
#include <wincodec.h>
#include <algorithm>
#include <mutex>
#include <set>
#include "fpdf_text.h"
#include "fpdfview.h"

namespace filo::meta {

namespace detail {

std::optional<std::string> PixelFingerprint(const fs::path& file, std::wstring* how) {
  ComInit ci(COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreate(CLSID_WICImagingFactory, factory))) return std::nullopt;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(factory->CreateDecoderFromFilename(LongPath(file).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return std::nullopt;
  UINT frames = 0;
  if (FAILED(dec->GetFrameCount(&frames)) || frames == 0) return std::nullopt;
  Sha256 h;
  std::vector<uint8_t> buf;
  for (UINT i = 0; i < std::min<UINT>(frames, 4096); ++i) {
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(i, &fr))) return std::nullopt;
    ComPtr<IWICBitmapSource> src;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, fr.Get(), &src))) return std::nullopt;
    UINT w = 0, ht = 0;
    if (FAILED(src->GetSize(&w, &ht)) || !w || !ht) return std::nullopt;
    h.Update(&w, sizeof w); h.Update(&ht, sizeof ht);
    UINT stride = w * 4;
    UINT rowsPerChunk = std::max<UINT>(1, (4u << 20) / stride);
    buf.resize((size_t)stride * std::min(rowsPerChunk, ht));
    for (UINT y = 0; y < ht; y += rowsPerChunk) {
      UINT rows = std::min(rowsPerChunk, ht - y);
      WICRect r{0, (INT)y, (INT)w, (INT)rows};
      if (FAILED(src->CopyPixels(&r, stride, stride * rows, buf.data()))) return std::nullopt;
      h.Update(buf.data(), (size_t)stride * rows);
    }
  }
  if (how) *how = L"decoded pixels (" + std::to_wstring(frames) + (frames == 1 ? L" frame)" : L" frames)");
  return h.Finish();
}

std::optional<std::string> PacketFingerprint(const fs::path& file, std::wstring* how, JobContext* ctx) {
  fs::path ffmpeg = ff::FfmpegPath();
  if (ffmpeg.empty()) return std::nullopt;
  ProcessOptions po;
  po.exe = ffmpeg;
  po.args = {L"-nostdin", L"-hide_banner", L"-v", L"error", L"-i", file.wstring(), L"-map", L"0:a?", L"-map", L"0:V?", L"-map", L"0:s?", L"-c", L"copy", L"-f", L"streamhash", L"-hash", L"sha256", L"-"};
  po.cancel = ctx ? ctx->CancelFlag() : nullptr;
  po.timeoutMs = 20 * 60 * 1000;
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  if (!r.Ok() || r.out.empty()) return std::nullopt;
  std::string norm;
  size_t pos = 0;
  while (pos < r.out.size()) {
    size_t e = r.out.find('\n', pos);
    std::string line = r.out.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.find("SHA256=") != std::string::npos) norm += line + "\n";
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  if (norm.empty()) return std::nullopt;
  if (how) *how = L"stream packets (ffmpeg stream hash)";
  return Sha256Hex(norm.data(), norm.size());
}

static std::optional<std::string> DecodeHash(const fs::path& file, std::wstring* how, JobContext* ctx) {
  fs::path ffmpeg = ff::FfmpegPath();
  if (ffmpeg.empty()) return std::nullopt;
  ProcessOptions po;
  po.exe = ffmpeg;
  po.args = {L"-nostdin", L"-hide_banner", L"-v", L"error", L"-i", file.wstring(), L"-map", L"0:v:0", L"-f", L"hash", L"-hash", L"sha256", L"-"};
  po.cancel = ctx ? ctx->CancelFlag() : nullptr;
  po.timeoutMs = 5 * 60 * 1000;
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  size_t p = r.out.find("SHA256=");
  if (!r.Ok() || p == std::string::npos) return std::nullopt;
  if (how) *how = L"decoded frames (ffmpeg)";
  return r.out.substr(p, 71);
}

static void InitPdfium() {
  static std::once_flag once;
  std::call_once(once, [] { FPDF_InitLibrary(); });     // never destroyed: other engines share the process
}

std::optional<std::string> PdfFingerprint(const fs::path& file, std::wstring* how) {
  InitPdfium();
  HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  if (sz.QuadPart <= 0 || sz.QuadPart > (1ll << 31)) { CloseHandle(h); return std::nullopt; }
  std::vector<uint8_t> data((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < data.size()) { DWORD g = 0; if (!ReadFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1u << 28), &g, nullptr) || !g) break; done += g; }
  CloseHandle(h);
  if (done != data.size()) return std::nullopt;
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(data.data(), data.size(), nullptr);
  if (!doc) return std::nullopt;
  auto close = OnExit([&] { FPDF_CloseDocument(doc); });
  int pages = FPDF_GetPageCount(doc);
  if (pages <= 0) return std::nullopt;
  Sha256 hash;
  hash.Update(&pages, sizeof pages);
  size_t chars = 0;
  for (int i = 0; i < pages; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) return std::nullopt;
    float w = FPDF_GetPageWidthF(page), ht = FPDF_GetPageHeightF(page);
    hash.Update(&w, sizeof w); hash.Update(&ht, sizeof ht);
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (tp) {
      int n = FPDFText_CountChars(tp);
      if (n > 0) {
        std::vector<unsigned short> txt((size_t)n + 1);
        int got = FPDFText_GetText(tp, 0, n, txt.data());
        if (got > 0) { hash.Update(txt.data(), (size_t)(got - 1) * 2); chars += (size_t)got; }
      }
      FPDFText_ClosePage(tp);
    }
    if (i < 3) {       // render the first pages at 50 dpi: catches changes in graphics / images, not only text
      int bw = std::max(1, (int)(w * 50 / 72)), bh = std::max(1, (int)(ht * 50 / 72));
      if ((int64_t)bw * bh < 4000000) {
        FPDF_BITMAP bmp = FPDFBitmap_Create(bw, bh, 0);
        if (bmp) {
          FPDFBitmap_FillRect(bmp, 0, 0, bw, bh, 0xFFFFFFFF);
          FPDF_RenderPageBitmap(bmp, page, 0, 0, bw, bh, 0, 0);
          const uint8_t* px = (const uint8_t*)FPDFBitmap_GetBuffer(bmp);
          int stride = FPDFBitmap_GetStride(bmp);
          for (int y = 0; y < bh; ++y) hash.Update(px + (size_t)y * stride, (size_t)bw * 4);
          FPDFBitmap_Destroy(bmp);
        }
      }
    }
    FPDF_ClosePage(page);
  }
  if (how) *how = L"PDF page count, page sizes, text (" + std::to_wstring(chars) + L" chars) and rendering";
  return hash.Finish();
}

std::optional<std::string> OfficeFingerprint(const fs::path& file, std::wstring* how) {
  HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  if (sz.QuadPart <= 0 || sz.QuadPart > (1ll << 30)) { CloseHandle(h); return std::nullopt; }
  std::vector<uint8_t> data((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < data.size()) { DWORD g = 0; if (!ReadFile(h, data.data() + done, (DWORD)std::min<size_t>(data.size() - done, 1u << 28), &g, nullptr) || !g) break; done += g; }
  CloseHandle(h);
  mz_zip_archive z{};
  if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return std::nullopt;
  auto close = OnExit([&] { mz_zip_reader_end(&z); });
  std::map<std::string, std::string> parts;
  mz_uint n = mz_zip_reader_get_num_files(&z);
  for (mz_uint i = 0; i < n; ++i) {
    if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
    char name[1024];
    mz_zip_reader_get_filename(&z, i, name, sizeof name);
    std::string nm = name;
    if (nm == "docProps/core.xml" || nm == "docProps/app.xml" || nm == "docProps/custom.xml" || nm.rfind("docProps/thumbnail", 0) == 0 || nm == "_rels/.rels" || nm == "[Content_Types].xml") continue;   // package plumbing that metadata edits may touch
    size_t len = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, i, &len, 0);
    if (!p && len != 0) return std::nullopt;
    parts[nm] = Sha256Hex(p, len);
    mz_free(p);
  }
  Sha256 h2;
  for (auto& [k, v] : parts) { h2.Update(k.data(), k.size()); h2.Update(v.data(), v.size()); }
  if (how) *how = L"all " + std::to_wstring(parts.size()) + L" document parts except docProps";
  return h2.Finish();
}

} // namespace detail

std::optional<std::string> ContentFingerprint(const fs::path& file, const std::wstring& format, std::wstring* how, JobContext* ctx) {
  using namespace detail;
  switch (KindOf(format)) {
    case Kind::Image: {
      if (auto p = PixelFingerprint(file, how)) return p;
      return DecodeHash(file, how, ctx);
    }
    case Kind::Audio: case Kind::Video: return PacketFingerprint(file, how, ctx);
    case Kind::Pdf: return PdfFingerprint(file, how);
    case Kind::Office: if (OfficeFormat(format)) return OfficeFingerprint(file, how); return std::nullopt;
    default: return std::nullopt;
  }
}

} // namespace filo::meta
