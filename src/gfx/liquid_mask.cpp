#include "gfx/liquid_mask.h"
#include "liquid_mask_vs.h"
#include "liquid_mask_ps.h"
#include <cstring>

namespace filo::gfx {
bool LiquidMasks::Create(int w,int h) {
  auto& d=Device::Get();
  if(!vs_) {
    if(FAILED(d.d3d->CreateVertexShader(g_liquid_mask_vs,sizeof g_liquid_mask_vs,nullptr,&vs_))) return false;
    if(FAILED(d.d3d->CreatePixelShader(g_liquid_mask_ps,sizeof g_liquid_mask_ps,nullptr,&ps_))) return false;
    D3D11_BUFFER_DESC cb{sizeof(LiquidShape),D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    if(FAILED(d.d3d->CreateBuffer(&cb,nullptr,&cb_))) return false;
    D3D11_BLEND_DESC bl{};bl.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(FAILED(d.d3d->CreateBlendState(&bl,&blend_))) return false;
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
    if(FAILED(d.d3d->CreateRasterizerState(&rs,&raster_))) return false;
  }
  for(int k=0;k<5;++k) {
    targets_[k].Reset(); swaps_[k].Reset();
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.Scaling=DXGI_SCALING_STRETCH;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
    if(FAILED(d.factory->CreateSwapChainForComposition(d.d3d.Get(),&desc,nullptr,&swaps_[k]))) return false;
    ComPtr<ID3D11Texture2D> bb;
    if(FAILED(swaps_[k]->GetBuffer(0,__uuidof(ID3D11Texture2D),bb.AsVoid()))) return false;
    if(FAILED(d.d3d->CreateRenderTargetView(bb.Get(),nullptr,&targets_[k]))) return false;
  }
  width_=w;height_=h;valid_=false;return true;
}
bool LiquidMasks::Update(int w,int h,const LiquidShape& shape) {
  if(w<1||h<1) return false;
  if(width_!=w||height_!=h) if(!Create(w,h)) return false;
  if(valid_&&std::memcmp(&previous_,&shape,sizeof shape)==0) return true;
  auto& d=Device::Get();
  ComPtr<ID3D11RenderTargetView> old;ComPtr<ID3D11DepthStencilView> depth;
  d.ctx->OMGetRenderTargets(1,&old,&depth);
  D3D11_VIEWPORT saved{};UINT n=1;d.ctx->RSGetViewports(&n,&saved);
  D3D11_VIEWPORT vp{0,0,(float)w,(float)h,0,1};d.ctx->RSSetViewports(1,&vp);
  d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d.ctx->VSSetShader(vs_.Get(),nullptr,0);d.ctx->PSSetShader(ps_.Get(),nullptr,0);
  ID3D11Buffer* cb=cb_.Get();d.ctx->PSSetConstantBuffers(0,1,&cb);
  d.ctx->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);d.ctx->RSSetState(raster_.Get());
  bool ok=true;
  for(int k=0;k<5;++k) {
    LiquidShape data=shape;data.maskIndex=(float)k;
    D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(d.ctx->Map(cb_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))) {ok=false;break;}
    std::memcpy(map.pData,&data,sizeof data);d.ctx->Unmap(cb_.Get(),0);
    auto* target=targets_[k].Get();d.ctx->OMSetRenderTargets(1,&target,nullptr);d.ctx->Draw(3,0);
    d.ctx->OMSetRenderTargets(0,nullptr,nullptr);
    if(FAILED(swaps_[k]->Present(0,0))) {ok=false;break;}
  }
  ID3D11RenderTargetView* rt=old.Get();d.ctx->OMSetRenderTargets(1,&rt,depth.Get());
  if(n) d.ctx->RSSetViewports(1,&saved);
  if(ok) {previous_=shape;valid_=true;++updates_;}
  return ok;
}
}
