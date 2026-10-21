#include "engines/pdf/pdf_wic.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace filo::pdf::wic {

ComScope::ComScope() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) { ok_ = true; uninit_ = true; }
  else if (hr == RPC_E_CHANGED_MODE) ok_ = true;      // thread already has an apartment: fine for WIC
}
ComScope::~ComScope() { if (uninit_) CoUninitialize(); }

static ComPtr<IWICImagingFactory> Factory() {
  ComPtr<IWICImagingFactory> f;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  return f;
}

static bool WriteFrame(IWICBitmapEncoder* enc, bool jpeg, int quality, bool sub444, const uint8_t* bgra, int w, int h, bool hasAlpha,
                       bool gray, double dpi, std::wstring* err) {
  ComPtr<IWICBitmapFrameEncode> fr;
  ComPtr<IPropertyBag2> bag;
  if (FAILED(enc->CreateNewFrame(&fr, &bag))) { if (err) *err = L"The image encoder could not be created."; return false; }
  if (jpeg && bag) {
    PROPBAG2 pb[2] = {};
    VARIANT v[2];
    VariantInit(&v[0]); VariantInit(&v[1]);
    pb[0].pstrName = (LPOLESTR)L"ImageQuality"; v[0].vt = VT_R4; v[0].fltVal = std::clamp(quality, 1, 100) / 100.f;
    int n = 1;
    if (sub444 && !gray) { pb[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling"; v[1].vt = VT_UI1; v[1].bVal = WICJpegYCrCbSubsampling444; n = 2; }
    bag->Write(n, pb, v);
  }
  if (FAILED(fr->Initialize(bag.Get()))) { if (err) *err = L"The image encoder could not be initialised."; return false; }
  fr->SetSize(w, h);
  if (dpi > 0) fr->SetResolution(dpi, dpi);
  GUID want = jpeg ? (gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR)
                   : (hasAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR);
  GUID fmt = want;
  fr->SetPixelFormat(&fmt);
  if (fmt != want) { if (err) *err = L"The image encoder does not support the pixel format."; return false; }
  size_t bpp = gray ? 1 : (want == GUID_WICPixelFormat32bppBGRA ? 4 : 3);
  std::vector<uint8_t> row((size_t)w * bpp);
  const int chunk = 64;
  std::vector<uint8_t> buf;
  if (bpp == 4) {
    HRESULT hr = fr->WritePixels(h, w * 4, (UINT)((size_t)w * h * 4), const_cast<BYTE*>(bgra));
    if (FAILED(hr)) { if (err) *err = L"The image could not be written."; return false; }
  } else {
    for (int y0 = 0; y0 < h; y0 += chunk) {
      int rows = std::min(chunk, h - y0);
      buf.resize((size_t)rows * w * bpp);
      for (int r = 0; r < rows; ++r) {
        const uint8_t* s = bgra + (size_t)(y0 + r) * w * 4; uint8_t* d = &buf[(size_t)r * w * bpp];
        if (gray) for (int x = 0; x < w; ++x) d[x] = (uint8_t)((s[4 * x + 2] * 77 + s[4 * x + 1] * 150 + s[4 * x] * 29) >> 8);
        else for (int x = 0; x < w; ++x) { d[3 * x] = s[4 * x]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x + 2]; }
      }
      if (FAILED(fr->WritePixels(rows, (UINT)(w * bpp), (UINT)buf.size(), buf.data()))) { if (err) *err = L"The image could not be written."; return false; }
    }
  }
  if (FAILED(fr->Commit()) || FAILED(enc->Commit())) { if (err) *err = L"The image could not be saved."; return false; }
  return true;
}

static bool EncodeToFile(const fs::path& out, bool jpeg, int quality, const uint8_t* bgra, int w, int h, bool hasAlpha, double dpi, std::wstring* err) {
  auto f = Factory();
  if (!f) { if (err) *err = L"The Windows imaging component is not available."; return false; }
  ComPtr<IWICStream> st;
  f->CreateStream(&st);
  if (FAILED(st->InitializeFromFilename(LongPath(out).c_str(), GENERIC_WRITE))) { if (err) *err = L"The image file could not be created."; return false; }
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) ||
      FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) { if (err) *err = L"The image encoder is not available."; return false; }
  return WriteFrame(enc.Get(), jpeg, quality, quality >= 90, bgra, w, h, hasAlpha, false, dpi, err);
}

