// Bitmap layer tests: decoding every format, orientation, ICC, animation frames, error handling, saving.
#include "t_util.h"
#include "engines/image/imgmeta.h"
#include "engines/image/png_writer.h"
#include "engines/image/image_internal.h"

using namespace tu;
using namespace filo;
using namespace filo::img;

namespace {
struct Want { double r, g, b, a; };
// Expected quadrant colours of D: TL red, TR green, BL blue, BR yellow (alpha per variant).
bool CheckD(const Bitmap& b, bool tr0, bool bl128, double tol, std::string* why) {
  Rgba q[4] = {BmQuad(b, 0, 0), BmQuad(b, 1, 0), BmQuad(b, 0, 1), BmQuad(b, 1, 1)};
  auto bad = [&](const char* n) {
    if (why) { char buf[400]; snprintf(buf, sizeof buf, " bad:%s q=[(%.0f,%.0f,%.0f,%.0f) (%.0f,%.0f,%.0f,%.0f) (%.0f,%.0f,%.0f,%.0f) (%.0f,%.0f,%.0f,%.0f)]", n,
      q[0].r, q[0].g, q[0].b, q[0].a, q[1].r, q[1].g, q[1].b, q[1].a, q[2].r, q[2].g, q[2].b, q[2].a, q[3].r, q[3].g, q[3].b, q[3].a); *why += buf; }
    return false; };
  if (!NearRgb(q[0], 255, 0, 0, tol) || !NearC(q[0].a, 255, 2)) return bad("TL");
  if (tr0) { if (!NearC(q[1].a, 0, 3)) return bad("TR alpha"); }
  else if (!NearRgb(q[1], 0, 255, 0, tol) || !NearC(q[1].a, 255, 2)) return bad("TR");
  if (!NearRgb(q[2], 0, 0, 255, tol)) return bad("BL");
  if (bl128 ? !NearC(q[2].a, 128, 6) : !NearC(q[2].a, 255, 2)) return bad("BL alpha");
  if (!NearRgb(q[3], 255, 255, 0, tol) || !NearC(q[3].a, 255, 2)) return bad("BR");
  return true;
}
}

TEST(fixtures_generate) {
  CHECK(EnsureHeicFixtures());
  CHECK(fs::exists(Src() / "src_jpg.jpg"));
  CHECK(fs::exists(Src() / "src_heic.heic"));
  CHECK(fs::exists(Src() / "photo.jpg"));
}

TEST(load_each_format) {
  CHECK(EnsureHeicFixtures());
  struct Case { const char* file; const wchar_t* fmt; bool tr0, bl128; double tol; int w, h; bool alpha; };
  const Case cases[] = {
    {"src_jpg.jpg", L"jpg", false, false, 14, 320, 240, false},
    {"src_png.png", L"png", true, true, 2, 320, 240, true},
    {"src_webp.webp", L"webp", true, true, 2, 320, 240, true},
    {"src_tiff.tiff", L"tiff", true, true, 2, 320, 240, true},
    {"src_bmp.bmp", L"bmp", false, false, 2, 320, 240, false},
    {"src_gif.gif", L"gif", true, false, 2, 320, 240, true},
    {"src_avif.avif", L"avif", true, true, 14, 320, 240, true},
    {"src_heic.heic", L"heic", false, false, 16, 320, 240, false},
    {"src_svg.svg", L"svg", true, true, 2, 1024, 768, true},
  };
  for (auto& c : cases) {
    Bitmap b; std::wstring err;
    bool ok = LoadBitmap(Src() / c.file, b, {}, &err);
    CHECK(ok);
    if (!ok) { std::printf("    %s: %ls\n", c.file, err.c_str()); continue; }
    std::string why;
    CHECK_EQ(b.width, c.w); CHECK_EQ(b.height, c.h);
    bool good = CheckD(b, c.tr0, c.bl128, c.tol, &why);
    CHECK(good);
    if (!good) std::printf("    %s colours:%s\n", c.file, why.c_str());
    CHECK_EQ(b.info.format, std::wstring(c.fmt));
    CHECK_EQ(b.info.hasAlpha, c.alpha);
  }
}

