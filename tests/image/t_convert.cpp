// Conversion tests through the real registry path (JobManager -> RunConversion -> engine -> verify -> atomic commit).
#include "t_util.h"
#include "engines/image/imgmeta.h"
#include "engines/image/image_convert.h"
#include <set>

using namespace tu;
using namespace filo;
using namespace filo::img;

namespace {

struct SrcSpec { const char* file; bool tr0; bool bl128; bool lossy; int w, h; };
const std::map<std::wstring, SrcSpec>& Sources() {
  static const std::map<std::wstring, SrcSpec> m = {
    {L"jpg",  {"src_jpg.jpg",   false, false, true,  320, 240}},     // stored 240x320 + EXIF orientation 6
    {L"png",  {"src_png.png",   true,  true,  false, 320, 240}},
    {L"webp", {"src_webp.webp", true,  true,  false, 320, 240}},
    {L"tiff", {"src_tiff.tiff", true,  true,  false, 320, 240}},
    {L"bmp",  {"src_bmp.bmp",   false, false, false, 320, 240}},
    {L"gif",  {"src_gif.gif",   true,  false, false, 320, 240}},
    {L"avif", {"src_avif.avif", true,  true,  true,  320, 240}},
    {L"heic", {"src_heic.heic", false, false, true,  320, 240}},
    {L"svg",  {"src_svg.svg",   true,  true,  false, 1024, 768}},
  };
  return m;
}

bool IsLossyOut(const std::wstring& o) { return o == L"jpg" || o == L"webp" || o == L"avif" || o == L"heic" || o == L"gif"; }

struct Quads { Rgba q[4]; int w = 0, h = 0; bool ok = false; std::string detail; };

// Decodes `p` with an independent reader: PDFium for PDF, WIC for HEIC, PIL for the rest.
Quads Independent(const fs::path& p, const std::wstring& fmt, const SrcSpec& src) {
  Quads r;
  if (fmt == L"pdf") {
    double wpt = 0, hpt = 0; int pages = 0; Px px;
    int pw = src.w == 1024 ? 320 : src.w, ph = src.w == 1024 ? 240 : src.h;
    r.ok = RenderPdfPage(p, 0, pw, ph, &px, &wpt, &hpt, &pages);
    if (!r.ok) { r.detail = "pdfium failed"; return r; }
    // page size = image size at 96 dpi -> 72 pt  (320x240 px -> 240x180 pt)
    if (std::fabs(wpt - 240) > 0.6 || std::fabs(hpt - 180) > 0.6 || pages != 1) { r.detail = "page " + std::to_string(wpt) + "x" + std::to_string(hpt) + " pages " + std::to_string(pages); r.ok = false; return r; }
    r.w = 320; r.h = 240;
    for (int i = 0; i < 4; ++i) { r.q[i] = PxQuad(px, i & 1, i >> 1); r.q[i].a = 255; }
    return r;
  }
  if (fmt == L"heic") {
    Px px; r.ok = WicRead(p, px);
    if (!r.ok) { r.detail = "wic failed"; return r; }
    r.w = px.w; r.h = px.h;
    for (int i = 0; i < 4; ++i) r.q[i] = PxQuad(px, i & 1, i >> 1);
    return r;
  }
  Json j = Inspect(p);
  r.ok = j.Get("ok").AsBool();
  if (!r.ok) { r.detail = j.Get("error").AsString(); return r; }
  r.w = (int)j.Get("w").AsInt(); r.h = (int)j.Get("h").AsInt();
  for (int i = 0; i < 4; ++i) r.q[i] = Quad(j, i);
  return r;
}

std::string RunMatrixCase(const std::wstring& in, const std::wstring& out) {
  const SrcSpec& src = Sources().at(in);
  fs::path input = Src() / src.file;
  fs::path dest = OutDir() / (std::wstring(L"m_") + in + L"_to." + out);
  RunOut r = RunCap(in, out, input, dest);
  if (!r.ranCap) return "not runnable: " + Narrow(r.error);
  if (!r.ok) return "conversion failed: " + Narrow(r.error);
  Quads q = Independent(r.out, out, src);
  if (!q.ok) return "independent decoder failed: " + q.detail;

  int ew = src.w, eh = src.h;
  if (out == L"pdf") { ew = 320; eh = 240; }
  if (q.w != ew || q.h != eh) return "dimensions " + std::to_string(q.w) + "x" + std::to_string(q.h) + " expected " + std::to_string(ew) + "x" + std::to_string(eh);

  bool alphaOut = FormatSupportsAlpha(out) && out != L"pdf";
  bool srcAlpha = src.tr0 || src.bl128;
  bool lossy = src.lossy || IsLossyOut(out);
  double tol = lossy ? 24 : 4;
  char buf[256];
  auto fail = [&](const char* what, int quad) {
    snprintf(buf, sizeof buf, "%s (quad %d = %.0f,%.0f,%.0f,%.0f)", what, quad, q.q[quad].r, q.q[quad].g, q.q[quad].b, q.q[quad].a);
    return std::string(buf);
  };
  if (!NearRgb(q.q[0], 255, 0, 0, tol) || (alphaOut && q.q[0].a < 240)) return fail("TL not red/opaque", 0);
  if (!NearRgb(q.q[3], 255, 255, 0, tol) || (alphaOut && q.q[3].a < 240)) return fail("BR not yellow/opaque", 3);
  // TR: green; transparent in the source
  if (src.tr0) {
    if (alphaOut) { if (q.q[1].a > 12) return fail("TR should be transparent", 1); }
    else if (!NearRgb(q.q[1], 255, 255, 255, tol)) return fail("TR should be flattened to white", 1);
  } else if (!NearRgb(q.q[1], 0, 255, 0, tol)) return fail("TR not green", 1);
  // BL: blue, 50 % alpha in the source
  if (src.bl128) {
    if (alphaOut && out != L"gif") { if (!NearC(q.q[2].a, 128, 16) || !NearRgb(q.q[2], 0, 0, 255, tol)) return fail("BL should be 50% blue", 2); }
    else if (out == L"gif") { if (q.q[2].a > 12 && !(q.q[2].a > 240 && NearRgb(q.q[2], 0, 0, 255, tol))) return fail("BL gif on/off alpha", 2); }
    else if (!NearRgb(q.q[2], 127, 127, 255, tol + 8)) return fail("BL should be blue flattened on white", 2);
  } else if (!NearRgb(q.q[2], 0, 0, 255, tol) || (alphaOut && q.q[2].a < 240)) return fail("BL not blue", 2);

  bool warned = HasWarning(r, L"Transparency was flattened");
  bool shouldWarn = srcAlpha && !alphaOut && out != L"pdf";
  if (warned != shouldWarn) return std::string("flatten warning ") + (warned ? "present" : "missing");
  if (in == L"jpg") {   // the stored pixels are rotated: the output must be upright (checked through the quadrant layout above)
    Json meta = (out == L"heic" || out == L"pdf") ? Json::MakeObject() : Inspect(r.out);
    if (meta.Has("orientation") && !meta.Get("orientation").IsNull() && meta.Get("orientation").AsInt() != 1) return "output still carries an EXIF orientation != 1";
  }
  return "";
}

} // namespace

