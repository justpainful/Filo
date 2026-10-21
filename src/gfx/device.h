// Shared GPU device stack: D3D11 + DXGI + Direct2D + DirectWrite + DirectComposition + WIC.
// One device for the whole process; recreated transparently on device loss (listeners are notified).
#pragma once
#include "core/com.h"
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <functional>
#include <vector>

namespace filo::gfx {

class Device {
 public:
  static Device& Get();

  bool Ensure();                       // creates everything on first use; false => no GPU at all (even WARP failed)
  bool IsWarp() const { return warp_; }
  void Release();                      // drop all GPU objects (device loss / shutdown)
  // Called by surfaces when Present returns DXGI_ERROR_DEVICE_REMOVED/RESET.
  void HandleDeviceLost(HRESULT why);
  // cb(false) = device is about to be destroyed (drop resources); cb(true) = new device is ready (rebuild).
  int AddLostListener(std::function<void(bool rebuilt)> cb);
  void RemoveLostListener(int id);

  ComPtr<ID3D11Device> d3d;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<ID2D1Factory1> d2dFactory;
  ComPtr<ID2D1Device> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IWICImagingFactory> wic;

 private:
  bool Create(bool warp);
  bool warp_ = false;
  bool ok_ = false;
  int nextId_ = 1;
  std::vector<std::pair<int, std::function<void(bool)>>> listeners_;
};

// Releases every process-wide GPU object. Call once before CoUninitialize / process exit.
void Shutdown();

} // namespace filo::gfx
