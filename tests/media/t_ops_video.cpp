// Video operations: trim, crop, speed, join, split, frames, redact, remove audio, compress.
#include "media_test_util.h"
#include "engines/media/media_ops.h"
#include "engines/media/media_info.h"
#include "engines/media/media_internal.h"
#include <algorithm>
#include <cmath>

using namespace mt;
namespace m = filo::media;

template <class R> struct OpRun { R r; JobResult jr; };
template <class R, class F> OpRun<R> RunOp(F f) {
  OpRun<R> o;
  o.jr = RunJob([&](JobContext& c) { o.r = f(c); return static_cast<JobResult>(o.r); });
  return o;
}
static bool HasWarning(const JobResult& r, const wchar_t* needle) {
  for (auto& w : r.warnings) if (w.find(needle) != std::wstring::npos) return true;
  return false;
}
static double StreamDuration(const fs::path& p, const char* sel) {
  std::string s = Probe1(p, {L"-select_streams", Widen(sel), L"-show_entries", L"stream=duration"});
  return atof(s.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(info_summary) {
  const Fx& fx = Fixtures();
  auto s = m::SummarizeMedia(fx.mp4);
  CHECK(s.ok);
  CHECK_EQ(s.width, 320); CHECK_EQ(s.height, 240);
  CHECK_NEAR(s.fps, 25, 0.01);
  CHECK_NEAR(s.duration, 3.0, 0.1);
  CHECK(s.videoCodec == "h264"); CHECK(s.audioCodec == "aac");
  CHECK_EQ(s.channels, 2); CHECK_EQ(s.sampleRate, 44100);
  CHECK(s.hasVideo && s.hasAudio && !s.isAudioOnly);
  CHECK(!s.Describe().empty());
  auto p = m::SummarizeMedia(fx.portrait);
  CHECK(p.ok); CHECK_EQ(p.width, 180); CHECK_EQ(p.height, 320); CHECK_EQ(p.rotation, 270); CHECK_EQ(p.codedWidth, 320);
  auto v = m::SummarizeMedia(fx.vfr);
  CHECK(v.ok && v.variableFrameRate);
  auto c = m::SummarizeMedia(fx.coverMp3);
  CHECK(c.ok && c.hasCoverArt && c.isAudioOnly); CHECK(c.title == L"Fixture Title"); CHECK(c.artist == L"Filo Tests");
  auto t = m::SummarizeMedia(fx.twoAudio);
  CHECK_EQ(t.audioTracks.size(), (size_t)2);
  auto bad = m::SummarizeMedia(Work() / L"does-not-exist.mp4");
  CHECK(!bad.ok && !bad.error.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(trim_video_frame_accurate) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("trim");
  // the base clip has ONE keyframe (x264 ultrafast, 3 s): a cut at 1.0..2.0 can only be right if the engine decodes up to the exact frame
  uint64_t h0 = FileHash(fx.mp4);
  m::TrimVideoRequest rq; rq.start = 1.0; rq.end = 2.0; rq.outputPath = d / L"t1.mp4";
  auto run = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimVideo(fx.mp4, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  fs::path o = run.jr.outputs[0];
  CHECK_EQ(CountVideoFrames(o), 25);
  auto mi = Pr(o);
  CHECK(mi && std::fabs(mi->duration - 1.0) < 0.06);
  CHECK(mi && mi->FirstAudio());
  CHECK_NEAR(StreamDuration(o, "a:0"), 1.0, 0.08);
  CHECK_NEAR(run.r.actualStart, 1.0, 0.001); CHECK(!run.r.snappedToKeyframes); CHECK_NEAR(run.r.outputDuration, 1.0, 0.06);
  // content: first frame of the output equals the source frame at t=1.0
  CHECK(FramePng(o, 0.0, d / L"o0.png")); CHECK(FramePng(fx.mp4, 1.0, d / L"s0.png")); CHECK(FramePng(fx.mp4, 0.0, d / L"s_wrong.png"));
  double same = PngDiff(d / L"o0.png", d / L"s0.png", 0, 0, 320, 240), wrong = PngDiff(d / L"o0.png", d / L"s_wrong.png", 0, 0, 320, 240);
  CHECK(same >= 0 && same < 6); CHECK(wrong > 12);
  // non-frame-aligned start still gives the right number of frames within one
  m::TrimVideoRequest r2; r2.start = 0.5; r2.end = 1.7; r2.outputPath = d / L"t2.mp4";
  auto run2 = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimVideo(fx.mp4, r2, c); });
  CHECK(run2.jr.ok);
  if (run2.jr.ok) CHECK(std::abs(CountVideoFrames(run2.jr.outputs[0]) - 30) <= 1);
  CHECK_EQ(FileHash(fx.mp4), h0);
  // too short / bad range
  m::TrimVideoRequest r3; r3.start = 1.0; r3.end = 1.01;
  auto run3 = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimVideo(fx.mp4, r3, c); });
  CHECK(!run3.jr.ok && !run3.jr.error.empty());
}

