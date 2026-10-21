// SVG rasteriser on top of Direct2D's built-in SVG support (ID2D1DeviceContext5 / ID2D1SvgDocument, Windows 10 1703+).
// No network access: the document is parsed from memory and external references are never resolved.
#include "engines/image/image_internal.h"
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>

namespace filo::img {

// ---- tiny root-element scanner ---------------------------------------------------------------------------------------
static bool ParseLength(const std::string& s0, double& out) {
  std::string s = s0;
  size_t b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return false;
  s = s.substr(b);
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  std::string unit = end; while (!unit.empty() && isspace((unsigned char)unit.back())) unit.pop_back();
  for (auto& c : unit) c = (char)tolower((unsigned char)c);
  double k = 1;
  if (unit.empty() || unit == "px") k = 1;
  else if (unit == "pt") k = 4.0 / 3.0; else if (unit == "pc") k = 16; else if (unit == "mm") k = 96 / 25.4;
  else if (unit == "cm") k = 96 / 2.54; else if (unit == "in") k = 96; else if (unit == "em") k = 16; else if (unit == "ex") k = 8;
  else return false;                                  // % and unknown units: no intrinsic size
  out = v * k;
  return out > 0;
}

static std::string RootTag(const Bytes& svg) {
  std::string s((const char*)svg.data(), std::min<size_t>(svg.size(), 16384));
  size_t pos = 0;
  while (true) {
    size_t lt = s.find('<', pos);
    if (lt == std::string::npos) return "";
    if (s.compare(lt, 4, "<!--") == 0) { size_t e = s.find("-->", lt); if (e == std::string::npos) return ""; pos = e + 3; continue; }
    if (s.compare(lt, 2, "<?") == 0 || s.compare(lt, 2, "<!") == 0) {
      if (s.compare(lt, 9, "<!DOCTYPE") == 0) {
        size_t br = s.find('[', lt), gt = s.find('>', lt);
        if (br != std::string::npos && gt != std::string::npos && br < gt) { size_t e = s.find("]>", lt); if (e == std::string::npos) return ""; pos = e + 2; continue; }
      }
      size_t e = s.find('>', lt); if (e == std::string::npos) return ""; pos = e + 1; continue;
    }
    size_t e = lt; char q = 0;
    while (e < s.size()) { char c = s[e]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = c; else if (c == '>') break; ++e; }
    return s.substr(lt, e - lt + 1);
  }
}
static std::string Attr(const std::string& tag, const char* name) {
  size_t pos = 0; std::string nm = name;
  while ((pos = tag.find(nm, pos)) != std::string::npos) {
    bool startOk = pos > 0 && isspace((unsigned char)tag[pos - 1]);
    size_t e = pos + nm.size();
    while (e < tag.size() && isspace((unsigned char)tag[e])) ++e;
    if (startOk && e < tag.size() && tag[e] == '=') {
      ++e; while (e < tag.size() && isspace((unsigned char)tag[e])) ++e;
      if (e < tag.size() && (tag[e] == '"' || tag[e] == '\'')) { char q = tag[e]; size_t c = tag.find(q, e + 1); if (c != std::string::npos) return tag.substr(e + 1, c - e - 1); }
    }
    pos += nm.size();
  }
  return "";
}

bool SvgIntrinsicSize(const Bytes& svg, double& w, double& h) {
  std::string tag = RootTag(svg);
  if (tag.compare(0, 4, "<svg") != 0 && tag.find(":svg") == std::string::npos) return false;
  double ww = 0, hh = 0, vx = 0, vy = 0, vw = 0, vh = 0;
  bool okw = ParseLength(Attr(tag, "width"), ww), okh = ParseLength(Attr(tag, "height"), hh);
  std::string vb = Attr(tag, "viewBox");
  bool okv = false;
  if (!vb.empty()) { for (auto& c : vb) if (c == ',') c = ' '; okv = sscanf(vb.c_str(), "%lf %lf %lf %lf", &vx, &vy, &vw, &vh) == 4 && vw > 0 && vh > 0; }
  if (okw && okh) { w = ww; h = hh; return true; }
  if (okv) {
    if (okw) { w = ww; h = ww * vh / vw; } else if (okh) { h = hh; w = hh * vw / vh; } else { w = vw; h = vh; }
    return true;
  }
  return false;
}

bool RasterizeSvg(const Bytes& svg, int longSide, Bitmap& out, std::wstring* err) {
  ComInit com(COINIT_MULTITHREADED);
  double iw = 512, ih = 512;
  SvgIntrinsicSize(svg, iw, ih);
  double long0 = std::max(iw, ih);
  double target = longSide > 0 ? longSide : std::min(4096.0, std::max(long0, 1024.0));
  if (target > 8192) target = 8192;
  double scale = target / long0;
  int pw = std::max(1, (int)std::lround(iw * scale)), ph = std::max(1, (int)std::lround(ih * scale));
  if (pw > 8192 || ph > 8192) { double k = 8192.0 / std::max(pw, ph); pw = std::max(1, (int)(pw * k)); ph = std::max(1, (int)(ph * k)); scale *= k; }

  ComPtr<ID3D11Device> d3d;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
  if (FAILED(hr)) { if (err) *err = L"The graphics system could not be initialised to draw this SVG."; return false; }
  ComPtr<IDXGIDevice> dxgi = d3d.As<IDXGIDevice>();
  ComPtr<ID2D1Factory1> fac;
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr, fac.AsVoid());
  if (FAILED(hr) || !dxgi) { if (err) *err = L"Direct2D is not available."; return false; }
  ComPtr<ID2D1Device> dev; hr = fac->CreateDevice(dxgi.Get(), &dev);
  ComPtr<ID2D1DeviceContext> dc; if (SUCCEEDED(hr)) hr = dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
  ComPtr<ID2D1DeviceContext5> dc5; if (SUCCEEDED(hr)) dc5 = dc.As<ID2D1DeviceContext5>();
  if (FAILED(hr) || !dc5) { if (err) *err = L"SVG drawing needs Windows 10 version 1703 or later."; return false; }

  ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(svg.data(), (UINT)svg.size()));
  if (!stream) { if (err) *err = L"Out of memory."; return false; }
  ComPtr<ID2D1SvgDocument> doc;
  hr = dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF((float)iw, (float)ih), &doc);
  if (FAILED(hr) || !doc) { if (err) *err = L"This SVG file could not be read (it may be malformed)."; return false; }

  ComPtr<ID2D1Bitmap1> rt, cpu;
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  hr = dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &bp, &rt);
  D2D1_BITMAP_PROPERTIES1 bp2 = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &bp2, &cpu);
  if (FAILED(hr)) { if (err) *err = L"The SVG is too large to draw."; return false; }
  dc->SetTarget(rt.Get());
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0.0f));
  dc->SetTransform(D2D1::Matrix3x2F::Scale((float)scale, (float)scale));
  dc5->DrawSvgDocument(doc.Get());
  hr = dc->EndDraw();
  dc->SetTarget(nullptr);
  if (FAILED(hr)) { if (err) *err = L"This SVG could not be drawn."; return false; }
  hr = cpu->CopyFromBitmap(nullptr, rt.Get(), nullptr);
  D2D1_MAPPED_RECT mr{};
  if (SUCCEEDED(hr)) hr = cpu->Map(D2D1_MAP_OPTIONS_READ, &mr);
  if (FAILED(hr)) { if (err) *err = L"The drawn SVG could not be read back."; return false; }
  out.Allocate(pw, ph);
  for (int y = 0; y < ph; ++y) {
    const uint8_t* s = mr.bits + (size_t)y * mr.pitch; uint8_t* d = out.Row(y);
    for (int x = 0; x < pw; ++x, s += 4, d += 4) {
      uint8_t a = s[3];
      if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
      else if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; }
      else { d[0] = (uint8_t)std::min(255, (s[0] * 255 + a / 2) / a); d[1] = (uint8_t)std::min(255, (s[1] * 255 + a / 2) / a); d[2] = (uint8_t)std::min(255, (s[2] * 255 + a / 2) / a); d[3] = a; }
    }
  }
  cpu->Unmap();
  out.info.storedWidth = (int)std::lround(iw); out.info.storedHeight = (int)std::lround(ih);
  return true;
}

} // namespace filo::img
