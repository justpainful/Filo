// Compress API tests: target size, percent, presets, resize, not-smaller, estimates, independent verification with PIL.
#include "t_util.h"
#include "engines/image/image_compress.h"

using namespace tu;
using namespace filo;
using namespace filo::img;

namespace {
uintmax_t Size(const fs::path& p) { std::error_code ec; return fs::file_size(p, ec); }

CompressRequest Req(const char* file, const wchar_t* outName) {
  CompressRequest r; r.input = Src() / file; r.output = OutDir() / outName;
  std::error_code ec; fs::remove(r.output, ec);
  return r;
}
void Record2(const char* id, bool ok, const std::string& note) { Record(id, ok, note); }
}

TEST(compress_target_bytes_jpeg_hits_target_and_keeps_quality) {
  CHECK(EnsureFixtures());
  uintmax_t original = Size(Src() / "photo.jpg");
  CompressRequest r = Req("photo.jpg", L"c_target300.jpg");
  r.mode = CompressMode::TargetBytes; r.targetBytes = 300 * 1000;
  JobContext ctx;
  CompressResult res = CompressImage(r, ctx);
  CHECK(res.ok); CHECK(!res.notSmaller); CHECK(res.targetMet);
  std::printf("    jpeg %ju -> %ju bytes (target 300000) quality=%d %dx%d\n", original, (uintmax_t)res.outputBytes, res.qualityUsed, res.widthOut, res.heightOut);
  CHECK(res.outputBytes <= 300000);
  CHECK(res.outputBytes >= 200000);                      // not needlessly far below the target
  CHECK_EQ(res.widthOut, 1600); CHECK_EQ(res.heightOut, 1200);   // quality alone was enough: no downscale
  CHECK(!res.downscaled);
  CHECK_EQ(Size(r.output), (uintmax_t)res.outputBytes);
  Json j = Inspect(r.output);
  CHECK(j.Get("ok").AsBool()); CHECK_EQ(j.Get("format").AsString(), std::string("JPEG")); CHECK_EQ((int)j.Get("w").AsInt(), 1600);
  CHECK_EQ(j.Get("make").AsString(), std::string("FiloTest"));      // EXIF kept
}

TEST(compress_target_too_small_downscales_without_distortion) {
  CHECK(EnsureFixtures());
  CompressRequest r = Req("photo.jpg", L"c_target25.jpg");
  r.mode = CompressMode::TargetBytes; r.targetBytes = 25 * 1000;
  JobContext ctx;
  CompressResult res = CompressImage(r, ctx);
  CHECK(res.ok); CHECK(res.targetMet); CHECK(res.downscaled);
  std::printf("    jpeg -> %ju bytes (target 25000) %dx%d quality=%d\n", (uintmax_t)res.outputBytes, res.widthOut, res.heightOut, res.qualityUsed);
  CHECK(res.outputBytes <= 25000);
  CHECK(res.widthOut < 1600);
  CHECK(std::abs((double)res.widthOut / res.heightOut - 4.0 / 3.0) < 0.02);   // aspect ratio kept
  Json j = Inspect(r.output);
  CHECK_EQ((int)j.Get("w").AsInt(), res.widthOut); CHECK_EQ((int)j.Get("h").AsInt(), res.heightOut);
  bool warned = false; for (auto& w : res.warnings) if (w.find(L"reduced to") != std::wstring::npos) warned = true;
  CHECK(warned);
  // without permission to shrink, the result is flagged as not reaching the target and the size is untouched
  CompressRequest r2 = Req("photo.jpg", L"c_target25_nodown.jpg");
  r2.mode = CompressMode::TargetBytes; r2.targetBytes = 25 * 1000; r2.allowDownscale = false;
  CompressResult res2 = CompressImage(r2, ctx);
  CHECK(res2.ok); CHECK(!res2.targetMet); CHECK(!res2.downscaled);
  CHECK_EQ(res2.widthOut, 1600);
  CHECK(res2.outputBytes > 25000);
  std::printf("    no-downscale: %ju bytes, quality=%d, targetMet=%d\n", (uintmax_t)res2.outputBytes, res2.qualityUsed, (int)res2.targetMet);
}

TEST(compress_percent_reduction) {
  CHECK(EnsureFixtures());
  for (const char* file : {"photo.jpg", "photo.webp"}) {
    CompressRequest r = Req(file, L"c_pct.tmp"); r.output = OutDir() / (std::wstring(L"c_pct_") + Widen(file));
    r.mode = CompressMode::PercentReduction; r.percentReduction = 60;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK(res.targetMet);
    uintmax_t in = Size(r.input);
    std::printf("    %s: %ju -> %ju (%.0f%% of original, asked 40%%)\n", file, in, (uintmax_t)res.outputBytes, 100.0 * res.outputBytes / in);
    CHECK(res.outputBytes <= in * 0.40 + 1);
    CHECK(res.outputBytes >= in * 0.20);
  }
}

