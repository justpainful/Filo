#include "common.hlsli"

Texture2D src : register(t0);
SamplerState lin : register(s0);

cbuffer BlurCB : register(b0) {
  float2 texel;       // 1 / source size
  float  sigma;       // in source texels
  float  pad0;
};

float4 PSBlur(VSOut i, float2 dir) {
  // 13-tap separable gaussian using bilinear taps.
  float4 acc = 0;
  float wsum = 0;
  [unroll]
  for (int k = -6; k <= 6; ++k) {
    float w = exp(-0.5 * (k * k) / max(sigma * sigma, 0.0001));
    acc += src.SampleLevel(lin, i.uv + dir * texel * (float)k * (sigma * 0.5 + 0.5), 0) * w;
    wsum += w;
  }
  return acc / wsum;
}

float4 PSBlurH(VSOut i) : SV_Target { return PSBlur(i, float2(1, 0)); }
float4 PSBlurV(VSOut i) : SV_Target { return PSBlur(i, float2(0, 1)); }
VSOut VSMain(uint id : SV_VertexID) { return FullscreenVS(id); }
