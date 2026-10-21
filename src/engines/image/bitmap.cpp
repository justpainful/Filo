#include "engines/image/image_internal.h"
#include "engines/image/png_writer.h"
#include "engines/common/ffmpeg.h"
#include "registry/filetype.h"
#include "core/log.h"
#include <wincodec.h>
#include <wincodecsdk.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "propsys.lib")

namespace filo::img {

// =====================================================================================================================
// small utilities
// =====================================================================================================================
Scratch::Scratch() {
  static std::atomic<unsigned> counter{0};
  wchar_t name[96];
  swprintf_s(name, L"img-%lu-%llu-%u", GetCurrentProcessId(), (unsigned long long)GetTickCount64(), counter.fetch_add(1));
  dir = TempDir() / name;
  std::error_code ec; fs::create_directories(dir, ec);
}
Scratch::~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }

bool ReadFileBytes(const fs::path& p, Bytes& out, std::wstring* err) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = L"The file could not be opened (it may be missing, in use or protected)."; return false; }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (1536ll << 20)) { CloseHandle(h); if (err) *err = L"The file is too large to process."; return false; }
  out.resize((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < out.size()) {
    DWORD chunk = (DWORD)std::min<size_t>(out.size() - done, 16u << 20), got = 0;
    if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) break;
    done += got;
  }
  CloseHandle(h);
  if (done != out.size()) { if (err) *err = L"The file could not be read completely."; return false; }
  return true;
}

bool WriteFileBytes(const fs::path& p, const Bytes& data, std::wstring* err) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = L"The output file could not be created."; return false; }
  size_t done = 0; bool ok = true;
  while (done < data.size()) {
    DWORD chunk = (DWORD)std::min<size_t>(data.size() - done, 16u << 20), put = 0;
    if (!WriteFile(h, data.data() + done, chunk, &put, nullptr) || put == 0) { ok = false; break; }
    done += put;
  }
  CloseHandle(h);
  if (!ok && err) *err = L"The output file could not be written (disk full?).";
  return ok;
}

// =====================================================================================================================
// pixel utilities
// =====================================================================================================================
bool UsesAlpha(const Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; const uint8_t* p = bm.px.data() + 3;
  for (size_t i = 0; i < n; ++i, p += 4) if (*p != 255) return true;
  return false;
}
bool HasPartialAlpha(const Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; const uint8_t* p = bm.px.data() + 3;
  for (size_t i = 0; i < n; ++i, p += 4) if (*p != 255 && *p != 0) return true;
  return false;
}
void FlattenOnWhite(Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; uint8_t* p = bm.px.data();
  for (size_t i = 0; i < n; ++i, p += 4) {
    unsigned a = p[3];
    if (a == 255) continue;
    for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((p[c] * a + 255 * (255 - a) + 127) / 255);
    p[3] = 255;
  }
}
void Premultiply(Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; uint8_t* p = bm.px.data();
  for (size_t i = 0; i < n; ++i, p += 4) { unsigned a = p[3]; if (a == 255) continue; for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((p[c] * a + 127) / 255); }
}
void Unpremultiply(Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; uint8_t* p = bm.px.data();
  for (size_t i = 0; i < n; ++i, p += 4) { unsigned a = p[3]; if (a == 255 || a == 0) continue; for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::min(255u, (p[c] * 255 + a / 2) / a); }
}

void ApplyOrientation(Bitmap& bm, int o) {
  if (o < 2 || o > 8 || bm.Empty()) return;
  int w = bm.width, h = bm.height;
  bool swap = o >= 5;
  int nw = swap ? h : w, nh = swap ? w : h;
  std::vector<uint8_t> dst((size_t)nw * nh * 4);
  const uint32_t* s = (const uint32_t*)bm.px.data(); uint32_t* d = (uint32_t*)dst.data();
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    int dx, dy;
    switch (o) {
      case 2: dx = w - 1 - x; dy = y; break;
      case 3: dx = w - 1 - x; dy = h - 1 - y; break;
      case 4: dx = x; dy = h - 1 - y; break;
      case 5: dx = y; dy = x; break;
      case 6: dx = h - 1 - y; dy = x; break;
      case 7: dx = h - 1 - y; dy = w - 1 - x; break;
      default: dx = y; dy = w - 1 - x; break;
    }
    d[(size_t)dy * nw + dx] = s[(size_t)y * w + x];
  }
  bm.px.swap(dst); bm.width = nw; bm.height = nh;
}

// =====================================================================================================================
// WIC helpers
// =====================================================================================================================
static ComPtr<IWICImagingFactory> NewFactory() {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), f.AsVoid()))) return nullptr;
  return f;
}

static ComPtr<IWICBitmap> BitmapFromBgra(IWICImagingFactory* f, const Bitmap& bm, bool premultiplied = false) {
  ComPtr<IWICBitmap> wb;
  f->CreateBitmapFromMemory(bm.width, bm.height, premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA, (UINT)bm.Stride(), (UINT)bm.px.size(), const_cast<BYTE*>(bm.px.data()), &wb);
  return wb;
}

