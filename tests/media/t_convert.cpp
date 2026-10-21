// One real conversion for EVERY registered capability, verified with ffprobe / ffmpeg decoding (independent of our code).
#include "media_test_util.h"
#include "engines/media/media_convert.h"
#include <algorithm>
#include <cmath>
#include <map>

using namespace mt;

static fs::path FixtureFor(const Fx& fx, const std::wstring& fmt) {
  static const std::map<std::wstring, fs::path Fx::*> m = {
    {L"mp4", &Fx::mp4}, {L"mov", &Fx::mov}, {L"mkv", &Fx::mkv}, {L"webm", &Fx::webm}, {L"avi", &Fx::avi}, {L"wmv", &Fx::wmv}, {L"gif", &Fx::gif},
    {L"mp3", &Fx::mp3}, {L"m4a", &Fx::m4a}, {L"wav", &Fx::wav}, {L"flac", &Fx::flac}, {L"ogg", &Fx::ogg}, {L"opus", &Fx::opus}, {L"aiff", &Fx::aiff}, {L"wma", &Fx::wma}};
  auto it = m.find(fmt);
  return it == m.end() ? fs::path() : fx.*(it->second);
}
static bool IsAudioExt(const std::wstring& e) { return e == L"mp3" || e == L"m4a" || e == L"wav" || e == L"flac" || e == L"ogg" || e == L"opus" || e == L"aiff" || e == L"wma"; }
static std::string VideoCodecFor(const std::wstring& c) {
  if (c == L"webm") return "vp9"; if (c == L"avi") return "mpeg4"; if (c == L"wmv") return "wmv2"; if (c == L"gif") return "gif"; return "h264";
}
static std::string AudioCodecFor(const std::wstring& c) {
  if (c == L"mp4" || c == L"mov" || c == L"mkv" || c == L"m4a") return "aac"; if (c == L"webm" || c == L"opus") return "opus"; if (c == L"avi" || c == L"mp3") return "mp3";
  if (c == L"wmv" || c == L"wma") return "wmav2"; if (c == L"ogg") return "vorbis"; if (c == L"flac") return "flac"; if (c == L"wav") return "pcm_s16le"; if (c == L"aiff") return "pcm_s16be";
  return "";
}