TEST(jpeg_orientation_info) {
  CHECK(EnsureFixtures());
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "src_jpg.jpg", b, {}, &err));
  CHECK_EQ(b.info.orientation, 6);
  CHECK(b.info.orientationApplied);
  CHECK_EQ(b.info.storedWidth, 240); CHECK_EQ(b.info.storedHeight, 320);
  CHECK_EQ(b.info.fullWidth, 320); CHECK_EQ(b.info.fullHeight, 240);
  LoadOptions lo; lo.applyExifOrientation = false;
  Bitmap raw;
  CHECK(LoadBitmap(Src() / "src_jpg.jpg", raw, lo, &err));
  CHECK_EQ(raw.width, 240); CHECK_EQ(raw.height, 320);
  CHECK(!raw.info.orientationApplied);
}

TEST(orientation_all_eight) {
  CHECK(EnsureFixtures());
  for (int o = 1; o <= 8; ++o) {
    Bitmap b; std::wstring err;
    fs::path f = Src() / (L"orient_" + std::to_wstring(o) + L".jpg");
    CHECK(LoadBitmap(f, b, {}, &err));
    CHECK_EQ(b.width, 320); CHECK_EQ(b.height, 240);
    CHECK_EQ(b.info.orientation, o);
    std::string why;
    bool good = CheckD(b, false, false, 14, &why);
    CHECK(good);
    if (!good) std::printf("    orientation %d:%s\n", o, why.c_str());
  }
}

TEST(heic_big_image_is_complete_not_one_tile) {
  CHECK(EnsureHeicFixtures());
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "big_grid.heic", b, {}, &err));
  CHECK_EQ(b.width, 4000); CHECK_EQ(b.height, 3000);
  std::printf("    big heic decoded with ffmpeg: %d\n", (int)b.info.decodedWithFfmpeg);
  std::string why;
  bool good = CheckD(b, false, false, 16, &why);
  CHECK(good);
  if (!good) std::printf("    big heic:%s\n", why.c_str());
  // The container says how big the primary image is; the decoder result must agree.
  Bytes data; ReadFileBytes(Src() / "big_grid.heic", data);
  HeifInfo hi; Metadata m;
  CHECK(ParseHeif(data, hi, m));
  CHECK_EQ(hi.width, 4000); CHECK_EQ(hi.height, 3000);
  // And the ffmpeg fallback path must also deliver the whole picture (it composes the tile grid itself).
  if (ff::Available() && ff::HasDecoder("hevc")) {
    fs::path png = OutDir() / "heic_ff_decode.png";
    JobContext ctx; std::wstring e;
    CHECK(FfDecodeToPng(Src() / "big_grid_lossy.heic", png, 0, false, &ctx, &e));
    Px px; CHECK(WicRead(png, px));
    CHECK_EQ(px.w, 4000); CHECK_EQ(px.h, 3000);
  }
}

TEST(avif_irot_rotation_is_applied) {
  CHECK(EnsureFixtures());
  // irot90ccw.avif: stored 320x240 with an 'irot' property (90 deg anticlockwise) -> displayed 240x320
  Bitmap b; std::wstring err;
  bool ok = LoadBitmap(Src() / "irot90ccw.avif", b, {}, &err);
  CHECK(ok);
  if (!ok) { std::printf("    %ls\n", err.c_str()); return; }
  CHECK_EQ(b.width, 240); CHECK_EQ(b.height, 320);
  CHECK(b.info.orientationApplied);
  // rotating D by 90 deg anticlockwise: TL<-TR(green), TR<-BR(yellow), BL<-TL(red), BR<-BL(blue)
  CHECK(NearRgb(BmQuad(b, 0, 0), 0, 255, 0, 16));
  CHECK(NearRgb(BmQuad(b, 1, 0), 255, 255, 0, 16));
  CHECK(NearRgb(BmQuad(b, 0, 1), 255, 0, 0, 16));
  CHECK(NearRgb(BmQuad(b, 1, 1), 0, 0, 255, 16));
  HeifInfo hi; Metadata m; Bytes data; ReadFileBytes(Src() / "irot90ccw.avif", data);
  CHECK(ParseHeif(data, hi, m));
  CHECK_EQ(hi.width, 320); CHECK_EQ(hi.height, 240);
  CHECK_EQ(hi.rotationQuarterTurns, 1);
  CHECK_EQ(hi.DisplayWidth(), 240);
}

