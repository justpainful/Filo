// SVG rasterising and other edge cases (multi-page TIFF, 16-bit PNG, big images, encoder limits).
#include "t_util.h"
#include "engines/image/image_convert.h"
#include "engines/image/image_internal.h"
#include "engines/image/imgmeta.h"

using namespace tu;
using namespace filo;
using namespace filo::img;

TEST(svg_sizes_units_and_transparency) {
  CHECK(EnsureFixtures());
  struct C { const char* file; int w, h; };
  // long side = max(intrinsic, 1024), at most 4096; aspect ratio from width/height/viewBox
  for (C c : {C{"tiny_icon.svg", 1024, 1024}, C{"src_svg.svg", 1024, 768}, C{"svg_percent_only.svg", 1024, 512}, C{"svg_units.svg", 1024, 512}, C{"svg_px.svg", 1024, 512}, C{"svg_huge.svg", 4096, 2048}}) {
    Bitmap b; std::wstring err;
    bool ok = LoadBitmap(Src() / c.file, b, {}, &err);
    CHECK(ok);
    if (!ok) { std::printf("    %s: %ls\n", c.file, err.c_str()); continue; }
    CHECK_EQ(b.width, c.w); CHECK_EQ(b.height, c.h);
    CHECK(b.info.isVector);
  }
  Bitmap icon; std::wstring err; CHECK(LoadBitmap(Src() / "tiny_icon.svg", icon, {}, &err));
  CHECK_EQ(icon.info.storedWidth, 24); CHECK_EQ(icon.info.storedHeight, 24);
  CHECK_EQ((int)icon.Row(2)[2 * 4 + 3], 0);                               // corners are transparent
  const uint8_t* c = icon.Row(512) + 512 * 4;                              // centre is the pink disc
  CHECK(std::abs(c[2] - 0xE9) < 3 && std::abs(c[1] - 0x1E) < 3 && std::abs(c[0] - 0x63) < 3 && c[3] == 255);
  // explicit size
  LoadOptions lo; lo.svgLongSide = 300;
  Bitmap s300; CHECK(LoadBitmap(Src() / "src_svg.svg", s300, lo, &err)); CHECK_EQ(s300.width, 300); CHECK_EQ(s300.height, 225);
  // colour check of a sized SVG
  Bitmap u; CHECK(LoadBitmap(Src() / "svg_units.svg", u, {}, &err));
  const uint8_t* uc = u.Row(256) + 512 * 4; CHECK(std::abs(uc[2] - 0xCC) < 3 && std::abs(uc[1] - 0x33) < 3 && std::abs(uc[0] - 0x33) < 3);
}

TEST(svg_text_is_a_known_limitation_and_is_reported) {
  CHECK(EnsureFixtures());
  // Direct2D's SVG renderer has no <text> support: the shapes render, the text does not, and the user is told.
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "arabic_text.svg", b, {}, &err));
  CHECK_EQ(b.width, 1024); CHECK_EQ(b.height, 307);
  bool warned = false; for (auto& w : b.info.warnings) if (w.find(L"contains text") != std::wstring::npos) warned = true;
  CHECK(warned);
  RunOut r = RunCap(L"svg", L"png", Src() / "arabic_text.svg", OutDir() / L"svg_text.png");
  CHECK(r.ok); CHECK(HasWarning(r, L"contains text"));
}

TEST(svg_external_references_and_scripts_are_ignored_and_broken_files_fail_cleanly) {
  CHECK(EnsureFixtures());
  Bitmap b; std::wstring err;
  double t0 = NowSeconds();
  CHECK(LoadBitmap(Src() / "svg_external.svg", b, {}, &err));                // must not touch the network (the URL points at a closed local port)
  double dt = NowSeconds() - t0;
  CHECK(dt < 15.0);
  const uint8_t* centre = b.Row(b.height / 2) + (b.width / 2) * 4;
  CHECK(centre[0] > 200 && centre[2] < 60);                                  // the blue circle (BGRA: B high)
  Bitmap bad; err.clear();
  CHECK(!LoadBitmap(Src() / "svg_broken.svg", bad, {}, &err));
  CHECK(!err.empty());
  // renamed: svg content in a .png file
  fs::path d = OutDir() / "svgname"; std::error_code ec; fs::create_directories(d, ec);
  fs::copy_file(Src() / "src_svg.svg", d / "vector.png", fs::copy_options::overwrite_existing, ec);
  Bitmap r; CHECK(LoadBitmap(d / "vector.png", r, {}, &err)); CHECK_EQ(r.info.format, std::wstring(L"svg"));
}

