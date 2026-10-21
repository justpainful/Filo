#include "pdf_testutil.h"
#include "core/json.h"
#include "fpdfview.h"
#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdf_save.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace ptest {

fs::path Root() {
  static fs::path root = [] {
    fs::path p = ExeDir();
    for (int i = 0; i < 8; ++i) {
      if (fs::exists(p / L"testdata") && fs::exists(p / L"CMakeLists.txt")) return p;
      p = p.parent_path();
    }
    return fs::path(L"C:\\Users\\kuroi\\OneDrive\\Desktop\\Filo");
  }();
  return root;
}
fs::path DataDir() { fs::path d = Root() / L"testdata" / L"pdf"; std::error_code ec; fs::create_directories(d, ec); return d; }
fs::path Scratch() {
  static fs::path s = [] {
    fs::path d = ExeDir() / L"pdf_test_out";
    std::error_code ec; fs::create_directories(d, ec);
    return d;
  }();
  return s;
}

std::string Utf8(const std::wstring& w) { return Narrow(w); }
bool Contains(const std::wstring& hay, const std::wstring& needle) { return hay.find(needle) != std::wstring::npos; }

std::string ReadFileBytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}
bool WriteFileBytes(const fs::path& p, const std::string& d) {
  std::ofstream f(p, std::ios::binary);
  f.write(d.data(), (std::streamsize)d.size());
  return (bool)f;
}

std::string RunPython(const std::string& code, const std::vector<std::wstring>& args, int* rc) {
  fs::path py = FindTool(L"python.exe");
  ProcessOptions o;
  o.exe = py.empty() ? fs::path(L"python.exe") : py;
  o.args = {L"-I", L"-c", Widen(code)};
  for (auto& a : args) o.args.push_back(a);
  o.timeoutMs = 120000; o.lowPriority = false;
  auto r = RunProcess(o);
  if (rc) *rc = r.started ? (int)r.exitCode : -1;
  return r.out + (r.exitCode ? "\n[stderr]" + r.err : "");
}
bool HavePil() {
  static int have = -1;
  if (have < 0) { int rc = 0; RunPython("import PIL", {}, &rc); have = rc == 0; }
  return have == 1;
}

fs::path Fixture(const std::string& name) {
  fs::path f = DataDir() / Widen(name);
  if (fs::exists(f)) return f;
  if (name.rfind("lo_", 0) == 0 || name.rfind("pw_", 0) == 0 || name.rfind("outlines_", 0) == 0 || name == "labels.pdf" || name == "form.pdf" || name == "heavy_outlines.pdf") {
    static std::once_flag once;
    std::call_once(once, [] {
      fs::path so = L"C:\\Program Files\\LibreOffice\\program\\soffice.exe";
      if (!fs::exists(so)) { fs::path t = FindTool(L"soffice.exe"); if (!t.empty()) so = t; }
      ProcessOptions o; o.exe = FindTool(L"python.exe");
      o.args = {L"-I", (DataDir() / L"gen_fixtures.py").wstring(), DataDir().wstring(), so.wstring()};
      o.timeoutMs = 600000; o.lowPriority = false;
      auto r = RunProcess(o);
      if (!r.Ok()) std::printf("  fixture generation failed: %s\n", r.out.c_str());
    });
  }
  return f;
}

// ---- WIC -------------------------------------------------------------------------------------------------------------------

static ComPtr<IWICImagingFactory> Wic() {
  ComPtr<IWICImagingFactory> f;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  return f;
}

bool DecodeImage(const fs::path& p, Img& out) {
  auto f = Wic(); if (!f) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(LongPath(p).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, &fr))) return false;
  ComPtr<IWICFormatConverter> conv;
  f->CreateFormatConverter(&conv);
  if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  UINT w, h; conv->GetSize(&w, &h);
  out.w = (int)w; out.h = (int)h; out.bgra.resize((size_t)w * h * 4);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.bgra.size(), out.bgra.data()));
}