TEST(matrix_every_capability) {
  const Fx& fx = Fixtures();
  fs::path outDir = OutDir("matrix");
  int ran = 0, passed = 0;
  for (auto& cap : CapabilityRegistry::Get().All()) {
    if (cap.engine != "ffmpeg") continue;
    std::string id = cap.id;
    fs::path in = FixtureFor(fx, cap.in);
    std::vector<std::wstring> problems;
    auto bad = [&](const std::wstring& m) { problems.push_back(m); };
    if (in.empty() || !fs::exists(in)) { bad(L"no fixture for input"); }
    else {
      CHECK(cap.Runnable());
      auto srcInfo = Pr(in);
      std::string fname = id; std::replace(fname.begin(), fname.end(), '>', '_');
      fs::path out = outDir / Widen(fname + "." + Narrow(cap.out));
      // hardware off for determinism (the NVENC path has its own test)
      Json opt = Json::MakeObject(); opt["hardwareAcceleration"] = false;
      std::wstring outExt = cap.out;
      JobResult r = RunCap(cap.id, in, opt, out);
      ++ran;
      if (!r.ok) bad(L"conversion failed: " + r.error);
      else if (r.outputs.size() != 1 || !fs::exists(r.outputs[0])) bad(L"no output file");
      else {
        fs::path o = r.outputs[0];
        auto mi = Pr(o);
        if (!mi) bad(L"output not readable by ffprobe");
        else if (!srcInfo) bad(L"source not readable");
        else {
          std::string err;
          if (!NullDecode(o, &err)) bad(L"output does not decode cleanly: " + Widen(err.substr(0, 200)));
          bool outAudio = IsAudioExt(outExt);
          if (outAudio) {
            auto* a = mi->FirstAudio();
            if (!a) bad(L"no audio stream");
            else {
              if (a->codec != AudioCodecFor(outExt)) bad(L"audio codec " + Widen(a->codec) + L" != " + Widen(AudioCodecFor(outExt)));
              if (mi->FirstVideo()) bad(L"unexpected video stream in audio file");
              if (a->channels != 2) bad(L"channels " + std::to_wstring(a->channels));
            }
            double want = srcInfo->duration;
            if (std::fabs(mi->duration - want) > 0.15) bad(L"duration " + std::to_wstring(mi->duration) + L" vs " + std::to_wstring(want));
            // content: left channel 440 Hz, right 880 Hz survive the round trip
            double f0 = ChannelFreq(o, 0), f1 = ChannelFreq(o, 1);
            double s0 = ChannelFreq(in, 0), s1 = ChannelFreq(in, 1);   // lossy sources (wma/aac) shift zero-crossing estimates slightly
            if (std::fabs(f0 - s0) > 25 || std::fabs(s0 - 440) > 40) bad(L"left tone " + std::to_wstring(f0) + L" (source " + std::to_wstring(s0) + L")");
            if (std::fabs(f1 - s1) > 40 || std::fabs(s1 - 880) > 60) bad(L"right tone " + std::to_wstring(f1) + L" (source " + std::to_wstring(s1) + L")");
            // lossless chain: PCM identical
            bool srcLossless = cap.in == L"wav" || cap.in == L"flac" || cap.in == L"aiff";
            bool dstLossless = outExt == L"wav" || outExt == L"flac" || outExt == L"aiff";
            if (srcLossless && dstLossless) {
              std::string m1 = PcmMd5(in), m2 = PcmMd5(o);
              if (m1.empty() || m1 != m2) bad(L"lossless PCM differs: " + Widen(m1) + L" vs " + Widen(m2));
            } else {
              auto a1 = DecodeMonoF32(in), a2 = DecodeMonoF32(o);
              double r1 = Rms(a1), r2 = Rms(a2);
              if (r1 <= 0 || std::fabs(20 * std::log10(r2 / r1)) > 2.5) bad(L"level changed: " + std::to_wstring(r1) + L" -> " + std::to_wstring(r2));
            }
          } else {
            auto* v = mi->FirstVideo();
                        if (!v) bad(L"no video stream");
            else {
              if (v->codec != VideoCodecFor(outExt)) bad(L"video codec " + Widen(v->codec) + L" != " + Widen(VideoCodecFor(outExt)));
              int w, h, sw, sh; mi->DisplaySize(w, h); srcInfo->DisplaySize(sw, sh);
              if (w != sw || h != sh) bad(L"size " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" vs " + std::to_wstring(sw) + L"x" + std::to_wstring(sh));
              if (outExt != L"gif" && v->pixFmt != "yuv420p") bad(L"pix_fmt " + Widen(v->pixFmt));
            }
            double want = srcInfo->duration;
            double tol = outExt == L"gif" ? 0.35 : 0.3;
            if (cap.in == L"gif") tol = 0.3;
            if (std::fabs(mi->duration - want) > tol) bad(L"duration " + std::to_wstring(mi->duration) + L" vs " + std::to_wstring(want));
            if (outExt == L"gif") {
              if (mi->FirstAudio()) bad(L"gif has audio");
              int n = CountVideoFrames(o);
              if (n < 10) bad(L"gif frames " + std::to_wstring(n));
            } else if (srcInfo->FirstAudio()) {
              auto* a = mi->FirstAudio();
              if (!a) bad(L"audio track lost");
              else if (a->codec != AudioCodecFor(outExt)) bad(L"audio codec " + Widen(a->codec));
              else { double fo = ChannelFreq(o, 0), fs0 = ChannelFreq(in, 0); if (std::fabs(fo - fs0) > 25) bad(L"audio content changed (left tone " + std::to_wstring(fo) + L" vs source " + std::to_wstring(fs0) + L")"); }
            }
          }
        }
      }
    }
    if (problems.empty()) { ++passed; Record(cap.id, true, L"verified with ffprobe + full decode"); }
    else {
      std::wstring all; for (auto& p : problems) all += p + L"; ";
      std::printf("  [%s] FAIL: %s\n", cap.id.c_str(), Narrow(all).c_str());
      ++ftest::Failures();
      Record(cap.id, false, all);
    }
  }
  std::printf("  matrix: %d/%d capabilities passed\n", passed, ran);
  CHECK(ran > 100);
}