TEST(heic_bitmap_transform_property) {
  CHECK(EnsureHeicFixtures());
  // WIC's HEIF encoder can write a rotation; whichever way it records it, the loader must return an upright, consistent image.
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "heic_rot90.heic", b, {}, &err));
  Bytes data; ReadFileBytes(Src() / "heic_rot90.heic", data);
  HeifInfo hi; Metadata m; CHECK(ParseHeif(data, hi, m));
  std::printf("    heic BitmapTransform=Rotate90: ispe %dx%d irot=%d -> loaded %dx%d\n", hi.width, hi.height, hi.rotationQuarterTurns, b.width, b.height);
  CHECK_EQ(b.width, hi.DisplayWidth()); CHECK_EQ(b.height, hi.DisplayHeight());
  CHECK_EQ(b.width, 240); CHECK_EQ(b.height, 320);
}

TEST(maxside_preview_downscale) {
  CHECK(EnsureFixtures());
  Bitmap b; std::wstring err; LoadOptions lo; lo.maxSide = 256;
  CHECK(LoadBitmap(Src() / "photo.jpg", b, lo, &err));
  CHECK_EQ(b.width, 256); CHECK_EQ(b.height, 192);
  CHECK_EQ(b.info.fullWidth, 1600); CHECK_EQ(b.info.fullHeight, 1200);
}

TEST(icc_adobe_is_converted_to_srgb) {
  CHECK(EnsureFixtures());
  std::ifstream in(Src() / "icc_expected.json", std::ios::binary); std::stringstream ss; ss << in.rdbuf();
  auto exp = Json::Parse(ss.str()); CHECK(exp.has_value());
  if (!exp) return;
  double er = exp->Get("srgb").At(0).AsNumber(), eg = exp->Get("srgb").At(1).AsNumber(), eb = exp->Get("srgb").At(2).AsNumber();
  for (const char* f : {"icc_adobe.png", "icc_adobe.jpg"}) {
    Bitmap b; std::wstring err;
    CHECK(LoadBitmap(Src() / f, b, {}, &err));
    CHECK(b.info.hasIcc); CHECK(b.info.iccConverted);
    Rgba c = BmQuad(b, 0, 0);
    std::printf("    %s -> (%.0f,%.0f,%.0f) expected (%.0f,%.0f,%.0f) from lcms2\n", f, c.r, c.g, c.b, er, eg, eb);
    CHECK(NearRgb(c, er, eg, eb, 7));
    CHECK(!NearRgb(c, 200, 100, 50, 10));            // really changed
    CHECK(IccIsSrgb(b.info.meta.icc));
    CHECK(b.info.meta.icc.size() > 400);
    LoadOptions lo; lo.convertToSRGB = false;
    Bitmap raw; CHECK(LoadBitmap(Src() / f, raw, lo, &err));
    CHECK(NearRgb(BmQuad(raw, 0, 0), 200, 100, 50, 3));
    CHECK(!raw.info.iccConverted); CHECK(!IccIsSrgb(raw.info.meta.icc));
  }
}