static bool EncodeWic(const fs::path& p, const Img& img, const GUID& container, int quality) {
  auto f = Wic(); if (!f) return false;
  ComPtr<IWICStream> st; f->CreateStream(&st);
  if (FAILED(st->InitializeFromFilename(LongPath(p).c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(container, nullptr, &enc))) return false;
  enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> bag;
  enc->CreateNewFrame(&fr, &bag);
  if (container == GUID_ContainerFormatJpeg && bag) {
    PROPBAG2 pb{}; pb.pstrName = (LPOLESTR)L"ImageQuality"; VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = quality / 100.f;
    bag->Write(1, &pb, &v);
  }
  fr->Initialize(bag.Get());
  fr->SetSize(img.w, img.h);
  GUID fmt = container == GUID_ContainerFormatJpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
  GUID want = fmt;
  fr->SetPixelFormat(&fmt);
  if (fmt != want) return false;
  if (container == GUID_ContainerFormatJpeg) {
    std::vector<uint8_t> row((size_t)img.w * 3);
    for (int y = 0; y < img.h; ++y) {
      for (int x = 0; x < img.w; ++x) std::memcpy(&row[x * 3], &img.bgra[((size_t)y * img.w + x) * 4], 3);
      if (FAILED(fr->WritePixels(1, (UINT)row.size(), (UINT)row.size(), row.data()))) return false;
    }
  } else {
    if (FAILED(fr->WritePixels(img.h, img.w * 4, (UINT)img.bgra.size(), const_cast<BYTE*>(img.bgra.data())))) return false;
  }
  return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}
bool EncodePng(const fs::path& p, const Img& img) { return EncodeWic(p, img, GUID_ContainerFormatPng, 0); }
bool EncodeJpeg(const fs::path& p, const Img& img, int q) { return EncodeWic(p, img, GUID_ContainerFormatJpeg, q); }

double NonBlankRatio(const Img& i) {
  if (i.w <= 0 || i.h <= 0) return 0;
  size_t n = (size_t)i.w * i.h, cnt = 0;
  for (size_t k = 0; k < n; ++k) {
    const uint8_t* p = &i.bgra[k * 4];
    if (p[3] > 8 && (p[0] < 235 || p[1] < 235 || p[2] < 235)) ++cnt;
  }
  return (double)cnt / n;
}
double MeanAbsDiff(const Img& a, const Img& b) {
  if (a.w != b.w || a.h != b.h) return 255;
  double s = 0; size_t n = (size_t)a.w * a.h;
  for (size_t k = 0; k < n; ++k) for (int c = 0; c < 3; ++c) s += std::abs((int)a.bgra[k * 4 + c] - (int)b.bgra[k * 4 + c]);
  return s / (n * 3.0);
}

// ---- PDF construction ----------------------------------------------------------------------------------------------------

static std::vector<unsigned short> U16(const std::wstring& s) {
  std::vector<unsigned short> v(s.begin(), s.end()); v.push_back(0); return v;
}

bool BuildPdf(const fs::path& out, const std::vector<PageSpec>& pages, std::wstring* err) {
  filo::pdf::PdfLock lock;
  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc) { if (err) *err = L"create"; return false; }
  int idx = 0;
  for (auto& ps : pages) {
    FPDF_PAGE pg = FPDFPage_New(doc, idx++, ps.w, ps.h);
    for (auto& t : ps.texts) {
      FPDF_PAGEOBJECT to = FPDFPageObj_NewTextObj(doc, t.font.c_str(), t.size);
      auto u = U16(t.text);
      FPDFText_SetText(to, (FPDF_WIDESTRING)u.data());
      FPDFPageObj_SetFillColor(to, (t.rgb >> 16) & 255, (t.rgb >> 8) & 255, t.rgb & 255, 255);
      double rad = t.rotateDeg * 3.14159265358979 / 180.0, c = std::cos(rad), s = std::sin(rad);
      FPDFPageObj_Transform(to, c, s, -s, c, t.x, ps.h - t.y - t.size);
      FPDFPage_InsertObject(pg, to);
    }
    for (auto& l : ps.lines) {
      FPDF_PAGEOBJECT po = FPDFPageObj_CreateNewPath(l.x1, ps.h - l.y1);
      FPDFPath_LineTo(po, l.x2, ps.h - l.y2);
      FPDFPageObj_SetStrokeColor(po, 0, 0, 0, 255);
      FPDFPageObj_SetStrokeWidth(po, l.width);
      FPDFPath_SetDrawMode(po, 0, 1);
      FPDFPage_InsertObject(pg, po);
    }
    for (auto& im : ps.images) {
      FPDF_PAGEOBJECT io = FPDFPageObj_NewImageObj(doc);
      // stored image: horizontal gradient (blue), vertical gradient (green), checker (red)
      Img px; px.w = im.pw; px.h = im.ph; px.bgra.resize((size_t)im.pw * im.ph * 4);
      uint32_t seed = 12345;
      for (int y = 0; y < im.ph; ++y)
        for (int x = 0; x < im.pw; ++x) {
          uint8_t* p = &px.bgra[((size_t)y * im.pw + x) * 4];
          seed = seed * 1664525u + 1013904223u;
          int n = im.noise ? (int)((seed >> 24) % 17) - 8 : 0;
          auto cl = [](int v) { return (uint8_t)std::clamp(v, 0, 255); };
          p[0] = cl(x * 255 / std::max(1, im.pw - 1) + n);
          p[1] = cl(y * 255 / std::max(1, im.ph - 1) + n);
          p[2] = cl(128 + ((x / 16 + y / 16) & 1) * 40 + n);
          p[3] = 255;
        }
      bool loaded = false;
      std::string jpegBytes;
      if (im.jpeg) {
        fs::path tmp = out.parent_path() / L"_tmp_img.jpg";
        if (EncodeJpeg(tmp, px, im.jpegQuality)) {
          jpegBytes = ReadFileBytes(tmp);
          std::error_code ec; fs::remove(tmp, ec);
          struct MA : FPDF_FILEACCESS { std::string* d; } ma;
          ma.m_FileLen = (unsigned long)jpegBytes.size(); ma.d = &jpegBytes; ma.m_Param = &jpegBytes;
          ma.m_GetBlock = [](void* param, unsigned long pos, unsigned char* buf, unsigned long size) -> int {
            auto* d = (std::string*)param; if (pos + size > d->size()) return 0; memcpy(buf, d->data() + pos, size); return 1; };
          loaded = FPDFImageObj_LoadJpegFileInline(&pg, 1, io, &ma) != 0;
        }
      }
      if (!loaded) {
        FPDF_BITMAP bmp = FPDFBitmap_CreateEx(im.pw, im.ph, FPDFBitmap_BGRx, px.bgra.data(), im.pw * 4);
        FPDFImageObj_SetBitmap(&pg, 1, io, bmp);
        FPDFBitmap_Destroy(bmp);
      }
      // matrix: stored unit square -> scale(iw,ih) -> rotate ccw by rot90 quarter turns -> shift so the box starts at (x, y)
      double bw = im.w, bh = im.h;
      bool odd = (im.rot90 & 1) != 0;
      double iw = odd ? bh : bw, ih = odd ? bw : bh;
      double th = (im.rot90 & 3) * 3.14159265358979 / 2, cs = std::round(std::cos(th)), sn = std::round(std::sin(th));
      double fx = im.flipX ? -1 : 1;
      // point (u,v) -> (fx*iw*u, ih*v) -> rotate
      double a = cs * iw * fx, b = sn * iw * fx, c2 = -sn * ih, d = cs * ih;
      double xs[4] = {0, a, c2, a + c2}, ys[4] = {0, b, d, b + d};
      double minx = *std::min_element(xs, xs + 4), miny = *std::min_element(ys, ys + 4);
      double e = im.x - minx, f = (ps.h - im.y - im.h) - miny;
      FPDFImageObj_SetMatrix(io, a, b, c2, d, e, f);
      FPDFPage_InsertObject(pg, io);
    }
    if (ps.rotate) FPDFPage_SetRotation(pg, ((ps.rotate / 90) % 4 + 4) % 4);
    FPDFPage_GenerateContent(pg);
    FPDF_ClosePage(pg);
  }
  struct W : FPDF_FILEWRITE { std::string data; } w;
  w.version = 1;
  w.WriteBlock = [](FPDF_FILEWRITE* self, const void* d, unsigned long n) -> int { static_cast<W*>(self)->data.append((const char*)d, n); return 1; };
  bool ok = FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL) != 0;
  FPDF_CloseDocument(doc);
  if (!ok) { if (err) *err = L"save"; return false; }
  return WriteFileBytes(out, w.data);
}

