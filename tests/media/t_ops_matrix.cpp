// The operation APIs against every container / audio format we can write, to catch codec-specific argument problems.
#include "media_test_util.h"
#include "engines/media/media_ops.h"
#include <algorithm>
#include <cmath>
#include <fstream>

using namespace mt;
namespace m = filo::media;

static std::string AudioCodecFor(const std::wstring& c) {
  if (c == L"mp4" || c == L"mov" || c == L"mkv" || c == L"m4a") return "aac"; if (c == L"webm" || c == L"opus") return "opus"; if (c == L"avi" || c == L"mp3") return "mp3";
  if (c == L"wmv" || c == L"wma") return "wmav2"; if (c == L"ogg") return "vorbis"; if (c == L"flac") return "flac"; if (c == L"wav") return "pcm_s16le"; if (c == L"aiff") return "pcm_s16be";
  return "";
}
static std::string VideoCodecFor(const std::wstring& c) {
  if (c == L"webm") return "vp9"; if (c == L"avi") return "mpeg4"; if (c == L"wmv") return "wmv2"; return "h264";
}

TEST(video_ops_on_every_container) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("opsmatrix_video");
  struct In { const wchar_t* ext; fs::path p; };
  for (In in : {In{L"mov", fx.mov}, In{L"mkv", fx.mkv}, In{L"webm", fx.webm}, In{L"avi", fx.avi}, In{L"wmv", fx.wmv}}) {
    std::wstring e = in.ext;
    auto tag = [&](const wchar_t* op) { return Narrow(e + L"/" + op); };
    // trim
    {
      m::TrimVideoRequest rq; rq.start = 0.8; rq.end = 2.2; rq.outputPath = d / (e + L"_trim." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::TrimVideo(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) { std::printf("  %s: %ls\n", tag(L"trim").c_str(), r.error.c_str()); } else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 1.4) < 0.15 && mi->FirstVideo()->codec == VideoCodecFor(e) && mi->FirstAudio() && NullDecode(r.outputs[0]));
        CHECK(std::abs(CountVideoFrames(r.outputs[0]) - 35) <= 1);
      }
    }
    // crop
    {
      m::CropRequest rq; rq.rect = {20, 10, 160, 120}; rq.outputPath = d / (e + L"_crop." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::CropVideo(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) { std::printf("  %s: %ls\n", tag(L"crop").c_str(), r.error.c_str()); } else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && mi->FirstVideo()->width == 160 && mi->FirstVideo()->height == 120 && std::fabs(mi->duration - 3.0) < 0.2 && NullDecode(r.outputs[0]));
      }
    }
    // speed
    {
      m::SpeedRequest rq; rq.factor = 1.5; rq.outputPath = d / (e + L"_speed." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::ChangeSpeed(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) { std::printf("  %s: %ls\n", tag(L"speed").c_str(), r.error.c_str()); } else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 2.0) < 0.2 && mi->FirstAudio() && NullDecode(r.outputs[0]));
      }
    }
    // redact
    {
      m::RedactRegion rg; rg.x = 20; rg.y = 20; rg.w = 80; rg.h = 60; rg.start = 0.5; rg.end = 1.5; rg.style = m::RedactStyle::Solid; rg.color = 0x00FF00;
      m::RedactRequest rq; rq.regions = {rg}; rq.outputPath = d / (e + L"_redact." + e);
      JobResult r = RunJob([&](JobContext& c) { return m::RedactVideo(in.p, rq, c); });
      CHECK(r.ok); if (!r.ok) { std::printf("  %s: %ls\n", tag(L"redact").c_str(), r.error.c_str()); } else {
        CHECK(FramePng(r.outputs[0], 1.0, d / (e + L"_r.png")));
        Rgb g = MeanRgb(d / (e + L"_r.png"), 20, 20, 80, 60);
        CHECK(g.ok && g.g > 190 && g.r < 70 && g.b < 70);
      }
    }
    // remove audio (copy)
    {
      m::RemoveAudioRequest rq; rq.outputPath = d / (e + L"_noaudio." + e);
      JobResult r = RunJob([&](JobContext& c) { return m::RemoveAudio(in.p, rq, c); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"noaudio").c_str(), r.error.c_str());
      else { auto mi = Pr(r.outputs[0]); CHECK(mi && !mi->FirstAudio() && mi->FirstVideo()); }
    }
    // split into two (frame accurate)
    {
      m::SplitRequest rq; rq.mode = m::SplitMode::EqualParts; rq.parts = 2; rq.outputDir = d / (e + L"_split");
      auto res = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::SplitVideo(in.p, rq, c)); });
      CHECK(res.ok); if (!res.ok) std::printf("  %s: %ls\n", tag(L"split").c_str(), res.error.c_str());
      else { int n = 0; for (auto& f : fs::directory_iterator(res.outputs[0])) { ++n; CHECK(NullDecode(f.path())); } CHECK_EQ(n, 2); }
    }
    // join: a single container type among others
  }
  // compress with target size on each container (two-pass with the container's codec)
  for (In in : {In{L"mov", fx.mov}, In{L"mkv", fx.mkv}, In{L"webm", fx.webm}, In{L"avi", fx.avi}, In{L"wmv", fx.wmv}}) {
    std::wstring e = in.ext;
    std::error_code ec; uint64_t inBytes = fs::file_size(in.p, ec);
    m::CompressRequest rq; rq.mode = m::CompressMode::TargetSize; rq.targetBytes = inBytes * 6 / 10; rq.outputPath = d / (e + L"_cmp." + e);
    m::CompressResult cr;
    JobResult r = RunJob([&](JobContext& c) { cr = m::CompressMedia(in.p, rq, c); return static_cast<JobResult>(cr); });
    if (!r.ok) {
      std::printf("  %ls compress: %ls (notSmaller=%d)\n", e.c_str(), r.error.c_str(), (int)cr.notSmaller);
      CHECK(cr.notSmaller || !r.error.empty());       // clear outcome either way
    } else {
      uint64_t ob = fs::file_size(r.outputs[0], ec);
      std::printf("  %ls: target %llu -> %llu\n", e.c_str(), (unsigned long long)rq.targetBytes, (unsigned long long)ob);
      CHECK(ob <= rq.targetBytes * 1.1);
      auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 3.0) < 0.2 && NullDecode(r.outputs[0]));
    }
    m::CompressRequest pr; pr.preset = m::CompressPreset::Small; pr.outputPath = d / (e + L"_cmp2." + e);
    m::CompressResult c2;
    JobResult r2 = RunJob([&](JobContext& c) { c2 = m::CompressMedia(in.p, pr, c); return static_cast<JobResult>(c2); });
    CHECK(r2.ok || c2.notSmaller);
    if (r2.ok) { auto mi = Pr(r2.outputs[0]); CHECK(mi && std::fabs(mi->duration - 3.0) < 0.2 && NullDecode(r2.outputs[0])); }
  }
}

