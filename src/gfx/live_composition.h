#pragma once
#include <windows.h>
#include <dxgi1_3.h>
#include <memory>
#include "gfx/liquid_mask.h"

namespace filo::gfx {
// A compositor-owned host backdrop. No screen capture, readback or refresh timer.
class LiveComposition {
 public:
  LiveComposition();
  ~LiveComposition();
  bool Create(HWND window, IDXGISwapChain1* swap, int width, int height);
  bool Configure(const RECT& bounds, float radius, bool dark, float intensity, const LiquidShape* shape = nullptr);
  bool CloseBubble(const RECT& bounds, float dpiScale, bool enabled);
  void Resize(int width, int height);
  void Disable();
  void SetOpacity(float opacity);
  unsigned MaskUpdates() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