TEST(svg_to_pdf_page_size_follows_the_intrinsic_size) {
  CHECK(EnsureFixtures());
  RunOut r = RunCap(L"svg", L"pdf", Src() / "tiny_icon.svg", OutDir() / L"icon.pdf");
  CHECK(r.ok);
  double w, h; Px px;
  CHECK(RenderPdfPage(r.out, 0, 96, 96, &px, &w, &h));
  CHECK_NEAR(w, 18, 0.3); CHECK_NEAR(h, 18, 0.3);                            // 24 css px = 18 pt
  Rgba centre = PxQuad(px, 0, 0); (void)centre;
  const uint8_t* c = &px.bgra[((size_t)48 * 96 + 48) * 4];
  CHECK(std::abs(c[2] - 0xE9) < 6 && std::abs(c[1] - 0x1E) < 6);
}

TEST(multipage_tiff_converts_first_page_with_a_warning) {
  CHECK(EnsureFixtures());
  RunOut r = RunCap(L"tiff", L"png", Src() / "multipage.tiff", OutDir() / L"mp_first.png");
  CHECK(r.ok);
  CHECK(HasWarning(r, L"first page"));
  Json j = Inspect(r.out);
  CHECK(NearRgb(Quad(j, 0), 255, 0, 0, 3));
  Bitmap b; std::wstring err; LoadOptions lo; lo.frame = 1;
  CHECK(LoadBitmap(Src() / "multipage.tiff", b, lo, &err));
  CHECK_EQ(b.info.frameCount, 2);
  CHECK(NearRgb(BmQuad(b, 0, 0), 10, 10, 10, 2));
}

TEST(sixteen_bit_gray_png_maps_to_eight_bit) {
  CHECK(EnsureFixtures());
  Bitmap b; std::wstring err;
  CHECK(LoadBitmap(Src() / "gray16.png", b, {}, &err));
  const uint8_t* l = b.Row(30) + 10 * 4; const uint8_t* r = b.Row(30) + 70 * 4;
  std::printf("    16-bit gray left %d right %d (expected ~32 / ~192)\n", l[2], r[2]);
  CHECK(std::abs(l[2] - 32) <= 3); CHECK(std::abs(r[2] - 192) <= 3);
  RunOut o = RunCap(L"png", L"jpg", Src() / "gray16.png", OutDir() / L"gray16.jpg");
  CHECK(o.ok);
  Json j = Inspect(o.out); CHECK_EQ(j.Get("mode").AsString(), std::string("L"));      // grayscale JPEG
}

TEST(twelve_megapixel_jpeg_converts_everywhere) {
  CHECK(EnsureHeicFixtures());
  fs::path in = Src() / "big_12mp.jpg";
  for (const wchar_t* out : {L"png", L"webp", L"pdf", L"tiff", L"bmp"}) {
    double t0 = NowSeconds();
    RunOut r = RunCap(L"jpg", out, in, OutDir() / (std::wstring(L"big12.") + out));
    CHECK(r.ok);
    std::printf("    12 MP jpg -> %ls in %.1f s\n", out, NowSeconds() - t0);
    if (!r.ok) { std::printf("    %ls\n", r.error.c_str()); continue; }
    if (std::wstring(out) == L"pdf") { double w, h; CHECK(RenderPdfPage(r.out, 0, 0, 0, nullptr, &w, &h)); CHECK_NEAR(w, 3000, 1); CHECK_NEAR(h, 2250, 1); continue; }
    Json j = Inspect(r.out); CHECK_EQ((int)j.Get("w").AsInt(), 4000); CHECK_EQ((int)j.Get("h").AsInt(), 3000);
  }
  // HEIC output of a large (tiled) picture keeps the dimensions
  RunOut h = RunCap(L"jpg", L"heic", in, OutDir() / L"big12.heic");
  CHECK(h.ok);
  if (h.ok) { Px px; CHECK(WicRead(h.out, px)); CHECK_EQ(px.w, 4000); CHECK_EQ(px.h, 3000); }
}

