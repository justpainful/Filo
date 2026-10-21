// Conversion edge cases: rotation, VFR, odd sizes, multiple audio tracks, subtitles, cover art, metadata, options, hardware.
#include "media_test_util.h"
#include "engines/media/media_internal.h"
#include <algorithm>
#include <cmath>

using namespace mt;

static Json Opts(std::initializer_list<std::pair<const char*, Json>> kv) {
  Json j = Json::MakeObject();
  j["hardwareAcceleration"] = false;
  for (auto& [k, v] : kv) j[k] = v;
  return j;
}
static bool HasWarning(const JobResult& r, const wchar_t* needle) {
  for (auto& w : r.warnings) if (w.find(needle) != std::wstring::npos) return true;
  return false;
}
static std::string Tag(const ff::MediaInfo& mi, const char* k) { return mi.raw.Get("format").Get("tags").Get(k).AsString(); }

TEST(rotation_metadata_is_applied) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_rot");
  auto src = Pr(fx.portrait);
  CHECK(src && src->FirstVideo()->rotation == 270 && src->FirstVideo()->width == 320);
  for (auto id : {"mp4>mkv", "mp4>webm", "mp4>avi"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.portrait, Opts({}), d / Widen(std::string("rot.") + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->FirstVideo()->width == 180 && mi->FirstVideo()->height == 320 && mi->FirstVideo()->rotation == 0);   // rotation baked into the pixels
    CHECK(FramePng(r.outputs[0], 1.0, d / Widen(std::string("o_") + ext + ".png")));
    CHECK(FramePng(fx.portrait, 1.0, d / L"ref.png"));
    double diff = PngDiff(d / Widen(std::string("o_") + ext + ".png"), d / L"ref.png", 0, 0, 180, 320);
    CHECK(diff >= 0 && diff < 14);
  }
  // remux keeps the rotation metadata instead (copy mode)
  JobResult c = RunCap("mp4>mkv", fx.portrait, Opts({{"copyStreams", true}}), d / L"copy.mkv");
  CHECK(c.ok);
  if (c.ok) { auto mi = Pr(c.outputs[0]); CHECK(mi && mi->FirstVideo()->rotation == 270 && mi->FirstVideo()->codec == "h264"); CHECK(HasWarning(c, L"copied")); }
}

TEST(variable_frame_rate) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_vfr");
  auto src = Pr(fx.vfr);
  CHECK(src != std::nullopt);
  double srcDur = src ? src->duration : 0;
  CHECK_NEAR(srcDur, 4.5, 0.1);
  // avi / wmv require a constant frame rate: duration and sync survive, a warning is given
  for (auto id : {"mp4>avi", "mp4>wmv"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.vfr, Opts({}), d / Widen("v." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && std::fabs(mi->duration - srcDur) < 0.3);
    std::string rf = Probe1(r.outputs[0], {L"-select_streams", L"v:0", L"-show_entries", L"stream=r_frame_rate,avg_frame_rate"});
    double a = 0, b = 0; sscanf(rf.c_str(), "%lf", &a); (void)b;
    int frames = CountVideoFrames(r.outputs[0]);
    double fps = frames / mi->duration;
    CHECK(fps > 15 && fps < 30);                                    // ~20 fps average kept, not 30 with duplicated frames
    CHECK(HasWarning(r, L"variable frame rate"));
    CHECK(NullDecode(r.outputs[0]));
  }
  // mkv / mp4 keep the original timing
  JobResult m = RunCap("mp4>mkv", fx.vfr, Opts({}), d / L"v.mkv");
  CHECK(m.ok);
  if (m.ok) { auto mi = Pr(m.outputs[0]); CHECK(mi && std::fabs(mi->duration - srcDur) < 0.2); CHECK(std::abs(CountVideoFrames(m.outputs[0]) - 90) <= 2); }
}

