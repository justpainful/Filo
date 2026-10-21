#pragma once
#include "gfx/device.h"
#include <array>

namespace filo::gfx {
// Physical-pixel optical geometry. Masks contain geometry only, never desktop pixels.
struct alignas(16) LiquidShape {
  float cx=0, cy=0, halfW=0, halfH=0;
  float radius=0, count=0, gap=0, corner=0;
  float outer=.955f, inner=.389f, hole=.362f, appearScale=1;
  float dpiScale=1, maskIndex=0, refract=1, refractionPx=2.4f;
  float scales[16]{};
};
static_assert(sizeof(LiquidShape)==128);

class LiquidMasks {
 public:
  bool Update(int width, int height, const LiquidShape& shape);
  IDXGISwapChain1* Swap(int i) const { return swaps_[i].Get(); }
  unsigned Updates() const { return updates_; }
 private:
  bool Create(int width, int height);
  int width_=0, height_=0;
  bool valid_=false;
  unsigned updates_=0;
  LiquidShape previous_{};
  std::array<ComPtr<IDXGISwapChain1>,5> swaps_;
  std::array<ComPtr<ID3D11RenderTargetView>,5> targets_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11RasterizerState> raster_;
};
}