TEST(encoder_limits_produce_clear_messages_not_crashes) {
  CHECK(EnsureFixtures());
  // 1x1 HEIC: HEVC cannot code it - the user gets a sentence, the temp file is cleaned up
  RunOut r = RunCap(L"png", L"heic", Src() / "one_px.png", OutDir() / L"one_px.heic");
  CHECK(!r.ok); CHECK(r.error.find(L"8 x 8") != std::wstring::npos); CHECK(!fs::exists(OutDir() / L"one_px.heic"));
  // JPEG / WebP size limits are checked up front
  Bitmap wide; wide.Allocate(17000, 8);
  for (size_t i = 0; i < wide.px.size(); i += 4) { wide.px[i + 3] = 255; }
  SaveResult w = SaveBitmap(OutDir() / L"too_wide.webp", wide, L"webp");
  CHECK(!w.ok); CHECK(w.error.find(L"too large") != std::wstring::npos);
  SaveResult j = SaveBitmap(OutDir() / L"ok_wide.jpg", wide, L"jpg");
  CHECK(j.ok);
}

TEST(parsers_and_loader_survive_damaged_files) {
  CHECK(EnsureHeicFixtures());
  struct S { const char* file; };
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
  int loadTried = 0, loadOk = 0;
  for (const char* f : {"src_jpg.jpg", "src_png.png", "src_webp.webp", "src_heic.heic", "src_avif.avif", "irot90ccw.avif", "anim.webp", "src_gif.gif", "src_tiff.tiff", "src_bmp.bmp"}) {
    Bytes orig; CHECK(ReadFileBytes(Src() / f, orig));
    for (int i = 0; i < 400; ++i) {
      Bytes d = orig;
      int mode = (int)(next() % 3);
      if (mode == 0 || mode == 2) d.resize(1 + next() % d.size());                 // truncate
      if (mode == 1 || mode == 2) for (int k = 0; k < 8; ++k) d[next() % d.size()] = (uint8_t)next();   // flip bytes
      Metadata m; JpegInfo ji; PngInfo pi; WebpInfo wi; HeifInfo hi;
      ParseJpeg(d, ji, m);
      ParsePng(d, pi, m);
      ParseWebp(d, wi, m);
      ParseHeif(d, hi, m);
      Bytes ex = d; ExifOrientation(ex); ExifPatch(ex, 1, 100, 100);
      Bytes copy = d; InjectJpegMeta(copy, m); copy = d; InjectWebpMeta(copy, m);
    }
    // a handful of full decodes (WIC / ffmpeg paths) on damaged data must fail or succeed, never crash or hang
    for (int i = 0; i < 6; ++i) {
      Bytes d = orig;
      if (i % 2 == 0) d.resize(d.size() / 2 + next() % (d.size() / 2)); else for (int k = 0; k < 4; ++k) d[next() % d.size()] = (uint8_t)next();
      Bitmap b; std::wstring err; ++loadTried;
      if (LoadBitmapFromMemory(d, b, {}, &err, Widen(std::string(f).substr(std::string(f).rfind('.') + 1)))) { ++loadOk; CHECK(!b.Empty()); }
      else CHECK(!err.empty());
    }
  }
  std::printf("    %d damaged decodes: %d produced an image, the rest failed with a message\n", loadTried, loadOk);
}