TEST(compress_presets_are_monotonic_and_keep_the_format) {
  CHECK(EnsureHeicFixtures());
  struct C { const char* file; const wchar_t* ext; const char* pilFormat; };
  for (C c : {C{"photo.jpg", L"jpg", "JPEG"}, C{"photo.png", L"png", "PNG"}, C{"photo.webp", L"webp", "WEBP"}, C{"photo.avif", L"avif", "AVIF"}, C{"photo.tiff", L"tiff", "TIFF"}}) {
    uintmax_t sizes[3] = {0, 0, 0}; uintmax_t in = Size(Src() / c.file);
    for (int p = 0; p < 3; ++p) {
      CompressRequest r = Req(c.file, L"x"); r.output = OutDir() / (std::wstring(L"c_preset") + std::to_wstring(p) + L"_" + Widen(c.file));
      r.preset = (CompressPreset)p;
      JobContext ctx; CompressResult res = CompressImage(r, ctx);
      CHECK(res.ok);
      if (res.notSmaller) { sizes[p] = in; continue; }
      sizes[p] = Size(r.output);
      Json j = Inspect(r.output);
      CHECK_EQ(j.Get("format").AsString(), std::string(c.pilFormat));
      CHECK_EQ((int)j.Get("w").AsInt(), 1600); CHECK_EQ((int)j.Get("h").AsInt(), 1200);
    }
    std::printf("    %s: original %ju | low %ju medium %ju high %ju\n", c.file, in, sizes[0], sizes[1], sizes[2]);
    CHECK(sizes[2] <= sizes[1] && sizes[1] <= sizes[0]);
    CHECK(sizes[2] < in);
  }
  // HEIC: made by the Windows encoder from the photo
  {
    Bitmap b; std::wstring err; CHECK(LoadBitmap(Src() / "photo.png", b, {}, &err));
    SaveOptions so; so.quality = 97; CHECK(SaveBitmap(OutDir() / "photo_src.heic", b, L"heic", so).ok);
    uintmax_t in = Size(OutDir() / "photo_src.heic"); uintmax_t sizes[3];
    for (int p = 0; p < 3; ++p) {
      CompressRequest r; r.input = OutDir() / "photo_src.heic"; r.output = OutDir() / (L"c_preset" + std::to_wstring(p) + L".heic"); r.preset = (CompressPreset)p;
      JobContext ctx; CompressResult res = CompressImage(r, ctx);
      CHECK(res.ok); sizes[p] = res.notSmaller ? in : Size(r.output);
      if (!res.notSmaller) { Px px; CHECK(WicRead(r.output, px)); CHECK_EQ(px.w, 1600); CHECK_EQ(px.h, 1200); }
    }
    std::printf("    heic: original %ju | low %ju medium %ju high %ju\n", in, sizes[0], sizes[1], sizes[2]);
    CHECK(sizes[2] <= sizes[1] && sizes[1] <= sizes[0]);
  }
}