TEST(trim_video_copy_mode_reports_keyframe) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("trimcopy");
  m::TrimVideoRequest rq; rq.start = 2.4; rq.end = 4.2; rq.copyMode = true; rq.outputPath = d / L"c.mp4";
  auto run = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimVideo(fx.kf, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  CHECK(run.r.snappedToKeyframes);
  CHECK_NEAR(run.r.actualStart, 2.0, 0.05);                 // keyframes every second -> snapped back to 2.0
  CHECK_NEAR(run.r.actualEnd, 4.2, 0.12);
  auto mi = Pr(run.jr.outputs[0]);
  CHECK(mi && std::fabs(mi->duration - (4.2 - 2.0)) < 0.15);
  CHECK(mi && mi->FirstVideo() && mi->FirstVideo()->codec == "h264");
  CHECK(NullDecode(run.jr.outputs[0]));
  CHECK(HasWarning(run.jr, L"keyframe"));
  // first frame equals source frame at the reported start
  CHECK(FramePng(run.jr.outputs[0], 0.0, d / L"o.png")); CHECK(FramePng(fx.kf, 2.0, d / L"s.png"));
  double diff = PngDiff(d / L"o.png", d / L"s.png", 0, 0, 320, 240);
  CHECK(diff >= 0 && diff < 4);
}

