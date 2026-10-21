#include "engines/image/pdf_writer.h"
#include "engines/image/png_writer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace filo::img {

bool JpegPassthroughOk(const JpegInfo& ji) {
  return ji.width > 0 && ji.height > 0 && ji.precision == 8 && (ji.components == 1 || ji.components == 3) && !ji.arithmetic && !ji.lossless;
}

namespace {
struct Pdf {
  Bytes buf;
  std::vector<size_t> offsets;   // object number - 1 -> offset
  void Raw(const std::string& s) { buf.insert(buf.end(), s.begin(), s.end()); }
  void RawB(const Bytes& b) { buf.insert(buf.end(), b.begin(), b.end()); }
  int Reserve() { offsets.push_back(0); return (int)offsets.size(); }
  void Begin(int obj) { offsets[obj - 1] = buf.size(); Raw(std::to_string(obj) + " 0 obj\n"); }
  void End() { Raw("\nendobj\n"); }
  void Stream(int obj, const std::string& dict, const Bytes& data) {
    Begin(obj);
    Raw("<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n");
    RawB(data);
    Raw("\nendstream");
    End();
  }
};
static std::string Num(double v) {
  char b[64]; snprintf(b, sizeof b, "%.4f", v);
  std::string s = b;
  if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
  return s.empty() ? "0" : s;
}
static std::string EscapePdfString(const std::string& s) {
  std::string o = "(";
  for (unsigned char c : s) { if (c == '(' || c == ')' || c == '\\') { o += '\\'; o += (char)c; } else if (c < 32 || c > 126) o += '?'; else o += (char)c; }
  return o + ")";
}

// Orientation -> affine matrix mapping the unit square (image space, v up) to page points.
static void OrientationMatrix(int o, double w, double h, double k, double& a, double& b, double& c, double& d, double& e, double& f, double& dispW, double& dispH) {
  bool swap = o >= 5;
  dispW = swap ? h : w; dispH = swap ? w : h;
  auto P = [&](double u, double v, double& px, double& py) {
    double x = u * w, y = (1 - v) * h, dx, dy;
    switch (o) {
      case 2: dx = w - x; dy = y; break;
      case 3: dx = w - x; dy = h - y; break;
      case 4: dx = x; dy = h - y; break;
      case 5: dx = y; dy = x; break;
      case 6: dx = h - y; dy = x; break;
      case 7: dx = h - y; dy = w - x; break;
      case 8: dx = y; dy = w - x; break;
      default: dx = x; dy = y; break;
    }
    px = k * dx; py = k * (dispH - dy);
  };
  double x0, y0, x1, y1, x2, y2;
  P(0, 0, x0, y0); P(1, 0, x1, y1); P(0, 1, x2, y2);
  a = x1 - x0; b = y1 - y0; c = x2 - x0; d = y2 - y0; e = x0; f = y0;
}
} // namespace

