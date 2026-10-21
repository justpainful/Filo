#include "gfx/imageio.h"
#include "gfx/device.h"
#include "core/util.h"
#include "core/log.h"
#include <algorithm>

namespace filo::gfx {

bool SavePng(const std::filesystem::path& path, const uint8_t* bgra, int w, int h, bool premultiplied) {
  auto& dev = Device::Get();
  if (!dev.Ensure()) return false;
  ComPtr<IWICStream> stream;
  if (FAILED(dev.wic->CreateStream(&stream))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(dev.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&fr, &props))) return false;
  fr->Initialize(props.Get());
  fr->SetSize(w, h);
  WICPixelFormatGUID fmt = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
  fr->SetPixelFormat(&fmt);
  ComPtr<IWICBitmap> bmp;
  WICPixelFormatGUID srcFmt = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
  if (FAILED(dev.wic->CreateBitmapFromMemory(w, h, srcFmt, w * 4, (UINT)(w * 4 * h), const_cast<BYTE*>(bgra), &bmp))) return false;
  ComPtr<IWICFormatConverter> conv;
  dev.wic->CreateFormatConverter(&conv);
  // PNG stores straight alpha; WIC converts from premultiplied for us.
  WICPixelFormatGUID dst = GUID_WICPixelFormat32bppBGRA;
  fr->SetPixelFormat(&dst);
  if (FAILED(conv->Initialize(bmp.Get(), dst, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
  if (FAILED(fr->WriteSource(conv.Get(), nullptr))) return false;
  if (FAILED(fr->Commit())) return false;
  return SUCCEEDED(enc->Commit());
}

bool LoadBgra(const std::filesystem::path& path, std::vector<uint8_t>& out, int& w, int& h) {
  auto& dev = Device::Get();
  if (!dev.Ensure()) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(dev.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, &fr))) return false;
  ComPtr<IWICFormatConverter> conv;
  dev.wic->CreateFormatConverter(&conv);
  if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
  UINT uw, uh; conv->GetSize(&uw, &uh);
  w = (int)uw; h = (int)uh;
  out.resize((size_t)w * h * 4);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.size(), out.data()));
}

bool BackdropFromImage(const std::filesystem::path& path, const RECT& rect, float sigma, BackdropImage& out) {
  std::vector<uint8_t> img; int iw = 0, ih = 0;
  if (!LoadBgra(path, img, iw, ih)) return false;
  RECT rc{std::max<LONG>(rect.left, 0), std::max<LONG>(rect.top, 0), std::min<LONG>(rect.right, iw), std::min<LONG>(rect.bottom, ih)};
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;
  out.screenRect = rect; out.w = (int)(rect.right - rect.left); out.h = (int)(rect.bottom - rect.top);
  out.sharp.assign((size_t)out.w * out.h, 0xFF000000u);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const uint8_t* p = &img[((size_t)(rc.top + y) * iw + (rc.left + x)) * 4];
    out.sharp[(size_t)(y + (rc.top - rect.top)) * out.w + (x + (rc.left - rect.left))] = 0xFF000000u | p[0] | (p[1] << 8) | (p[2] << 16);
  }
  const int f = 4;
  out.bw = std::max(1, out.w / f); out.bh = std::max(1, out.h / f);
  out.blurred.assign((size_t)out.bw * out.bh, 0xFF000000u);
  for (int y = 0; y < out.bh; ++y) for (int x = 0; x < out.bw; ++x) {
    uint32_t acc[3] = {0, 0, 0}; int cnt = 0;
    for (int dy = 0; dy < f; ++dy) for (int dx = 0; dx < f; ++dx) {
      int sx = std::min(x * f + dx, out.w - 1), sy = std::min(y * f + dy, out.h - 1);
      uint32_t c = out.sharp[(size_t)sy * out.w + sx];
      acc[0] += c & 255; acc[1] += (c >> 8) & 255; acc[2] += (c >> 16) & 255; ++cnt;
    }
    out.blurred[(size_t)y * out.bw + x] = 0xFF000000u | (acc[0] / cnt) | ((acc[1] / cnt) << 8) | ((acc[2] / cnt) << 16);
  }
  BlurBgra(out.blurred, out.bw, out.bh, sigma / f);
  return true;
}

bool CaptureScreenToPng(const RECT& r, const std::filesystem::path& path) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return false;
  HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
  BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    ok = BitBlt(mem, 0, 0, w, h, scr, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
    SelectObject(mem, old);
    if (ok) {
      uint8_t* px = (uint8_t*)bits;
      for (size_t i = 3; i < (size_t)w * h * 4; i += 4) px[i] = 255;
      ok = SavePng(path, px, w, h, false);
    }
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem); ReleaseDC(nullptr, scr);
  return ok;
}

} // namespace filo::gfx
