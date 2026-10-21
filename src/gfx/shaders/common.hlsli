// Shared HLSL helpers for Filo's custom Liquid Glass renderer.
#ifndef FILO_COMMON_HLSLI
#define FILO_COMMON_HLSLI

static const float PI = 3.14159265358979;

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;      // 0..1 across the window
};

// Full-screen triangle, no vertex buffer.
VSOut FullscreenVS(uint id) {
  VSOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = t;
  return o;
}

float3 SaturateColor(float3 c, float s) {
  float l = dot(c, float3(0.2126, 0.7152, 0.0722));
  return lerp(l.xxx, c, s);
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// sRGB <-> linear (we blend in gamma space like the reference's macOS compositor, so these are only for specular maths)
float3 ToLinear(float3 c) { return pow(max(c, 0.0), 2.2); }
float3 ToGamma(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

#endif