TEST(registered_capabilities_are_complete) {
  filo::RegisterImageEngine();
  const std::vector<std::wstring> raster = {L"jpg", L"png", L"webp", L"heic", L"tiff", L"avif", L"bmp", L"gif"};
  size_t count = 0;
  for (auto& c : CapabilityRegistry::Get().All()) if (c.group == Group::Image) ++count;
  CHECK_EQ(count, (size_t)(8 * 7 + 8 + 8 + 1));      // raster->raster (56) + raster->pdf (8) + svg->raster (8) + svg->pdf (1)
  for (auto& in : raster) for (auto& out : raster) if (in != out) CHECK(CapabilityRegistry::Get().Find(in, out) != nullptr);
  for (auto& in : raster) CHECK(CapabilityRegistry::Get().Find(in, L"pdf") != nullptr);
  for (auto& out : raster) CHECK(CapabilityRegistry::Get().Find(L"svg", out) != nullptr);
  CHECK(CapabilityRegistry::Get().Find(L"svg", L"pdf") != nullptr);
  for (auto& c : CapabilityRegistry::Get().All()) {
    if (c.group != Group::Image) continue;
    CHECK(c.run != nullptr); CHECK(c.verify != nullptr); CHECK(!c.engine.empty());
    if (c.Runnable()) continue;
    std::printf("    NOT runnable here: %s (%ls)\n", c.id.c_str(), c.limitation.c_str());
  }
}

