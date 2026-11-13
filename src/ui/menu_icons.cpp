#include "ui/menu_icons.h"
#include "design/theme.h"
#include "gfx/device.h"
#include "gfx/icons.h"
#include <cstring>

namespace filo::ui {

HBITMAP MenuIconBitmap(const std::string& icon, int px, COLORREF inkColor) {
  auto& dev = gfx::Device::Get();
  if (!dev.Ensure() || px <= 0) return nullptr;
  ComPtr<ID2D1DeviceContext> dc;
  if (FAILED(dev.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return nullptr;
  dc->SetDpi(96.f, 96.f);
  const D2D1_PIXEL_FORMAT fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  ComPtr<ID2D1Bitmap1> target, readback;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt), &target))) return nullptr;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(px, px), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ, fmt), &readback))) return nullptr;

  const COLORREF ink = inkColor == CLR_INVALID ? GetSysColor(COLOR_MENUTEXT) : inkColor;
  ComPtr<ID2D1SolidColorBrush> brush;
  dc->SetTarget(target.Get());
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0, 0, 0));
  bool drawn = false;
  if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(GetRValue(ink) / 255.f, GetGValue(ink) / 255.f, GetBValue(ink) / 255.f), &brush)))
    drawn = gfx::IconCache::Get().DrawPack(dc.Get(), ds::Theme::Get().State().iconPack, icon, px / 2.f, px / 2.f, px * 0.86f, brush.Get(), 1.7f);
  const HRESULT end = dc->EndDraw();
  dc->SetTarget(nullptr);
  if (!drawn || FAILED(end)) return nullptr;
  if (FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr))) return nullptr;

  D2D1_MAPPED_RECT m{};
  if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &m))) return nullptr;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = px; bi.bmiHeader.biHeight = -px;   // top-down
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp && bits) for (int y = 0; y < px; ++y) memcpy((uint8_t*)bits + (size_t)y * px * 4, m.bits + (size_t)y * m.pitch, (size_t)px * 4);
  readback->Unmap();
  return bmp;
}

}  // namespace filo::ui