bool BuildMarkerPdf(const fs::path& out, int pages, float w, float h, const std::wstring& prefix) {
  std::vector<PageSpec> v;
  for (int i = 0; i < pages; ++i) {
    PageSpec ps; ps.w = w; ps.h = h;
    wchar_t b[64]; swprintf_s(b, L"%s-%04d", prefix.c_str(), i + 1);
    ps.texts.push_back({72, 72, 24, b});
    ps.texts.push_back({72, 120, 11, L"The quick brown fox jumps over the lazy dog. Page body text for extraction tests."});
    v.push_back(std::move(ps));
  }
  return BuildPdf(out, v);
}

fs::path Big300() {
  fs::path f = Scratch() / L"big300.pdf";
  if (fs::exists(f)) return f;
  std::vector<PageSpec> pages;
  for (int i = 0; i < 300; ++i) {
    PageSpec p;
    wchar_t head[64]; swprintf_s(head, L"Chapter page %03d heading", i + 1);
    p.texts.push_back({72, 60, 18, head, "Helvetica-Bold"});
    for (int l = 0; l < 44; ++l) {
      wchar_t b[160]; swprintf_s(b, L"P%03d L%02d the quick brown fox jumps over the lazy dog and keeps running until the end of line", i + 1, l);
      p.texts.push_back({72, 100.f + l * 15.f, 10, b});
    }
    pages.push_back(std::move(p));
  }
  BuildPdf(f, pages);
  return f;
}