TEST(convert_matrix_every_capability) {
  CHECK(EnsureHeicFixtures());
  filo::RegisterImageEngine();
  int pass = 0, fail = 0, skipped = 0;
  std::vector<const Capability*> caps;
  for (auto& c : CapabilityRegistry::Get().All()) if (c.group == Group::Image && Sources().count(c.in)) caps.push_back(&c);
  for (const Capability* c : caps) {
    std::string err = RunMatrixCase(c->in, c->out);
    if (err.rfind("not runnable", 0) == 0) { Record(c->id, false, err); ++skipped; std::printf("    SKIP %s: %s\n", c->id.c_str(), err.c_str()); continue; }
    if (err.empty()) { ++pass; Record(c->id, true, "converted a real file; verified with an independent decoder (dimensions, orientation, colours, alpha)"); }
    else { ++fail; Record(c->id, false, err); std::printf("    FAIL %s: %s\n", c->id.c_str(), err.c_str()); }
  }
  std::printf("    matrix: %d pass, %d fail, %d not runnable\n", pass, fail, skipped);
  CHECK_EQ(fail, 0);
  CHECK(pass >= 70);
}

TEST(wrong_extension_is_converted_by_content) {
  CHECK(EnsureFixtures());
  fs::path d = OutDir() / "wrongext"; std::error_code ec; fs::create_directories(d, ec);
  fs::path in = d / "really_png.jpg";
  fs::copy_file(Src() / "src_png.png", in, fs::copy_options::overwrite_existing, ec);
  // The registry's DetectFile reports "png"; the capability chosen for a png input must work on this file.
  FileKind k = DetectFile(in);
  CHECK_EQ(k.format, std::wstring(L"png")); CHECK(k.extensionMismatch);
  RunOut r = RunCap(L"png", L"webp", in, d / "really_png_out.webp");
  CHECK(r.ok);
  CHECK(HasWarning(r, L"does not match"));
  Json j = Inspect(r.out); CHECK_EQ(j.Get("format").AsString(), std::string("WEBP")); CHECK_EQ((int)j.Get("w").AsInt(), 320);
}

TEST(corrupt_and_empty_inputs_fail_cleanly) {
  CHECK(EnsureFixtures());
  fs::path d = OutDir() / "corrupt"; std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d, ec);
  { std::ofstream(d / "zero.jpg", std::ios::binary); }
  Bytes jpg; ReadFileBytes(Src() / "photo.jpg", jpg); jpg.resize(4000); WriteFileBytes(d / "cut.jpg", jpg);
  Bytes junk(6000); for (size_t i = 0; i < junk.size(); ++i) junk[i] = (uint8_t)(i * 37 + 11); junk[0] = 0xFF; junk[1] = 0xD8; junk[2] = 0xFF;
  WriteFileBytes(d / "junk.jpg", junk);
  for (const char* n : {"zero.jpg", "cut.jpg", "junk.jpg"}) {
    for (const wchar_t* out : {L"png", L"webp", L"pdf"}) {
      RunOut r = RunCap(L"jpg", out, d / n, d / (std::wstring(L"o_") + Widen(n) + L"." + out));
      CHECK(!r.ok);
      CHECK(!r.error.empty());
      CHECK(r.error.find(L"0x") == std::wstring::npos);   // user-facing sentence, no raw HRESULT dump
    }
  }
  // nothing may be left behind: no outputs and no temp files
  int leftovers = 0;
  for (auto& e : fs::directory_iterator(d, ec)) { std::wstring n = e.path().filename().wstring(); if (n.rfind(L"o_", 0) == 0 || n.find(L".tmp") != std::wstring::npos) ++leftovers; }
  CHECK_EQ(leftovers, 0);
}

