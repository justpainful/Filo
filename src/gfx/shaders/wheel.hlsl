// Filo radial wheel: exact SDF geometry + custom liquid-glass shading.
// The geometry mirrors src/wheel/geometry.cpp (CPU hit testing) line for line.
#include "common.hlsli"

#define MAX_SECTORS 16

Texture2D    backSharp : register(t0);   // desktop snapshot (cropped around the wheel), BGRA
Texture2D    backBlur  : register(t1);   // blurred copy
SamplerState lin       : register(s0);

struct SectorState {
  float hover;     // 0..1
  float press;     // 0..1
  float scale;     // geometric scale about the sector centroid
  float alpha;     // per-sector appear alpha (stagger)
};

cbuffer WheelCB : register(b0) {
  float2 centerPx;        // wheel centre in window pixels
  float  Rpx;             // rim radius in pixels
  float  n;               // sector count

  float  gapRad;
  float  corner;          // fraction of R
  float  sectorOuter;
  float  sectorInner;

  float  holeRadius;
  float  appear;          // 0..1 overall scale/alpha progress
  float  glassMix;        // 0 = flat fallback, 1 = full glass
  float  hasBackdrop;

  float4 fillColor;       // straight rgba
  float4 hoverColor;
  float4 rimColor;
  float4 holeColor;
  float4 shadowParams;    // x blur px, y offset px, z alpha, w appearScaleFrom
  float4 glassParams;     // x refract px, y bevel px, z specular, w fresnel power
  float4 backRect;        // x,y = backdrop texture origin in window px, z,w = backdrop size in px
  float4 lightDir;        // xy = unit vector toward the light (screen space, y down), z = saturation boost, w = frost
  float4 fillGrad;        // x = sector fill alpha at the outer edge, y = at the inner edge
  float4 optics;          // material: 0 Soft, 1 Liquid, 2 Acrylic; reflections, refraction, DPI
  float4 acrylic;          // x: independent Acrylic transparency
  SectorState sec[MAX_SECTORS];
};

VSOut VSMain(uint id : SV_VertexID) { return FullscreenVS(id); }

// ---- geometry --------------------------------------------------------------------------------------------------------
float SectorSdf(int k, float2 p, float scale, float pitch, float halfW, float R) {
  float ang = pitch * k;
  float rm = (sectorInner + sectorOuter) * 0.5 * R;
  if (scale != 1.0) {
    float2 c = float2(sin(ang), -cos(ang)) * rm;
    p = c + (p - c) / scale;
  }
  float r = length(p);
  float rc = corner * R;
  float dOut = r - sectorOuter * R, dIn = sectorInner * R - r;
  float dEdge = -1e9;
  if (n > 1.5) {
    float phi = atan2(p.x, -p.y) - ang;
    phi = phi - 2 * PI * floor((phi + PI) / (2 * PI));   // wrap to [-PI, PI)
    float a = r * cos(phi), l = r * sin(phi);
    dEdge = abs(l) * cos(halfW) - a * sin(halfW);
  }
  float3 q = float3(dOut, dIn, dEdge) + rc;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - rc;
}

int NearestSector(float2 p, float pitch) {
  if (n < 1.5) return 0;
  float phi = atan2(p.x, -p.y);
  if (phi < 0) phi += 2 * PI;
  return ((int)floor(phi / pitch + 0.5)) % (int)n;
}

// ---- glass ----------------------------------------------------------------------------------------------------------
float3 SampleBackdrop(float2 winPx, float2 offsetPx, float frost) {
  if (hasBackdrop < 0.5) {
    // Designed fallback: soft vertical gradient so the wheel never shows black/empty glass.
    float t = saturate(winPx.y / max(backRect.w, 1.0));
    return lerp(float3(0.93, 0.94, 0.97), float3(0.80, 0.83, 0.90), t);
  }
  float2 uv = (winPx + offsetPx - backRect.xy) / backRect.zw;
  float3 s = backSharp.SampleLevel(lin, uv, 0).rgb;
  float3 b = backBlur.SampleLevel(lin, uv, 0).rgb;
  return lerp(s, b, frost);
}