PdfFacts ReadPdf(const fs::path& p, const std::wstring& pw) {
  PdfFacts f;
  auto r = filo::pdf::PdfDocument::Open(p, pw);
  if (!r.ok()) { f.err = r.message; return f; }
  f.pages = r.doc->PageCount();
  for (int i = 0; i < f.pages; ++i) {
    float w = 0, h = 0; r.doc->PageSize(i, &w, &h);
    f.w.push_back(w); f.h.push_back(h); f.rot.push_back(r.doc->PageRotation(i));
    f.text.push_back(r.doc->PageText(i));
  }
  f.ok = true;
  return f;
}

// ---- results ---------------------------------------------------------------------------------------------------------------

static std::map<std::string, std::pair<bool, std::string>>& Results() { static std::map<std::string, std::pair<bool, std::string>> r; return r; }
static std::mutex g_resMu;
void FlushResults();
void RecordResult(const std::string& id, bool pass, const std::string& note) {
  std::lock_guard<std::mutex> l(g_resMu);
  static const bool registered = (Results(), std::atexit([] { FlushResults(); }), true);   // registered after the map exists => runs before it is destroyed
  (void)registered;
  auto it = Results().find(id);
  if (it != Results().end() && !it->second.first) return;               // a failure sticks
  Results()[id] = {pass, note};
}
void FlushResults() {
  std::lock_guard<std::mutex> l(g_resMu);
  Json root = Json::MakeObject();
  Json res = Json::MakeObject();
  // keep entries written by earlier runs for ids that were not exercised this time
  fs::path file = DataDir() / L"matrix-results.json";
  if (auto old = Json::Parse(ReadFileBytes(file))) for (auto& [k, v] : old->Get("results").Items()) res[k] = v;
  for (auto& [id, r] : Results()) {
    Json e = Json::MakeObject();
    e["status"] = r.first ? "pass" : "fail";
    e["note"] = r.second;
    res[id] = e;
  }
  root["results"] = res;
  WriteFileBytes(file, root.Dump(true));
}

} // namespace ptest