bool ResizeBitmap(const Bitmap& in, int nw, int nh, Bitmap& out) {
  if (in.Empty() || nw < 1 || nh < 1) return false;
  if (nw == in.width && nh == in.height) { out = in; return true; }
  ComInit com(COINIT_MULTITHREADED);
  auto f = NewFactory(); if (!f) return false;
  bool alpha = UsesAlpha(in);
  ComPtr<IWICBitmap> src = BitmapFromBgra(f.Get(), in);
  if (!src) return false;
  ComPtr<IWICBitmapSource> chain(src.Get());
  ComPtr<IWICFormatConverter> toP;
  if (alpha) {
    f->CreateFormatConverter(&toP);
    if (FAILED(toP->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    chain = ComPtr<IWICBitmapSource>(toP.Get());
  }
  ComPtr<IWICBitmapScaler> sc; f->CreateBitmapScaler(&sc);
  WICBitmapInterpolationMode mode = (nw < in.width || nh < in.height) ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeHighQualityCubic;
  if (FAILED(sc->Initialize(chain.Get(), nw, nh, mode))) return false;
  ComPtr<IWICBitmapSource> res(sc.Get());
  ComPtr<IWICFormatConverter> back;
  if (alpha) {
    f->CreateFormatConverter(&back);
    if (FAILED(back->Initialize(sc.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    res = ComPtr<IWICBitmapSource>(back.Get());
  }
  Bitmap tmp; tmp.Allocate(nw, nh);
  if (FAILED(res->CopyPixels(nullptr, (UINT)tmp.Stride(), (UINT)tmp.px.size(), tmp.px.data()))) return false;
  tmp.info = in.info;
  out = std::move(tmp);
  return true;
}

bool ConvertToSRGB(Bitmap& bm, const Bytes& icc) {
  if (bm.Empty() || icc.empty() || IccColorSpace(icc) != "RGB ") return false;
  ComInit com(COINIT_MULTITHREADED);
  auto f = NewFactory(); if (!f) return false;
  ComPtr<IWICColorContext> src, dst;
  if (FAILED(f->CreateColorContext(&src)) || FAILED(src->InitializeFromMemory(icc.data(), (UINT)icc.size()))) return false;
  if (FAILED(f->CreateColorContext(&dst)) || FAILED(dst->InitializeFromExifColorSpace(1))) return false;
  ComPtr<IWICBitmap> wb = BitmapFromBgra(f.Get(), bm); if (!wb) return false;
  ComPtr<IWICFormatConverter> to24; f->CreateFormatConverter(&to24);
  if (FAILED(to24->Initialize(wb.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  ComPtr<IWICColorTransform> xf; if (FAILED(f->CreateColorTransformer(&xf))) return false;
  if (FAILED(xf->Initialize(to24.Get(), src.Get(), dst.Get(), GUID_WICPixelFormat24bppBGR))) return false;
  std::vector<uint8_t> rgb((size_t)bm.width * bm.height * 3);
  if (FAILED(xf->CopyPixels(nullptr, bm.width * 3, (UINT)rgb.size(), rgb.data()))) return false;
  size_t n = (size_t)bm.width * bm.height;
  for (size_t i = 0; i < n; ++i) { bm.px[i * 4] = rgb[i * 3]; bm.px[i * 4 + 1] = rgb[i * 3 + 1]; bm.px[i * 4 + 2] = rgb[i * 3 + 2]; }
  return true;
}

static unsigned GetUInt(IWICMetadataQueryReader* q, const wchar_t* name, unsigned def) {
  if (!q) return def;
  PROPVARIANT v; PropVariantInit(&v);
  unsigned r = def;
  if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
    switch (v.vt) { case VT_UI1: r = v.bVal; break; case VT_UI2: r = v.uiVal; break; case VT_UI4: r = v.ulVal; break; case VT_I4: r = (unsigned)v.lVal; break; case VT_I2: r = (unsigned)v.iVal; break; case VT_BOOL: r = v.boolVal ? 1 : 0; break; default: break; }
  }
  PropVariantClear(&v);
  return r;
}

static bool ConvertFrameToBgra(IWICImagingFactory* f, IWICBitmapSource* src, Bitmap& out) {
  UINT w = 0, h = 0; src->GetSize(&w, &h);
  if (!w || !h || (uint64_t)w * h > (1ull << 29)) return false;
  ComPtr<IWICFormatConverter> conv; if (FAILED(f->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  out.Allocate((int)w, (int)h);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
}

struct WicResult {
  Bitmap bm;
  int frameCount = 1;
  bool formatAlpha = false;
  Bytes wicIcc;
  int tiffOrientation = 0;
};

static bool WicDecodeGif(IWICImagingFactory* f, IWICBitmapDecoder* dec, int target, WicResult& r, std::wstring* err) {
  UINT n = 0; dec->GetFrameCount(&n);
  target = std::clamp(target, 0, (int)n - 1);
  ComPtr<IWICMetadataQueryReader> dq; dec->GetMetadataQueryReader(&dq);
  unsigned cw = GetUInt(dq.Get(), L"/logscrdesc/Width", 0), ch = GetUInt(dq.Get(), L"/logscrdesc/Height", 0);
  Bitmap canvas, saved;
  int prevDisposal = 0; RECT prevRect{};
  for (int i = 0; i <= target; ++i) {
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(i, &fr))) { if (err) *err = L"The GIF frame could not be read."; return false; }
    Bitmap fb; if (!ConvertFrameToBgra(f, fr.Get(), fb)) { if (err) *err = L"The GIF frame could not be decoded."; return false; }
    if (i == 0) { if (!cw || !ch) { cw = fb.width; ch = fb.height; } canvas.Allocate((int)cw, (int)ch); }
    ComPtr<IWICMetadataQueryReader> q; fr->GetMetadataQueryReader(&q);
    int left = (int)GetUInt(q.Get(), L"/imgdesc/Left", 0), top = (int)GetUInt(q.Get(), L"/imgdesc/Top", 0);
    int disposal = (int)GetUInt(q.Get(), L"/grctlext/Disposal", 0);
    if (i > 0) {
      if (prevDisposal == 2) { for (int y = prevRect.top; y < prevRect.bottom && y < canvas.height; ++y) for (int x = prevRect.left; x < prevRect.right && x < canvas.width; ++x) memset(canvas.Row(y) + x * 4, 0, 4); }
      else if (prevDisposal == 3 && !saved.Empty()) canvas.px = saved.px;
    }
    if (disposal == 3) saved.px = canvas.px, saved.width = canvas.width, saved.height = canvas.height;
    for (int y = 0; y < fb.height; ++y) {
      int cy = top + y; if (cy < 0 || cy >= canvas.height) continue;
      for (int x = 0; x < fb.width; ++x) {
        int cx = left + x; if (cx < 0 || cx >= canvas.width) continue;
        const uint8_t* s = fb.Row(y) + x * 4;
        if (s[3] == 0) continue;
        memcpy(canvas.Row(cy) + cx * 4, s, 4);
      }
    }
    prevDisposal = disposal; prevRect = {left, top, left + fb.width, top + fb.height};
  }
  r.bm = std::move(canvas); r.frameCount = (int)n; r.formatAlpha = true;
  return true;
}

static bool WicDecode(const Bytes& data, const std::wstring& fmt, int frameIndex, WicResult& r, std::wstring* err) {
  auto f = NewFactory();
  if (!f) { if (err) *err = L"The Windows imaging component is not available."; return false; }
  ComPtr<IWICStream> st; f->CreateStream(&st);
  if (!st || FAILED(st->InitializeFromMemory(const_cast<BYTE*>(data.data()), (DWORD)data.size()))) { if (err) *err = L"The image could not be opened."; return false; }
  ComPtr<IWICBitmapDecoder> dec;
  HRESULT hr = f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
  if (FAILED(hr) || !dec) { if (err) *err = L"This image could not be read. The file may be damaged or its codec is not installed."; return false; }
  UINT n = 1; dec->GetFrameCount(&n); if (n < 1) n = 1;
  if (fmt == L"gif") return WicDecodeGif(f.Get(), dec.Get(), frameIndex, r, err);
  ComPtr<IWICBitmapFrameDecode> fr;
  hr = dec->GetFrame(std::clamp(frameIndex, 0, (int)n - 1), &fr);
  if (FAILED(hr) || !fr) { if (err) *err = L"This image could not be read. The file may be damaged."; return false; }
  r.frameCount = (int)n;
  WICPixelFormatGUID pf{}; fr->GetPixelFormat(&pf);
  { ComPtr<IWICComponentInfo> ci; if (SUCCEEDED(f->CreateComponentInfo(pf, &ci))) { ComPtr<IWICPixelFormatInfo2> pi = ci.As<IWICPixelFormatInfo2>(); if (pi) { BOOL tr = FALSE; pi->SupportsTransparency(&tr); r.formatAlpha = tr != FALSE; } } }
  // embedded ICC profile (for formats whose container we do not parse ourselves)
  {
    UINT cnt = 0; fr->GetColorContexts(0, nullptr, &cnt);
    if (cnt > 0 && cnt < 8) {
      std::vector<ComPtr<IWICColorContext>> ctx(cnt); std::vector<IWICColorContext*> raw(cnt);
      for (UINT i = 0; i < cnt; ++i) { f->CreateColorContext(&ctx[i]); raw[i] = ctx[i].Get(); }
      UINT got = 0;
      if (SUCCEEDED(fr->GetColorContexts(cnt, raw.data(), &got))) for (UINT i = 0; i < got; ++i) {
        WICColorContextType t{}; ctx[i]->GetType(&t);
        if (t == WICColorContextProfile) { UINT sz = 0; ctx[i]->GetProfileBytes(0, nullptr, &sz); if (sz > 128 && sz < (64u << 20)) { r.wicIcc.resize(sz); ctx[i]->GetProfileBytes(sz, r.wicIcc.data(), &sz); break; } }
      }
    }
  }
  if (fmt == L"tiff") {
    ComPtr<IWICMetadataQueryReader> q; if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) r.tiffOrientation = (int)GetUInt(q.Get(), L"/ifd/{ushort=274}", 0);
  }
  bool cmyk = IsEqualGUID(pf, GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(pf, GUID_WICPixelFormat64bppCMYK);
  if (cmyk && !r.wicIcc.empty() && IccColorSpace(r.wicIcc) == "CMYK") {
    ComPtr<IWICColorContext> src, dst; f->CreateColorContext(&src); f->CreateColorContext(&dst);
    if (SUCCEEDED(src->InitializeFromMemory(r.wicIcc.data(), (UINT)r.wicIcc.size())) && SUCCEEDED(dst->InitializeFromExifColorSpace(1))) {
      ComPtr<IWICColorTransform> xf; f->CreateColorTransformer(&xf);
      if (xf && SUCCEEDED(xf->Initialize(fr.Get(), src.Get(), dst.Get(), GUID_WICPixelFormat24bppBGR))) {
        if (ConvertFrameToBgra(f.Get(), xf.Get(), r.bm)) { r.wicIcc.clear(); r.formatAlpha = false; return true; }
      }
    }
  }
  if (!ConvertFrameToBgra(f.Get(), fr.Get(), r.bm)) { if (err) *err = L"This image could not be decoded (unsupported pixel format or too large)."; return false; }
  return true;
}

// =====================================================================================================================
// capability probes
// =====================================================================================================================
static bool WicHasDecoderForExt(const wchar_t* ext) {
  auto f = NewFactory(); if (!f) return false;
  ComPtr<IEnumUnknown> e; if (FAILED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &e))) return false;
  ComPtr<IUnknown> u; ULONG n = 0;
  while (e->Next(1, u.GetAddressOfReset(), &n) == S_OK && n) {
    ComPtr<IWICBitmapCodecInfo> ci; if (SUCCEEDED(u->QueryInterface(__uuidof(IWICBitmapCodecInfo), ci.AsVoid())) && ci) {
      wchar_t exts[512] = {}; UINT l = 0; ci->GetFileExtensions(512, exts, &l);
      std::wstring s = ToLower(exts);
      if (s.find(ext) != std::wstring::npos) return true;
    }
    u.Reset();
  }
  return false;
}

static bool WicEncodeToBytes(const GUID& container, const Bitmap& bm, bool gray, bool wantAlpha, const std::function<void(IPropertyBag2*)>& props,
                             const std::function<void(IWICBitmapFrameEncode*, IWICImagingFactory*)>& frameSetup, Bytes& out, std::wstring* err,
                             const GUID* forcedFormat = nullptr, const std::function<void(const std::vector<uint8_t>&, IWICBitmapFrameEncode*)>& custom = nullptr);

static std::once_flag g_heicOnce; static bool g_heicEnc = false;
static bool ProbeHeicEncoder() {
  std::call_once(g_heicOnce, [] {
    ComInit com(COINIT_MULTITHREADED);
    Bitmap bm; bm.Allocate(64, 64);
    for (size_t i = 0; i < bm.px.size(); i += 4) { bm.px[i] = 40; bm.px[i + 1] = 90; bm.px[i + 2] = 200; bm.px[i + 3] = 255; }
    Bytes out; std::wstring e;
    g_heicEnc = WicEncodeToBytes(GUID_ContainerFormatHeif, bm, false, false, [](IPropertyBag2*) {}, nullptr, out, &e) && out.size() > 100;
  });
  return g_heicEnc;
}

bool CanEncode(const std::wstring& f0) {
  std::wstring f = CanonicalExt(f0);
  if (f == L"jpg" || f == L"png" || f == L"tiff" || f == L"bmp" || f == L"gif") return true;
  if (f == L"webp") return ff::Available() && ff::HasEncoder("libwebp");
  if (f == L"avif") return ff::Available() && ff::HasEncoder("libaom-av1") && FfHasMuxer("avif");
  if (f == L"heic") return ProbeHeicEncoder();
  return false;
}

static std::once_flag g_decOnce; static bool g_wicWebp = false, g_wicHeif = false, g_wicAvif = false;
bool CanDecode(const std::wstring& f0) {
  std::wstring f = CanonicalExt(f0);
  if (f == L"jpg" || f == L"png" || f == L"tiff" || f == L"bmp" || f == L"gif" || f == L"svg") return true;
  std::call_once(g_decOnce, [] {
    ComInit com(COINIT_MULTITHREADED);
    g_wicWebp = WicHasDecoderForExt(L".webp");
    g_wicHeif = WicHasDecoderForExt(L".heic");
    g_wicAvif = WicHasDecoderForExt(L".avif");
  });
  if (f == L"webp") return g_wicWebp || (ff::Available() && ff::HasDecoder("webp"));
  if (f == L"avif") return g_wicAvif || (ff::Available() && (ff::HasDecoder("libdav1d") || ff::HasDecoder("libaom-av1") || ff::HasDecoder("av1")));
  if (f == L"heic") return g_wicHeif || (ff::Available() && ff::HasDecoder("hevc"));
  return false;
}

bool FormatSupportsAlpha(const std::wstring& f0) {
  std::wstring f = CanonicalExt(f0);
  return f == L"png" || f == L"webp" || f == L"tiff" || f == L"avif" || f == L"gif" || f == L"svg";
}
bool FormatSupportsMetadata(const std::wstring& f0) {
  std::wstring f = CanonicalExt(f0);
  return f == L"jpg" || f == L"png" || f == L"webp" || f == L"tiff" || f == L"heic";
}

// =====================================================================================================================
// decoding driver
// =====================================================================================================================
static int HeifToExifOrientation(const HeifInfo& h) {
  int o = 1;
  switch (h.rotationQuarterTurns & 3) { case 0: o = 1; break; case 1: o = 8; break; case 2: o = 3; break; case 3: o = 6; break; }
  return o;
}

static bool LoadImpl(const Bytes& data, const std::wstring& fmt, const fs::path* srcPath, Bitmap& out, const LoadOptions& opt, std::wstring* err) {
  ImageInfo info; info.format = fmt;
  Metadata raw;                                  // metadata as found in the file
  Bitmap work;
  std::wstring localErr;
  bool decoded = false;
  bool decoderAppliesRotation = false;
  int tiffOrientation = 0;
  Bytes wicIcc;

  if (fmt == L"svg") {
    if (!RasterizeSvg(data, opt.svgLongSide, work, err)) return false;
    info.isVector = true; info.formatHasAlpha = true;
    { std::string low((const char*)data.data(), std::min<size_t>(data.size(), 4u << 20)); for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
      if (low.find("<text") != std::string::npos || low.find(":text") != std::string::npos) info.warnings.push_back(L"This SVG contains text, which the Windows SVG renderer cannot draw; the text is missing from the result");
      if (low.find("<foreignobject") != std::string::npos) info.warnings.push_back(L"This SVG contains embedded HTML, which cannot be drawn and was left out"); }
    info.storedWidth = work.info.storedWidth; info.storedHeight = work.info.storedHeight;
    decoded = true;
  } else {
    JpegInfo ji; PngInfo pi; WebpInfo wi; HeifInfo hi; bool haveHeif = false;
    static const wchar_t* kTruncated = L"This image file is incomplete or damaged (it ends too early).";
    if (fmt == L"jpg") { if (ParseJpeg(data, ji, raw)) { info.storedWidth = ji.width; info.storedHeight = ji.height; if (!ji.complete) { if (err) *err = kTruncated; return false; } } }
    else if (fmt == L"png") { if (ParsePng(data, pi, raw)) { info.storedWidth = pi.width; info.storedHeight = pi.height; if (!pi.complete) { if (err) *err = kTruncated; return false; } } }
    else if (fmt == L"webp") { if (ParseWebp(data, wi, raw)) { info.storedWidth = wi.width; info.storedHeight = wi.height; info.isAnimated = wi.animated && wi.frames > 1; info.frameCount = wi.frames; info.formatHasAlpha = wi.hasAlpha; if (!wi.complete) { if (err) *err = kTruncated; return false; } } }
    else if (fmt == L"bmp" && data.size() >= 6 && (data[2] | (data[3] << 8) | (data[4] << 16) | ((size_t)data[5] << 24)) > data.size()) { if (err) *err = kTruncated; return false; }
    else if (fmt == L"heic" || fmt == L"avif") { haveHeif = ParseHeif(data, hi, raw); if (haveHeif) { info.storedWidth = hi.width; info.storedHeight = hi.height; info.formatHasAlpha = hi.hasAlphaAux; } }

    Scratch scratch;
    auto stagedInput = [&]() -> fs::path {
      if (srcPath) return StageForFfmpeg(*srcPath, scratch.dir, (std::wstring(L"in.") + fmt).c_str());
      fs::path p = scratch.File((std::wstring(L"in.") + fmt).c_str()); WriteFileBytes(p, data, nullptr); return p;
    };
    auto viaFfmpeg = [&](int frame, bool alphaMerge) -> bool {
      fs::path in = stagedInput(); fs::path png = scratch.File(L"dec.png");
      std::wstring e;
      if (!FfDecodeToPng(in, png, frame, alphaMerge, opt.ctx, &e)) { localErr = e; return false; }
      Bytes pb; if (!ReadFileBytes(png, pb, nullptr)) return false;
      WicResult r; if (!WicDecode(pb, L"png", 0, r, &e)) { localErr = e; return false; }
      work = std::move(r.bm); info.decodedWithFfmpeg = true; info.formatHasAlpha = info.formatHasAlpha || alphaMerge;
      return true;
    };

    if (fmt == L"avif") {
      bool useFf = ff::Available() && (ff::HasDecoder("libdav1d") || ff::HasDecoder("libaom-av1") || ff::HasDecoder("av1"));
      if (useFf) {
        bool alphaMerge = false;
        if (haveHeif && hi.hasAlphaAux) alphaMerge = true;
        else if (auto mi = ff::Probe(stagedInput())) { int v = 0; for (auto& s : mi->streams) if (s.type == "video" && !s.attachedPic) ++v; alphaMerge = v >= 2; }
        decoded = viaFfmpeg(0, alphaMerge);
        if (!decoded && alphaMerge) decoded = viaFfmpeg(0, false);
        if (decoded) decoderAppliesRotation = true;
      }
      if (!decoded) {
        WicResult r;
        if (WicDecode(data, fmt, 0, r, &localErr)) { work = std::move(r.bm); wicIcc = r.wicIcc; decoded = true; decoderAppliesRotation = true; info.formatHasAlpha = r.formatAlpha;
          if (haveHeif && hi.hasAlphaAux) info.warnings.push_back(L"The transparency of this AVIF could not be decoded; it was ignored."); }
      }
    } else if (fmt == L"heic") {
      WicResult r;
      if (WicDecode(data, fmt, 0, r, &localErr)) { work = std::move(r.bm); wicIcc = r.wicIcc; decoded = true; decoderAppliesRotation = true; info.formatHasAlpha = r.formatAlpha; }
      // Guard against tile-only output: the primary item's size is known from the container.
      if (decoded && haveHeif && hi.width > 0 && (work.width != hi.DisplayWidth() || work.height != hi.DisplayHeight())) {
        decoded = false; info.warnings.clear();
        FILO_LOG(Warn, "image", "heic decode size mismatch (%dx%d vs %dx%d); trying ffmpeg", work.width, work.height, hi.DisplayWidth(), hi.DisplayHeight());
      }
      if (!decoded && ff::Available() && ff::HasDecoder("hevc")) {
        if (viaFfmpeg(0, false)) { decoded = true; decoderAppliesRotation = true; }
      }
      if (decoded && haveHeif && hi.width > 0 && (work.width != hi.DisplayWidth() || work.height != hi.DisplayHeight())) {
        if (err) *err = L"Only part of this HEIC image could be decoded (tile grid unsupported by the installed codecs).";
        return false;
      }
    } else if (fmt == L"webp") {
      if (info.isAnimated && ff::Available()) {
        decoded = viaFfmpeg(std::clamp(opt.frame, 0, std::max(0, info.frameCount - 1)), false);
      }
      if (!decoded) {
        WicResult r;
        if (WicDecode(data, fmt, 0, r, &localErr)) { work = std::move(r.bm); wicIcc = r.wicIcc; decoded = true; info.formatHasAlpha = r.formatAlpha || info.formatHasAlpha; }
        else if (ff::Available() && ff::HasDecoder("webp")) decoded = viaFfmpeg(0, false);
      }
    } else {
      WicResult r;
      if (WicDecode(data, fmt, opt.frame, r, &localErr)) {
        work = std::move(r.bm); wicIcc = r.wicIcc; decoded = true; info.formatHasAlpha = r.formatAlpha; tiffOrientation = r.tiffOrientation;
        if (fmt == L"gif" || fmt == L"tiff") { info.frameCount = r.frameCount; info.isAnimated = (fmt == L"gif" && r.frameCount > 1); }
      }
    }
    if (!decoded) { if (err) *err = localErr.empty() ? L"This image could not be read. The file may be damaged." : localErr; return false; }

    if (info.storedWidth == 0) { info.storedWidth = work.width; info.storedHeight = work.height; }
    if (haveHeif) { info.orientation = HeifToExifOrientation(hi); info.orientationApplied = decoderAppliesRotation && (hi.rotationQuarterTurns || hi.mirror >= 0); }
    else if (fmt == L"jpg" || fmt == L"png" || fmt == L"webp") info.orientation = std::max(1, ExifOrientation(raw.exif));
    else if (fmt == L"tiff") info.orientation = (tiffOrientation >= 1 && tiffOrientation <= 8) ? tiffOrientation : 1;
    if (raw.icc.empty() && !wicIcc.empty() && fmt != L"heic" && fmt != L"avif") raw.icc = wicIcc;
    if (raw.icc.empty() && !wicIcc.empty() && (fmt == L"heic" || fmt == L"avif")) raw.icc = wicIcc;
  }

  if (work.Empty()) { if (err) *err = L"The image has no pixels."; return false; }
  if (opt.ctx && opt.ctx->Cancelled()) { if (err) *err = L"Cancelled"; return false; }

  // ---- colour management ----------------------------------------------------------------------------------------------
  info.hasIcc = !raw.icc.empty();
  Metadata meta = raw;
  if (info.hasIcc) {
    info.iccName = IccDescription(raw.icc);
    bool isRgb = IccColorSpace(raw.icc) == "RGB ";
    if (!isRgb) meta.icc.clear();                                  // gray / CMYK profiles do not describe our RGB output
    else if (opt.convertToSRGB && !IccIsSrgb(raw.icc)) {
      if (ConvertToSRGB(work, raw.icc)) { info.iccConverted = true; meta.icc = SrgbIccProfile(); }
      else { info.warnings.push_back(L"The embedded colour profile could not be applied; colours may look slightly different."); meta.icc.clear(); }
    } else if (opt.convertToSRGB && IccIsSrgb(raw.icc)) {
      meta.icc.clear();                                            // plain sRGB: untagged output is sRGB by definition
    }
  }

  // ---- orientation ----------------------------------------------------------------------------------------------------
  if (opt.applyExifOrientation && !info.orientationApplied && info.orientation > 1 && fmt != L"heic" && fmt != L"avif") {
    ApplyOrientation(work, info.orientation);
    info.orientationApplied = true;
  }
  info.fullWidth = work.width; info.fullHeight = work.height;
  info.hasAlpha = UsesAlpha(work);
  if (info.hasAlpha) info.formatHasAlpha = true;

  // ---- preview downscale ----------------------------------------------------------------------------------------------
  if (opt.maxSide > 0 && std::max(work.width, work.height) > opt.maxSide) {
    double k = (double)opt.maxSide / std::max(work.width, work.height);
    int nw = std::max(1, (int)std::lround(work.width * k)), nh = std::max(1, (int)std::lround(work.height * k));
    Bitmap scaled; if (ResizeBitmap(work, nw, nh, scaled)) work = std::move(scaled);
  }

  info.meta = std::move(meta);
  out.width = work.width; out.height = work.height; out.px = std::move(work.px); out.info = std::move(info);
  return true;
}

static std::wstring FormatError(const FileKind& k) {
  if (!k.exists) return L"The file no longer exists.";
  if (k.isDir) return L"This is a folder, not an image.";
  if (!k.readable) return L"The file cannot be opened (access denied or in use).";
  if (k.size == 0) return L"The file is empty.";
  return L"This file is not a supported image.";
}

bool LoadBitmap(const fs::path& path, Bitmap& out, const LoadOptions& opt, std::wstring* err) {
  ComInit com(COINIT_MULTITHREADED);
  FileKind k = DetectFile(path);
  if (!k.exists || k.isDir || !k.readable || k.size == 0) { if (err) *err = FormatError(k); return false; }
  Bytes data; if (!ReadFileBytes(path, data, err)) return false;
  // re-classify from the bytes we hold (the file could have changed since DetectFile)
  FileKind c = ClassifyBytes(data.data(), std::min<size_t>(data.size(), 65536), k.declaredExt);
  std::wstring fmt = c.format;
  if (c.group != Group::Image || fmt.empty() || fmt == L"ico") { if (err) *err = L"This file is not a supported image (or it is damaged)."; return false; }
  return LoadImpl(data, fmt, &path, out, opt, err);
}

bool LoadBitmapFromMemory(const Bytes& data, Bitmap& out, const LoadOptions& opt, std::wstring* err, const std::wstring& declaredExt) {
  ComInit com(COINIT_MULTITHREADED);
  if (data.empty()) { if (err) *err = L"The image data is empty."; return false; }
  FileKind c = ClassifyBytes(data.data(), std::min<size_t>(data.size(), 65536), declaredExt);
  if (c.group != Group::Image || c.format.empty()) { if (err) *err = L"This data is not a supported image."; return false; }
  return LoadImpl(data, c.format, nullptr, out, opt, err);
}

// =====================================================================================================================
// encoding
// =====================================================================================================================
static bool WicEncodeToBytes(const GUID& container, const Bitmap& bm, bool gray, bool wantAlpha, const std::function<void(IPropertyBag2*)>& props,
                             const std::function<void(IWICBitmapFrameEncode*, IWICImagingFactory*)>& frameSetup, Bytes& out, std::wstring* err,
                             const GUID* forcedFormat, const std::function<void(const std::vector<uint8_t>&, IWICBitmapFrameEncode*)>& custom) {
  auto f = NewFactory();
  if (!f) { if (err) *err = L"The Windows imaging component is not available."; return false; }
  // Encode into an ASCII-named scratch file: CreateStreamOnHGlobal grows by the size of every write (quadratic for big TIFFs).
  Scratch sc;
  fs::path tmpFile = sc.File(L"wic.out");
  ComPtr<IWICStream> mem;
  if (FAILED(f->CreateStream(&mem)) || FAILED(mem->InitializeFromFilename(tmpFile.c_str(), GENERIC_WRITE))) { if (err) *err = L"The encoder could not create its scratch file."; return false; }
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(container, nullptr, &enc)) || !enc) { if (err) *err = L"This image format cannot be written on this computer."; return false; }
  if (FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache))) { if (err) *err = L"The image encoder could not be started."; return false; }
  ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> pb;
  if (FAILED(enc->CreateNewFrame(&fr, &pb))) { if (err) *err = L"The image encoder could not be started."; return false; }
  if (props) props(pb.Get());
  if (FAILED(fr->Initialize(pb.Get()))) { if (err) *err = L"The image encoder rejected the settings."; return false; }
  if (FAILED(fr->SetSize(bm.width, bm.height))) { if (err) *err = L"The image size is not supported by this format."; return false; }
  WICPixelFormatGUID want = forcedFormat ? *forcedFormat : (gray ? GUID_WICPixelFormat8bppGray : (wantAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR));
  WICPixelFormatGUID got = want;
  if (FAILED(fr->SetPixelFormat(&got))) { if (err) *err = L"The image encoder rejected the pixel format."; return false; }
  if (frameSetup) frameSetup(fr.Get(), f.Get());
  // feed pixels, converting to what the encoder accepted
  ComPtr<IWICBitmap> src = BitmapFromBgra(f.Get(), bm);
  if (!src) { if (err) *err = L"Out of memory."; return false; }
  ComPtr<IWICBitmapSource> feed(src.Get());
  ComPtr<IWICFormatConverter> conv;
  if (!IsEqualGUID(got, GUID_WICPixelFormat32bppBGRA)) {
    f->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(src.Get(), got, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) { if (err) *err = L"The image could not be converted for this format."; return false; }
    feed = ComPtr<IWICBitmapSource>(conv.Get());
  }
  if (FAILED(fr->WriteSource(feed.Get(), nullptr))) { if (err) *err = L"The image could not be encoded."; return false; }
  if (FAILED(fr->Commit()) || FAILED(enc->Commit())) { if (err) *err = L"The image could not be finalised."; return false; }
  mem->Commit(STGC_DEFAULT); mem.Reset(); enc.Reset(); fr.Reset();
  if (!ReadFileBytes(tmpFile, out, err)) return false;
  (void)custom;
  return true;
}

// ---- EXIF replay into a WIC metadata writer (TIFF and HEIC targets) ------------------------------------------------------
static int ReplayExif(IWICMetadataQueryWriter* w, const Bytes& exif) {
  if (!w || exif.size() < 8) return 0;
  bool le = exif[0] == 'I';
  auto r16 = [&](size_t o) -> unsigned { return le ? (exif[o] | (exif[o + 1] << 8)) : ((exif[o] << 8) | exif[o + 1]); };
  auto r32 = [&](size_t o) -> uint32_t { return le ? (exif[o] | (exif[o + 1] << 8) | (exif[o + 2] << 16) | ((uint32_t)exif[o + 3] << 24)) : (((uint32_t)exif[o] << 24) | (exif[o + 1] << 16) | (exif[o + 2] << 8) | exif[o + 3]); };
  int written = 0;
  auto walk = [&](size_t ifdOff, const std::wstring& prefix, bool isIfd0, auto&& self) -> void {
    if (ifdOff < 8 || ifdOff + 2 > exif.size()) return;
    unsigned cnt = r16(ifdOff);
    if (ifdOff + 2 + (size_t)cnt * 12 > exif.size()) return;
    for (unsigned i = 0; i < cnt; ++i) {
      size_t e = ifdOff + 2 + (size_t)i * 12;
      unsigned tag = r16(e), type = r16(e + 2); uint32_t n = r32(e + 4);
      if (tag == 0x8769 || tag == 0x8825) { self(r32(e + 8), tag == 0x8769 ? L"/ifd/exif" : L"/ifd/gps", false, self); continue; }
      if (tag == 0xA005 || tag == 0x0112 || tag == 0x927C /*MakerNote*/ || tag == 0x0201 || tag == 0x0202) continue;
      static const unsigned skipIfd0[] = {0xFE, 0x100, 0x101, 0x102, 0x103, 0x106, 0x111, 0x115, 0x116, 0x117, 0x11C, 0x140, 0x152, 0x153, 0x13D, 0x11A, 0x11B, 0x128};
      bool skip = false;
      if (isIfd0) for (unsigned s : skipIfd0) if (s == tag && tag != 0x11A && tag != 0x11B && tag != 0x128) skip = true;
      if (skip) continue;
      static const unsigned sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
      if (type < 1 || type > 12 || n == 0 || n > 4096) continue;
      size_t bytes = (size_t)sizes[type] * n;
      size_t dataOff = bytes <= 4 ? e + 8 : r32(e + 8);
      if (dataOff + bytes > exif.size()) continue;
      const uint8_t* d = exif.data() + dataOff;
      PROPVARIANT v; PropVariantInit(&v);
      bool ok = true;
      std::vector<ULONG> u32; std::vector<USHORT> u16; std::vector<ULONGLONG> u64; std::vector<LONGLONG> i64; std::vector<BYTE> b8;
      std::string ascii;
      auto rd16 = [&](const uint8_t* p) -> unsigned { return le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]); };
      auto rd32 = [&](const uint8_t* p) -> uint32_t { return le ? (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) : (((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]); };
      switch (type) {
        case 2: ascii.assign((const char*)d, n); while (!ascii.empty() && ascii.back() == 0) ascii.pop_back(); v.vt = VT_LPSTR; v.pszVal = (LPSTR)ascii.c_str(); break;
        case 3: for (uint32_t k = 0; k < n; ++k) u16.push_back((USHORT)rd16(d + 2 * k)); if (n == 1) { v.vt = VT_UI2; v.uiVal = u16[0]; } else { v.vt = VT_VECTOR | VT_UI2; v.caui.cElems = n; v.caui.pElems = u16.data(); } break;
        case 4: for (uint32_t k = 0; k < n; ++k) u32.push_back(rd32(d + 4 * k)); if (n == 1) { v.vt = VT_UI4; v.ulVal = u32[0]; } else { v.vt = VT_VECTOR | VT_UI4; v.caul.cElems = n; v.caul.pElems = u32.data(); } break;
        case 5: for (uint32_t k = 0; k < n; ++k) u64.push_back((ULONGLONG)rd32(d + 8 * k) | ((ULONGLONG)rd32(d + 8 * k + 4) << 32)); if (n == 1) { v.vt = VT_UI8; v.uhVal.QuadPart = u64[0]; } else { v.vt = VT_VECTOR | VT_UI8; v.cauh.cElems = n; v.cauh.pElems = (ULARGE_INTEGER*)u64.data(); } break;
        case 10: for (uint32_t k = 0; k < n; ++k) i64.push_back((LONGLONG)((ULONGLONG)rd32(d + 8 * k) | ((ULONGLONG)rd32(d + 8 * k + 4) << 32))); if (n == 1) { v.vt = VT_I8; v.hVal.QuadPart = i64[0]; } else { v.vt = VT_VECTOR | VT_I8; v.cah.cElems = n; v.cah.pElems = (LARGE_INTEGER*)i64.data(); } break;
        case 1: b8.assign(d, d + n); if (n == 1) { v.vt = VT_UI1; v.bVal = b8[0]; } else { v.vt = VT_VECTOR | VT_UI1; v.caub.cElems = n; v.caub.pElems = b8.data(); } break;
        case 7: b8.assign(d, d + n); v.vt = VT_BLOB; v.blob.cbSize = n; v.blob.pBlobData = b8.data(); break;
        default: ok = false; break;
      }
      if (!ok) continue;
      wchar_t path[96]; swprintf_s(path, L"%ls/{ushort=%u}", prefix.c_str(), tag);
      if (SUCCEEDED(w->SetMetadataByName(path, &v))) ++written;
    }
  };
  walk(r32(4), L"/ifd", true, walk);
  return written;
}