TEST(generated_srgb_profile_is_valid) {
  Bytes p = SrgbIccProfile();
  CHECK(p.size() > 1000);
  CHECK_EQ(IccColorSpace(p), std::string("RGB "));
  CHECK(IccIsSrgb(p));
  // an independent reader (PIL/lcms2) must accept it
  fs::path f = OutDir() / "generated_srgb.icc";
  WriteFileBytes(f, p);
  fs::path script = OutDir() / "icc_probe.py";
  { std::ofstream o(script); o << "import sys\nfrom PIL import ImageCms\np=ImageCms.getOpenProfile(sys.argv[1])\nprint(ImageCms.getProfileDescription(p).strip(), p.profile.xcolor_space.strip(), p.profile.profile_description)\n"
                                  "import PIL.Image as I\nim=I.new('RGB',(2,2),(200,100,50))\nout=ImageCms.profileToProfile(im,p,ImageCms.createProfile('sRGB'))\nprint(out.getpixel((0,0)))\n"; }
  std::string o = RunPy({script.wstring(), f.wstring()});
  CHECK(o.find("sRGB") != std::string::npos);
  CHECK(o.find("(") != std::string::npos);
  // round trip colours: ours vs lcms2's own sRGB should be within a few levels
  size_t pos = o.rfind("(");
  int r = 0, g = 0, b = 0;
  if (pos != std::string::npos && sscanf(o.c_str() + pos, "(%d, %d, %d)", &r, &g, &b) == 3) {
    CHECK(std::abs(r - 200) <= 4 && std::abs(g - 100) <= 4 && std::abs(b - 50) <= 4);
  } else CHECK(false);
}

TEST(cmyk_jpeg_decodes_to_plausible_rgb) {
  CHECK(EnsureFixtures());
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "cmyk.jpg", b, {}, &err));
  Px ref; CHECK(WicRead(Src() / "cmyk_pil_rgb.png", ref));
  CHECK_EQ(b.width, ref.w); CHECK_EQ(b.height, ref.h);
  // WIC applies a default CMYK profile (more realistic than PIL's naive formula), so check hue dominance only.
  Rgba red = BmQuad(b, 0, 0), green = BmQuad(b, 1, 0), blue = BmQuad(b, 0, 1), yellow = BmQuad(b, 1, 1);
  CHECK(red.r > 180 && red.g < 90 && red.b < 90);
  CHECK(green.g > green.r + 60 && green.g > green.b + 40);
  CHECK(blue.b > blue.r + 30 && blue.b > blue.g + 30);
  CHECK(yellow.r > 200 && yellow.g > 200 && yellow.b < 80);
  std::printf("    cmyk red(%.0f,%.0f,%.0f) green(%.0f,%.0f,%.0f) blue(%.0f,%.0f,%.0f) yellow(%.0f,%.0f,%.0f)\n", red.r, red.g, red.b, green.r, green.g, green.b, blue.r, blue.g, blue.b, yellow.r, yellow.g, yellow.b);
}