// Returns premultiplied RGBA for one glass body.
//   depthPx: distance in px from the edge toward the inside (>=0 inside)
//   nrm: outward 2D normal of the body edge
float4 ShadeGlass(float2 winPx, float depthPx, float2 nrm, float4 tint, float hov, float press, float R, float zone) {
  float bevel = max(glassParams.y, 1.0);
  float edge = saturate(depthPx / bevel);            // 0 at the rim, 1 in the flat interior
  float bend = (1.0 - edge); bend *= bend;           // lens-like curvature profile
  float2 refractOff = -nrm * bend * glassParams.x * glassMix;

  if (hasBackdrop < -0.5) {
    if (optics.x > 1.5) {
      // Acrylic is a flat frosted volume: fine stable grain, no lens or rolled lip.
      float alpha=1-saturate(acrylic.x);
      if(zone>1.5)alpha=1-saturate(acrylic.x)*.90;
      if(zone<.5)alpha=1-saturate(acrylic.x)*.85;
      alpha=lerp(1,alpha,glassMix);alpha=lerp(alpha,.97,hov);
      float3 colour=fillGrad.z>.5?float3(.082,.098,.133):float3(.969,.973,.980);
      colour=lerp(colour,hoverColor.rgb*.80,hov);
      float grain=frac(sin(dot(floor(winPx),float2(12.9898,78.233)))*43758.5453)-.5;
      colour=saturate(colour+grain*.012*glassMix*(1-hov));
      colour*=1-press*.065;
      return float4(colour*alpha,alpha);
    }
    if (optics.x > .5) {
      float dpi=max(optics.w,.5);
      float depth=max(depthPx,0);
      float normalLight=dot(nrm,lightDir.xy);
      float lit=saturate(normalLight),far=saturate(-normalLight);
      float bodyAlpha=zone<.5?.26:(zone>1.5?.64:.75);
      if(fillGrad.z>.5)bodyAlpha=zone<.5?.34:(zone>1.5?.76:.84);
      // A clearer rolled lip gives way to the milkier reading surface.
      float a=lerp(bodyAlpha*.54,bodyAlpha,smoothstep(0,6*dpi,depth));
      a=lerp(1,a,glassMix);
      a=lerp(a,.97,hov);
      float3 tintColour=fillGrad.z>.5?float3(.075,.085,.105):float3(1,1,1);
      tintColour=lerp(tintColour,hoverColor.rgb*.80,hov);
      float lip=exp(-pow((depth-.65*dpi)/(.72*dpi),2));
      float shoulder=exp(-depth/(3.5*dpi));
      float spec=(lip*(.075+.55*pow(lit,3)+.18*pow(far,5))+shoulder*.10*lit)*optics.y*glassMix*(1+.28*hov);
      float shade=exp(-pow((depth-2.5*dpi)/(1.5*dpi),2))*(.025+.14*far)*optics.y*glassMix;
      tintColour*=1-shade;
      // Broad, quiet surface reflection; fixed illumination, no cursor spotlight.
      float2 pos=(winPx-centerPx)/max(R,1);
      float sheen=exp(-pow((pos.y+.62)/.72,2))*.035*optics.y*glassMix;
      a+= (1-a)*sheen;
      float4 base=float4(tintColour*a,a);
      float reflection=saturate(spec);
      float4 result=float4(reflection,reflection,reflection,reflection)+base*(1-reflection);
      result.rgb*=1-press*.065;
      return result;
    }
    // The OS supplies the animated desktop below this transparent swap chain.
    // Draw just sector colour, highlights and interaction above it.
    float a = lerp(tint.a, 0.96, hov);
    float3 colour = lerp(tint.rgb, hoverColor.rgb * 0.84, hov);
    float fresnel = pow(1.0 - edge, glassParams.w);
    colour = saturate(colour + fresnel * glassParams.z * glassMix);
    colour *= 1.0 - press * 0.08;
    return float4(colour * a, a);
  }

  float3 bg = SampleBackdrop(winPx, refractOff, lightDir.w * glassMix);
  bg = SaturateColor(bg, lerp(1.0, lightDir.z, glassMix));

  // Tint over the backdrop. glassMix=0 turns the tint into a flat opaque-ish surface.
  float3 col = lerp(bg, tint.rgb, tint.a);

  // Keep the blue hover legible and make the transparency slider affect the
  // material itself. Small bright desktop details are fully frosted.
  if (hov > 0) col = lerp(col, hoverColor.rgb, hov * 0.88);
  float3 solid = fillGrad.z > 0.5 ? float3(0.15, 0.16, 0.19) : float3(0.95, 0.96, 0.98);
  if (hov < 0.01) {
    if (fillGrad.z > 0.5) {
      col = lerp(col, solid, 0.52);
      col *= min(1.0, 0.29 / max(max(col.r, col.g), max(col.b, 0.001)));
    } else {
      float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
      float floorBrightness = tint.a < 0.39 ? 0.73 : 0.80;
      col = lerp(col, float3(1, 1, 1), saturate((floorBrightness - lum) / max(1.0 - lum, 0.001)));
    }
  }
  col = lerp(lerp(solid, hoverColor.rgb, hov), col, glassMix);

  // Soft top-to-bottom sheen and edge lighting (fresnel-like).
  float sheen = 0.5 + 0.5 * dot(float2(0, -1), nrm * 0.0 + float2(0, -1));
  float fres = pow(1.0 - edge, glassParams.w);
  float lit = saturate(0.5 + 0.5 * dot(nrm, lightDir.xy));          // 1 on the lit side, 0 on the far side
  float spec = fres * (0.30 + 0.70 * lit) * glassParams.z * glassMix;
  float3 rimLight = lerp(float3(1, 1, 1), tint.rgb * 0.6 + 0.4, 0.0);
  col += rimLight * spec;
  // dark inner edge on the shadow side gives the lens depth
  col *= 1.0 - 0.025 * fres * (1.0 - lit) * glassMix;
  // gentle internal gloss
  col += 0.010 * glassMix * (1.0 - edge) * lit;

  col = lerp(col, col * 0.88, press * 0.6);
  return float4(saturate(col), 1.0);
}

