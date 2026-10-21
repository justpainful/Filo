// Shared helpers for the image engine tests: fixture generation (python -I + PIL), independent decoders (PIL via
// check_image.py, WIC, ffprobe, PDFium) and the machine-readable matrix results file.
#pragma once
#include "test.h"
#include "core/job.h"
#include "core/json.h"
#include "core/process.h"
#include "core/util.h"
#include "registry/capability.h"
#include "registry/filetype.h"
#include "engines/engines.h"
#include "engines/image/bitmap.h"
#include "engines/common/ffmpeg.h"
#include "fpdfview.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace tu {
using namespace filo;
using Microsoft::WRL::ComPtr;

inline fs::path Root() {
  static fs::path r = [] {
    fs::path p = ExeDir();
    for (int i = 0; i < 8 && !p.empty(); ++i, p = p.parent_path())
      if (fs::exists(p / "CMakeLists.txt") && fs::exists(p / "src" / "engines")) return p;
    return fs::current_path();
  }();
  return r;
}
inline fs::path DataDir() { return Root() / "testdata" / "image"; }
inline fs::path Src() { return DataDir() / "src"; }
inline fs::path OutDir() { fs::path p = DataDir() / "out"; std::error_code ec; fs::create_directories(p, ec); return p; }

inline std::string RunPy(const std::vector<std::wstring>& args, int* code = nullptr, DWORD timeoutMs = 300000) {
  ProcessOptions po;
  po.exe = FindTool(L"python.exe");
  po.args = {L"-I"};
  po.args.insert(po.args.end(), args.begin(), args.end());
  po.timeoutMs = timeoutMs;
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  if (code) *code = r.started ? (int)r.exitCode : -1;
  if (!r.started || r.exitCode != 0) std::printf("  python failed (%d): %s\n", (int)r.exitCode, r.err.substr(0, 400).c_str());
  return r.out;
}

inline bool EnsureFixtures() {
  static std::once_flag once; static bool ok = false;
  std::call_once(once, [] {
    fs::path marker = Src() / "icc_expected.json";
    fs::path script = Root() / "tests" / "image" / "gen_fixtures.py";
    std::error_code ec;
    bool fresh = fs::exists(marker, ec) && fs::exists(Src() / "qr_text.svg", ec) && fs::last_write_time(marker, ec) >= fs::last_write_time(script, ec);
    if (!fresh) {
      int code = 0; RunPy({script.wstring(), Src().wstring()}, &code);
      ok = code == 0;
    } else ok = true;
  });
  return ok;
}

inline Json Inspect(const fs::path& p) {
  static fs::path script = Root() / "tests" / "image" / "check_image.py";
  std::string out = RunPy({script.wstring(), p.wstring()});
  std::string line = out.substr(0, out.find('\n'));
  auto j = Json::Parse(line);
  return j ? *j : Json::MakeObject();
}