TEST(compress_png_target_uses_lossless_then_palette) {
  CHECK(EnsureFixtures());
  // flat graphic: lossless optimisation alone gets below a generous target and must stay bit-exact
  {
    CompressRequest r = Req("graphic.png", L"c_graphic.png"); r.mode = CompressMode::TargetBytes; r.targetBytes = Size(r.input) * 95 / 100;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK(res.targetMet);
    std::printf("    graphic.png %ju -> %ju (target %ju), colours %d, %dx%d\n", Size(Src() / "graphic.png"), (uintmax_t)res.outputBytes, (uintmax_t)r.targetBytes, res.colorsUsed, res.widthOut, res.heightOut);
    CHECK_EQ(res.colorsUsed, 0);
    if (!res.notSmaller) {
      Px a, b; CHECK(WicRead(r.output, a)); CHECK(WicRead(Src() / "graphic.png", b));
      CHECK_EQ(a.w, b.w); CHECK(a.bgra == b.bgra);               // lossless: bit-exact pixels
    }
    // a harsher target needs a lossy palette; the result still has to be a valid, undistorted PNG
    CompressRequest r2 = Req("graphic.png", L"c_graphic2.png"); r2.mode = CompressMode::TargetBytes; r2.targetBytes = Size(r2.input) / 2;
    CompressResult res2 = CompressImage(r2, ctx);
    CHECK(res2.ok);
    std::printf("    graphic.png harsh target %ju -> %ju, colours %d, %dx%d, met=%d\n", (uintmax_t)r2.targetBytes, (uintmax_t)res2.outputBytes, res2.colorsUsed, res2.widthOut, res2.heightOut, (int)res2.targetMet);
    if (!res2.notSmaller) { Json j = Inspect(r2.output); CHECK_EQ((int)j.Get("w").AsInt(), res2.widthOut); CHECK_EQ((int)j.Get("h").AsInt(), res2.heightOut); }
  }
  // photo: needs the palette
  {
    CompressRequest r = Req("photo.png", L"c_photo_png.png"); r.mode = CompressMode::TargetBytes; r.targetBytes = Size(r.input) * 30 / 100;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK(res.targetMet);
    std::printf("    photo.png %ju -> %ju (target %ju), colours %d\n", Size(Src() / "photo.png"), (uintmax_t)res.outputBytes, (uintmax_t)r.targetBytes, res.colorsUsed);
    CHECK(res.outputBytes <= r.targetBytes);
    CHECK(res.colorsUsed > 0 && res.colorsUsed <= 256);
    Json j = Inspect(r.output); CHECK_EQ(j.Get("mode").AsString(), std::string("P")); CHECK_EQ((int)j.Get("w").AsInt(), 1600);
  }
  // alpha survives palette compression
  {
    CompressRequest r = Req("src_png.png", L"c_alpha.png"); r.preset = CompressPreset::High;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok);
    if (!res.notSmaller) { Json j = Inspect(r.output); CHECK(NearC(Quad(j, 1).a, 0, 4)); CHECK(NearC(Quad(j, 2).a, 128, 8)); }
  }
}

TEST(compress_resize_options) {
  CHECK(EnsureFixtures());
  {
    CompressRequest r = Req("photo.jpg", L"c_max800.jpg"); r.maxSide = 800; r.preset = CompressPreset::Low;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK_EQ(res.widthOut, 800); CHECK_EQ(res.heightOut, 600);
    Json j = Inspect(r.output); CHECK_EQ((int)j.Get("w").AsInt(), 800); CHECK_EQ((int)j.Get("h").AsInt(), 600);
  }
  {
    CompressRequest r = Req("photo.jpg", L"c_half.jpg"); r.resizePercent = 50;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK_EQ(res.widthOut, 800); CHECK_EQ(res.heightOut, 600);
  }
  {   // never upscales
    CompressRequest r = Req("photo.jpg", L"c_noup.jpg"); r.maxSide = 5000; r.preset = CompressPreset::High;
    JobContext ctx; CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok); CHECK_EQ(res.widthOut, 1600);
  }
}

TEST(compress_reports_not_smaller_instead_of_pretending) {
  CHECK(EnsureFixtures());
  // an already strongly compressed JPEG: re-encoding it gently cannot make it smaller
  JobContext ctx;
  CompressRequest first = Req("photo.jpg", L"c_strong.jpg"); first.preset = CompressPreset::High;
  CHECK(CompressImage(first, ctx).ok);
  CompressRequest r; r.input = first.output; r.output = OutDir() / L"c_notsmaller.jpg"; r.preset = CompressPreset::Low;
  std::error_code ec0; fs::remove(r.output, ec0);
  CompressResult res = CompressImage(r, ctx);
  CHECK(res.ok); CHECK(res.notSmaller);
  CHECK(!fs::exists(r.output));                           // nothing written: the caller keeps the original
  CHECK(res.outputBytes >= res.inputBytes);
  // a target above the current size is honest too
  CompressRequest r2 = Req("photo.jpg", L"c_bigtarget.jpg"); r2.mode = CompressMode::TargetBytes; r2.targetBytes = 50 * 1000 * 1000;
  CompressResult res2 = CompressImage(r2, ctx);
  CHECK(res2.ok);
  if (!res2.notSmaller) CHECK(res2.outputBytes < res2.inputBytes);
}