TEST(unicode_spaces_emoji_and_long_paths_convert) {
  CHECK(EnsureFixtures());
  fs::path d = OutDir() / L"names2"; std::error_code ec; fs::create_directories(d, ec);
  const std::vector<std::wstring> names = {L"صورة جميلة.png", L"a b  c (1).png", L"emoji \U0001F600 \U0001F680.png", L"-dash first.png", L"quote's x & more %s.png"};
  for (auto& n : names) {
    fs::path in = d / n;
    fs::copy_file(Src() / "src_png.png", in, fs::copy_options::overwrite_existing, ec);
    for (const wchar_t* out : {L"webp", L"jpg", L"avif", L"pdf"}) {
      fs::path dest = d / (n + L"." + out);
      RunOut r = RunCap(L"png", out, in, dest);
      CHECK(r.ok);
      if (!r.ok) { std::printf("    %ls -> %ls: %ls\n", n.c_str(), out, r.error.c_str()); continue; }
      CHECK(fs::exists(r.out));
    }
  }
  fs::path longDir = d;
  for (int i = 0; i < 5; ++i) longDir /= std::wstring(50, (wchar_t)(L'a' + i));
  fs::create_directories(fs::path(LongPath(longDir)), ec);
  fs::path lin = longDir / L"long input.png";
  CopyFileW(LongPath(Src() / "src_png.png").c_str(), LongPath(lin).c_str(), FALSE);
  CHECK(lin.wstring().size() > 260);
  // long INPUT path through the registry path (the output goes to a short folder)
  for (const wchar_t* out : {L"webp", L"jpg", L"avif", L"pdf", L"gif"}) {
    RunOut r = RunCap(L"png", out, lin, d / (std::wstring(L"from_long.") + out));
    CHECK(r.ok);
    if (!r.ok) std::printf("    long input -> %ls: %ls\n", out, r.error.c_str());
  }
  // long OUTPUT path straight through the engine API (OutputTransaction's own checks use plain std::filesystem calls, see report)
  for (const wchar_t* out : {L"webp", L"jpg", L"avif", L"pdf", L"gif", L"heic", L"tiff", L"png"}) {
    JobContext ctx; fs::path dest = longDir / (std::wstring(L"direct long output.") + out);
    EngineResult er = img::ConvertImageFile(lin, dest, out, img::ConvertOptions{}, ctx);
    CHECK(er.ok);
    if (!er.ok) std::printf("    long output -> %ls: %ls\n", out, er.error.c_str());
    CHECK(GetFileAttributesW(LongPath(dest).c_str()) != INVALID_FILE_ATTRIBUTES);
  }
}