TEST(odd_dimensions_and_no_audio) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_odd");
  auto src = Pr(fx.odd);
  CHECK(src && src->FirstVideo()->width == 853 && src->FirstVideo()->height == 479);
  for (auto id : {"mkv>mp4", "mkv>webm", "mkv>avi", "mkv>wmv", "mkv>mov"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.odd, Opts({}), d / Widen("odd." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->FirstVideo()->width == 852 && mi->FirstVideo()->height == 478 && mi->FirstVideo()->pixFmt == "yuv420p");
    CHECK(HasWarning(r, L"odd"));
    CHECK(NullDecode(r.outputs[0]));
  }
  // video without audio: no audio stream invented
  JobResult r = RunCap("mp4>webm", fx.noAudio, Opts({}), d / L"na.webm");
  CHECK(r.ok);
  if (r.ok) { auto mi = Pr(r.outputs[0]); CHECK(mi && !mi->FirstAudio() && mi->FirstVideo()->codec == "vp9"); }
  JobResult g = RunCap("mp4>gif", fx.noAudio, Opts({}), d / L"na.gif");
  CHECK(g.ok);
  JobResult a = RunCap("mp4>mp3", fx.noAudio, Opts({}), d / L"na.mp3");
  CHECK(!a.ok && a.error.find(L"no audio") != std::wstring::npos);        // clear message, nothing written
  CHECK(!fs::exists(d / L"na.mp3"));
}