TEST(compress_unsupported_inputs_have_clear_errors) {
  CHECK(EnsureFixtures());
  JobContext ctx;
  { CompressRequest r = Req("src_bmp.bmp", L"c_bmp.out"); CompressResult res = CompressImage(r, ctx); CHECK(!res.ok); CHECK(res.error.find(L"BMP") != std::wstring::npos); }
  { CompressRequest r = Req("src_bmp.bmp", L"c_bmp.png"); r.outFormat = L"png"; CompressResult res = CompressImage(r, ctx); CHECK(res.ok); CHECK(res.notSmaller || fs::exists(r.output)); }
  { CompressRequest r = Req("anim.gif", L"c_gif.out"); CompressResult res = CompressImage(r, ctx); CHECK(!res.ok); CHECK(!res.error.empty()); }
  { CompressRequest r = Req("src_svg.svg", L"c_svg.png"); CompressResult res = CompressImage(r, ctx); CHECK(res.ok); CHECK_EQ(res.format, std::wstring(L"png")); }
  { CompressRequest r = Req("noise.png", L"c_zero.jpg"); r.mode = CompressMode::TargetBytes; r.targetBytes = 0; CompressResult res = CompressImage(r, ctx); CHECK(!res.ok); }
  { CompressRequest r; r.input = OutDir() / "does_not_exist.jpg"; r.output = OutDir() / "c_missing.jpg"; CompressResult res = CompressImage(r, ctx); CHECK(!res.ok); CHECK(!res.error.empty()); }
  // format change: jpg -> webp with a transparent source warns about flattening
  { CompressRequest r = Req("src_png.png", L"c_flat.jpg"); r.outFormat = L"jpg"; r.preset = CompressPreset::Low;
    CompressResult res = CompressImage(r, ctx); CHECK(res.ok);
    bool warned = false; for (auto& w : res.warnings) if (w.find(L"Transparency was flattened") != std::wstring::npos) warned = true;
    CHECK(warned || res.notSmaller); }
}

TEST(compress_estimate_is_close_to_the_real_result) {
  CHECK(EnsureFixtures());
  struct C { const char* file; CompressMode mode; CompressPreset preset; uint64_t target; int pct; };
  const C cases[] = {
    {"photo.jpg", CompressMode::Preset, CompressPreset::Medium, 0, 0},
    {"photo.jpg", CompressMode::TargetBytes, CompressPreset::Medium, 400000, 0},
    {"photo.webp", CompressMode::Preset, CompressPreset::High, 0, 0},
    {"photo.png", CompressMode::Preset, CompressPreset::Medium, 0, 0},
    {"photo.jpg", CompressMode::PercentReduction, CompressPreset::Medium, 0, 70},
    {"src_png.png", CompressMode::Preset, CompressPreset::High, 0, 0},
  };
  int i = 0;
  for (const C& c : cases) {
    CompressRequest r = Req(c.file, L"x"); r.output = OutDir() / (L"c_est" + std::to_wstring(i++) + L"_" + Widen(c.file));
    r.mode = c.mode; r.preset = c.preset; r.targetBytes = c.target; r.percentReduction = c.pct;
    JobContext ctx;
    double t0 = NowSeconds();
    SizeEstimate est = EstimateCompressedSize(r, &ctx);
    double dt = NowSeconds() - t0;
    CHECK(est.ok);
    CompressResult res = CompressImage(r, ctx);
    CHECK(res.ok);
    double real = (double)res.outputBytes;
    double ratio = est.bytes / real;
    std::printf("    %-12s mode=%d estimate %ju (%s, %.2fs) real %.0f ratio %.2f\n", c.file, (int)c.mode, (uintmax_t)est.bytes, est.exact ? "exact" : "extrapolated", dt, real, ratio);
    if (est.exact) CHECK(ratio > 0.97 && ratio < 1.03);
    else CHECK(ratio > 0.6 && ratio < 1.6);
  }
}

TEST(compress_cancellation) {
  CHECK(EnsureFixtures());
  if (!ff::Available()) return;
  CompressRequest r = Req("photo.avif", L"c_cancel.avif"); r.mode = CompressMode::TargetBytes; r.targetBytes = 50 * 1000;
  JobSpec spec; spec.operation = L"compress"; spec.title = L"cancel"; spec.inputs = {r.input};
  std::shared_ptr<CompressResult> out = std::make_shared<CompressResult>();
  spec.work = [r, out](JobContext& ctx) { *out = CompressImage(r, ctx); return out->ok ? JobResult::Ok() : JobResult::Fail(out->error); };
  uint64_t id = JobManager::Get().Submit(std::move(spec));
  Sleep(300);
  JobManager::Get().Cancel(id);
  JobManager::Get().WaitFor(id);
  auto info = JobManager::Get().Info(id);
  CHECK(info.has_value());
  std::printf("    cancelled compress: state %ls, error '%ls'\n", JobStateName(info->state), out->error.c_str());
  if (info->state == JobState::Cancelled) CHECK(!fs::exists(r.output));
}