TEST(audio_ops_on_every_format) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("opsmatrix_audio");
  struct In { const wchar_t* ext; fs::path p; };
  for (In in : {In{L"mp3", fx.mp3}, In{L"m4a", fx.m4a}, In{L"wav", fx.wav}, In{L"flac", fx.flac}, In{L"ogg", fx.ogg}, In{L"opus", fx.opus}, In{L"aiff", fx.aiff}, In{L"wma", fx.wma}}) {
    std::wstring e = in.ext;
    auto tag = [&](const wchar_t* op) { return Narrow(e + L"/" + op); };
    // trim 0.5 .. 2.0 (re-encode)
    {
      m::TrimAudioRequest rq; rq.start = 0.5; rq.end = 2.0; rq.outputPath = d / (e + L"_trim." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::TrimAudio(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"trim").c_str(), r.error.c_str()); else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 1.5) < 0.12 && mi->FirstAudio()->codec == AudioCodecFor(e) && NullDecode(r.outputs[0]));
      }
    }
    // trim copy mode
    {
      m::TrimAudioRequest rq; rq.start = 0.5; rq.end = 2.0; rq.copyMode = true; rq.outputPath = d / (e + L"_trimcopy." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::TrimAudio(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"trimcopy").c_str(), r.error.c_str()); else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 1.5) < 0.2 && NullDecode(r.outputs[0]));
      }
    }
    // speed 2x
    {
      m::SpeedRequest rq; rq.factor = 2; rq.outputPath = d / (e + L"_speed." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::ChangeSpeed(in.p, rq, c)); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"speed").c_str(), r.error.c_str()); else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 1.5) < 0.15 && NullDecode(r.outputs[0]));
      }
    }
    // bleep 1.0 .. 1.4
    {
      m::BleepRequest rq; rq.ranges = {{1.0, 1.4}}; rq.outputPath = d / (e + L"_bleep." + e);
      JobResult r = RunJob([&](JobContext& c) { return m::BleepAudio(in.p, rq, c); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"bleep").c_str(), r.error.c_str()); else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 3.0) < 0.15 && mi->FirstAudio()->codec == AudioCodecFor(e) && NullDecode(r.outputs[0]));
        auto inside = DecodeMonoF32(r.outputs[0], 44100, "", 1.1, 0.2); size_t zc = 0; for (size_t i = 1; i < inside.size(); ++i) if ((inside[i - 1] < 0) != (inside[i] < 0)) ++zc;
        CHECK_NEAR(zc / 2.0 / (inside.size() / 44100.0), 1000, 90);
      }
    }
    // channels -> mono
    {
      m::ChannelsRequest rq; rq.outChannels = 1; rq.outputPath = d / (e + L"_mono." + e);
      JobResult r = RunJob([&](JobContext& c) { return m::ConvertChannels(in.p, rq, c); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"mono").c_str(), r.error.c_str()); else {
        auto mi = Pr(r.outputs[0]); CHECK(mi && mi->FirstAudio()->channels == 1 && std::fabs(mi->duration - 3.0) < 0.15 && NullDecode(r.outputs[0]));
      }
    }
    // normalise to -20 LUFS (use the 10 s noise through each format first)
    {
      fs::path src = d / (e + L"_noise." + e);
      std::vector<std::string> a = {"-i", Narrow(fx.noise.wstring()), "-map", "0:a"};
      if (e == L"mp3") a.insert(a.end(), {"-c:a", "libmp3lame", "-b:a", "192k"}); else if (e == L"m4a") a.insert(a.end(), {"-c:a", "aac", "-b:a", "192k"});
      else if (e == L"ogg") a.insert(a.end(), {"-c:a", "libvorbis", "-b:a", "192k"}); else if (e == L"opus") a.insert(a.end(), {"-c:a", "libopus", "-b:a", "160k"});
      else if (e == L"wma") a.insert(a.end(), {"-c:a", "wmav2", "-b:a", "192k"}); else if (e == L"flac") a.insert(a.end(), {"-c:a", "flac"});
      else if (e == L"aiff") a.insert(a.end(), {"-c:a", "pcm_s16be"}); else a.insert(a.end(), {"-c:a", "pcm_s16le"});
      a.push_back(Narrow(src.wstring()));
      CHECK(Ff(a));
      m::NormalizeRequest rq; rq.targetLufs = -20; rq.outputPath = d / (e + L"_norm." + e);
      JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::NormalizeVolume(src, rq, c)); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"normalize").c_str(), r.error.c_str()); else {
        double l = MeasureLufs(r.outputs[0]);
        CHECK(std::fabs(l + 20) <= 0.5);
        if (std::fabs(l + 20) > 0.5) std::printf("  %s: measured %.2f\n", tag(L"normalize").c_str(), l);
      }
    }
    // compress (lossy formats shrink; lossless ones report why not)
    {
      m::CompressRequest rq; rq.preset = m::CompressPreset::Smallest; rq.outputPath = d / (e + L"_cmp." + e);
      m::CompressResult cr;
      JobResult r = RunJob([&](JobContext& c) { cr = m::CompressMedia(in.p, rq, c); return static_cast<JobResult>(cr); });
      bool lossy = e == L"mp3" || e == L"m4a" || e == L"ogg" || e == L"opus" || e == L"wma";
      if (lossy) { CHECK(r.ok || cr.notSmaller); if (!r.ok) std::printf("  %ls compress: %ls\n", e.c_str(), r.error.c_str()); }
      else CHECK(!r.ok && cr.notSmaller);
    }
    // waveform of each format
    {
      auto w = m::ComputeWaveform(in.p, 60);
      CHECK(w.ok && w.peaks.size() == 60 && std::fabs(w.duration - 3.0) < 0.15);
      if (w.ok) { float mx = 0; for (float p : w.peaks) mx = std::max(mx, p); CHECK(mx > 0.3f && mx <= 1.0f); }
    }
    // visualizer
    {
      m::VisualizerRequest rq; rq.fps = 10; rq.crf = 34; rq.layout = m::VizLayout::Square; rq.outputPath = d / (e + L"_viz.mp4");
      JobResult r = RunJob([&](JobContext& c) { return m::AudioVisualizer(in.p, rq, c); });
      CHECK(r.ok); if (!r.ok) std::printf("  %s: %ls\n", tag(L"visualizer").c_str(), r.error.c_str());
    }
  }
}