TEST(multiple_audio_tracks_and_subtitles) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_tracks");
  for (auto id : {"mkv>mp4", "mkv>mov", "mkv>webm", "mkv>avi", "mkv>wmv"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.twoAudio, Opts({}), d / Widen("two." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->AudioCount() == 2);
    // the two tracks keep their identity (440 and 880 Hz)
    std::string o1 = FfOutput({"-i", Narrow(r.outputs[0].wstring()), "-map", "0:a:0", "-ac", "1", "-ar", "16000", "-t", "1.5", "-f", "f32le", "pipe:1"});
    std::string o2 = FfOutput({"-i", Narrow(r.outputs[0].wstring()), "-map", "0:a:1", "-ac", "1", "-ar", "16000", "-t", "1.5", "-f", "f32le", "pipe:1"});
    auto zc = [](const std::string& s) { const float* f = (const float*)s.data(); size_t n = s.size() / 4, z = 0; for (size_t i = 1; i < n; ++i) if ((f[i - 1] < 0) != (f[i] < 0)) ++z; return n ? z / 2.0 / (n / 16000.0) : 0.0; };
    CHECK_NEAR(zc(o1), 440, 40); CHECK_NEAR(zc(o2), 880, 60);
  }
  // audio extraction takes the first track and says so
  JobResult x = RunCap("mkv>mp3", fx.twoAudio, Opts({}), d / L"first.mp3");
  CHECK(x.ok && HasWarning(x, L"first one"));
  if (x.ok) CHECK_NEAR(ChannelFreq(x.outputs[0], 0), 440, 40);
  // subtitles: kept as mov_text in mp4 / mov, webvtt in webm, srt copied into mkv; dropped (with a warning) for avi / wmv
  auto subCodec = [&](const fs::path& p) { auto mi = Pr(p); std::string c; if (mi) for (auto& s : mi->streams) if (s.type == "subtitle") c = s.codec; return c; };
  struct Case { const char* id; const char* ext; const char* codec; };
  for (Case c : {Case{"mkv>mp4", "mp4", "mov_text"}, Case{"mkv>mov", "mov", "mov_text"}, Case{"mkv>webm", "webm", "webvtt"}, Case{"mkv>avi", "avi", ""}, Case{"mkv>wmv", "wmv", ""}}) {
    JobResult r = RunCap(c.id, fx.subMkv, Opts({}), d / Widen(std::string("sub.") + c.ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", c.id, r.error.c_str()); continue; }
    CHECK(subCodec(r.outputs[0]) == c.codec);
    if (std::string(c.codec).empty()) CHECK(HasWarning(r, L"subtitle"));
  }
  // the subtitle text survived the mp4 conversion
  std::string txt = FfOutput({"-i", Narrow((d / L"sub.mp4").wstring()), "-map", "0:s:0", "-f", "srt", "-"});
  CHECK(txt.find("Hello Filo") != std::string::npos);
}

TEST(cover_art_and_metadata) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_cover");
  auto hasCover = [](const ff::MediaInfo& mi) { for (auto& s : mi.streams) if (s.attachedPic) return true; return false; };
  for (auto id : {"mp3>m4a", "mp3>flac"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.coverMp3, Opts({}), d / Widen("c." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && hasCover(*mi) && !mi->FirstVideo());                                  // picture kept as cover, not as a video stream
    CHECK(mi && (Tag(*mi, "title") == "Fixture Title" || Tag(*mi, "TITLE") == "Fixture Title"));
    CHECK(mi && (Tag(*mi, "artist") == "Filo Tests" || Tag(*mi, "ARTIST") == "Filo Tests"));
  }
  for (auto id : {"mp3>wav", "mp3>ogg", "mp3>opus", "mp3>aiff", "mp3>wma"}) {
    std::string ext = std::string(id).substr(4);
    JobResult r = RunCap(id, fx.coverMp3, Opts({}), d / Widen("c." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && !mi->FirstVideo() && mi->streams.size() == 1);                       // never a video stream in an audio-only format
    CHECK(HasWarning(r, L"cover"));
  }
  // preserveMetadata=false strips tags and cover
  JobResult s = RunCap("mp3>m4a", fx.coverMp3, Opts({{"preserveMetadata", false}}), d / L"strip.m4a");
  CHECK(s.ok);
  if (s.ok) { auto mi = Pr(s.outputs[0]); CHECK(mi && !hasCover(*mi) && Tag(*mi, "title").empty() && Tag(*mi, "artist").empty()); }
  // video metadata: title survives mp4 -> mkv, and is removed on request
  fs::path tagged = d / L"tagged.mp4";
  CHECK(Ff({"-i", Narrow(fx.mp4.wstring()), "-c", "copy", "-metadata", "title=My Holiday", "-metadata", "comment=secret note", Narrow(tagged.wstring())}));
  JobResult k = RunCap("mp4>mkv", tagged, Opts({}), d / L"tagged.mkv");
  CHECK(k.ok);
  if (k.ok) { auto mi = Pr(k.outputs[0]); CHECK(mi && (Tag(*mi, "title") == "My Holiday" || Tag(*mi, "TITLE") == "My Holiday")); }
  JobResult n = RunCap("mp4>mkv", tagged, Opts({{"preserveMetadata", false}}), d / L"clean.mkv");
  CHECK(n.ok);
  if (n.ok) { auto mi = Pr(n.outputs[0]); CHECK(mi && Tag(*mi, "title").empty() && Tag(*mi, "TITLE").empty() && Tag(*mi, "comment").empty() && Tag(*mi, "COMMENT").empty()); }
}

TEST(options_codecs_audio_params) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_opts");
  // codec choices
  struct C { const char* id; const char* codec; const char* expect; const char* ext; };
  for (C c : {C{"mp4>mp4x", "hevc", "hevc", "mp4"}, C{"mp4>mkv", "av1", "av1", "mkv"}, C{"mp4>webm", "av1", "av1", "webm"}, C{"mp4>mkv", "vp9", "vp9", "mkv"}, C{"mp4>avi", "h264", "h264", "avi"}}) {
    std::string id = c.id; if (id == "mp4>mp4x") id = "mov>mp4";
    JobResult r = RunCap(id, id == "mov>mp4" ? fx.mov : fx.mp4, Opts({{"codec", c.codec}, {"crf", 30}}), d / Widen(std::string(c.codec) + "." + c.ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s %s: %ls\n", id.c_str(), c.codec, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->FirstVideo()->codec == c.expect);
    if (std::string(c.codec) == "hevc") CHECK(Probe1(r.outputs[0], {L"-select_streams", L"v:0", L"-show_entries", L"stream=codec_tag_string"}) == "hvc1");
    CHECK(NullDecode(r.outputs[0]));
  }
  // an impossible codec for the container falls back with a warning
  JobResult bad = RunCap("mp4>webm", fx.mp4, Opts({{"codec", "hevc"}}), d / L"fallback.webm");
  CHECK(bad.ok && HasWarning(bad, L"cannot be stored"));
  if (bad.ok) CHECK(Pr(bad.outputs[0])->FirstVideo()->codec == "vp9");
  // crf changes the size monotonically
  JobResult lo = RunCap("mp4>mkv", fx.big, Opts({{"crf", 18}}), d / L"q18.mkv"), hi = RunCap("mp4>mkv", fx.big, Opts({{"crf", 34}}), d / L"q34.mkv");
  CHECK(lo.ok && hi.ok);
  if (lo.ok && hi.ok) { std::error_code ec; CHECK(fs::file_size(lo.outputs[0], ec) > fs::file_size(hi.outputs[0], ec) * 2); }
  // audio parameters: bitrate, mono, sample rate
  JobResult a = RunCap("wav>mp3", fx.wav, Opts({{"audioBitrateKbps", 96}, {"channels", 1}, {"sampleRate", 22050}}), d / L"a.mp3");
  CHECK(a.ok);
  if (a.ok) {
    auto mi = Pr(a.outputs[0]);
    CHECK(mi && mi->FirstAudio()->channels == 1 && mi->FirstAudio()->sampleRate == 22050);
    CHECK(mi && std::fabs(mi->FirstAudio()->bitRate - 96000) < 6000);
  }
  JobResult big = RunCap("wav>mp3", fx.wav, Opts({{"audioBitrateKbps", 320}}), d / L"b.mp3");
  CHECK(big.ok);
  if (big.ok) CHECK(std::fabs(Pr(big.outputs[0])->FirstAudio()->bitRate - 320000) < 12000);
  // opus always runs at 48 kHz; 24-bit sources stay 24-bit in lossless targets
  JobResult o = RunCap("wav>opus", fx.wav, Opts({}), d / L"o.opus");
  CHECK(o.ok && Pr(o.outputs[0])->FirstAudio()->sampleRate == 48000);
  JobResult f24 = RunCap("wav>flac", fx.wav24, Opts({}), d / L"f24.flac");
  CHECK(f24.ok);
  if (f24.ok) {
    auto st = Probe1(f24.outputs[0], {L"-select_streams", L"a:0", L"-show_entries", L"stream=bits_per_raw_sample"});
    CHECK(st == "24");
    JobResult back = RunCap("flac>wav", f24.outputs[0], Opts({}), d / L"back24.wav");
    CHECK(back.ok);
    if (back.ok) {
      CHECK(Probe1(back.outputs[0], {L"-select_streams", L"a:0", L"-show_entries", L"stream=codec_name"}) == "pcm_s24le");
      CHECK(PcmMd5(fx.wav24) == PcmMd5(back.outputs[0]));      // decoded at s16 by both; lossless chain
    }
  }
  JobResult f16 = RunCap("wav>flac", fx.wav24, Opts({{"bitDepth", 16}}), d / L"f16.flac");
  CHECK(f16.ok && Probe1(f16.outputs[0], {L"-select_streams", L"a:0", L"-show_entries", L"stream=bits_per_raw_sample"}) == "16");
  JobResult alac = RunCap("wav>m4a", fx.wav, Opts({{"lossless", true}}), d / L"alac.m4a");
  CHECK(alac.ok && Pr(alac.outputs[0])->FirstAudio()->codec == "alac");
  if (alac.ok) CHECK(PcmMd5(alac.outputs[0]) == PcmMd5(fx.wav));
  // copyStreams: video -> mp3 / m4a copies the audio when it already has the right codec
  JobResult cm = RunCap("avi>mp3", fx.avi, Opts({{"copyStreams", true}}), d / L"copy.mp3");
  CHECK(cm.ok && Pr(cm.outputs[0])->FirstAudio()->codec == "mp3");
  JobResult c4 = RunCap("mp4>m4a", fx.mp4, Opts({{"copyStreams", true}}), d / L"copy.m4a");
  CHECK(c4.ok && Pr(c4.outputs[0])->FirstAudio()->codec == "aac");
  // copyStreams where it is impossible: mkv(h264) -> webm re-encodes and says so
  JobResult ci = RunCap("mkv>webm", fx.mkv, Opts({{"copyStreams", true}}), d / L"nocopy.webm");
  CHECK(ci.ok && Pr(ci.outputs[0])->FirstVideo()->codec == "vp9" && HasWarning(ci, L"re-encoded"));
  // gif options
  JobResult g = RunCap("mp4>gif", fx.mp4, Opts({{"gifFps", 5}, {"gifWidth", 100}}), d / L"g.gif");
  CHECK(g.ok);
  if (g.ok) {
    auto mi = Pr(g.outputs[0]);
    CHECK(mi && mi->FirstVideo()->width == 100 && mi->FirstVideo()->height == 75 && std::fabs(mi->duration - 3.0) < 0.35);
    CHECK(std::abs(CountVideoFrames(g.outputs[0]) - 15) <= 2);
  }
  // gif -> video
  JobResult gv = RunCap("gif>mp4", fx.gif, Opts({}), d / L"g.mp4");
  CHECK(gv.ok);
}

TEST(hardware_acceleration_only_when_it_works) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_hw");
  bool listed = ff::HasEncoder("h264_nvenc");
  bool usable = filo::media::detail::NvencUsable(filo::media::detail::VCodec::H264);
  std::printf("  h264_nvenc listed=%d trial-encode-ok=%d\n", (int)listed, (int)usable);
  Json hw = Json::MakeObject(); hw["hardwareAcceleration"] = true;
  JobResult r = RunCap("mp4>mkv", fx.big, hw, d / L"hw.mkv");
  CHECK(r.ok);
  if (r.ok) {
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->FirstVideo()->codec == "h264" && std::fabs(mi->duration - 8.0) < 0.2);
    CHECK(NullDecode(r.outputs[0]));
    // the encoder tag reveals which encoder ran
    std::string tag = Probe1(r.outputs[0], {L"-select_streams", L"v:0", L"-show_entries", L"stream_tags=ENCODER"});
    std::printf("  encoder tag: %s\n", tag.c_str());
    if (usable) CHECK(tag.find("nvenc") != std::string::npos); else CHECK(tag.find("libx264") != std::string::npos || tag.empty());
  } else std::printf("  %ls\n", r.error.c_str());
  Json sw = Json::MakeObject(); sw["hardwareAcceleration"] = false;
  JobResult s = RunCap("mp4>mkv", fx.big, sw, d / L"sw.mkv");
  CHECK(s.ok);
  if (s.ok) CHECK(Probe1(s.outputs[0], {L"-select_streams", L"v:0", L"-show_entries", L"stream_tags=ENCODER"}).find("libx264") != std::string::npos);
}

TEST(hdr_ten_bit_surround_highrate_and_tiny) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("special_misc");
  // 10-bit HDR10-tagged clip -> 8-bit yuv420p with a clear warning
  fs::path hdr = d / L"hdr.mkv";
  bool made = Ff({"-f", "lavfi", "-i", "testsrc2=s=320x240:r=25:d=1", "-c:v", "libx265", "-preset", "ultrafast", "-pix_fmt", "yuv420p10le", "-x265-params",
                  "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc", Narrow(hdr.wstring())});
  CHECK(made);
  if (made) {
    JobResult r = RunCap("mkv>mp4", hdr, Opts({}), d / L"hdr.mp4");
    CHECK(r.ok && HasWarning(r, L"HDR"));
    if (r.ok) CHECK(Pr(r.outputs[0])->FirstVideo()->pixFmt == "yuv420p");
  }
  // 5.1 audio: kept where the format allows it, mixed down with a warning where it does not
  fs::path surround = d / L"surround.wav";
  CHECK(Ff({"-f", "lavfi", "-i", "sine=f=500:r=48000:d=2", "-af", "aformat=channel_layouts=5.1", "-c:a", "pcm_s16le", Narrow(surround.wstring())}));
  struct S { const char* id; int channels; bool warn; };
  for (S s : {S{"wav>m4a", 6, false}, S{"wav>flac", 6, false}, S{"wav>ogg", 6, false}, S{"wav>opus", 6, false}, S{"wav>aiff", 6, false}, S{"wav>mp3", 2, true}, S{"wav>wma", 2, true}}) {
    std::string ext = std::string(s.id).substr(4);
    JobResult r = RunCap(s.id, surround, Opts({}), d / Widen("s." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", s.id, r.error.c_str()); continue; }
    auto mi = Pr(r.outputs[0]);
    CHECK(mi && mi->FirstAudio()->channels == s.channels);
    CHECK_EQ(HasWarning(r, L"mixed down"), s.warn);
    CHECK(NullDecode(r.outputs[0]));
  }
  // 5.1 video audio -> mkv keeps six channels
  fs::path v51 = d / L"v51.mp4";
  CHECK(Ff({"-f", "lavfi", "-i", "testsrc2=s=160x120:r=25:d=1", "-i", Narrow(surround.wstring()), "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", Narrow(v51.wstring())}));
  JobResult k = RunCap("mp4>mkv", v51, Opts({}), d / L"v51.mkv");
  CHECK(k.ok && Pr(k.outputs[0])->FirstAudio()->channels == 6);
  // 96 kHz source: formats with a lower limit are resampled to the nearest allowed rate, the rest keep 96 kHz
  fs::path hi = d / L"hi96.wav";
  CHECK(Ff({"-f", "lavfi", "-i", "sine=f=440:r=96000:d=2", "-ac", "2", "-c:a", "pcm_s16le", Narrow(hi.wstring())}));
  struct R { const char* id; int rate; };
  for (R x : {R{"wav>mp3", 48000}, R{"wav>wma", 48000}, R{"wav>opus", 48000}, R{"wav>flac", 96000}, R{"wav>aiff", 96000}, R{"wav>ogg", 96000}, R{"wav>m4a", 96000}}) {
    std::string ext = std::string(x.id).substr(4);
    JobResult r = RunCap(x.id, hi, Opts({}), d / Widen("h." + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %s: %ls\n", x.id, r.error.c_str()); continue; }
    CHECK_EQ(Pr(r.outputs[0])->FirstAudio()->sampleRate, x.rate);
    CHECK_NEAR(Pr(r.outputs[0])->duration, 2.0, 0.12);
  }
  // 8 kHz mono telephone-quality source into every audio format
  fs::path tel = d / L"tel.wav";
  CHECK(Ff({"-f", "lavfi", "-i", "sine=f=300:r=8000:d=1", "-ac", "1", "-c:a", "pcm_s16le", Narrow(tel.wstring())}));
  for (auto ext : {"mp3", "m4a", "flac", "ogg", "opus", "aiff", "wma"}) {
    JobResult r = RunCap(std::string("wav>") + ext, tel, Opts({}), d / Widen(std::string("t.") + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  wav>%s: %ls\n", ext, r.error.c_str()); continue; }
    CHECK(NullDecode(r.outputs[0]));
    CHECK_EQ(Pr(r.outputs[0])->FirstAudio()->channels, 1);
  }
  // a 0.5 s 64x64 clip into every video container and gif (smallest legal sizes for SVT-AV1 / x265)
  for (auto ext : {"mkv", "webm", "avi", "wmv", "mov", "gif"}) {
    JobResult r = RunCap(std::string("mp4>") + ext, fx.tiny, Opts({}), d / Widen(std::string("tiny.") + ext));
    CHECK(r.ok);
    if (!r.ok) { std::printf("  mp4>%s: %ls\n", ext, r.error.c_str()); continue; }
    CHECK(NullDecode(r.outputs[0]));
  }
  JobResult av1 = RunCap("mp4>mkv", fx.tiny, Opts({{"codec", "av1"}}), d / L"tiny_av1.mkv");
  CHECK(av1.ok);
  JobResult hevc = RunCap("mp4>mp4x", fx.tiny, Opts({{"codec", "hevc"}}), d / L"tiny_hevc.mp4");
  CHECK(!hevc.ok);   // unknown capability id: reported, not a crash
  JobResult hevc2 = RunCap("mov>mp4", fx.mov, Opts({{"codec", "hevc"}}), d / L"hevc.mp4");
  CHECK(hevc2.ok);
}

// Sweeps source sample rates / channel counts / frame rates / odd sizes through every encoder path: catches "encoder could not be started" combos.
TEST(parameter_sweep_audio_and_video) {
  fs::path d = OutDir("special_sweep");
  int audioRuns = 0, audioBad = 0;
  for (int rate : {8000, 11025, 22050, 32000, 44100, 48000, 88200, 96000, 192000}) {
    for (int ch : {1, 2, 6}) {
      fs::path src = d / Widen("src_" + std::to_string(rate) + "_" + std::to_string(ch) + ".wav");
      std::string layout = ch == 1 ? "mono" : ch == 2 ? "stereo" : "5.1";
      if (!Ff({"-f", "lavfi", "-i", "sine=f=440:r=" + std::to_string(rate) + ":d=0.6", "-af", "aformat=channel_layouts=" + layout, "-c:a", "pcm_s16le", Narrow(src.wstring())})) { CHECK(false); continue; }
      for (auto ext : {"mp3", "m4a", "flac", "ogg", "opus", "aiff", "wma", "wav"}) {
        if (std::string(ext) == "wav") continue;
        ++audioRuns;
        JobResult r = RunCap(std::string("wav>") + ext, src, Opts({}), d / Widen(std::to_string(rate) + "_" + std::to_string(ch) + "." + ext));
        bool ok = r.ok;
        if (ok) { auto mi = Pr(r.outputs[0]); ok = mi && mi->FirstAudio() && std::fabs(mi->duration - 0.6) < 0.15 && mi->FirstAudio()->channels >= 1; }
        if (!ok) { ++audioBad; std::printf("  FAIL wav>%s rate %d ch %d: %ls\n", ext, rate, ch, r.error.c_str()); CHECK(false); }
        std::error_code ec; if (r.ok) fs::remove(r.outputs[0], ec);
      }
      std::error_code ec; fs::remove(src, ec);
    }
  }
  std::printf("  audio sweep: %d runs, %d failures\n", audioRuns, audioBad);
  int videoRuns = 0, videoBad = 0;
  struct Sz { int w, h; };
  for (Sz sz : {Sz{96, 96}, Sz{134, 78}, Sz{250, 140}}) {
    for (const char* fps : {"5", "24000/1001", "30", "60"}) {
      fs::path src = d / Widen("v_" + std::to_string(sz.w) + "x" + std::to_string(sz.h) + "_" + std::string(fps == std::string("24000/1001") ? "23976" : fps) + ".mp4");
      if (!Ff({"-f", "lavfi", "-i", "testsrc2=s=" + std::to_string(sz.w) + "x" + std::to_string(sz.h) + ":r=" + fps + ":d=0.7", "-f", "lavfi", "-i", "sine=d=0.7", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", Narrow(src.wstring())})) { CHECK(false); continue; }
      for (auto ext : {"mkv", "webm", "avi", "wmv", "mov", "gif"}) {
        ++videoRuns;
        JobResult r = RunCap(std::string("mp4>") + ext, src, Opts({}), d / Widen(src.stem().string() + "." + ext));
        bool ok = r.ok;
        if (ok) { auto mi = Pr(r.outputs[0]); ok = mi && mi->FirstVideo() && mi->FirstVideo()->width == sz.w && mi->FirstVideo()->height == sz.h && std::fabs(mi->duration - 0.7) < 0.45; }
        if (!ok) { ++videoBad; std::printf("  FAIL mp4>%s %dx%d @%s: %ls\n", ext, sz.w, sz.h, fps, r.error.c_str()); CHECK(false); }
        std::error_code ec; if (r.ok) fs::remove(r.outputs[0], ec);
      }
    }
  }
  std::printf("  video sweep: %d runs, %d failures\n", videoRuns, videoBad);
}