TEST(trim_audio_exact_and_silence) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("trimaudio");
  m::TrimAudioRequest rq; rq.start = 0.5; rq.end = 1.5; rq.outputPath = d / L"t.wav";
  auto run = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimAudio(fx.wav, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  auto src = DecodeMonoF32(fx.wav, 44100), out = DecodeMonoF32(run.jr.outputs[0], 44100);
  CHECK_EQ(out.size(), (size_t)44100);
  double maxd = 0; for (size_t i = 0; i < out.size() && i + 22050 < src.size(); ++i) maxd = std::max(maxd, (double)std::fabs(out[i] - src[i + 22050]));
  CHECK(maxd < 1e-4);                                      // sample-exact for PCM
  // mp3 re-encode keeps the tags and the length
  m::TrimAudioRequest r2; r2.start = 1; r2.end = 2.5; r2.outputPath = d / L"t.mp3";
  auto run2 = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimAudio(fx.coverMp3, r2, c); });
  CHECK(run2.jr.ok);
  if (run2.jr.ok) {
    auto mi = Pr(run2.jr.outputs[0]);
    CHECK(mi && std::fabs(mi->duration - 1.5) < 0.1);
    CHECK(mi && mi->raw.Get("format").Get("tags").Get("title").AsString() == "Fixture Title");
    bool cover = false; if (mi) for (auto& s : mi->streams) if (s.attachedPic) cover = true;
    CHECK(cover);
  }
  // copy mode on mp3
  m::TrimAudioRequest r3; r3.start = 0.5; r3.end = 2.0; r3.copyMode = true; r3.outputPath = d / L"c.mp3";
  auto run3 = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimAudio(fx.mp3, r3, c); });
  CHECK(run3.jr.ok);
  if (run3.jr.ok) { auto mi = Pr(run3.jr.outputs[0]); CHECK(mi && mi->FirstAudio()->codec == "mp3" && std::fabs(mi->duration - 1.5) < 0.1); }
  // silence detection: 1 s silence + 4 s tone + 1 s silence
  auto si = m::DetectSilentEnds(fx.speech);
  CHECK(si.ok && !si.allSilent);
  CHECK_NEAR(si.start, 1.0, 0.08); CHECK_NEAR(si.end, 5.0, 0.08); CHECK_NEAR(si.duration, 6.0, 0.05);
  m::TrimAudioRequest r4; r4.trimSilentEnds = true; r4.outputPath = d / L"s.wav";
  auto run4 = RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimAudio(fx.speech, r4, c); });
  CHECK(run4.jr.ok);
  if (run4.jr.ok) { auto mi = Pr(run4.jr.outputs[0]); CHECK(mi && std::fabs(mi->duration - 4.0) < 0.12); CHECK_NEAR(run4.r.actualStart, 1.0, 0.08); }
  auto noAudio = m::DetectSilentEnds(fx.noAudio);                 // a video without sound: clear error
  CHECK(!noAudio.ok && !noAudio.error.empty());
  fs::path allQuiet = d / L"quiet.wav";
  CHECK(Ff({"-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "2", Narrow(allQuiet.wstring())}));
  auto quiet = m::DetectSilentEnds(allQuiet);
  CHECK(quiet.ok && quiet.allSilent);
  m::TrimAudioRequest rq5; rq5.trimSilentEnds = true; rq5.outputPath = d / L"q.wav";
  CHECK(!RunOp<m::TrimResult>([&](JobContext& c) { return m::TrimAudio(allQuiet, rq5, c); }).jr.ok);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(crop_geometry_and_content) {
  using m::AspectPreset;
  auto sq = m::AspectCropRect(1920, 1080, AspectPreset::Square1x1);
  CHECK_EQ(sq.w, 1080); CHECK_EQ(sq.h, 1080); CHECK_EQ(sq.x, 420); CHECK_EQ(sq.y, 0);
  auto v = m::AspectCropRect(1920, 1080, AspectPreset::R9x16);
  CHECK_EQ(v.h, 1080); CHECK_EQ(v.w, 606); CHECK(v.w % 2 == 0 && v.x % 2 == 0);
  auto w = m::AspectCropRect(1080, 1920, AspectPreset::R16x9);
  CHECK_EQ(w.w, 1080); CHECK_EQ(w.h, 606);
  auto z = m::AspectCropRect(1000, 1000, AspectPreset::R4x3, 0.0, 0.0, 2.0);
  CHECK_EQ(z.x, 0); CHECK_EQ(z.y, 0); CHECK(z.w <= 500 && z.w % 2 == 0);

  const Fx& fx = Fixtures();
  fs::path d = OutDir("crop");
  m::CropRequest rq; rq.rect = {40, 20, 200, 150}; rq.outputPath = d / L"c.mp4";
  auto run = RunOp<m::CropResult>([&](JobContext& c) { return m::CropVideo(fx.mp4, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  auto mi = Pr(run.jr.outputs[0]);
  CHECK(mi && mi->FirstVideo()->width == 200 && mi->FirstVideo()->height == 150);
  CHECK(mi && mi->FirstAudio() && mi->FirstAudio()->codec == "aac");
  CHECK(mi && std::fabs(mi->duration - 3.0) < 0.1);
  CHECK(FramePng(run.jr.outputs[0], 1.0, d / L"o.png")); CHECK(FramePng(fx.mp4, 1.0, d / L"s.png"));
  double diff = PngDiffOffset(d / L"o.png", 0, 0, d / L"s.png", 40, 20, 200, 150);
  CHECK(diff >= 0 && diff < 5);
  double wrong = PngDiffOffset(d / L"o.png", 0, 0, d / L"s.png", 100, 60, 200, 150);
  CHECK(wrong > 10);
  // odd rectangle is rounded to even numbers; out of bounds is refused
  m::CropRequest odd; odd.rect = {41, 21, 101, 77}; odd.outputPath = d / L"odd.mp4";
  auto ro = RunOp<m::CropResult>([&](JobContext& c) { return m::CropVideo(fx.mp4, odd, c); });
  CHECK(ro.jr.ok && ro.r.outWidth == 100 && ro.r.outHeight == 76);
  if (ro.jr.ok) { auto om = Pr(ro.jr.outputs[0]); CHECK(om && om->FirstVideo()->width == 100 && om->FirstVideo()->height == 76); }
  m::CropRequest bad; bad.rect = {300, 0, 100, 100};
  auto rb = RunOp<m::CropResult>([&](JobContext& c) { return m::CropVideo(fx.mp4, bad, c); });
  CHECK(!rb.jr.ok && !rb.jr.error.empty());
}

TEST(crop_rotated_video_uses_display_coordinates) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("croprot");
  auto before = Pr(fx.portrait);
  int dw, dh; before->DisplaySize(dw, dh);
  CHECK_EQ(dw, 180); CHECK_EQ(dh, 320);
  m::CropRequest rq; rq.rect = {40, 60, 100, 200}; rq.outputPath = d / L"c.mp4";
  auto run = RunOp<m::CropResult>([&](JobContext& c) { return m::CropVideo(fx.portrait, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  auto mi = Pr(run.jr.outputs[0]);
  CHECK(mi && mi->FirstVideo()->width == 100 && mi->FirstVideo()->height == 200 && mi->FirstVideo()->rotation == 0);
  // the reference frame extracted by ffmpeg (which auto-rotates) matches the cropped region
  CHECK(FramePng(run.jr.outputs[0], 1.0, d / L"o.png")); CHECK(FramePng(fx.portrait, 1.0, d / L"s.png"));
  double diff = PngDiffOffset(d / L"o.png", 0, 0, d / L"s.png", 40, 60, 100, 200);
  CHECK(diff >= 0 && diff < 5);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(speed_duration_audio_pitch_sync) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("speed");
  CHECK_NEAR(m::SpeedDurationPreview(10, 2), 5, 1e-9); CHECK_NEAR(m::SpeedDurationPreview(10, 0.5), 20, 1e-9);
  struct Case { double f; bool pitch; };
  for (Case cs : {Case{2.0, false}, Case{0.5, false}, Case{4.0, false}, Case{0.25, false}, Case{2.0, true}}) {
    m::SpeedRequest rq; rq.factor = cs.f; rq.changePitch = cs.pitch; rq.outputPath = d / (L"s" + std::to_wstring((int)(cs.f * 100)) + (cs.pitch ? L"p" : L"") + L".mp4");
    auto run = RunOp<m::SpeedResult>([&](JobContext& c) { return m::ChangeSpeed(fx.mp4, rq, c); });
    CHECK(run.jr.ok);
    if (!run.jr.ok) { std::printf("  factor %g: %ls\n", cs.f, run.jr.error.c_str()); continue; }
    fs::path o = run.jr.outputs[0];
    auto mi = Pr(o);
    double want = 3.0 / cs.f;
    CHECK(mi && std::fabs(mi->duration - want) < 0.15 + 0.03 * want);
    double vd = StreamDuration(o, "v:0"), ad = StreamDuration(o, "a:0");
    CHECK(std::fabs(vd - want) < 0.12 + 0.02 * want); CHECK(std::fabs(ad - want) < 0.12 + 0.02 * want); CHECK(std::fabs(vd - ad) < 0.12);   // A/V stay in sync
    CHECK_NEAR(mi->FirstVideo()->fps, 25, 0.5);
    double f0 = ChannelFreq(o, 0);
    if (cs.pitch) CHECK_NEAR(f0, 440 * cs.f, 40); else CHECK_NEAR(f0, 440, 25);
    CHECK(NullDecode(o));
  }
  // silent video
  m::SpeedRequest rq; rq.factor = 2; rq.outputPath = d / L"silent.mp4";
  auto run = RunOp<m::SpeedResult>([&](JobContext& c) { return m::ChangeSpeed(fx.noAudio, rq, c); });
  CHECK(run.jr.ok);
  if (run.jr.ok) { auto mi = Pr(run.jr.outputs[0]); CHECK(mi && !mi->FirstAudio() && std::fabs(mi->duration - 1.5) < 0.1); }
  // audio-only
  m::SpeedRequest ra; ra.factor = 1.5; ra.outputPath = d / L"a.mp3";
  auto runa = RunOp<m::SpeedResult>([&](JobContext& c) { return m::ChangeSpeed(fx.mp3, ra, c); });
  CHECK(runa.jr.ok);
  if (runa.jr.ok) { auto mi = Pr(runa.jr.outputs[0]); CHECK(mi && std::fabs(mi->duration - 2.0) < 0.12); }
  // invalid
  m::SpeedRequest rb; rb.factor = 0;
  auto runb = RunOp<m::SpeedResult>([&](JobContext& c) { return m::ChangeSpeed(fx.mp4, rb, c); });
  CHECK(!runb.jr.ok);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(join_normalises_clips) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("join");
  m::JoinRequest rq; rq.inputs = {fx.joinA, fx.joinB, fx.joinC, fx.joinD}; rq.outputPath = d / L"joined.mp4";
  uint64_t hA = FileHash(fx.joinA);
  auto run = RunOp<m::JoinResult>([&](JobContext& c) { return m::JoinVideos(rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  fs::path o = run.jr.outputs[0];
  auto mi = Pr(o);
  CHECK(mi != std::nullopt);
  if (!mi) return;
  CHECK_EQ(mi->FirstVideo()->width, 320); CHECK_EQ(mi->FirstVideo()->height, 240);     // FIRST clip's size
  CHECK_NEAR(mi->FirstVideo()->fps, 25, 0.01);
  CHECK(mi->FirstVideo()->codec == "h264" && mi->FirstVideo()->pixFmt == "yuv420p");
  CHECK(mi->FirstAudio() && mi->FirstAudio()->sampleRate == 48000 && mi->FirstAudio()->channels == 2);
  CHECK(std::fabs(mi->duration - 8.0) < 0.3);
  CHECK_NEAR(run.r.expectedDuration, 8.0, 0.1);
  CHECK(NullDecode(o));
  CHECK_EQ(FileHash(fx.joinA), hA);
  CHECK(HasWarning(run.jr, L"no sound"));
  // clip C (t 4..6) has no audio -> silence; clip A (0..2) and B (2..4) have sound
  auto a = DecodeMonoF32(o, 16000);
  CHECK(Rms(a, 8000, 24000) > 0.05);                 // A
  CHECK(Rms(a, 40000, 56000) > 0.05);                // B (mono upmixed)
  CHECK(Rms(a, 70000, 90000) < 0.002);               // C silent
  CHECK(Rms(a, 104000, 120000) > 0.05);              // D
  // portrait clip D (180x320 after rotation) is letterboxed, not stretched or rotated sideways: black bars left/right, picture in the middle
  CHECK(FramePng(o, 7.0, d / L"d.png"));
  Rgb left = MeanRgb(d / L"d.png", 4, 100, 30, 40), mid = MeanRgb(d / L"d.png", 120, 100, 80, 40);
  CHECK(left.ok && left.r < 12 && left.g < 12 && left.b < 12);
  Rgb midc = mid;
  CHECK(midc.ok && (midc.r + midc.g + midc.b) > 60);
  // clip B (640x360, 16:9) in 4:3: letterboxed with black bars top/bottom
  CHECK(FramePng(o, 3.0, d / L"b.png"));
  Rgb top = MeanRgb(d / L"b.png", 10, 2, 300, 20);
  CHECK(top.ok && top.r < 12 && top.g < 12 && top.b < 12);
  // errors
  m::JoinRequest one; one.inputs = {fx.joinA};
  auto r1 = RunOp<m::JoinResult>([&](JobContext& c) { return m::JoinVideos(one, c); });
  CHECK(!r1.jr.ok);
  m::JoinRequest noVid; noVid.inputs = {fx.joinA, fx.wav};
  auto r2 = RunOp<m::JoinResult>([&](JobContext& c) { return m::JoinVideos(noVid, c); });
  CHECK(!r2.jr.ok && !r2.jr.error.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(split_equal_markers_segments) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("split");
  m::SplitRequest rq; rq.mode = m::SplitMode::EqualParts; rq.parts = 3; rq.outputDir = d / L"equal";
  auto run = RunOp<m::SplitResult>([&](JobContext& c) { return m::SplitVideo(fx.mp4, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  CHECK_EQ(run.r.segments.size(), (size_t)3);
  int frames = 0; double total = 0;
  for (auto& s : run.r.segments) {
    CHECK(fs::exists(s.path));
    auto mi = Pr(s.path); CHECK(mi && std::fabs(mi->duration - 1.0) < 0.08); if (mi) total += mi->duration;
    frames += CountVideoFrames(s.path);
    CHECK(NullDecode(s.path));
  }
  CHECK_EQ(frames, 75);                                        // frame accurate: no frame lost or duplicated at the cut points
  CHECK_NEAR(total, 3.0, 0.15);
  CHECK(Leftovers(d).empty());
  // markers
  m::SplitRequest r2; r2.mode = m::SplitMode::Markers; r2.markers = {2.0, 0.8}; r2.outputDir = d / L"marks";
  auto run2 = RunOp<m::SplitResult>([&](JobContext& c) { return m::SplitVideo(fx.mp4, r2, c); });
  CHECK(run2.jr.ok && run2.r.segments.size() == 3);
  if (run2.jr.ok) {
    CHECK_NEAR(Pr(run2.r.segments[0].path)->duration, 0.8, 0.08); CHECK_NEAR(Pr(run2.r.segments[1].path)->duration, 1.2, 0.08); CHECK_NEAR(Pr(run2.r.segments[2].path)->duration, 1.0, 0.08);
  }
  // custom segments with a gap
  m::SplitRequest r3; r3.mode = m::SplitMode::Segments; r3.segments = {{0.2, 0.7}, {1.5, 2.5}}; r3.outputDir = d / L"seg";
  auto run3 = RunOp<m::SplitResult>([&](JobContext& c) { return m::SplitVideo(fx.mp4, r3, c); });
  CHECK(run3.jr.ok && run3.r.segments.size() == 2);
  if (run3.jr.ok) { CHECK(std::abs(CountVideoFrames(run3.r.segments[0].path) - 13) <= 1); CHECK(std::abs(CountVideoFrames(run3.r.segments[1].path) - 25) <= 1); }
  // copy mode on a keyframe-rich file
  m::SplitRequest r4; r4.mode = m::SplitMode::EqualParts; r4.parts = 2; r4.copyMode = true; r4.outputDir = d / L"copy";
  auto run4 = RunOp<m::SplitResult>([&](JobContext& c) { return m::SplitVideo(fx.kf, r4, c); });
  CHECK(run4.jr.ok && run4.r.segments.size() == 2);
  if (run4.jr.ok) for (auto& s : run4.r.segments) { auto mi = Pr(s.path); CHECK(mi && std::fabs(mi->duration - 3.0) < 0.2); CHECK(NullDecode(s.path)); }
  // invalid
  m::SplitRequest r5; r5.mode = m::SplitMode::Markers; r5.markers = {99};
  auto run5 = RunOp<m::SplitResult>([&](JobContext& c) { return m::SplitVideo(fx.mp4, r5, c); });
  CHECK(!run5.jr.ok);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(frames_save_and_preview) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("frames");
  m::SaveFramesRequest rq; rq.timestamps = {0.5, 1.5, 2.9, 99.0}; rq.outputDir = d / L"png";
  auto run = RunOp<m::FramesResult>([&](JobContext& c) { return m::SaveVideoFrames(fx.portrait, rq, c); });
  CHECK(run.jr.ok);
  if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); return; }
  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(run.r.folder)) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  CHECK_EQ(files.size(), (size_t)4);
  CHECK(files.size() == 4 && files[0].filename().wstring() == L"portrait169 - frame 001.png");
  for (auto& f : files) { int w = 0, h = 0; CHECK(PngSize(f, w, h)); CHECK_EQ(w, 180); CHECK_EQ(h, 320); }   // native resolution, rotation applied
  // a different moment gives a different picture
  CHECK(PngDiff(files[0], files[1], 0, 0, 180, 320) > 5);
  // UI preview
  m::FrameImage fi = m::ExtractFrame(fx.mp4, 1.0, 160, 160);
  CHECK(fi.ok); CHECK_EQ(fi.width, 160); CHECK_EQ(fi.height, 120); CHECK_EQ(fi.bgra.size(), (size_t)(160 * 120 * 4));
  size_t nonZero = 0; for (size_t i = 0; i < fi.bgra.size(); i += 4) if (fi.bgra[i] || fi.bgra[i + 1] || fi.bgra[i + 2]) ++nonZero;
  CHECK(nonZero > 1000);
  // BGRA channel order: compare with PIL's RGB of the same frame (testsrc2 is colourful, a swapped order is detectable)
  CHECK(FramePng(fx.mp4, 1.0, d / L"ref.png"));
  Rgb ref = MeanRgb(d / L"ref.png", 0, 0, 320, 240);
  double b = 0, g = 0, r = 0; size_t n = 0;
  m::FrameImage full = m::ExtractFrame(fx.mp4, 1.0, 0, 0);
  CHECK(full.ok && full.width == 320 && full.height == 240);
  for (size_t i = 0; i + 3 < full.bgra.size(); i += 4) { b += full.bgra[i]; g += full.bgra[i + 1]; r += full.bgra[i + 2]; ++n; }
  CHECK(ref.ok && std::fabs(r / n - ref.r) < 3 && std::fabs(g / n - ref.g) < 3 && std::fabs(b / n - ref.b) < 3);
  // rotated source: display size
  m::FrameImage rot = m::ExtractFrame(fx.portrait, 0.5, 1000, 1000);
  CHECK(rot.ok && rot.width == 180 && rot.height == 320);
  // fast (keyframe) seek on a long GOP file returns a frame quickly
  double t0 = NowSeconds();
  m::FrameScrubber sc(fx.kf);
  std::wstring why;
  CHECK(sc.Open(&why));
  CHECK_EQ(sc.Width(), 320); CHECK_NEAR(sc.Duration(), 6.0, 0.1);
  auto f1 = sc.Get(3.3, 200, 200, false);
  CHECK(f1.ok);
  double t1 = NowSeconds();
  auto f2 = sc.Get(3.3, 200, 200, false);                   // cached
  CHECK(f2.ok && NowSeconds() - t1 < 0.01);
  std::printf("  scrub first frame %.0f ms (incl. probe), cached %.2f ms\n", (t1 - t0) * 1000, (NowSeconds() - t1) * 1000);
  CHECK(!m::ExtractFrame(fx.wav, 0, 100, 100).ok);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(redact_only_inside_time_window) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("redact");
  m::RedactRegion solid; solid.x = 40; solid.y = 40; solid.w = 100; solid.h = 80; solid.start = 1.0; solid.end = 2.0; solid.style = m::RedactStyle::Solid; solid.color = 0xFF0000;
  m::RedactRegion blur; blur.x = 180; blur.y = 120; blur.w = 120; blur.h = 100; blur.start = 1.0; blur.end = 2.0; blur.style = m::RedactStyle::Blur; blur.blurStrength = 30;
  m::RedactRequest rq; rq.regions = {solid, blur}; rq.outputPath = d / L"r.mp4";
  uint64_t h0 = FileHash(fx.mp4);
  JobResult jr = RunJob([&](JobContext& c) { return m::RedactVideo(fx.mp4, rq, c); });
  CHECK(jr.ok);
  if (!jr.ok) { std::printf("  %ls\n", jr.error.c_str()); return; }
  fs::path o = jr.outputs[0];
  auto mi = Pr(o);
  CHECK(mi && std::fabs(mi->duration - 3.0) < 0.1 && mi->FirstAudio());
  CHECK(NullDecode(o));
  auto grab = [&](double t, const char* tag) { fs::path a = d / (std::wstring(L"o_") + Widen(tag) + L".png"), b = d / (std::wstring(L"s_") + Widen(tag) + L".png"); FramePng(o, t, a); FramePng(fx.mp4, t, b); return std::make_pair(a, b); };
  // inside the window (t = 1.5)
  auto in = grab(1.5, "in");
  Rgb red = MeanRgb(in.first, 40, 40, 100, 80);
  CHECK(red.ok && red.r > 200 && red.g < 50 && red.b < 50);                                   // solid colour box
  CHECK(PngDiff(in.first, in.second, 180, 120, 120, 100) > 6);                                // blurred area differs from the original
  CHECK(PngDiff(in.first, in.second, 200, 10, 100, 60) < 8);                                  // outside both regions: unchanged
  CHECK(PngDiff(in.first, in.second, 0, 160, 150, 70) < 8);
  // blur really lowers detail: horizontal gradient energy inside the blurred area is smaller than in the source
  // outside the time window (t = 0.5 and 2.5): identical to the source in both regions
  for (auto t : {0.5, 2.6}) {
    auto out = grab(t, t < 1 ? "pre" : "post");
    CHECK(PngDiff(out.first, out.second, 40, 40, 100, 80) < 8);
    CHECK(PngDiff(out.first, out.second, 180, 120, 120, 100) < 8);
  }
  CHECK_EQ(FileHash(fx.mp4), h0);
  // region outside the picture / too small
  m::RedactRegion bad; bad.x = 400; bad.y = 10; bad.w = 50; bad.h = 50;
  m::RedactRequest rb; rb.regions = {bad};
  JobResult jb = RunJob([&](JobContext& c) { return m::RedactVideo(fx.mp4, rb, c); });
  CHECK(!jb.ok && !jb.error.empty());
  m::RedactRequest rn;
  JobResult jn = RunJob([&](JobContext& c) { return m::RedactVideo(fx.mp4, rn, c); });
  CHECK(!jn.ok);
}

TEST(remove_audio_copies_video_bit_exact) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("noaudio");
  m::RemoveAudioRequest rq; rq.outputPath = d / L"na.mp4";
  JobResult jr = RunJob([&](JobContext& c) { return m::RemoveAudio(fx.mp4, rq, c); });
  CHECK(jr.ok);
  if (!jr.ok) { std::printf("  %ls\n", jr.error.c_str()); return; }
  auto mi = Pr(jr.outputs[0]);
  CHECK(mi && !mi->FirstAudio() && mi->FirstVideo() && mi->FirstVideo()->codec == "h264");
  auto vmd5 = [&](const fs::path& p) { std::string o = FfOutput({"-i", Narrow(p.wstring()), "-map", "0:v:0", "-c", "copy", "-f", "md5", "-"}); size_t k = o.find("MD5="); return k == std::string::npos ? std::string() : o.substr(k + 4, 32); };
  CHECK(!vmd5(fx.mp4).empty()); CHECK(vmd5(fx.mp4) == vmd5(jr.outputs[0]));
  JobResult j2 = RunJob([&](JobContext& c) { return m::RemoveAudio(fx.noAudio, rq, c); });
  CHECK(!j2.ok && !j2.error.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(compress_presets_target_percent_audio) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("compress");
  std::error_code ec;
  uint64_t inBytes = fs::file_size(fx.big, ec);
  uint64_t h0 = FileHash(fx.big);
  // preset
  m::CompressRequest p; p.mode = m::CompressMode::Preset; p.preset = m::CompressPreset::Small; p.outputPath = d / L"p.mp4";
  auto est = m::EstimateCompressedSize(fx.big, p);
  CHECK(!est.exact && est.bytes > 0 && est.bytes <= inBytes);
  auto run = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.big, p, c); });
  CHECK(run.jr.ok && !run.r.notSmaller);
  if (run.jr.ok) {
    uint64_t ob = fs::file_size(run.jr.outputs[0], ec);
    CHECK(ob < inBytes / 2); CHECK_EQ(run.r.outputBytes, ob);
    auto mi = Pr(run.jr.outputs[0]); CHECK(mi && std::fabs(mi->duration - 8.0) < 0.15 && mi->FirstVideo()->width == 640);
    CHECK(NullDecode(run.jr.outputs[0]));
    std::printf("  preset small: %llu -> %llu bytes (estimate %llu)\n", (unsigned long long)inBytes, (unsigned long long)ob, (unsigned long long)est.bytes);
  } else std::printf("  %ls\n", run.jr.error.c_str());
  // target size: 40% of the original
  uint64_t target = inBytes * 4 / 10;
  m::CompressRequest t; t.mode = m::CompressMode::TargetSize; t.targetBytes = target; t.outputPath = d / L"t.mp4";
  auto e2 = m::EstimateCompressedSize(fx.big, t);
  CHECK(e2.exact && e2.bytes == target && e2.feasible);
  auto run2 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.big, t, c); });
  CHECK(run2.jr.ok);
  if (run2.jr.ok) {
    uint64_t ob = fs::file_size(run2.jr.outputs[0], ec);
    std::printf("  target %llu -> got %llu bytes (%.1f%%)\n", (unsigned long long)target, (unsigned long long)ob, 100.0 * ob / target);
    CHECK(ob <= target * 1.06); CHECK(ob >= target * 0.5);
    auto mi = Pr(run2.jr.outputs[0]); CHECK(mi && std::fabs(mi->duration - 8.0) < 0.15);
  } else std::printf("  %ls\n", run2.jr.error.c_str());
  // percent
  m::CompressRequest pc; pc.mode = m::CompressMode::Percent; pc.percentSmaller = 70; pc.outputPath = d / L"pc.mp4";
  auto run3 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.big, pc, c); });
  CHECK(run3.jr.ok);
  if (run3.jr.ok) CHECK(fs::file_size(run3.jr.outputs[0], ec) <= inBytes * 0.3 * 1.06);
  // unreachable target
  m::CompressRequest tiny; tiny.mode = m::CompressMode::TargetSize; tiny.targetBytes = 2000;
  CHECK(!m::EstimateCompressedSize(fx.big, tiny).feasible);
  auto run4 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.big, tiny, c); });
  CHECK(!run4.jr.ok && !run4.jr.error.empty());
  // already small: compressing the compressed result with the lightest preset must not be kept when it is not smaller
  m::CompressRequest again; again.mode = m::CompressMode::Preset; again.preset = m::CompressPreset::High; again.outputPath = d / L"again.mp4";
  if (run.jr.ok) {
    auto run5 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(run2.jr.ok ? run2.jr.outputs[0] : run.jr.outputs[0], again, c); });
    if (!run5.jr.ok) { CHECK(run5.r.notSmaller); CHECK(!fs::exists(again.outputPath)); CHECK(Leftovers(d).empty()); }
    else CHECK(run5.r.outputBytes < fs::file_size(run2.jr.ok ? run2.jr.outputs[0] : run.jr.outputs[0], ec));
    std::printf("  recompress: ok=%d notSmaller=%d\n", (int)run5.jr.ok, (int)run5.r.notSmaller);
  }
  // deterministic "not smaller": a clip squeezed at crf 45 re-encoded with the High preset (crf 23) grows -> nothing is kept
  fs::path squeezed = d / L"squeezed.mp4";
  CHECK(Ff({"-f", "lavfi", "-i", "testsrc2=s=320x240:r=25:d=4", "-f", "lavfi", "-i", "sine=f=300:d=4", "-c:v", "libx264", "-preset", "veryfast", "-crf", "45", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "32k", "-shortest", Narrow(squeezed.wstring())}));
  m::CompressRequest hi; hi.preset = m::CompressPreset::High; hi.outputPath = d / L"hi.mp4";
  auto runNs = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(squeezed, hi, c); });
  CHECK(!runNs.jr.ok && runNs.r.notSmaller && !runNs.jr.error.empty());
  CHECK(!fs::exists(hi.outputPath)); CHECK(Leftovers(d).empty()); CHECK(runNs.jr.outputs.empty());
  std::printf("  not-smaller case: %ls\n", runNs.jr.error.c_str());
  // audio: 192k mp3 -> preset small (96k)
  m::CompressRequest au; au.preset = m::CompressPreset::Small; au.outputPath = d / L"a.mp3";
  auto run6 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.coverMp3, au, c); });
  CHECK(run6.jr.ok);
  if (run6.jr.ok) { CHECK(fs::file_size(run6.jr.outputs[0], ec) < fs::file_size(fx.coverMp3, ec)); auto mi = Pr(run6.jr.outputs[0]); CHECK(mi && std::fabs(mi->duration - 3.0) < 0.1); }
  else std::printf("  audio compress: %ls\n", run6.jr.error.c_str());
  // uncompressed 16-bit wav cannot shrink without a format change -> reported, nothing kept
  m::CompressRequest aw; aw.outputPath = d / L"w.wav";
  auto run7 = RunOp<m::CompressResult>([&](JobContext& c) { return m::CompressMedia(fx.wav, aw, c); });
  CHECK(!run7.jr.ok && run7.r.notSmaller && !fs::exists(aw.outputPath));
  CHECK_EQ(FileHash(fx.big), h0);
}