TEST(metadata_survives_and_orientation_is_baked) {
  CHECK(EnsureHeicFixtures());
  fs::path in = Src() / "src_jpg.jpg";        // EXIF: Make FiloTest, DateTimeOriginal, orientation 6, PixelX/Y 240x320, XMP FiloTestXmp
  struct C { const wchar_t* out; bool xmp; };
  for (C c : {C{L"png", true}, C{L"webp", true}, C{L"tiff", false}}) {
    RunOut r = RunCap(L"jpg", c.out, in, OutDir() / (std::wstring(L"meta_keep.") + c.out));
    CHECK(r.ok);
    Json j = Inspect(r.out);
    CHECK_EQ(j.Get("make").AsString(), std::string("FiloTest"));
    CHECK_EQ(j.Get("datetime_original").AsString(), std::string("2024:05:06 07:08:09"));
    int o = (int)j.Get("orientation").AsInt(1);
    CHECK_EQ(o, 1);                                         // orientation baked into the pixels, tag reset
    if (std::wstring(c.out) != L"tiff") {
      CHECK_EQ((int)j.Get("pixel_x").AsInt(), 320); CHECK_EQ((int)j.Get("pixel_y").AsInt(), 240);   // dimension tags follow the rotation
      CHECK(j.Get("xmp_marker").AsBool() == c.xmp);
    }
    CHECK(!HasWarning(r, L"Metadata"));
    // dropping metadata on request
    Json o2 = Json::MakeObject(); o2["preserveMetadata"] = false;
    RunOut r2 = RunCap(L"jpg", c.out, in, OutDir() / (std::wstring(L"meta_drop.") + c.out), o2);
    CHECK(r2.ok);
    Json j2 = Inspect(r2.out);
    CHECK(j2.Get("make").IsNull());
    CHECK(!j2.Get("xmp_marker").AsBool());
  }
  // HEIC: EXIF tags go through the WIC metadata writer; read back with WIC
  {
    RunOut r = RunCap(L"jpg", L"heic", in, OutDir() / L"meta_keep.heic");
    CHECK(r.ok);
    ComPtr<IWICImagingFactory> f; CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> fr; ComPtr<IWICMetadataQueryReader> q;
    CHECK(SUCCEEDED(f->CreateDecoderFromFilename(r.out.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)));
    CHECK(SUCCEEDED(dec->GetFrame(0, &fr)));
    CHECK(SUCCEEDED(fr->GetMetadataQueryReader(&q)));
    PROPVARIANT v; PropVariantInit(&v);
    bool gotMake = SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=271}", &v)) && v.vt == VT_LPSTR && std::string(v.pszVal) == "FiloTest";
    CHECK(gotMake);
    PropVariantClear(&v);
    bool gotDate = SUCCEEDED(q->GetMetadataByName(L"/ifd/exif/{ushort=36867}", &v)) && v.vt == VT_LPSTR && std::string(v.pszVal) == "2024:05:06 07:08:09";
    CHECK(gotDate);
    PropVariantClear(&v);
    UINT w, h; fr->GetSize(&w, &h); CHECK_EQ((int)w, 320); CHECK_EQ((int)h, 240);
  }
  // formats that cannot carry EXIF say so
  RunOut rb = RunCap(L"jpg", L"bmp", in, OutDir() / L"meta_bmp.bmp");
  CHECK(rb.ok); CHECK(HasWarning(rb, L"Metadata"));
  RunOut ra = RunCap(L"jpg", L"avif", in, OutDir() / L"meta_avif.avif");
  CHECK(ra.ok); CHECK(HasWarning(ra, L"Metadata"));
  RunOut rg = RunCap(L"jpg", L"gif", in, OutDir() / L"meta_gif.gif");
  CHECK(rg.ok); CHECK(HasWarning(rg, L"Metadata"));
}

TEST(icc_profile_converted_and_tagged_srgb) {
  CHECK(EnsureFixtures());
  std::ifstream fin(Src() / "icc_expected.json", std::ios::binary); std::stringstream ss; ss << fin.rdbuf();
  auto exp = Json::Parse(ss.str());
  double er = exp->Get("srgb").At(0).AsNumber(), eg = exp->Get("srgb").At(1).AsNumber(), eb = exp->Get("srgb").At(2).AsNumber();
  struct C { const wchar_t* in; const char* file; const wchar_t* out; bool lossy; bool iccExpected; };
  for (C c : {C{L"png", "icc_adobe.png", L"jpg", true, true}, C{L"png", "icc_adobe.png", L"webp", true, true}, C{L"png", "icc_adobe.png", L"tiff", false, true},
              C{L"jpg", "icc_adobe.jpg", L"png", false, true}, C{L"jpg", "icc_adobe.jpg", L"bmp", false, false}, C{L"png", "icc_adobe.png", L"avif", true, false}}) {
    RunOut r = RunCap(c.in, c.out, Src() / c.file, OutDir() / (std::wstring(L"icc_") + c.in + L"_" + c.out + L"." + c.out));
    CHECK(r.ok);
    Json j = Inspect(r.out);
    Rgba q0 = Quad(j, 0);
    CHECK(NearRgb(q0, er, eg, eb, c.lossy ? 12 : 6));
    CHECK_EQ(j.Get("icc").AsBool(), c.iccExpected);
    if (c.iccExpected) CHECK(j.Get("icc_is_srgb").AsBool());
  }
}