TEST(speed_interpolation_gif_loop_and_cover_background) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("opsmatrix_misc");
  m::SpeedRequest rq; rq.factor = 0.5; rq.interpolate = true; rq.outputPath = d / L"slow.mp4";
  JobResult r = RunJob([&](JobContext& c) { return static_cast<JobResult>(m::ChangeSpeed(fx.tiny, rq, c)); });
  CHECK(r.ok);
  if (r.ok) { auto mi = Pr(r.outputs[0]); CHECK(mi && std::fabs(mi->duration - 1.0) < 0.1 && std::fabs(mi->FirstVideo()->fps - 25) < 0.5 && NullDecode(r.outputs[0])); }
  else std::printf("  %ls\n", r.error.c_str());
  // gif loop flag: the NETSCAPE2.0 application extension carries the loop count
  auto hasLoop = [&](bool loop) {
    Json o = Json::MakeObject(); o["hardwareAcceleration"] = false; o["gifLoop"] = loop;
    JobResult g = RunCap("mp4>gif", fx.tiny, o, d / (loop ? L"loop.gif" : L"once.gif"));
    if (!g.ok) return -1;
    std::ifstream f(g.outputs[0], std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return (int)(s.find("NETSCAPE2.0") != std::string::npos);
  };
  CHECK_EQ(hasLoop(true), 1); CHECK_EQ(hasLoop(false), 0);
  // cover art used as the visualiser background when no image is given
  m::VisualizerRequest vq; vq.layout = m::VizLayout::Square; vq.fps = 10; vq.crf = 30; vq.backgroundColor = 0x0B1220; vq.outputPath = d / L"cover.mp4";
  JobResult v = RunJob([&](JobContext& c) { return m::AudioVisualizer(fx.coverMp3, vq, c); });
  CHECK(v.ok);
  if (v.ok) {
    CHECK(FramePng(v.outputs[0], 1.0, d / L"cv.png"));
    // the cover (testsrc2 colour bars, 200x200) is scaled to cover the whole 1080x1080 frame: the top-left must not be the plain default colour
    Rgb px = MeanRgb(d / L"cv.png", 0, 0, 200, 100);
    CHECK(px.ok && (std::fabs(px.r - 11) + std::fabs(px.g - 18) + std::fabs(px.b - 32)) > 40);
  }
  // audio preview from a video file with a selection
  m::AudioPreviewRequest pq; pq.start = 1; pq.end = 2.5; pq.outChannels = 2;
  fs::path w = d / L"pv.wav";
  CHECK(RunJob([&](JobContext& c) { auto e = m::RenderAudioPreviewWav(fx.mp4, pq, w, c); return e.ok ? JobResult::Ok() : JobResult::Fail(e.error); }).ok);
  auto mi = Pr(w); CHECK(mi && std::fabs(mi->duration - 1.5) < 0.03);
}