float4 Over(float4 top, float4 bottom) {      // both premultiplied
  return top + bottom * (1.0 - top.a);
}

float4 PSMain(VSOut i) : SV_Target {
  float2 winPx = i.pos.xy;
  float2 p0 = winPx - centerPx;

  // Overall appear: scale about the wheel centre.
  float s = lerp(shadowParams.w, 1.0, appear);
  float2 p = p0 / s;
  float R = Rpx;
  float aa = 1.0 / s;                         // keep anti-aliasing 1 screen pixel wide while scaling

  float pitch = 2 * PI / max(n, 1.0);
  float halfW = (n < 1.5) ? PI : (pitch * 0.5 - gapRad * 0.5);
  float r = length(p);

  float4 outc = 0;

  // ---- drop shadow of the whole wheel (soft, offset down)
  {
    float2 ps = p - float2(0, shadowParams.y);
    float d = length(ps) - R;
    float sh = shadowParams.z * (1.0 - smoothstep(-shadowParams.x * 0.35, shadowParams.x, d));
    if (hasBackdrop < -0.5) sh *= smoothstep(-aa, aa, length(p) - R);
    outc = float4(0, 0, 0, sh * appear);
  }

  // ---- rim (outer circular surface)
  float dRim = r - R;
  float rimCov = saturate(0.5 - dRim / aa);
  if (rimCov > 0.0) {
    float2 nrm = r > 0.001 ? p / r : float2(0, -1);
    float depth = -dRim;
    float4 g = ShadeGlass(winPx, depth, nrm, rimColor, 0, 0, R, 0);
    // carve the inner hole
    float dHole = r - holeRadius * R;
    float holeCov = saturate(0.5 - dHole / aa);
    float rimBody=optics.x>.5?rimCov*saturate(.5+(r-(R-7*optics.w))/aa):rimCov;
    float4 rimPx = g * rimBody;
    outc = Over(rimPx, outc);
    if (holeCov > 0.0) {
      float2 hn = -nrm;
      float4 hg = ShadeGlass(winPx, -dHole, hn, holeColor, 0, 0, R, 2);
      float4 holePx = hg * holeCov;
      outc = Over(holePx, outc);
    }
  }

  // ---- sector. A hovered (scaled) sector may extend past its angular slice, so hovered sectors are tested first.
  int k = NearestSector(p, pitch);
  float sd = SectorSdf(k, p, sec[k].scale, pitch, halfW, R);
  [loop] for (int j = 0; j < (int)n && j < MAX_SECTORS; ++j) {
    if (j != k && sec[j].scale > 1.001) {
      float s2 = SectorSdf(j, p, sec[j].scale, pitch, halfW, R);
      if (s2 < 0.5) { k = j; sd = s2; break; }
    }
  }
  float hov = sec[k].hover, prs = sec[k].press;
  float cov = saturate(0.5 - sd / aa);
  if (cov > 0.0) {
    float2 g2 = float2(ddx(sd), ddy(sd));
    float gl = length(g2);
    float2 nrm = gl > 1e-5 ? g2 / gl : float2(0, -1);
    float tr = saturate((sectorOuter * R - length(p)) / ((sectorOuter - sectorInner) * R));      // 0 outer .. 1 inner
    float4 base = fillColor;
    if (fillGrad.x > 0) base.a = lerp(fillGrad.x, fillGrad.y, tr);
    float4 tint = lerp(base, hoverColor, hov);
    float4 g = ShadeGlass(winPx, -sd, nrm, tint, hov, prs, R, 1);
    float a = cov * sec[k].alpha;
    // soft contact shadow of the sector on the rim
    outc = Over(g * a, outc);
  } else if (sd < 14.0) {
    // faint ambient occlusion around sectors, drawn only over the rim ring
    float ao = (optics.x>.5?.055:0.10) * (1.0 - smoothstep(0.0, 10.0, sd)) * sec[k].alpha;
    outc = Over(float4(0, 0, 0, ao * rimCov), outc);
  }

  outc *= appear;
  return outc;
}