TEST(animated_gif_webp_keep_animation) {
  CHECK(EnsureFixtures());
  if (!ff::Available()) return;
  RunOut a = RunCap(L"gif", L"webp", Src() / "anim.gif", OutDir() / L"anim_g2w.webp");
  CHECK(a.ok);
  auto mi = ff::Probe(a.out);
  CHECK(mi.has_value());
  Json j = Inspect(a.out);
  CHECK_EQ((int)j.Get("frames").AsInt(), 4);
  CHECK_EQ((int)j.Get("w").AsInt(), 160); CHECK_EQ((int)j.Get("h").AsInt(), 120);
  CHECK(!HasWarning(a, L"Animation was reduced"));
  // frame timing through ffprobe: 0.0, 0.1, 0.3, 0.6
  int code = 0;
  std::string pr = RunCapture(ff::FfprobePath(), {L"-v", L"error", L"-show_entries", L"frame=pts_time", L"-of", L"csv=p=0", a.out.wstring()}, 30000, &code);
  std::vector<double> t; { std::stringstream s(pr); std::string l; while (std::getline(s, l)) if (!l.empty()) t.push_back(atof(l.c_str())); }
  CHECK_EQ(t.size(), (size_t)4);
  if (t.size() == 4) { CHECK_NEAR(t[1], 0.1, 0.02); CHECK_NEAR(t[2], 0.3, 0.02); CHECK_NEAR(t[3], 0.6, 0.02); }

  RunOut b = RunCap(L"webp", L"gif", Src() / "anim.webp", OutDir() / L"anim_w2g.gif");
  CHECK(b.ok);
  Json jb = Inspect(b.out);
  CHECK_EQ((int)jb.Get("frames").AsInt(), 4);
  CHECK_EQ(jb.Get("durations").At(0).AsInt(), (int64_t)100); CHECK_EQ(jb.Get("durations").At(1).AsInt(), (int64_t)200);
  CHECK_EQ(jb.Get("durations").At(2).AsInt(), (int64_t)300); CHECK_EQ(jb.Get("durations").At(3).AsInt(), (int64_t)400);
  CHECK_EQ((int)jb.Get("loop").AsInt(-1), 0);
  // transparency of the animated frames survives both ways
  Bitmap fw; std::wstring err; LoadOptions lo; lo.frame = 1;
  CHECK(LoadBitmap(a.out, fw, lo, &err));
  CHECK_EQ((int)fw.Row(10)[10 * 4 + 3], 0);
  // animation cannot be kept elsewhere: first frame + a warning, never a distorted image
  for (const wchar_t* out : {L"png", L"jpg", L"tiff", L"bmp", L"pdf"}) {
    RunOut r = RunCap(L"gif", out, Src() / "anim.gif", OutDir() / (std::wstring(L"anim_first.") + out));
    CHECK(r.ok);
    CHECK(HasWarning(r, L"Animation was reduced to the first frame"));
    if (std::wstring(out) != L"pdf") { Json js = Inspect(r.out); CHECK_EQ((int)js.Get("frames").AsInt(), 1); CHECK_EQ((int)js.Get("w").AsInt(), 160); CHECK_EQ((int)js.Get("h").AsInt(), 120); }
  }
  RunOut hw = RunCap(L"webp", L"png", Src() / "anim.webp", OutDir() / L"anim_webp_first.png");
  CHECK(hw.ok); CHECK(HasWarning(hw, L"Animation was reduced"));
}