bool WritePdf(const fs::path& out, const std::vector<PdfImagePage>& pages, const PdfOptions& opt, std::wstring* err) {
  if (pages.empty()) { if (err) *err = L"There are no pages to write."; return false; }
  Pdf pdf;
  pdf.Raw("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
  int catalog = pdf.Reserve(), pagesObj = pdf.Reserve();
  std::vector<int> pageObjs;
  struct Pending { int obj; std::string dict; Bytes data; };
  std::vector<Pending> streams;      // written after the page objects
  std::string kids;
  for (size_t i = 0; i < pages.size(); ++i) {
    const PdfImagePage& pg = pages[i];
    bool jpegMode = !pg.jpeg.empty();
    int sw, sh;
    if (jpegMode) { sw = pg.jpegInfo.width; sh = pg.jpegInfo.height; }
    else { if (!pg.bitmap || pg.bitmap->Empty()) { if (err) *err = L"A page has no image."; return false; } sw = pg.bitmap->width; sh = pg.bitmap->height; }
    int o = jpegMode ? std::clamp(pg.orientation, 1, 8) : 1;
    double k = 72.0 / (pg.dpi > 0 ? pg.dpi : 96.0);
    double dispWpx = (o >= 5) ? sh : sw, dispHpx = (o >= 5) ? sw : sh;
    // keep the media box within the PDF limit (14400 units) by scaling the mapping, never the pixels
    double maxDim = std::max(dispWpx, dispHpx) * k;
    if (maxDim > 14400.0) k *= 14400.0 / maxDim;
    double a, b, c, d, e, f, dw, dh;
    OrientationMatrix(o, sw, sh, k, a, b, c, d, e, f, dw, dh);
    double pageW = dw * k, pageH = dh * k;
    // matrix maps the unit square, so scale by the stored size
    // (OrientationMatrix already folded w,h into the mapping)

    int pageObj = pdf.Reserve(), contentObj = pdf.Reserve(), imgObj = pdf.Reserve();
    int smaskObj = 0, iccObj = 0;
    pageObjs.push_back(pageObj);

    std::string cs; Pending img{imgObj, "", {}};
    std::string base = "/Type /XObject /Subtype /Image /Width " + std::to_string(sw) + " /Height " + std::to_string(sh) + " /BitsPerComponent 8";
    if (jpegMode) {
      int n = pg.jpegInfo.components;
      if (!pg.icc.empty() && ((n == 3 && IccColorSpace(pg.icc) == "RGB ") || (n == 1 && IccColorSpace(pg.icc) == "GRAY"))) {
        iccObj = pdf.Reserve();
        Bytes z; ZlibCompress(pg.icc.data(), pg.icc.size(), 9, z);
        streams.push_back({iccObj, "/N " + std::to_string(n) + " /Alternate " + (n == 3 ? "/DeviceRGB" : "/DeviceGray") + " /Filter /FlateDecode", std::move(z)});
        cs = "[/ICCBased " + std::to_string(iccObj) + " 0 R]";
      } else cs = n == 3 ? "/DeviceRGB" : "/DeviceGray";
      img.dict = base + " /ColorSpace " + cs + " /Filter /DCTDecode";
      img.data = pg.jpeg;
    } else {
      const Bitmap& bm = *pg.bitmap;
      size_t npx = (size_t)sw * sh;
      const int level = opt.flateLevel > 0 ? std::clamp(opt.flateLevel, 1, 9) : (npx <= 2000000 ? 6 : npx <= 8000000 ? 4 : 2);
      bool alpha = UsesAlpha(bm);
      bool gray = true;
      { const uint8_t* p = bm.px.data(); for (size_t j = 0; j < npx; ++j, p += 4) if (p[0] != p[1] || p[1] != p[2]) { gray = false; break; } }
      int comps = gray ? 1 : 3;
      Bytes raw(npx * comps);
      { const uint8_t* p = bm.px.data(); uint8_t* q = raw.data();
        if (gray) for (size_t j = 0; j < npx; ++j, p += 4) *q++ = p[2];
        else for (size_t j = 0; j < npx; ++j, p += 4) { *q++ = p[2]; *q++ = p[1]; *q++ = p[0]; } }
      Bytes filtered = PngFilterRows(raw.data(), sw * comps, sh, comps);
      Bytes z;
      if (!ZlibCompress(filtered.data(), filtered.size(), level, z)) { if (err) *err = L"Compression failed."; return false; }
      img.dict = base + " /ColorSpace " + (gray ? "/DeviceGray" : "/DeviceRGB") + " /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors " + std::to_string(comps) + " /Columns " + std::to_string(sw) + " /BitsPerComponent 8 >>";
      img.data = std::move(z);
      if (alpha) {
        smaskObj = pdf.Reserve();
        Bytes a8(npx); const uint8_t* p = bm.px.data() + 3; for (size_t j = 0; j < npx; ++j, p += 4) a8[j] = *p;
        Bytes af = PngFilterRows(a8.data(), sw, sh, 1), az;
        ZlibCompress(af.data(), af.size(), level, az);
        streams.push_back({smaskObj, "/Type /XObject /Subtype /Image /Width " + std::to_string(sw) + " /Height " + std::to_string(sh) + " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 1 /Columns " + std::to_string(sw) + " /BitsPerComponent 8 >>", std::move(az)});
        img.dict += " /SMask " + std::to_string(smaskObj) + " 0 R";
      }
    }
    streams.push_back(std::move(img));
    // page + content
    std::string content = "q " + Num(a) + " " + Num(b) + " " + Num(c) + " " + Num(d) + " " + Num(e) + " " + Num(f) + " cm /Im0 Do Q\n";
    pdf.Begin(pageObj);
    pdf.Raw("<< /Type /Page /Parent " + std::to_string(pagesObj) + " 0 R /MediaBox [0 0 " + Num(pageW) + " " + Num(pageH) + "] /Resources << /XObject << /Im0 " + std::to_string(imgObj) + " 0 R >> >> /Contents " + std::to_string(contentObj) + " 0 R >>");
    pdf.End();
    Bytes cb(content.begin(), content.end());
    pdf.Stream(contentObj, "", cb);
    kids += std::to_string(pageObj) + " 0 R ";
    // stream objects are written now so offsets stay ordered
    for (auto& s : streams) pdf.Stream(s.obj, s.dict, s.data);
    streams.clear();
  }
  pdf.Begin(catalog); pdf.Raw("<< /Type /Catalog /Pages " + std::to_string(pagesObj) + " 0 R >>"); pdf.End();
  pdf.Begin(pagesObj); pdf.Raw("<< /Type /Pages /Count " + std::to_string(pages.size()) + " /Kids [" + kids + "] >>"); pdf.End();
  int info = pdf.Reserve();
  {
    time_t t = time(nullptr); struct tm g{}; gmtime_s(&g, &t);
    char date[40]; snprintf(date, sizeof date, "D:%04d%02d%02d%02d%02d%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    pdf.Begin(info);
    pdf.Raw("<< /Producer (Filo) /CreationDate (" + std::string(date) + ")" + (opt.title.empty() ? "" : " /Title " + EscapePdfString(opt.title)) + " >>");
    pdf.End();
  }
  size_t xref = pdf.buf.size();
  pdf.Raw("xref\n0 " + std::to_string(pdf.offsets.size() + 1) + "\n0000000000 65535 f \n");
  for (size_t off : pdf.offsets) { char b[32]; snprintf(b, sizeof b, "%010llu 00000 n \n", (unsigned long long)off); pdf.Raw(b); }
  pdf.Raw("trailer\n<< /Size " + std::to_string(pdf.offsets.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
  return WriteFileBytes(out, pdf.buf, err);
}

} // namespace filo::img
