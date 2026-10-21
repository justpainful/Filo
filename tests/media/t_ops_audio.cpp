// Audio operations: normalise, visualiser, channels, bleep, waveform, previews.
#include "media_test_util.h"
#include "engines/media/media_ops.h"
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

// ---------------------------------------------------------------------------------------------------------------------------
TEST(normalize_loudness_within_half_lu) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("norm");
  uint64_t h0 = FileHash(fx.noise);
  double before = MeasureLufs(fx.noise);
  std::printf("  noise.wav is %.1f LUFS\n", before);
  for (double target : {-16.0, -23.0, -14.0}) {
    m::NormalizeRequest rq; rq.targetLufs = target; rq.truePeakDb = -1.0; rq.lra = 11;
    rq.outputPath = d / (L"n" + std::to_wstring((int)-target) + L".wav");
    auto run = RunOp<m::NormalizeResult>([&](JobContext& c) { return m::NormalizeVolume(fx.noise, rq, c); });
    CHECK(run.jr.ok);
    if (!run.jr.ok) { std::printf("  %ls\n", run.jr.error.c_str()); continue; }
    double after = MeasureLufs(run.jr.outputs[0]);                         // measured independently with ebur128
    std::printf("  target %.1f -> measured %.2f LUFS (engine says in %.1f, out %.2f, tp %.2f)\n", target, after, run.r.input.integratedLufs, run.r.output.integratedLufs, run.r.output.truePeakDb);
    CHECK(std::fabs(after - target) <= 0.5);
    CHECK(run.r.input.valid && std::fabs(run.r.input.integratedLufs - before) < 0.3);
    CHECK(run.r.output.valid && std::fabs(run.r.output.integratedLufs - target) <= 0.5);
    CHECK(run.r.output.truePeakDb <= -1.0 + 0.3);
    auto mi = Pr(run.jr.outputs[0]);
    CHECK(mi && mi->FirstAudio()->sampleRate == 44100 && mi->FirstAudio()->channels == 2 && std::fabs(mi->duration - 10.0) < 0.05);
  }
  CHECK_EQ(FileHash(fx.noise), h0);
  // standalone measurement
  std::wstring why;
  auto li = m::MeasureLoudness(fx.noise, nullptr, &why);
  CHECK(li.valid && std::fabs(li.integratedLufs - before) < 0.3);
  // a video's audio track: video stream copied, loudness reached
  m::NormalizeRequest rv; rv.targetLufs = -20; rv.outputPath = d / L"v.mp4";
  fs::path noiseVideo = d / L"noise_video.mp4";
  CHECK(Ff({"-f", "lavfi", "-i", "testsrc2=s=160x120:r=25:d=10", "-i", Narrow(fx.noise.wstring()), "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-shortest", Narrow(noiseVideo.wstring())}));
  auto runv = RunOp<m::NormalizeResult>([&](JobContext& c) { return m::NormalizeVolume(noiseVideo, rv, c); });
  CHECK(runv.jr.ok);
  if (runv.jr.ok) {
    double after = MeasureLufs(runv.jr.outputs[0]);
    CHECK(std::fabs(after + 20) <= 0.5);
    auto mi = Pr(runv.jr.outputs[0]);
    CHECK(mi && mi->FirstVideo() && mi->FirstVideo()->codec == "h264" && mi->FirstAudio() && mi->FirstAudio()->codec == "aac");
    auto vmd5 = [&](const fs::path& p) { std::string o = FfOutput({"-i", Narrow(p.wstring()), "-map", "0:v:0", "-c", "copy", "-f", "md5", "-"}); size_t k = o.find("MD5="); return k == std::string::npos ? std::string() : o.substr(k + 4, 32); };
    CHECK(!vmd5(noiseVideo).empty() && vmd5(noiseVideo) == vmd5(runv.jr.outputs[0]));
  } else std::printf("  %ls\n", runv.jr.error.c_str());
  // mp3 output keeps its format
  m::NormalizeRequest rm; rm.targetLufs = -18; rm.outputPath = d / L"m.mp3";
  auto runm = RunOp<m::NormalizeResult>([&](JobContext& c) { return m::NormalizeVolume(fx.mp3, rm, c); });
  CHECK(runm.jr.ok);
  if (runm.jr.ok) { CHECK(std::fabs(MeasureLufs(runm.jr.outputs[0]) + 18) <= 0.5); CHECK(Pr(runm.jr.outputs[0])->FirstAudio()->codec == "mp3"); }
  // silence is refused
  fs::path silent = d / L"silent.wav";
  CHECK(Ff({"-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "2", Narrow(silent.wstring())}));
  auto runs = RunOp<m::NormalizeResult>([&](JobContext& c) { return m::NormalizeVolume(silent, rv, c); });
  CHECK(!runs.jr.ok && !runs.jr.error.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(channels_mono_stereo_gains) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("channels");
  // stereo -> mono
  m::ChannelsRequest rq; rq.outChannels = 1; rq.outputPath = d / L"mono.wav";
  JobResult jr = RunJob([&](JobContext& c) { return m::ConvertChannels(fx.wav, rq, c); });
  CHECK(jr.ok);
  if (jr.ok) {
    auto mi = Pr(jr.outputs[0]);
    CHECK(mi && mi->FirstAudio()->channels == 1 && std::fabs(mi->duration - 3.0) < 0.05);
    auto src = DecodeMonoF32(fx.wav, 44100), out = DecodeMonoF32(jr.outputs[0], 44100);
    CHECK(std::fabs(Rms(out) - 0.25) < 0.01);                    // (L+R)/2 of two 0.5-amplitude tones: sqrt(0.125/4 + 0.125/4)
  } else std::printf("  %ls\n", jr.error.c_str());
  // stereo with left muted and right at 50 %
  m::ChannelsRequest g; g.outChannels = 2; g.leftGain = 0.0; g.rightGain = 0.5; g.outputPath = d / L"gain.wav";
  JobResult jg = RunJob([&](JobContext& c) { return m::ConvertChannels(fx.wav, g, c); });
  CHECK(jg.ok);
  if (jg.ok) {
    auto left = DecodeMonoF32(jg.outputs[0], 44100, "pan=mono|c0=c0"), right = DecodeMonoF32(jg.outputs[0], 44100, "pan=mono|c0=c1");
    auto srcRight = DecodeMonoF32(fx.wav, 44100, "pan=mono|c0=c1");
    CHECK(Rms(left) < 1e-4);
    CHECK(std::fabs(Rms(right) / Rms(srcRight) - 0.5) < 0.01);
    CHECK_NEAR(ChannelFreq(jg.outputs[0], 1), 880, 30);
  }
  // mono -> stereo duplicates; right gain applies to the copy
  m::ChannelsRequest up; up.outChannels = 2; up.rightGain = 0.25; up.outputPath = d / L"up.wav";
  JobResult ju = RunJob([&](JobContext& c) { return m::ConvertChannels(fx.mono, up, c); });
  CHECK(ju.ok);
  if (ju.ok) {
    auto mi = Pr(ju.outputs[0]);
    CHECK(mi && mi->FirstAudio()->channels == 2 && mi->FirstAudio()->codec == "pcm_s16le");   // same format as the input (wav) unless outputExt is given
  }
  m::ChannelsRequest up2; up2.outChannels = 2; up2.rightGain = 0.25; up2.outputExt = L"flac"; up2.outputPath = d / L"up2.flac";
  JobResult ju2 = RunJob([&](JobContext& c) { return m::ConvertChannels(fx.mono, up2, c); });
  CHECK(ju2.ok);
  if (ju2.ok) {
    auto mi = Pr(ju2.outputs[0]);
    CHECK(mi && mi->FirstAudio()->channels == 2 && mi->FirstAudio()->codec == "flac");
    auto l = DecodeMonoF32(ju2.outputs[0], 44100, "pan=mono|c0=c0"), r = DecodeMonoF32(ju2.outputs[0], 44100, "pan=mono|c0=c1");
    CHECK(std::fabs(Rms(r) / Rms(l) - 0.25) < 0.01);
  }
  // video input: video copied, audio converted
  m::ChannelsRequest vr; vr.outChannels = 1; vr.outputPath = d / L"v.mp4";
  JobResult jv = RunJob([&](JobContext& c) { return m::ConvertChannels(fx.mp4, vr, c); });
  CHECK(jv.ok);
  if (jv.ok) { auto mi = Pr(jv.outputs[0]); CHECK(mi && mi->FirstAudio()->channels == 1 && mi->FirstVideo()->codec == "h264" && std::fabs(mi->duration - 3.0) < 0.1); }
  // preview wav
  m::ChannelsRequest pv; pv.outChannels = 1; pv.leftGain = 1; pv.rightGain = 0;
  fs::path pw = d / L"preview.wav";
  EngineResult pr = RunJob([&](JobContext& c) { auto r = m::PreviewChannelsToWav(fx.wav, pv, 0.5, 1.5, pw, c); return r.ok ? JobResult::Ok() : JobResult::Fail(r.error); }).ok ? EngineResult::Ok() : EngineResult::Fail(L"preview");
  CHECK(pr.ok);
  auto pi = Pr(pw);
  CHECK(pi && pi->FirstAudio()->channels == 1 && std::fabs(pi->duration - 1.5) < 0.05 && pi->FirstAudio()->codec == "pcm_s16le");
  CHECK_NEAR(ChannelFreq(pw, 0), 440, 30);              // only the left channel survives
}

// ---------------------------------------------------------------------------------------------------------------------------
static double FreqOf(const std::vector<float>& v, int rate) {
  size_t zc = 0; for (size_t i = 1; i < v.size(); ++i) if ((v[i - 1] < 0) != (v[i] < 0)) ++zc;
  return v.empty() ? 0 : zc / 2.0 / (v.size() / (double)rate);
}

TEST(bleep_changes_region_only) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("bleep");
  uint64_t h0 = FileHash(fx.wav);
  m::BleepRequest rq; rq.ranges = {{1.0, 1.5}, {2.2, 2.4}}; rq.outputPath = d / L"b.wav";
  JobResult jr = RunJob([&](JobContext& c) { return m::BleepAudio(fx.wav, rq, c); });
  CHECK(jr.ok);
  if (!jr.ok) { std::printf("  %ls\n", jr.error.c_str()); return; }
  fs::path o = jr.outputs[0];
  auto mi = Pr(o);
  CHECK(mi && std::fabs(mi->duration - 3.0) < 0.02 && mi->FirstAudio()->channels == 2 && mi->FirstAudio()->sampleRate == 44100);
  // outside the ranges the PCM is bit-identical (md5 of decoded slices)
  auto slice = [&](const fs::path& p, double ss, double t) { std::string out = FfOutput({"-ss", std::to_string(ss), "-t", std::to_string(t), "-i", Narrow(p.wstring()), "-c:a", "pcm_s16le", "-f", "md5", "-"}); size_t k = out.find("MD5="); return k == std::string::npos ? std::string() : out.substr(k + 4, 32); };
  for (auto seg : {std::pair<double, double>{0.0, 0.95}, {1.6, 0.55}, {2.5, 0.5}}) {
    std::string a = slice(fx.wav, seg.first, seg.second), b = slice(o, seg.first, seg.second);
    CHECK(!a.empty() && a == b);
  }
  // inside the range: a 1 kHz tone at the requested level replaces the original (440 Hz / 880 Hz mix)
  auto inside = DecodeMonoF32(o, 44100, "", 1.1, 0.3), orig = DecodeMonoF32(fx.wav, 44100, "", 1.1, 0.3);
  CHECK_NEAR(FreqOf(inside, 44100), 1000, 40);
  CHECK(std::fabs(Rms(inside) - std::pow(10.0, -12.0 / 20) * 0.7071) < 0.02);
  CHECK(std::fabs(Rms(inside) - Rms(orig)) > 0.03);
  auto second = DecodeMonoF32(o, 44100, "", 2.25, 0.1);
  CHECK_NEAR(FreqOf(second, 44100), 1000, 80);
  CHECK_EQ(FileHash(fx.wav), h0);
  // silence variant on mp3 keeps format and length
  m::BleepRequest rs; rs.ranges = {{0.5, 1.0}}; rs.kind = m::BleepKind::Silence; rs.outputPath = d / L"s.mp3";
  JobResult js = RunJob([&](JobContext& c) { return m::BleepAudio(fx.mp3, rs, c); });
  CHECK(js.ok);
  if (js.ok) {
    auto m2 = Pr(js.outputs[0]); CHECK(m2 && m2->FirstAudio()->codec == "mp3" && std::fabs(m2->duration - 3.0) < 0.1);
    auto sil = DecodeMonoF32(js.outputs[0], 44100, "", 0.6, 0.3), loud = DecodeMonoF32(js.outputs[0], 44100, "", 1.5, 0.3);
    CHECK(Rms(sil) < 0.003); CHECK(Rms(loud) > 0.2);
  }
  // video input
  m::BleepRequest rv; rv.ranges = {{1.0, 2.0}}; rv.outputPath = d / L"v.mp4";
  JobResult jv = RunJob([&](JobContext& c) { return m::BleepAudio(fx.burstsMp4, rv, c); });
  CHECK(jv.ok);
  if (jv.ok) { auto m3 = Pr(jv.outputs[0]); CHECK(m3 && m3->FirstVideo() && m3->FirstVideo()->codec == "h264" && std::fabs(m3->duration - 6.0) < 0.1); CHECK(NullDecode(jv.outputs[0])); }
  // preview of a window that contains the first range
  fs::path pw = d / L"p.wav";
  JobResult jp = RunJob([&](JobContext& c) { auto r = m::RenderBleepPreview(fx.wav, rq, 0.5, 2.0, pw, c); return r.ok ? JobResult::Ok() : JobResult::Fail(r.error); });
  CHECK(jp.ok);
  auto pi = Pr(pw);
  CHECK(pi && std::fabs(pi->duration - 2.0) < 0.05);
  auto pin = DecodeMonoF32(pw, 44100, "", 0.6, 0.3);          // = source 1.1..1.4 -> bleep
  CHECK_NEAR(FreqOf(pin, 44100), 1000, 60);
  // no ranges / outside
  m::BleepRequest none;
  CHECK(!RunJob([&](JobContext& c) { return m::BleepAudio(fx.wav, none, c); }).ok);
  m::BleepRequest out; out.ranges = {{50, 60}};
  CHECK(!RunJob([&](JobContext& c) { return m::BleepAudio(fx.wav, out, c); }).ok);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(waveform_peaks) {
  const Fx& fx = Fixtures();
  auto w = m::ComputeWaveform(fx.burstsWav, 120);
  CHECK(w.ok); CHECK_EQ(w.peaks.size(), (size_t)120); CHECK_NEAR(w.duration, 6.0, 0.05); CHECK_EQ(w.channels, 2);
  if (w.ok) {
    // tone bursts at 1..2 s and 4..5 s, 0.354 per channel (a mono tone spread over stereo at -3 dB): buckets are 50 ms wide
    for (int b : {25, 30, 35}) CHECK(w.peaks[b] > 0.33 && w.peaks[b] < 0.38);
    for (int b : {85, 90, 95}) CHECK(w.peaks[b] > 0.33 && w.peaks[b] < 0.38);
    for (int b : {2, 10, 60, 65, 110, 118}) CHECK(w.peaks[b] < 0.01);
    CHECK(w.minima[30] < -0.33 && w.maxima[30] > 0.33);
    float mx = 0; for (float p : w.peaks) mx = std::max(mx, p);
    CHECK(mx <= 1.0f);
  }
  auto pc = m::ComputeWaveformPerChannel(Fixtures().baseWav, 50);
  CHECK(pc.ok); CHECK_EQ(pc.peaks.size(), (size_t)2); CHECK_EQ(pc.peaks[0].size(), (size_t)50);
  if (pc.ok) { CHECK_NEAR(*std::max_element(pc.peaks[0].begin(), pc.peaks[0].end()), 0.5, 0.03); CHECK_NEAR(*std::max_element(pc.peaks[1].begin(), pc.peaks[1].end()), 0.5, 0.03); }
  auto left = m::ComputeWaveformPerChannel(Fixtures().baseWav, 10);
  (void)left;
  auto vw = m::ComputeWaveform(fx.mp4, 40);                 // audio of a video file
  CHECK(vw.ok && vw.peaks.size() == 40);
  auto bad = m::ComputeWaveform(fx.noAudio, 40);
  CHECK(!bad.ok && !bad.error.empty());
  // cancel flag
  std::atomic<bool> cancel{true};
  auto c = m::ComputeWaveform(fx.long30, 100, &cancel);
  CHECK(!c.ok);
}

TEST(audio_preview_wav_selection_gains_bleeps) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("preview");
  m::AudioPreviewRequest rq; rq.start = 1.0; rq.end = 2.5; rq.outChannels = 1; rq.leftGain = 1; rq.rightGain = 0;
  rq.bleeps = {{1.5, 1.7}};
  fs::path w = d / L"p.wav";
  JobResult jr = RunJob([&](JobContext& c) { auto r = m::RenderAudioPreviewWav(fx.wav, rq, w, c); return r.ok ? JobResult::Ok() : JobResult::Fail(r.error); });
  CHECK(jr.ok);
  auto mi = Pr(w);
  CHECK(mi && std::fabs(mi->duration - 1.5) < 0.03 && mi->FirstAudio()->channels == 1);
  auto before = DecodeMonoF32(w, 44100, "", 0.1, 0.3), bleep = DecodeMonoF32(w, 44100, "", 0.55, 0.1);
  CHECK_NEAR(FreqOf(before, 44100), 440, 30);              // left channel only
  CHECK_NEAR(FreqOf(bleep, 44100), 1000, 80);              // 1.5..1.7 s in the source = 0.5..0.7 in the preview
  // maximum length is enforced
  m::AudioPreviewRequest lim; lim.start = 0; lim.end = 0; lim.maxSeconds = 1.2;
  fs::path w2 = d / L"p2.wav";
  CHECK(RunJob([&](JobContext& c) { auto r = m::RenderAudioPreviewWav(fx.wav, lim, w2, c); return r.ok ? JobResult::Ok() : JobResult::Fail(r.error); }).ok);
  auto mi2 = Pr(w2); CHECK(mi2 && std::fabs(mi2->duration - 1.2) < 0.03);
}

// ---------------------------------------------------------------------------------------------------------------------------
TEST(visualizer_reacts_to_real_audio) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("viz");
  uint64_t h0 = FileHash(fx.burstsWav);
  m::VisualizerRequest rq; rq.layout = m::VizLayout::Landscape; rq.fps = 25; rq.waveColor = 0xFF3030; rq.backgroundColor = 0x102030; rq.crf = 28; rq.outputPath = d / L"v.mp4";
  JobResult jr = RunJob([&](JobContext& c) { return m::AudioVisualizer(fx.burstsWav, rq, c); });
  CHECK(jr.ok);
  if (!jr.ok) { std::printf("  %ls\n", jr.error.c_str()); return; }
  fs::path o = jr.outputs[0];
  auto mi = Pr(o);
  CHECK(mi && mi->FirstVideo()->width == 1920 && mi->FirstVideo()->height == 1080 && std::fabs(mi->FirstVideo()->fps - 25) < 0.01 && std::fabs(mi->duration - 6.0) < 0.15);
  CHECK(mi && mi->FirstAudio() && mi->FirstAudio()->codec == "aac");
  CHECK(NullDecode(o));
  // background colour in the corner, wave colour present only while sound plays (burst 1..2 s and 4..5 s; silence at 3 s)
  CHECK(FramePng(o, 1.5, d / L"loud.png")); CHECK(FramePng(o, 3.0, d / L"quiet.png")); CHECK(FramePng(o, 4.5, d / L"loud2.png"));
  Rgb corner = MeanRgb(d / L"quiet.png", 4, 4, 40, 40);
  CHECK(corner.ok && std::fabs(corner.r - 16) < 8 && std::fabs(corner.g - 32) < 8 && std::fabs(corner.b - 48) < 8);
  int loud = PngCountNear(d / L"loud.png", 0, 300, 1920, 480, 255, 48, 48, 90), loud2 = PngCountNear(d / L"loud2.png", 0, 300, 1920, 480, 255, 48, 48, 90), quiet = PngCountNear(d / L"quiet.png", 0, 300, 1920, 480, 255, 48, 48, 90);
  std::printf("  wave pixels: loud %d, loud2 %d, quiet %d\n", loud, loud2, quiet);
  CHECK(loud > 3000 && loud2 > 3000);
  CHECK(quiet * 8 < loud);
  // other layouts, styles and a background image
  fs::path bg = d / L"bg.png";
  CHECK(Ff({"-f", "lavfi", "-i", "color=c=0x00AA00:s=640x360", "-frames:v", "1", Narrow(bg.wstring())}));
  struct Case { m::VizLayout l; m::VizStyle s; int w, h; bool img; const wchar_t* name; };
  for (Case c : {Case{m::VizLayout::Portrait, m::VizStyle::Spectrum, 1080, 1920, true, L"p.mp4"}, Case{m::VizLayout::Square, m::VizStyle::Vectorscope, 1080, 1080, false, L"s.mp4"}}) {
    m::VisualizerRequest r2; r2.layout = c.l; r2.style = c.s; r2.fps = 15; r2.crf = 30; r2.outputPath = d / c.name;
    if (c.img) r2.backgroundImage = bg;
    JobResult j2 = RunJob([&](JobContext& cx) { return m::AudioVisualizer(fx.coverMp3, r2, cx); });
    CHECK(j2.ok);
    if (!j2.ok) { std::printf("  %ls\n", j2.error.c_str()); continue; }
    auto m2 = Pr(j2.outputs[0]);
    CHECK(m2 && m2->FirstVideo()->width == c.w && m2->FirstVideo()->height == c.h && std::fabs(m2->duration - 3.0) < 0.15);
    CHECK(NullDecode(j2.outputs[0]));
    if (c.img) { CHECK(FramePng(j2.outputs[0], 1.0, d / L"pimg.png")); Rgb px = MeanRgb(d / L"pimg.png", 2, 2, 30, 30); CHECK(px.ok && px.g > 120 && px.r < 60); }
  }
  // cover art as background when no image is given
  m::VisualizerRequest rc; rc.layout = m::VizLayout::Square; rc.fps = 12; rc.crf = 32; rc.outputPath = d / L"cover.mp4";
  JobResult jc = RunJob([&](JobContext& c) { return m::AudioVisualizer(fx.coverMp3, rc, c); });
  CHECK(jc.ok);
  CHECK_EQ(FileHash(fx.burstsWav), h0);
  m::VisualizerRequest nov; nov.outputPath = d / L"x.mp4";
  CHECK(!RunJob([&](JobContext& c) { return m::AudioVisualizer(fx.noAudio, nov, c); }).ok);
}