TEST(quality_and_lossless_options_have_effect) {
  CHECK(EnsureFixtures());
  auto sizeFor = [&](const wchar_t* out, int q, bool lossless, fs::path* path = nullptr) -> uintmax_t {
    Json o = Json::MakeObject(); o["quality"] = q; o["lossless"] = lossless;
    RunOut r = RunCap(L"png", out, Src() / "photo.png", OutDir() / (std::wstring(L"q_") + std::to_wstring(q) + (lossless ? L"L." : L".") + out), o);
    if (!r.ok) { std::printf("    %ls q=%d: %ls\n", out, q, r.error.c_str()); return 0; }
    if (path) *path = r.out;
    std::error_code ec; return fs::file_size(r.out, ec);
  };
  uintmax_t j30 = sizeFor(L"jpg", 30, false), j95 = sizeFor(L"jpg", 95, false);
  CHECK(j30 > 0 && j95 > j30 * 2);
  uintmax_t w30 = sizeFor(L"webp", 30, false), w95 = sizeFor(L"webp", 95, false);
  CHECK(w30 > 0 && w95 > w30);
  uintmax_t a30 = sizeFor(L"avif", 30, false), a90 = sizeFor(L"avif", 90, false);
  CHECK(a30 > 0 && a90 > a30);
  uintmax_t h30 = sizeFor(L"heic", 30, false), h95 = sizeFor(L"heic", 95, false);
  std::printf("    sizes jpg %ju/%ju webp %ju/%ju avif %ju/%ju heic %ju/%ju\n", j30, j95, w30, w95, a30, a90, h30, h95);
  CHECK(h30 > 0 && h95 > h30);
  // lossless webp must be bit-exact (decoded by WIC, independent of the encoder)
  fs::path lw; uintmax_t wl = sizeFor(L"webp", 90, true, &lw);
  CHECK(wl > 0);
  Px a, b; CHECK(WicRead(lw, a)); CHECK(WicRead(Src() / "photo.png", b));
  CHECK_EQ(a.w, b.w); CHECK_EQ(a.h, b.h);
  size_t diff = 0; if (a.bgra.size() == b.bgra.size()) for (size_t i = 0; i < a.bgra.size(); ++i) if (a.bgra[i] != b.bgra[i]) ++diff;
  CHECK_EQ(diff, (size_t)0);
  // lossless AVIF: decode with PIL (libavif) and compare with the source
  fs::path la; uintmax_t al = sizeFor(L"avif", 90, true, &la);
  CHECK(al > 0);
  {
    fs::path script = OutDir() / "cmp_lossless.py";
    { std::ofstream o(script); o << "import sys\nimport numpy as np\nfrom PIL import Image\na=np.asarray(Image.open(sys.argv[1]).convert('RGB'),int)\nb=np.asarray(Image.open(sys.argv[2]).convert('RGB'),int)\nprint(a.shape==b.shape, int(np.abs(a-b).max()) if a.shape==b.shape else -1)\n"; }
    std::string out = RunPy({script.wstring(), la.wstring(), (Src() / "photo.png").wstring()});
    std::printf("    lossless avif vs source (same shape, max abs diff): %s", out.c_str());
    CHECK(out.rfind("True 0", 0) == 0);
  }
  // lossless HEIC
  fs::path lh; uintmax_t hl = sizeFor(L"heic", 90, true, &lh);
  CHECK(hl > 0);
  Px c; CHECK(WicRead(lh, c)); CHECK_EQ(c.w, b.w);
  size_t maxd = 0; if (c.bgra.size() == b.bgra.size()) for (size_t i = 0; i < c.bgra.size(); ++i) maxd = std::max<size_t>(maxd, (size_t)std::abs((int)c.bgra[i] - (int)b.bgra[i]));
  std::printf("    lossless heic max abs diff vs source: %zu\n", maxd);
  CHECK(maxd <= 3);
}

TEST(odd_and_extreme_sizes_are_never_distorted) {
  CHECK(EnsureHeicFixtures());
  struct C { const char* file; int w, h; };
  for (C c : {C{"odd_321x241.png", 321, 241}, C{"one_px.png", 1, 1}, C{"wide_4000x30.png", 4000, 30}}) {
    for (const wchar_t* out : {L"jpg", L"webp", L"heic", L"tiff", L"avif", L"bmp", L"gif", L"pdf"}) {
      if (Widen(c.file) == L"one_px.png" && std::wstring(out) == L"heic") continue;   // HEVC cannot encode a 1x1 picture (documented)
      RunOut r = RunCap(L"png", out, Src() / c.file, OutDir() / (std::wstring(L"sz_") + Widen(c.file) + L"." + out));
      if (!r.ok) { std::printf("    %s -> %ls: %ls\n", c.file, out, r.error.c_str()); CHECK(false); continue; }
      int w = 0, h = 0;
      if (std::wstring(out) == L"pdf") { double wp, hp; CHECK(RenderPdfPage(r.out, 0, 0, 0, nullptr, &wp, &hp)); CHECK_NEAR(wp, c.w * 0.75, 0.6); CHECK_NEAR(hp, c.h * 0.75, 0.6); continue; }
      if (std::wstring(out) == L"heic") { Px px; CHECK(WicRead(r.out, px)); w = px.w; h = px.h; }
      else { Json j = Inspect(r.out); w = (int)j.Get("w").AsInt(); h = (int)j.Get("h").AsInt(); }
      if (w != c.w || h != c.h) std::printf("    %s -> %ls gave %dx%d (expected %dx%d)\n", c.file, out, w, h, c.w, c.h);
      CHECK_EQ(w, c.w); CHECK_EQ(h, c.h);
    }
  }
}