static void FillWicProp(IPropertyBag2* pb, const wchar_t* name, VARTYPE vt, const std::function<void(VARIANT&)>& setv) {
  if (!pb) return;
  PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(name);
  VARIANT v; VariantInit(&v); v.vt = vt; setv(v);
  pb->Write(1, &opt, &v);
  VariantClear(&v);
}

static bool IsGrayImage(const Bitmap& bm) {
  size_t n = (size_t)bm.width * bm.height; const uint8_t* p = bm.px.data();
  for (size_t i = 0; i < n; ++i, p += 4) if (p[0] != p[1] || p[1] != p[2]) return false;
  return true;
}

static std::wstring UpperFmt(const std::wstring& f) { std::wstring u = f; for (auto& c : u) c = (wchar_t)towupper(c); return u; }

SaveResult SaveBitmap(const fs::path& out, const Bitmap& bm0, const std::wstring& format0, const SaveOptions& opt) {
  ComInit com(COINIT_MULTITHREADED);
  SaveResult res;
  std::wstring fmt = CanonicalExt(format0);
  if (bm0.Empty()) return SaveResult::Fail(L"There is no image to save.");
  if (!CanEncode(fmt)) return SaveResult::Fail(L"Saving as " + UpperFmt(fmt) + L" is not available on this computer.");
  if (fmt == L"jpg" && (bm0.width > 65500 || bm0.height > 65500)) return SaveResult::Fail(L"The image is too large for JPEG (maximum 65500 pixels per side).");
  if (fmt == L"webp" && (bm0.width > 16383 || bm0.height > 16383)) return SaveResult::Fail(L"The image is too large for WebP (maximum 16383 pixels per side).");
  if (fmt == L"gif" && (bm0.width > 65535 || bm0.height > 65535)) return SaveResult::Fail(L"The image is too large for GIF.");
  if (fmt == L"heic" && (bm0.width < 8 || bm0.height < 8)) return SaveResult::Fail(L"HEIC cannot store pictures smaller than 8 x 8 pixels.");
  const Bitmap* bm = &bm0;
  Bitmap flat;
  bool usesAlpha = UsesAlpha(bm0);
  if (usesAlpha && !FormatSupportsAlpha(fmt)) {
    flat = bm0; FlattenOnWhite(flat); bm = &flat; usesAlpha = false;
    res.warnings.push_back(L"Transparency was flattened onto white (" + UpperFmt(fmt) + L" has no transparency)");
  } else if (usesAlpha && fmt == L"heic") {}
  if (fmt == L"heic" && usesAlpha) {   // handled above because FormatSupportsAlpha(heic)=false; kept for clarity
  }

  // metadata to write
  Metadata m;
  m.icc = bm0.info.meta.icc;
  if (opt.preserveMetadata) { m.exif = bm0.info.meta.exif; m.xmp = bm0.info.meta.xmp; }
  int newOrient = (bm0.info.orientationApplied || bm0.info.orientation <= 1) ? 1 : 0;
  if (!m.exif.empty()) ExifPatch(m.exif, newOrient, bm->width, bm->height);
  bool canMeta = FormatSupportsMetadata(fmt);
  if (m.HasExifOrXmp() && !canMeta) {
    res.warnings.push_back(L"Metadata (EXIF/XMP) could not be kept: " + UpperFmt(fmt) + L" files written by Filo do not carry it");
    m.exif.clear(); m.xmp.clear();
  }
  if (!m.icc.empty() && !(fmt == L"jpg" || fmt == L"png" || fmt == L"webp" || fmt == L"tiff")) m.icc.clear();   // sRGB pixels need no tag in BMP/GIF/AVIF/HEIC

  Bytes bytes; std::wstring err;
  auto finishBytes = [&]() -> SaveResult {
    if (!WriteFileBytes(out, bytes, &err)) return SaveResult::Fail(err);
    res.ok = true; return res;
  };
  int q = std::clamp(opt.quality, 1, 100);

  if (fmt == L"png") {
    uint64_t npx = (uint64_t)bm->width * bm->height;
    PngOptions po; po.level = opt.pngLevel > 0 ? std::clamp(opt.pngLevel, 1, 9) : (npx <= 2000000 ? 6 : npx <= 8000000 ? 4 : 2); po.quantize = opt.pngPalette; po.maxColors = opt.pngMaxColors; po.meta = &m;
    po.srgbChunk = false;
    if (!EncodePng(*bm, po, bytes, &err)) return SaveResult::Fail(err);
    return finishBytes();
  }
  if (fmt == L"jpg") {
    bool gray = IsGrayImage(*bm);
    bool ok = WicEncodeToBytes(GUID_ContainerFormatJpeg, *bm, gray, false,
      [&](IPropertyBag2* pb) {
        FillWicProp(pb, L"ImageQuality", VT_R4, [&](VARIANT& v) { v.fltVal = q / 100.0f; });
        FillWicProp(pb, L"JpegYCrCbSubsampling", VT_UI1, [&](VARIANT& v) { v.bVal = (BYTE)(q >= 90 ? WICJpegYCrCbSubsampling444 : WICJpegYCrCbSubsampling420); });
      }, nullptr, bytes, &err);
    if (!ok) return SaveResult::Fail(err);
    if (!m.Empty()) { std::vector<std::wstring> dropped; InjectJpegMeta(bytes, m, &dropped); for (auto& d : dropped) res.warnings.push_back(d); }
    return finishBytes();
  }
  if (fmt == L"bmp") {
    if (!WicEncodeToBytes(GUID_ContainerFormatBmp, *bm, false, false, nullptr, nullptr, bytes, &err)) return SaveResult::Fail(err);
    return finishBytes();
  }
  if (fmt == L"tiff") {
    bool gray = !usesAlpha && IsGrayImage(*bm);
    int replayed = 0;
    bool ok = WicEncodeToBytes(GUID_ContainerFormatTiff, *bm, gray, usesAlpha,
      [&](IPropertyBag2* pb) { FillWicProp(pb, L"TiffCompressionMethod", VT_UI1, [&](VARIANT& v) { v.bVal = (BYTE)(opt.tiffZip ? WICTiffCompressionZIP : WICTiffCompressionLZW); }); },
      [&](IWICBitmapFrameEncode* fr, IWICImagingFactory* f) {
        if (!m.icc.empty()) { ComPtr<IWICColorContext> cc; if (SUCCEEDED(f->CreateColorContext(&cc)) && SUCCEEDED(cc->InitializeFromMemory(m.icc.data(), (UINT)m.icc.size()))) { IWICColorContext* arr[1] = {cc.Get()}; fr->SetColorContexts(1, arr); } }
        if (!m.exif.empty()) { ComPtr<IWICMetadataQueryWriter> qw; if (SUCCEEDED(fr->GetMetadataQueryWriter(&qw))) replayed = ReplayExif(qw.Get(), m.exif); }
      }, bytes, &err);
    if (!ok) return SaveResult::Fail(err);
    if (!m.exif.empty() && replayed == 0) res.warnings.push_back(L"Metadata (EXIF) could not be written to the TIFF");
    return finishBytes();
  }
  if (fmt == L"gif") {
    // palette (<=255 colours + transparent) via our quantiser, written through the WIC GIF encoder
    Bitmap g = *bm;
    bool partial = HasPartialAlpha(g);
    if (usesAlpha) {
      uint8_t* p = g.px.data(); size_t n = (size_t)g.width * g.height;
      for (size_t i = 0; i < n; ++i, p += 4) { if (p[3] < 128) { p[0] = p[1] = p[2] = p[3] = 0; } else p[3] = 255; }
      if (partial) res.warnings.push_back(L"Partial transparency was reduced to on/off (GIF supports no soft edges)");
    }
    QuantResult qr;
    if (!QuantizeBitmap(g, 256, true, qr)) return SaveResult::Fail(L"The image could not be reduced to GIF colours.");
    // WIC GIF: index 0.. palette, transparent entry flagged by alpha 0
    auto f = NewFactory(); if (!f) return SaveResult::Fail(L"The Windows imaging component is not available.");
    ComPtr<IStream> mem; CreateStreamOnHGlobal(nullptr, TRUE, &mem);
    ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> pb;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatGif, nullptr, &enc)) || FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fr, &pb)) || FAILED(fr->Initialize(pb.Get())))
      return SaveResult::Fail(L"The GIF encoder could not be started.");
    fr->SetSize(g.width, g.height);
    WICPixelFormatGUID pf = GUID_WICPixelFormat8bppIndexed; fr->SetPixelFormat(&pf);
    std::vector<WICColor> pal(qr.palette.size()); int transparentIdx = -1;
    for (size_t i = 0; i < pal.size(); ++i) { uint32_t c = qr.palette[i]; pal[i] = c; if ((c >> 24) == 0 && transparentIdx < 0) transparentIdx = (int)i; }
    ComPtr<IWICPalette> wp; f->CreatePalette(&wp);
    if (FAILED(wp->InitializeCustom(pal.data(), (UINT)pal.size()))) return SaveResult::Fail(L"The GIF palette could not be created.");
    fr->SetPalette(wp.Get());
    ComPtr<IWICMetadataQueryWriter> qw;
    if (SUCCEEDED(fr->GetMetadataQueryWriter(&qw)) && transparentIdx >= 0) {
      PROPVARIANT v; PropVariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; qw->SetMetadataByName(L"/grctlext/TransparencyFlag", &v);
      PropVariantInit(&v); v.vt = VT_UI1; v.bVal = (BYTE)transparentIdx; qw->SetMetadataByName(L"/grctlext/TransparentColorIndex", &v);
    }
    if (FAILED(fr->WritePixels(g.height, g.width, (UINT)qr.indices.size(), qr.indices.data())) || FAILED(fr->Commit()) || FAILED(enc->Commit()))
      return SaveResult::Fail(L"The GIF could not be encoded.");
    STATSTG stat{}; mem->Stat(&stat, STATFLAG_NONAME); bytes.resize((size_t)stat.cbSize.QuadPart);
    LARGE_INTEGER z{}; mem->Seek(z, STREAM_SEEK_SET, nullptr); ULONG got = 0; mem->Read(bytes.data(), (ULONG)bytes.size(), &got);
    return finishBytes();
  }
  if (fmt == L"heic") {
    int replayed = 0;
    bool ok = WicEncodeToBytes(GUID_ContainerFormatHeif, *bm, false, false,
      [&](IPropertyBag2* pb) {
        FillWicProp(pb, L"ImageQuality", VT_R4, [&](VARIANT& v) { v.fltVal = q / 100.0f; });
        if (opt.lossless) FillWicProp(pb, L"Lossless", VT_BOOL, [&](VARIANT& v) { v.boolVal = VARIANT_TRUE; });
      },
      [&](IWICBitmapFrameEncode* fr, IWICImagingFactory*) {
        if (!m.exif.empty()) { ComPtr<IWICMetadataQueryWriter> qw; if (SUCCEEDED(fr->GetMetadataQueryWriter(&qw))) replayed = ReplayExif(qw.Get(), m.exif); }
      }, bytes, &err);
    if (!ok) return SaveResult::Fail(err);
    if (!m.exif.empty() && replayed == 0) res.warnings.push_back(L"Metadata (EXIF) could not be written to the HEIC file");
    return finishBytes();
  }
  if (fmt == L"webp" || fmt == L"avif") {
    Scratch scratch;
    fs::path png = scratch.File(L"src.png"), enc = scratch.File(fmt == L"webp" ? L"out.webp" : L"out.avif");
    PngOptions po; po.level = 1;
    if (!EncodePng(*bm, po, bytes, &err) || !WriteFileBytes(png, bytes, &err)) return SaveResult::Fail(err);
    FfEncodeParams fp; fp.quality = q; fp.lossless = opt.lossless; fp.hasAlpha = usesAlpha; fp.avifSpeed = opt.avifSpeed;
    bool ok = fmt == L"webp" ? FfEncodeWebp(png, enc, fp, opt.ctx, &err) : FfEncodeAvif(png, enc, fp, opt.ctx, &err);
    if (!ok) return SaveResult::Fail(err);
    Bytes encoded; if (!ReadFileBytes(enc, encoded, &err)) return SaveResult::Fail(err);
    if (fmt == L"webp" && !m.Empty()) { if (!InjectWebpMeta(encoded, m)) res.warnings.push_back(L"Metadata could not be written to the WebP file"); }
    bytes.swap(encoded);
    return finishBytes();
  }
  return SaveResult::Fail(L"Unsupported output format.");
}

} // namespace filo::img