bool EncodePng(const fs::path& out, const uint8_t* bgra, int w, int h, bool hasAlpha, double dpi, std::wstring* err) {
  return EncodeToFile(out, false, 0, bgra, w, h, hasAlpha, dpi, err);
}
bool EncodeJpeg(const fs::path& out, const uint8_t* bgra, int w, int h, int quality, double dpi, std::wstring* err) {
  return EncodeToFile(out, true, quality, bgra, w, h, false, dpi, err);
}

bool EncodeJpegMemory(std::vector<uint8_t>& out, const uint8_t* bgra, int w, int h, int quality, bool gray) {
  auto f = Factory();
  if (!f) return false;
  ComPtr<IStream> stm;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) || FAILED(enc->Initialize(stm.Get(), WICBitmapEncoderNoCache))) return false;
  if (!WriteFrame(enc.Get(), true, quality, quality >= 90, bgra, w, h, false, gray, 0, nullptr)) return false;
  STATSTG ss{};
  if (FAILED(stm->Stat(&ss, STATFLAG_NONAME))) return false;
  LARGE_INTEGER z{}; stm->Seek(z, STREAM_SEEK_SET, nullptr);
  out.resize((size_t)ss.cbSize.QuadPart);
  ULONG got = 0;
  if (FAILED(stm->Read(out.data(), (ULONG)out.size(), &got))) return false;
  out.resize(got);
  return got > 0;
}

static int ReadOrientation(IWICBitmapFrameDecode* fr, bool jpeg) {
  ComPtr<IWICMetadataQueryReader> q;
  if (FAILED(fr->GetMetadataQueryReader(&q)) || !q) return 1;
  const wchar_t* paths[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/xmp/tiff:Orientation"};
  for (auto* p : paths) {
    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(q->GetMetadataByName(p, &v))) {
      int o = 1;
      if (v.vt == VT_UI2) o = v.uiVal; else if (v.vt == VT_UI1) o = v.bVal; else if (v.vt == VT_I4) o = v.lVal; else if (v.vt == VT_UI4) o = (int)v.ulVal;
      PropVariantClear(&v);
      if (o >= 1 && o <= 8) return o;
    }
  }
  (void)jpeg;
  return 1;
}

static bool OpenFrame(const fs::path& in, ComPtr<IWICBitmapDecoder>& dec, ComPtr<IWICBitmapFrameDecode>& fr, Decoded& d, std::wstring* err) {
  auto f = Factory();
  if (!f) { if (err) *err = L"The Windows imaging component is not available."; return false; }
  HRESULT hr = f->CreateDecoderFromFilename(LongPath(in).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
  if (FAILED(hr)) { if (err) *err = L"This image could not be read (unsupported or damaged)."; return false; }
  GUID cf{}; dec->GetContainerFormat(&cf);
  d.isJpeg = cf == GUID_ContainerFormatJpeg;
  UINT n = 1; dec->GetFrameCount(&n); d.frames = (int)n;
  if (FAILED(dec->GetFrame(0, &fr))) { if (err) *err = L"This image could not be read (unsupported or damaged)."; return false; }
  UINT w = 0, h = 0; fr->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 60000 || h > 60000 || (double)w * h > 400e6) { if (err) *err = L"This image is too large or empty."; return false; }
  d.w = (int)w; d.h = (int)h;
  fr->GetResolution(&d.dpiX, &d.dpiY);
  WICPixelFormatGUID pf{}; fr->GetPixelFormat(&pf);
  d.cmyk = pf == GUID_WICPixelFormat32bppCMYK || pf == GUID_WICPixelFormat64bppCMYK || pf == GUID_WICPixelFormat40bppCMYKAlpha || pf == GUID_WICPixelFormat80bppCMYKAlpha;
  d.hasAlpha = pf == GUID_WICPixelFormat32bppBGRA || pf == GUID_WICPixelFormat32bppPBGRA || pf == GUID_WICPixelFormat64bppRGBA ||
               pf == GUID_WICPixelFormat64bppPRGBA || pf == GUID_WICPixelFormat128bppRGBAFloat || pf == GUID_WICPixelFormat32bppRGBA;
  d.orientation = ReadOrientation(fr.Get(), d.isJpeg);
  return true;
}