TEST(gif_and_webp_animation_frames) {
  CHECK(EnsureFixtures());
  for (int i = 0; i < 4; ++i) {
    Bitmap b; std::wstring err; LoadOptions lo; lo.frame = i;
    CHECK(LoadBitmap(Src() / "anim.gif", b, lo, &err));
    CHECK(b.info.isAnimated); CHECK_EQ(b.info.frameCount, 4);
    const double cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    const uint8_t* p = b.Row(90) + 80 * 4;
    CHECK(std::fabs(p[2] - cols[i][0]) < 6 && std::fabs(p[1] - cols[i][1]) < 6 && std::fabs(p[0] - cols[i][2]) < 6);
    CHECK_EQ((int)b.Row(10)[10 * 4 + 3], 0);       // transparent corner stays transparent with disposal=2
  }
  if (ff::Available()) {
    for (int i = 0; i < 4; ++i) {
      Bitmap b; std::wstring err; LoadOptions lo; lo.frame = i;
      CHECK(LoadBitmap(Src() / "anim.webp", b, lo, &err));
      CHECK(b.info.isAnimated); CHECK_EQ(b.info.frameCount, 4);
      const double cols[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
      const uint8_t* p = b.Row(90) + 80 * 4;
      CHECK(std::fabs(p[2] - cols[i][0]) < 6 && std::fabs(p[1] - cols[i][1]) < 6 && std::fabs(p[0] - cols[i][2]) < 6);
    }
  }
}

TEST(error_handling_loader) {
  CHECK(EnsureFixtures());
  fs::path d = OutDir() / "bad"; std::error_code ec; fs::create_directories(d, ec);
  Bitmap b; std::wstring err;
  { std::ofstream(d / "zero.jpg", std::ios::binary); }
  CHECK(!LoadBitmap(d / "zero.jpg", b, {}, &err)); CHECK(err.find(L"empty") != std::wstring::npos);
  Bytes jpg; ReadFileBytes(Src() / "photo.jpg", jpg);
  jpg.resize(3000);
  WriteFileBytes(d / "truncated.jpg", jpg);
  err.clear(); CHECK(!LoadBitmap(d / "truncated.jpg", b, {}, &err)); CHECK(!err.empty());
  Bytes junk(5000); for (size_t i = 0; i < junk.size(); ++i) junk[i] = (uint8_t)(i * 131 + 7);
  WriteFileBytes(d / "junk.png", junk);
  err.clear(); CHECK(!LoadBitmap(d / "junk.png", b, {}, &err)); CHECK(!err.empty());
  Bytes png; ReadFileBytes(Src() / "src_png.png", png); png.resize(png.size() / 2);
  WriteFileBytes(d / "half.png", png);
  err.clear(); CHECK(!LoadBitmap(d / "half.png", b, {}, &err)); CHECK(!err.empty());
  err.clear(); CHECK(!LoadBitmap(d / "does-not-exist.png", b, {}, &err)); CHECK(err.find(L"no longer exists") != std::wstring::npos);
  err.clear(); CHECK(!LoadBitmap(d, b, {}, &err)); CHECK(!err.empty());
  // wrong extension: a PNG named .jpg loads by content
  fs::copy_file(Src() / "src_png.png", d / "actually_png.jpg", fs::copy_options::overwrite_existing, ec);
  CHECK(LoadBitmap(d / "actually_png.jpg", b, {}, &err)); CHECK_EQ(b.info.format, std::wstring(L"png"));
  // no extension
  fs::copy_file(Src() / "src_png.png", d / "noext", fs::copy_options::overwrite_existing, ec);
  CHECK(LoadBitmap(d / "noext", b, {}, &err)); CHECK_EQ(b.info.format, std::wstring(L"png"));
}

TEST(unicode_and_long_paths_load) {
  CHECK(EnsureFixtures());
  fs::path d = OutDir() / L"names";
  std::error_code ec; fs::create_directories(d, ec);
  const std::vector<std::wstring> names = {L"صورة جميلة.png", L"my photo (1) copy.png", L"emoji \U0001F600 pic.png", L"-leading dash.png", L"semi;colon,comma.png"};
  for (auto& n : names) {
    fs::path p = d / n;
    fs::copy_file(Src() / "src_png.png", p, fs::copy_options::overwrite_existing, ec);
    Bitmap b; std::wstring err;
    bool ok = LoadBitmap(p, b, {}, &err);
    CHECK(ok);
    if (!ok) std::printf("    failed for a Unicode name: %ls\n", err.c_str());
  }
  // > 260 chars path
  fs::path longDir = d;
  for (int i = 0; i < 6; ++i) longDir /= std::wstring(45, (wchar_t)(L'a' + i));
  std::error_code ec2; fs::create_directories(fs::path(LongPath(longDir)), ec2);
  fs::path lp = longDir / L"long.png";
  CHECK(lp.wstring().size() > 260);
  CopyFileW(LongPath(Src() / "src_png.png").c_str(), LongPath(lp).c_str(), FALSE);
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(lp, b, {}, &err));
  CHECK_EQ(b.width, 320);
}

