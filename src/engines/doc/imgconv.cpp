// WIC helpers: convert arbitrary decodable images to PNG; encode BGRA bitmaps to PNG / JPEG.
#include "engines/doc/docx.h"
#include "engines/doc/imgenc.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <shlwapi.h>

namespace filo::doc {

using Microsoft::WRL::ComPtr;

static ComPtr<IWICImagingFactory> Factory() {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
  return f;
}

namespace {
struct MemStream {
  ComPtr<IStream> s;
  MemStream() { s.Attach(SHCreateMemStream(nullptr, 0)); }
  bool Bytes(std::vector<uint8_t>& out) {
    STATSTG st{};
    if (!s || FAILED(s->Stat(&st, STATFLAG_NONAME))) return false;
    LARGE_INTEGER z{}; s->Seek(z, STREAM_SEEK_SET, nullptr);
    out.resize((size_t)st.cbSize.QuadPart);
    ULONG got = 0;
    if (!out.empty() && FAILED(s->Read(out.data(), (ULONG)out.size(), &got))) return false;
    out.resize(got);
    return true;
  }
};
}

bool ConvertImageToPng(const Bytes& in, Bytes& outPng, int* w, int* h) {
  ComScope com;
  auto f = Factory();
  if (!f || in.empty()) return false;
  ComPtr<IStream> src; src.Attach(SHCreateMemStream(in.data(), (UINT)in.size()));
  if (!src) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromStream(src.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return false;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  UINT cw = 0, ch = 0; conv->GetSize(&cw, &ch);
  if (w) *w = (int)cw;
  if (h) *h = (int)ch;
  MemStream ms;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(ms.s.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> fe; ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&fe, &props)) || FAILED(fe->Initialize(props.Get()))) return false;
  fe->SetSize(cw, ch);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  fe->SetPixelFormat(&fmt);
  if (FAILED(fe->WriteSource(conv.Get(), nullptr))) return false;
  if (FAILED(fe->Commit()) || FAILED(enc->Commit())) return false;
  return ms.Bytes(outPng);
}

bool EncodeBgra(const uint8_t* bgra, int width, int height, int stride, ImgFormat fmt, int jpegQuality, Bytes& out) {
  ComScope com;
  auto f = Factory();
  if (!f || width <= 0 || height <= 0) return false;
  MemStream ms;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(fmt == ImgFormat::Png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(ms.s.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> fe; ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&fe, &props))) return false;
  if (fmt == ImgFormat::Jpeg) {
    PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = (float)jpegQuality / 100.0f;
    props->Write(1, &opt, &v);
  }
  if (FAILED(fe->Initialize(props.Get())) || FAILED(fe->SetSize(width, height))) return false;
  WICPixelFormatGUID pf = fmt == ImgFormat::Png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
  fe->SetPixelFormat(&pf);
  if (fmt == ImgFormat::Jpeg) {
    std::vector<uint8_t> row((size_t)width * 3);
    for (int y = 0; y < height; ++y) {
      const uint8_t* s = bgra + (size_t)y * stride;
      for (int x = 0; x < width; ++x) { row[x * 3] = s[x * 4]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2]; }
      if (FAILED(fe->WritePixels(1, (UINT)row.size(), (UINT)row.size(), row.data()))) return false;
    }
  } else {
    if (FAILED(fe->WritePixels(height, stride, (UINT)((size_t)stride * height), const_cast<uint8_t*>(bgra)))) return false;
  }
  if (FAILED(fe->Commit()) || FAILED(enc->Commit())) return false;
  return ms.Bytes(out);
}

bool DecodeToBgra(const Bytes& in, int& w, int& h, Bytes& bgra) {
  ComScope com;
  auto f = Factory();
  if (!f || in.empty()) return false;
  ComPtr<IStream> src; src.Attach(SHCreateMemStream(in.data(), (UINT)in.size()));
  ComPtr<IWICBitmapDecoder> dec;
  if (!src || FAILED(f->CreateDecoderFromStream(src.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return false;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  UINT cw = 0, ch = 0; conv->GetSize(&cw, &ch);
  if (cw == 0 || ch == 0 || (uint64_t)cw * ch > 200000000ull) return false;
  w = (int)cw; h = (int)ch;
  bgra.resize((size_t)cw * ch * 4);
  return SUCCEEDED(conv->CopyPixels(nullptr, cw * 4, (UINT)bgra.size(), bgra.data()));
}

} // namespace filo::doc