bool Probe(const fs::path& in, Decoded& d, std::wstring* err) {
  ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> fr;
  return OpenFrame(in, dec, fr, d, err);
}

bool Decode(const fs::path& in, Decoded& d, std::wstring* err) {
  ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> fr;
  if (!OpenFrame(in, dec, fr, d, err)) return false;
  auto f = Factory();
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    if (err) *err = L"This image format could not be converted."; return false;
  }
  try { d.bgra.resize((size_t)d.w * d.h * 4); } catch (...) { if (err) *err = L"Not enough memory to read this image."; return false; }
  if (FAILED(conv->CopyPixels(nullptr, (UINT)d.w * 4, (UINT)d.bgra.size(), d.bgra.data()))) { if (err) *err = L"This image could not be decoded."; return false; }
  return true;
}

bool DecodeMemory(const uint8_t* data, size_t size, Decoded& d, std::wstring* err) {
  auto f = Factory();
  if (!f) { if (err) *err = L"The Windows imaging component is not available."; return false; }
  ComPtr<IWICStream> st;
  f->CreateStream(&st);
  if (FAILED(st->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size))) { if (err) *err = L"The image data could not be read."; return false; }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) { if (err) *err = L"The image data could not be decoded."; return false; }
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, &fr))) return false;
  UINT w = 0, h = 0; fr->GetSize(&w, &h);
  if (w == 0 || h == 0 || (double)w * h > 400e6) return false;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  d.w = (int)w; d.h = (int)h;
  try { d.bgra.resize((size_t)w * h * 4); } catch (...) { return false; }
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)d.bgra.size(), d.bgra.data()));
}

std::vector<uint8_t> Downscale(const uint8_t* src, int w, int h, int w2, int h2) {
  std::vector<uint8_t> out((size_t)w2 * h2 * 4);
  if (w2 == w && h2 == h) { std::memcpy(out.data(), src, out.size()); return out; }
  std::vector<int> xs(w2 + 1);
  for (int x = 0; x <= w2; ++x) xs[x] = (int)((int64_t)x * w / w2);
  std::vector<uint32_t> acc((size_t)w2 * 4);
  for (int y = 0; y < h2; ++y) {
    int y0 = (int)((int64_t)y * h / h2), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / h2));
    std::fill(acc.begin(), acc.end(), 0u);
    for (int sy = y0; sy < y1; ++sy) {
      const uint8_t* row = src + (size_t)sy * w * 4;
      for (int x = 0; x < w2; ++x) {
        int a = xs[x], b = std::max(a + 1, xs[x + 1]);
        uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int sx = a; sx < b; ++sx) { const uint8_t* p = row + (size_t)sx * 4; s0 += p[0]; s1 += p[1]; s2 += p[2]; s3 += p[3]; }
        acc[x * 4] += s0; acc[x * 4 + 1] += s1; acc[x * 4 + 2] += s2; acc[x * 4 + 3] += s3;
      }
    }
    for (int x = 0; x < w2; ++x) {
      uint32_t n = (uint32_t)(y1 - y0) * (uint32_t)std::max(1, xs[x + 1] - xs[x]);
      if (xs[x + 1] <= xs[x]) n = (uint32_t)(y1 - y0);
      for (int c = 0; c < 4; ++c) out[((size_t)y * w2 + x) * 4 + c] = (uint8_t)((acc[x * 4 + c] + n / 2) / n);
    }
  }
  return out;
}

} // namespace filo::pdf::wic