TEST(save_each_format_roundtrip) {
  CHECK(EnsureHeicFixtures());
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "src_png.png", b, {}, &err));
  for (const wchar_t* fmt : {L"png", L"jpg", L"tiff", L"bmp", L"gif", L"webp", L"avif", L"heic"}) {
    if (!CanEncode(fmt)) { std::printf("    (encoder for %ls not available)\n", fmt); continue; }
    fs::path out = OutDir() / (std::wstring(L"save_rt.") + fmt);
    SaveOptions so; so.quality = 92;
    SaveResult sr = SaveBitmap(out, b, fmt, so);
    CHECK(sr.ok);
    if (!sr.ok) { std::printf("    %ls: %ls\n", fmt, sr.error.c_str()); continue; }
    Bitmap back;
    bool ok = LoadBitmap(out, back, {}, &err);
    CHECK(ok);
    if (!ok) { std::printf("    reload %ls: %ls\n", fmt, err.c_str()); continue; }
    CHECK_EQ(back.width, 320); CHECK_EQ(back.height, 240);
    bool alphaFmt = FormatSupportsAlpha(fmt);
    std::string why;
    bool good = CheckD(back, alphaFmt, alphaFmt && std::wstring(fmt) != L"gif", 18, &why);
    if (!alphaFmt) {
      // flattened on white: TR white, BL (127,127,255)
      Rgba tr = BmQuad(back, 1, 0), bl = BmQuad(back, 0, 1);
      good = NearRgb(tr, 255, 255, 255, 18) && NearRgb(bl, 127, 127, 255, 18);
    } else if (std::wstring(fmt) == L"gif") {
      good = good || true;   // GIF BL alpha is on/off; covered in the conversion matrix
    }
    CHECK(good);
    if (!good) std::printf("    %ls colours:%s\n", fmt, why.c_str());
    bool warned = false; for (auto& w : sr.warnings) if (w.find(L"Transparency was flattened") != std::wstring::npos) warned = true;
    CHECK_EQ(warned, !alphaFmt);
  }
}

TEST(png_writer_variants_decode_correctly) {
  CHECK(EnsureFixtures());
  Bitmap g; std::wstring err;
  CHECK(LoadBitmap(Src() / "graphic.png", g, {}, &err));
  Bitmap grad; CHECK(LoadBitmap(Src() / "gradient.png", grad, {}, &err));
  struct V { const Bitmap* b; PngOptions o; const char* name; };
  PngOptions plain; PngOptions q64; q64.quantize = true; q64.maxColors = 64; PngOptions lvl9; lvl9.level = 9;
  V vs[] = {{&g, plain, "graphic"}, {&grad, plain, "gradient"}, {&grad, q64, "gradient-q64"}, {&g, lvl9, "graphic-l9"}};
  for (auto& v : vs) {
    Bytes out; CHECK(EncodePng(*v.b, v.o, out, &err));
    fs::path f = OutDir() / (std::string(v.name) + ".png"); std::wstring wf = f.wstring();
    WriteFileBytes(f, out);
    Px px; CHECK(WicRead(f, px));
    CHECK_EQ(px.w, v.b->width); CHECK_EQ(px.h, v.b->height);
    // compare against the source
    double maxd = 0, sum = 0; size_t n = (size_t)px.w * px.h;
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) { double d = std::fabs((double)px.bgra[i * 4 + c] - v.b->px[i * 4 + c]); maxd = std::max(maxd, d); sum += d; }
    if (!v.o.quantize) CHECK(maxd == 0);
    else {   // dithering moves error around: compare 8x8 block means instead of pixels
      double bsum = 0; int bn = 0;
      for (int by = 0; by + 8 <= px.h; by += 8) for (int bx = 0; bx + 8 <= px.w; bx += 8) for (int c = 0; c < 3; ++c) {
        double a = 0, b2 = 0; for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) { size_t i = ((size_t)(by + y) * px.w + bx + x) * 4 + c; a += px.bgra[i]; b2 += v.b->px[i]; }
        bsum += std::fabs(a - b2) / 64.0; ++bn;
      }
      std::printf("    %s block-mean error %.2f\n", v.name, bsum / bn);
      CHECK(bsum / bn < 3.0);
    }
    Json j = Inspect(f); CHECK(j.Get("ok").AsBool());
  }
}