struct Rgba { double r = 0, g = 0, b = 0, a = 0; };
inline Rgba Quad(const Json& j, int i) {
  const Json& q = j.Get("q").At(i);
  return {q.At(0).AsNumber(), q.At(1).AsNumber(), q.At(2).AsNumber(), q.At(3).AsNumber()};
}
inline bool NearC(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline bool NearRgb(const Rgba& c, double r, double g, double b, double tol) { return NearC(c.r, r, tol) && NearC(c.g, g, tol) && NearC(c.b, b, tol); }

// ---- WIC (independent of the engine's own loader: no orientation, no ICC, no ffmpeg) ---------------------------------------
struct Px { int w = 0, h = 0; std::vector<uint8_t> bgra; };
inline bool WicRead(const fs::path& p, Px& out) {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(LongPath(p).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> fr; if (FAILED(dec->GetFrame(0, &fr))) return false;
  UINT w, h; fr->GetSize(&w, &h);
  ComPtr<IWICFormatConverter> c; f->CreateFormatConverter(&c);
  if (FAILED(c->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  out.w = (int)w; out.h = (int)h; out.bgra.resize((size_t)w * h * 4);
  return SUCCEEDED(c->CopyPixels(nullptr, w * 4, (UINT)out.bgra.size(), out.bgra.data()));
}
// Mean colour of the centre of quadrant (qx,qy) like check_image.py; alpha-weighted colour, mean alpha.
inline Rgba PxQuad(const Px& p, int qx, int qy) {
  int x0 = (int)(qx * p.w / 2.0 + p.w / 8.0), x1 = (int)(qx * p.w / 2.0 + p.w * 3 / 8.0);
  int y0 = (int)(qy * p.h / 2.0 + p.h / 8.0), y1 = (int)(qy * p.h / 2.0 + p.h * 3 / 8.0);
  double r = 0, g = 0, b = 0, a = 0, wsum = 0, n = 0, rr = 0, gg = 0, bb = 0;
  for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
    const uint8_t* s = &p.bgra[((size_t)y * p.w + x) * 4];
    double al = s[3]; r += s[2] * al; g += s[1] * al; b += s[0] * al; wsum += al; a += al; ++n;
    rr += s[2]; gg += s[1]; bb += s[0];
  }
  if (wsum > 0) return {r / wsum, g / wsum, b / wsum, a / n};
  return {rr / n, gg / n, bb / n, 0};
}


// ---- quadrant helpers for engine bitmaps and test image synthesis -------------------------------------------------------------
inline Rgba QuadOf(const uint8_t* bgra, int w, int h, int qx, int qy) {
  Px p; p.w = w; p.h = h; p.bgra.assign(bgra, bgra + (size_t)w * h * 4);
  return PxQuad(p, qx, qy);
}
inline Rgba BmQuad(const img::Bitmap& b, int qx, int qy) { return QuadOf(b.px.data(), b.width, b.height, qx, qy); }

// Synthesises the display image D (TL red, TR green, BL blue, BR yellow) as BGRA, optionally with the alpha variant.
inline std::vector<uint8_t> MakeD(int w, int h, bool alpha = false) {
  std::vector<uint8_t> p((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    bool right = x >= w / 2, bottom = y >= h / 2;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    if (!right && !bottom) r = 255; else if (right && !bottom) g = 255; else if (!right && bottom) b = 255; else { r = 255; g = 255; }
    if (alpha) { if (right && !bottom) a = 0; if (!right && bottom) a = 128; }
    uint8_t* d = &p[((size_t)y * w + x) * 4]; d[0] = b; d[1] = g; d[2] = r; d[3] = a;
  }
  return p;
}

// Writes a HEIC with the Windows HEIF encoder (24-bit, quality 0.9). Returns false when the encoder is unavailable.
inline bool WicWriteHeic(const fs::path& out, const std::vector<uint8_t>& bgra, int w, int h, const char* make = nullptr, bool lossless = false, int transform = 0) {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  ComPtr<IWICStream> st; f->CreateStream(&st);
  if (FAILED(st->InitializeFromFilename(LongPath(out).c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatHeif, nullptr, &enc))) return false;
  if (FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> pb;
  if (FAILED(enc->CreateNewFrame(&fr, &pb))) return false;
  PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality"); VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.9f; pb->Write(1, &opt, &v);
  if (lossless) { PROPBAG2 lo{}; lo.pstrName = const_cast<LPOLESTR>(L"Lossless"); VARIANT lv; VariantInit(&lv); lv.vt = VT_BOOL; lv.boolVal = VARIANT_TRUE; pb->Write(1, &lo, &lv); }
  if (transform) { PROPBAG2 to{}; to.pstrName = const_cast<LPOLESTR>(L"BitmapTransform"); VARIANT tv; VariantInit(&tv); tv.vt = VT_UI1; tv.bVal = (BYTE)transform; pb->Write(1, &to, &tv); }
  if (FAILED(fr->Initialize(pb.Get())) || FAILED(fr->SetSize(w, h))) return false;
  WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR; fr->SetPixelFormat(&pf);
  if (make) { ComPtr<IWICMetadataQueryWriter> qw; if (SUCCEEDED(fr->GetMetadataQueryWriter(&qw))) { PROPVARIANT s; PropVariantInit(&s); s.vt = VT_LPSTR; s.pszVal = const_cast<LPSTR>(make); qw->SetMetadataByName(L"/ifd/{ushort=271}", &s); } }
  ComPtr<IWICBitmap> bmp;
  if (FAILED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)bgra.size(), const_cast<BYTE*>(bgra.data()), &bmp))) return false;
  ComPtr<IWICFormatConverter> conv; f->CreateFormatConverter(&conv);
  if (FAILED(conv->Initialize(bmp.Get(), pf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  if (FAILED(fr->WriteSource(conv.Get(), nullptr))) return false;
  return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}
inline bool EnsureHeicFixtures() {
  static bool done = false, ok = false;
  if (done) return ok;
  done = true;
  ok = EnsureFixtures();
  if (!ok) return false;
  auto d = MakeD(320, 240);
  ok = WicWriteHeic(Src() / "src_heic.heic", d, 320, 240, "FiloTest");
  auto big = MakeD(4000, 3000);
  ok = ok && WicWriteHeic(Src() / "big_grid_lossy.heic", big, 4000, 3000);
  ok = ok && WicWriteHeic(Src() / "heic_rot90.heic", d, 320, 240, nullptr, false, 1);
  ok = ok && WicWriteHeic(Src() / "big_grid.heic", big, 4000, 3000, nullptr, true);   // lossless: the Windows encoder's lossy tiled output decodes with a colour shift (see docs)
  return ok;
}


// ---- PDFium (independent PDF reader) ------------------------------------------------------------------------------------------
inline bool RenderPdfPage(const fs::path& p, int pageIndex, int pxW, int pxH, Px* out, double* wPt = nullptr, double* hPt = nullptr, int* pageCount = nullptr) {
  static std::once_flag once; std::call_once(once, [] { FPDF_InitLibrary(); });
  img::Bytes data; if (!img::ReadFileBytes(p, data)) return false;
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument(data.data(), (int)data.size(), nullptr);
  if (!doc) return false;
  if (pageCount) *pageCount = FPDF_GetPageCount(doc);
  FPDF_PAGE pg = FPDF_LoadPage(doc, pageIndex);
  if (!pg) { FPDF_CloseDocument(doc); return false; }
  if (wPt) *wPt = FPDF_GetPageWidth(pg);
  if (hPt) *hPt = FPDF_GetPageHeight(pg);
  bool ok = true;
  if (out && pxW > 0) {
    FPDF_BITMAP bmp = FPDFBitmap_Create(pxW, pxH, 1);
    FPDFBitmap_FillRect(bmp, 0, 0, pxW, pxH, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, pg, 0, 0, pxW, pxH, 0, FPDF_ANNOT);
    out->w = pxW; out->h = pxH; out->bgra.resize((size_t)pxW * pxH * 4);
    const uint8_t* src = (const uint8_t*)FPDFBitmap_GetBuffer(bmp); int stride = FPDFBitmap_GetStride(bmp);
    for (int y = 0; y < pxH; ++y) memcpy(&out->bgra[(size_t)y * pxW * 4], src + (size_t)y * stride, (size_t)pxW * 4);
    FPDFBitmap_Destroy(bmp);
  }
  FPDF_ClosePage(pg); FPDF_CloseDocument(doc);
  return ok;
}

// ---- results file ---------------------------------------------------------------------------------------------------------
inline std::mutex& ResMu() { static std::mutex m; return m; }
inline void Record(const std::string& id, bool pass, const std::string& note) {
  std::lock_guard lk(ResMu());
  fs::path file = DataDir() / "matrix-results.json";
  Json root = Json::MakeObject();
  {
    std::ifstream in(file, std::ios::binary);
    if (in) { std::stringstream ss; ss << in.rdbuf(); if (auto j = Json::Parse(ss.str())) root = *j; }
  }
  Json entry = Json::MakeObject();
  entry["status"] = pass ? "pass" : "fail";
  entry["note"] = note;
  root["results"][id] = entry;
  std::error_code ec; fs::create_directories(file.parent_path(), ec);
  std::ofstream o(file, std::ios::binary | std::ios::trunc);
  o << root.Dump(true);
}

// ---- running a capability through the real registry path ---------------------------------------------------------------------
struct RunOut { bool ok = false; fs::path out; std::wstring error; std::vector<std::wstring> warnings; bool ranCap = false; };
inline bool HasWarning(const RunOut& r, const std::wstring& needle) {
  for (auto& w : r.warnings) if (w.find(needle) != std::wstring::npos) return true;
  return false;
}
inline RunOut RunCap(const std::wstring& in, const std::wstring& out, const fs::path& src, const fs::path& dest, Json options = Json::MakeObject()) {
  RunOut r;
  filo::RegisterImageEngine();
  const Capability* cap = CapabilityRegistry::Get().Find(in, out);
  if (!cap) { r.error = L"capability not registered"; return r; }
  if (!cap->Runnable()) { r.error = L"capability not runnable on this machine"; return r; }
  r.ranCap = true;
  std::error_code ec; fs::remove(dest, ec);
  JobSpec spec; spec.operation = L"convert"; spec.title = src.filename().wstring(); spec.inputs = {src};
  spec.work = [cap, src, dest, options](JobContext& ctx) { fs::path d = dest; return RunConversion(*cap, src, options, ctx, &d); };
  uint64_t id = JobManager::Get().Submit(std::move(spec));
  JobManager::Get().WaitFor(id);
  auto info = JobManager::Get().Info(id);
  if (!info) { r.error = L"job vanished"; return r; }
  r.ok = info->result.ok; r.error = info->result.error; r.warnings = info->result.warnings;
  if (r.ok && !info->result.outputs.empty()) r.out = info->result.outputs[0];
  return r;
}
} // namespace tu