TEST(cancellation_leaves_nothing_behind) {
  CHECK(EnsureFixtures());
  if (!ff::Available()) return;
  filo::RegisterImageEngine();
  const Capability* cap = CapabilityRegistry::Get().Find(L"png", L"avif");
  if (!cap || !cap->Runnable()) return;
  fs::path d = OutDir() / "cancel"; std::error_code ec; fs::remove_all(d, ec); fs::create_directories(d, ec);
  fs::path dest = d / "photo_cancel.avif";
  Json o = Json::MakeObject(); o["quality"] = 95;
  JobSpec spec; spec.operation = L"convert"; spec.title = L"cancel test"; spec.inputs = {Src() / "photo.png"};
  spec.work = [=](JobContext& ctx) { fs::path dd = dest; return RunConversion(*cap, Src() / "photo.png", o, ctx, &dd); };
  uint64_t id = JobManager::Get().Submit(std::move(spec));
  Sleep(400);
  JobManager::Get().Cancel(id);
  JobManager::Get().WaitFor(id);
  auto info = JobManager::Get().Info(id);
  CHECK(info.has_value());
  CHECK(info->state == JobState::Cancelled || info->state == JobState::Failed || info->state == JobState::Completed);
  if (info->state != JobState::Completed) {
    CHECK(!fs::exists(dest));
    int leftovers = 0; for (auto& e : fs::directory_iterator(d, ec)) { (void)e; ++leftovers; }
    CHECK_EQ(leftovers, 0);
  }
  std::printf("    cancel result state: %ls\n", JobStateName(info->state));
}

TEST(ffmpeg_paths_with_unicode_and_odd_names_are_staged_safely) {
  CHECK(EnsureFixtures());
  if (!ff::Available()) return;
  fs::path d = OutDir() / L"ffnames"; std::error_code ec; fs::create_directories(d, ec);
  const std::vector<std::wstring> names = {L"رسوم متحركة \U0001F600", L"100% sure & [odd] 'name'", L"pipe+colon;semi"};
  for (auto& stem : names) {
    fs::path gif = d / (stem + L".gif"), avif = d / (stem + L".avif");
    fs::copy_file(Src() / "anim.gif", gif, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(Src() / "src_avif.avif", avif, fs::copy_options::overwrite_existing, ec);
    RunOut a = RunCap(L"gif", L"webp", gif, d / (stem + L".out.webp"));
    CHECK(a.ok); if (!a.ok) std::printf("    animated gif -> webp (%ls): %ls\n", stem.c_str(), a.error.c_str());
    RunOut b = RunCap(L"avif", L"png", avif, d / (stem + L".out.png"));
    CHECK(b.ok); if (!b.ok) std::printf("    avif -> png (%ls): %ls\n", stem.c_str(), b.error.c_str());
    if (b.ok) { Json j = Inspect(b.out); CHECK_EQ((int)j.Get("w").AsInt(), 320); CHECK(NearC(Quad(j, 1).a, 0, 5)); }   // alpha merged
  }
}

TEST(capabilities_report_availability_honestly) {
  filo::RegisterImageEngine();
  int runnable = 0, total = 0;
  for (auto& c : CapabilityRegistry::Get().All()) {
    if (c.group != Group::Image) continue;
    ++total; if (c.Runnable()) ++runnable;
  }
  std::printf("    %d of %d image capabilities runnable on this machine (heic encode=%d decode=%d, avif encode=%d, webp encode=%d)\n", runnable, total,
              (int)img::CanEncode(L"heic"), (int)img::CanDecode(L"heic"), (int)img::CanEncode(L"avif"), (int)img::CanEncode(L"webp"));
  // availability must follow the real codecs, not be hard-coded
  const Capability* toHeic = CapabilityRegistry::Get().Find(L"png", L"heic");
  CHECK(toHeic != nullptr);
  CHECK_EQ(toHeic->Runnable(), img::CanEncode(L"heic") && img::CanDecode(L"png"));
  const Capability* fromHeic = CapabilityRegistry::Get().Find(L"heic", L"png");
  CHECK_EQ(fromHeic->Runnable(), img::CanDecode(L"heic"));
}
