#include "gfx/device.h"
#include "core/log.h"
#include "core/util.h"
#include <algorithm>

namespace filo::gfx {

// Intentionally leaked: GPU/COM objects must be released explicitly (gfx::Shutdown) while COM is still initialised.
Device& Device::Get() { static Device* d = new Device(); return *d; }

bool Device::Ensure() {
  if (ok_) return true;
  if (Create(false)) return true;
  FILO_LOG(Warn, "gfx", "hardware D3D11 device failed; falling back to WARP");
  return Create(true);
}

bool Device::Create(bool warp) {
  Release();
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, (UINT)std::size(levels),
                                 D3D11_SDK_VERSION, &d3d, &got, &ctx);
#ifdef _DEBUG
  if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {   // debug layer not installed
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, (UINT)std::size(levels),
                           D3D11_SDK_VERSION, &d3d, &got, &ctx);
  }
#endif
  if (FAILED(hr)) { FILO_LOG(Error, "gfx", "D3D11CreateDevice failed: %ls", HResultMessage(hr).c_str()); return false; }
  if (FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), dxgi.AsVoid()))) return false;
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi->GetAdapter(&adapter))) return false;
  if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.AsVoid()))) return false;

  D2D1_FACTORY_OPTIONS fo{};
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, d2dFactory.AsVoid());
  if (FAILED(hr)) return false;
  if (FAILED(d2dFactory->CreateDevice(dxgi.Get(), &d2d))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.AsVoid())))) return false;
  if (FAILED(DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice), dcomp.AsVoid()))) return false;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.AsVoid()))) {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.AsVoid()))) return false;
  }
  warp_ = warp;
  ok_ = true;
  FILO_LOG(Info, "gfx", "device ready (%s, feature level 0x%x)", warp ? "WARP" : "hardware", (unsigned)got);
  return true;
}

void Device::Release() {
  ok_ = false;
  wic.Reset(); dcomp.Reset(); dwrite.Reset(); d2d.Reset(); d2dFactory.Reset(); factory.Reset(); dxgi.Reset();
  if (ctx) ctx->ClearState();
  ctx.Reset(); d3d.Reset();
}

void Device::HandleDeviceLost(HRESULT why) {
  FILO_LOG(Warn, "gfx", "device lost: %ls", HResultMessage(why).c_str());
  auto copy = listeners_;
  for (auto& l : copy) l.second(false);      // listeners drop their resources
  Release();
  Ensure();
  for (auto& l : copy) l.second(true);       // listeners rebuild
}

int Device::AddLostListener(std::function<void(bool)> cb) { listeners_.emplace_back(nextId_, std::move(cb)); return nextId_++; }
void Device::RemoveLostListener(int id) {
  listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](auto& p) { return p.first == id; }), listeners_.end());
}

} // namespace filo::gfx
