// Audio operations: loudness normalisation, channel conversion, bleeping, previews and waveform extraction.
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <rpc.h>

namespace filo::media {
using namespace detail;

namespace {

template <class R> R Failed(const std::wstring& why) { R r; static_cast<JobResult&>(r) = JobResult::Fail(why); return r; }

bool NeedAudio(const fs::path& input, Probed& pr, std::wstring& err) {
  if (!ProbeFile(input, pr, err)) return false;
  if (!pr.audio) { err = L"This file has no audio track."; return false; }
  return true;
}

// ---- loudnorm json -------------------------------------------------------------------------------------------------------------
struct LoudJson {
  bool ok = false;
  double inI = -70, inTP = -70, inLRA = 0, inThresh = -70, offset = 0;
  double outI = -70, outTP = -70, outLRA = 0;
  std::string type;
};
double JNum(const Json& j, const char* k, double def) {
  const Json& v = j.Get(k);
  if (v.IsString()) { const std::string& s = v.AsString(); if (s == "-inf") return -120; if (s == "inf") return 120; }
  return v.AsNumber(def);
}
LoudJson ParseLoudnorm(const std::string& err) {
  LoudJson l;
  size_t end = err.rfind('}');
  if (end == std::string::npos) return l;
  size_t beg = err.rfind('{', end);
  if (beg == std::string::npos) return l;
  auto j = Json::Parse(std::string_view(err).substr(beg, end - beg + 1));
  if (!j || !j->Has("input_i")) return l;
  l.inI = JNum(*j, "input_i", -70); l.inTP = JNum(*j, "input_tp", -70); l.inLRA = JNum(*j, "input_lra", 0); l.inThresh = JNum(*j, "input_thresh", -70);
  l.offset = JNum(*j, "target_offset", 0);
  l.outI = JNum(*j, "output_i", -70); l.outTP = JNum(*j, "output_tp", -70); l.outLRA = JNum(*j, "output_lra", 0);
  l.type = j->Get("normalization_type").AsString();
  l.ok = true;
  return l;
}

LoudJson MeasurePass(const fs::path& path, int audioIndex, double I, double TP, double LRA, double dur, JobContext& ctx, double p0, double p1, std::wstring* error) {
  Args a = {L"-i", P(path), L"-map", L"0:" + std::to_wstring(audioIndex), L"-vn", L"-af",
            L"loudnorm=I=" + N(I, 2) + L":TP=" + N(TP, 2) + L":LRA=" + N(LRA, 2) + L":print_format=json", L"-f", L"null", L"-"};
  FfOut r = RunFfmpeg(a, dur, ctx, p0, p1, true);
  LoudJson l;
  if (!r.ok) { if (error) *error = r.error; return l; }
  l = ParseLoudnorm(r.stderrText);
  if (!l.ok && error) *error = L"The loudness of this file could not be measured.";
  return l;
}

LoudnessInfo ToInfo(const LoudJson& l) {
  LoudnessInfo i; i.valid = l.ok && l.inI > -69; i.integratedLufs = l.inI; i.truePeakDb = l.inTP; i.lra = l.inLRA; i.thresholdLufs = l.inThresh; return i;
}

// ---- audio fx graph (channels + bleeps) -----------------------------------------------------------------------------------------
struct FxSpec {
  int srcChannels = 2;
  std::string layout = "stereo";
  int sampleRate = 44100;
  double leftGain = 1, rightGain = 1;
  int outChannels = 0;                   // 0 keep (but gains applied for 1/2 channel sources), 1, 2
  std::vector<BleepRange> bleeps;        // timeline of the filter input
  BleepKind kind = BleepKind::Tone;
  double toneHz = 1000, toneDb = -12;
  int fadeMs = 8;
  double totalSeconds = 0;               // length of the input (for the tone generator)
  bool forceFilter = false;
};

std::vector<BleepRange> MergeRanges(std::vector<BleepRange> v, double maxT = 1e9) {
  std::vector<BleepRange> o;
  for (auto& r : v) { r.start = std::max(0.0, r.start); r.end = std::min(maxT, r.end); if (r.end - r.start > 0.002) o.push_back(r); }
  std::sort(o.begin(), o.end(), [](auto& a, auto& b) { return a.start < b.start; });
  std::vector<BleepRange> m;
  for (auto& r : o) { if (!m.empty() && r.start <= m.back().end) m.back().end = std::max(m.back().end, r.end); else m.push_back(r); }
  return m;
}

bool FxNeeded(const FxSpec& f) {
  return f.forceFilter || !f.bleeps.empty() || f.outChannels != 0 || std::fabs(f.leftGain - 1) > 1e-9 || std::fabs(f.rightGain - 1) > 1e-9;
}

// Returns a filter_complex string "[0:a:N]...[aout]" (empty when nothing to do). `inLabel` is e.g. L"[0:1]".
std::wstring BuildFx(const FxSpec& f, const std::wstring& inLabel, const wchar_t* outLabel = L"[aout]") {
  std::wstring g = inLabel;
  std::wstring chain;
  auto gain = [&](double v) { return N(std::max(0.0, v), 5); };
  int ch = f.srcChannels;
  int outCh = f.outChannels > 0 ? f.outChannels : (ch == 1 ? 1 : 2);
  bool needsPan = f.outChannels != 0 || std::fabs(f.leftGain - 1) > 1e-9 || std::fabs(f.rightGain - 1) > 1e-9;
  if (needsPan) {
    if (ch > 2) chain += L"aformat=channel_layouts=stereo,";
    int inCh = ch > 2 ? 2 : ch;
    if (inCh == 1) {
      chain += outCh == 1 ? L"volume=" + gain(f.leftGain) : L"pan=stereo|c0=" + gain(f.leftGain) + L"*c0|c1=" + gain(f.rightGain) + L"*c0";
    } else {
      chain += outCh == 1 ? L"pan=mono|c0=" + gain(0.5 * f.leftGain) + L"*c0+" + gain(0.5 * f.rightGain) + L"*c1"
                          : L"pan=stereo|c0=" + gain(f.leftGain) + L"*c0|c1=" + gain(f.rightGain) + L"*c1";
    }
  } else if (f.outChannels == 0 && ch > 2) {
    // untouched layout
  }
  auto ranges = MergeRanges(f.bleeps, f.totalSeconds > 0 ? f.totalSeconds : 1e9);
  std::wstring outLayout = f.outChannels == 1 ? L"mono" : f.outChannels == 2 ? L"stereo" : (ch == 1 ? L"mono" : Widen(f.layout.empty() ? "stereo" : f.layout));
  if (ch > 2 && f.outChannels == 0) outLayout = Widen(f.layout);
  if (outLayout.empty()) outLayout = L"stereo";
  if (!ranges.empty()) {
    double fd = std::max(0.001, f.fadeMs / 1000.0);
    std::wstring env;
    for (size_t i = 0; i < ranges.size(); ++i) {
      std::wstring s = N(ranges[i].start, 4), e = N(ranges[i].end, 4);
      if (i) env += L"+";
      env += L"between(t," + s + L"," + e + L")*min(1,min(t-" + s + L"," + e + L"-t)/" + N(fd, 4) + L")";
    }
    env = L"min(1," + env + L")";
    if (!chain.empty()) chain += L",";
    chain += L"volume=eval=frame:volume='1-(" + env + L")'";
    if (f.kind == BleepKind::Tone) {
      double amp = std::pow(10.0, f.toneDb / 20.0);
      std::wstring body = !chain.empty() ? g + chain + L"[m];" : g + L"anull[m];";
      body += L"aevalsrc=exprs='" + N(amp, 5) + L"*sin(2*PI*" + N(f.toneHz, 2) + L"*t)*(" + env + L")':s=" + std::to_wstring(f.sampleRate) + L":d=" + N(f.totalSeconds + 1.0, 3) +
              L",aformat=sample_rates=" + std::to_wstring(f.sampleRate) + L":channel_layouts=" + outLayout + L"[b];[m][b]amix=inputs=2:normalize=0:duration=first" + outLabel;
      return body;
    }
  }
  if (chain.empty()) chain = L"anull";
  return g + chain + outLabel;
}

FxSpec SpecFor(const Probed& pr, double totalSeconds) {
  FxSpec f;
  f.srcChannels = pr.audio->channels > 0 ? pr.audio->channels : 2;
  f.layout = ChannelLayoutOf(pr.mi, *pr.audio);
  f.sampleRate = pr.audio->sampleRate > 0 ? pr.audio->sampleRate : 44100;
  f.totalSeconds = totalSeconds;
  return f;
}

// Shared by ConvertChannels / BleepAudio: renders the filtered audio of `input` to an audio file or back into a video.
JobResult ApplyFx(const fs::path& input, const Probed& pr, const FxSpec& fx, const std::wstring& outExtWanted, const std::wstring& suffix, bool preserve, int kbps,
                  const fs::path& outputPath, JobContext& ctx) {
  std::wstring label = L"[0:" + std::to_wstring(pr.audio->index) + L"]";
  std::wstring fc = BuildFx(fx, label);
  bool video = pr.video != nullptr && outExtWanted.empty();
  std::vector<std::wstring> warnings;
  if (video) {
    std::wstring container = OutputContainerFor(input);
    return WithTransaction(input, container, suffix, outputPath, ctx, [&](const fs::path& out) {
      StdEncode e; e.container = container; e.preserve = preserve; e.copyVideo = true; e.allowAudioCopy = false; e.audioKbps = kbps > 0 ? kbps : LikeSourceKbps(pr, 0);
      e.filterComplex = fc; e.audioLabel = L"[aout]";
      if (pr.mi.AudioCount() > 1) warnings.push_back(L"Only the first audio track was processed; the other tracks were left out.");
      Args a = {L"-i", P(input)};
      Args rest = BuildStdOutputArgs(pr, e, L"", L"", warnings);
      a.insert(a.end(), rest.begin(), rest.end());
      a.push_back(P(out));
      FfOut r = RunFfmpeg(a, pr.duration, ctx);
      EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
      er.warnings = warnings;
      return er;
    }, VerifyHasVideo);
  }
  std::wstring ext = outExtWanted.empty() ? AudioFormatFor(input) : outExtWanted;
  if (ext.empty()) { ext = L"wav"; warnings.push_back(L"This audio format cannot be written directly; the result was saved as WAV."); }
  if (!IsAudioFormat(ext)) return JobResult::Fail(L"That output format is not an audio format.");
  return WithTransaction(input, ext, suffix, outputPath, ctx, [&](const fs::path& out) {
    AudioEnc ae; ae.kbps = LikeSourceKbps(pr, kbps);
    Args a = {L"-i", P(input)};
    Args rest = AudioFileOutputArgs(pr, ext, ae, preserve, false, L"", fc, L"[aout]", warnings);
    a.insert(a.end(), rest.begin(), rest.end());
    a.push_back(P(out));
    FfOut r = RunFfmpeg(a, pr.duration, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    return er;
  }, VerifyHasAudio);
}

} // namespace

// ================================================================================================================================
// Loudness
// ================================================================================================================================
LoudnessInfo MeasureLoudness(const fs::path& input, JobContext* ctx, std::wstring* error) {
  Probed pr; std::wstring err;
  if (!NeedAudio(input, pr, err)) { if (error) *error = err; return {}; }
  JobContext local;   // JobContext is not copyable but default-constructible within the engine: used only for cancel/progress sinks
  JobContext& c = ctx ? *ctx : local;
  LoudJson l = MeasurePass(input, pr.audio->index, -24, -2, 7, pr.duration, c, 0, 1, &err);
  if (!l.ok) { if (error) *error = err; return {}; }
  return ToInfo(l);
}

NormalizeResult NormalizeVolume(const fs::path& input, const NormalizeRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedAudio(input, pr, err)) return Failed<NormalizeResult>(err);
  double I = std::max(-70.0, std::min(-5.0, req.targetLufs)), TP = std::max(-9.0, std::min(0.0, req.truePeakDb)), LRA = std::max(1.0, std::min(50.0, req.lra));
  NormalizeResult res;
  LoudJson m = MeasurePass(input, pr.audio->index, I, TP, LRA, pr.duration, ctx, 0.0, 0.4, &err);
  if (ctx.Cancelled()) return Failed<NormalizeResult>(L"Cancelled");
  if (!m.ok) return Failed<NormalizeResult>(err);
  res.input = ToInfo(m);
  if (!res.input.valid) return Failed<NormalizeResult>(L"The audio is silent, so there is nothing to normalise.");
  res.gainDb = I - m.inI;
  const int sr = pr.audio->sampleRate > 0 ? pr.audio->sampleRate : 44100;
  // second pass filter for target loudness `target` (offset follows the target so a corrected retry stays consistent)
  auto makeAf = [&](double target) {
    return L"loudnorm=I=" + N(target, 2) + L":TP=" + N(TP, 2) + L":LRA=" + N(LRA, 2) + L":measured_I=" + N(m.inI, 2) + L":measured_TP=" + N(m.inTP, 2) +
           L":measured_LRA=" + N(m.inLRA, 2) + L":measured_thresh=" + N(m.inThresh, 2) + L":offset=" + N(m.offset + (target - I), 2) + L":linear=true:print_format=json,aresample=" +
           std::to_wstring(sr);
  };
  std::vector<std::wstring> warnings;
  bool video = pr.video != nullptr;
  std::wstring container = video ? OutputContainerFor(input) : AudioFormatFor(input);
  if (container.empty()) { container = L"wav"; warnings.push_back(L"This audio format cannot be written directly; the result was saved as WAV."); }
  std::string normType;
  JobResult jr = WithTransaction(input, container, L" (normalized)", req.outputPath, ctx, [&](const fs::path& out) {
    double target = I;
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::wstring af = makeAf(target);
      Args a = {L"-i", P(input)};
      std::vector<std::wstring> w2;
      if (video) {
        StdEncode e; e.container = container; e.preserve = req.preserveMetadata; e.copyVideo = true; e.allowAudioCopy = false; e.audioKbps = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : LikeSourceKbps(pr, 0);
        if (pr.mi.AudioCount() > 1) w2.push_back(L"Only the first audio track was normalised; the other tracks were left out.");
        // map only the first audio stream: build the standard args on a probe copy that exposes just that stream
        Probed first = pr;
        first.mi.streams.erase(std::remove_if(first.mi.streams.begin(), first.mi.streams.end(), [&](const ff::StreamInfo& s) { return s.type == "audio" && s.index != pr.audio->index; }), first.mi.streams.end());
        first.video = first.mi.FirstVideo(); first.audio = first.mi.FirstAudio();
        Args rest = BuildStdOutputArgs(first, e, L"", af, w2);
        a.insert(a.end(), rest.begin(), rest.end());
      } else {
        AudioEnc ae; ae.kbps = LikeSourceKbps(pr, req.audioBitrateKbps);
        Args rest = AudioFileOutputArgs(pr, container, ae, req.preserveMetadata, false, af, L"", L"", w2);
        a.insert(a.end(), rest.begin(), rest.end());
      }
      a.push_back(P(out));
      FfOut r = RunFfmpeg(a, pr.duration, ctx, attempt ? 0.55 : 0.4, attempt ? 0.85 : 0.7, true);
      if (!r.ok) return EngineResult::Fail(r.error);
      normType = ParseLoudnorm(r.stderrText).type;
      if (attempt == 0) warnings.insert(warnings.end(), w2.begin(), w2.end());
      // independent measurement of what was actually written (lossy encoders move the loudness by a few tenths of a LU)
      Probed outPr; std::wstring e2;
      res.output = LoudnessInfo();
      if (ProbeFile(out, outPr, e2) && outPr.audio) {
        LoudJson o = MeasurePass(out, outPr.audio->index, I, TP, LRA, outPr.duration, ctx, attempt ? 0.85 : 0.7, attempt ? 1.0 : 0.85, &e2);
        if (o.ok) res.output = ToInfo(o);
      }
      if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
      if (attempt == 0 && res.output.valid && std::fabs(res.output.integratedLufs - I) > 0.2 && std::fabs(res.output.integratedLufs - I) < 3.0) {
        target = std::max(-70.0, std::min(-5.0, I + (I - res.output.integratedLufs)));   // compensate the encoder and run once more
        continue;
      }
      break;
    }
    EngineResult er = EngineResult::Ok(); er.warnings = warnings; return er;
  }, video ? VerifyFn2(VerifyHasVideo) : VerifyFn2(VerifyHasAudio));
  static_cast<JobResult&>(res) = jr;
  res.linear = normType == "linear";
  if (jr.ok) {
    if (!res.linear) ctx.AddWarning(L"The recording is too dynamic for a purely linear gain change, so the loudness was adjusted dynamically.");
    if (res.output.valid && std::fabs(res.output.integratedLufs - I) > 1.0)
      ctx.AddWarning(L"The loudness could not be raised all the way to the target without exceeding the peak limit (result: " + N(res.output.integratedLufs, 1) + L" LUFS).");
  }
  return res;
}

// ================================================================================================================================
// Channels
// ================================================================================================================================
static FxSpec ChannelSpec(const Probed& pr, const ChannelsRequest& r) {
  FxSpec f = SpecFor(pr, pr.duration);
  f.outChannels = r.outChannels == 1 ? 1 : 2;
  f.leftGain = std::max(0.0, std::min(4.0, r.leftGain)); f.rightGain = std::max(0.0, std::min(4.0, r.rightGain));
  f.forceFilter = true;
  return f;
}

JobResult ConvertChannels(const fs::path& input, const ChannelsRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedAudio(input, pr, err)) return JobResult::Fail(err);
  FxSpec f = ChannelSpec(pr, req);
  return ApplyFx(input, pr, f, req.outputExt, req.outChannels == 1 ? L" (mono)" : L" (stereo)", req.preserveMetadata, req.audioBitrateKbps, req.outputPath, ctx);
}

JobResult BleepAudio(const fs::path& input, const BleepRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedAudio(input, pr, err)) return JobResult::Fail(err);
  if (req.ranges.empty()) return JobResult::Fail(L"Select at least one part of the audio to bleep.");
  FxSpec f = SpecFor(pr, pr.duration);
  f.bleeps = req.ranges; f.kind = req.kind; f.toneHz = std::max(50.0, std::min(8000.0, req.toneHz)); f.toneDb = std::max(-40.0, std::min(0.0, req.toneLevelDb)); f.fadeMs = std::max(1, std::min(100, req.fadeMs));
  if (MergeRanges(f.bleeps, pr.duration).empty()) return JobResult::Fail(L"The selected parts are outside the audio.");
  return ApplyFx(input, pr, f, L"", L" (bleeped)", req.preserveMetadata, req.audioBitrateKbps, req.outputPath, ctx);
}

// ================================================================================================================================
// Previews
// ================================================================================================================================
EngineResult RenderAudioPreviewWav(const fs::path& input, const AudioPreviewRequest& req, const fs::path& outWav, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedAudio(input, pr, err)) return EngineResult::Fail(err);
  double start = std::max(0.0, std::min(req.start, std::max(0.0, pr.duration - 0.05)));
  double len = req.end > start ? req.end - start : req.maxSeconds;
  len = std::min({len, req.maxSeconds > 0 ? req.maxSeconds : 30.0, std::max(0.05, pr.duration - start)});
  FxSpec f = SpecFor(pr, len);
  f.outChannels = req.outChannels; f.leftGain = req.leftGain; f.rightGain = req.rightGain;
  f.kind = req.bleepKind; f.toneHz = req.toneHz; f.toneDb = req.toneLevelDb; f.fadeMs = std::max(1, req.fadeMs);
  for (auto b : req.bleeps) { b.start -= start; b.end -= start; if (b.end > 0 && b.start < len) f.bleeps.push_back(b); }
  std::wstring fc = BuildFx(f, L"[0:" + std::to_wstring(pr.audio->index) + L"]");
  Args a = {L"-ss", N(start, 4), L"-i", P(input), L"-t", N(len, 4), L"-filter_complex", fc, L"-map", L"[aout]", L"-c:a", L"pcm_s16le",
            L"-ar", std::to_wstring(req.sampleRate > 0 ? req.sampleRate : 44100), L"-map_metadata", L"-1", L"-f", L"wav", P(outWav)};
  FfOut r = RunFfmpeg(a, len, ctx);
  if (!r.ok) { RemoveWithRetry(outWav); return EngineResult::Fail(r.error); }
  return EngineResult::Ok();
}

EngineResult PreviewChannelsToWav(const fs::path& input, const ChannelsRequest& req, double startSec, double durationSec, const fs::path& outWav, JobContext& ctx) {
  AudioPreviewRequest p;
  p.start = startSec; p.end = durationSec > 0 ? startSec + durationSec : 0; p.maxSeconds = durationSec > 0 ? durationSec : 30;
  p.outChannels = req.outChannels == 1 ? 1 : 2; p.leftGain = req.leftGain; p.rightGain = req.rightGain;
  return RenderAudioPreviewWav(input, p, outWav, ctx);
}

EngineResult RenderBleepPreview(const fs::path& input, const BleepRequest& req, double startSec, double durationSec, const fs::path& outWav, JobContext& ctx) {
  AudioPreviewRequest p;
  p.start = startSec; p.end = durationSec > 0 ? startSec + durationSec : 0; p.maxSeconds = durationSec > 0 ? durationSec : 30;
  p.bleeps.reserve(req.ranges.size());
  for (auto& r : req.ranges) p.bleeps.push_back({r.start, r.end});
  p.bleepKind = req.kind; p.toneHz = req.toneHz; p.toneLevelDb = req.toneLevelDb; p.fadeMs = req.fadeMs;
  return RenderAudioPreviewWav(input, p, outWav, ctx);
}

// ================================================================================================================================
// Waveform
// ================================================================================================================================
namespace {
struct RawDecode { fs::path file; int channels = 1; int rate = 8000; bool ok = false; std::wstring error; ~RawDecode() { if (!file.empty()) { std::error_code ec; fs::remove(file, ec); } } };

fs::path TempRaw() {
  UUID u; UuidCreate(&u);
  wchar_t* s = nullptr; UuidToStringW(&u, (RPC_WSTR*)&s);
  fs::path p = TempDir() / (std::wstring(L"wave-") + (s ? s : L"x") + L".raw");
  if (s) RpcStringFreeW((RPC_WSTR*)&s);
  return p;
}
}

Waveform ComputeWaveform(const fs::path& path, int buckets, const std::atomic<bool>* cancel) {
  Waveform w;
  WaveformPerChannel pc;
  // peaks = largest absolute sample across all channels (never exceeds 1.0 and anti-phase channels cannot cancel out)
  Probed pr; std::wstring err;
  if (!NeedAudio(path, pr, err)) { w.error = err; return w; }
  buckets = std::max(1, std::min(200000, buckets));
  int ch = std::max(1, std::min(8, pr.audio->channels));
  w.duration = pr.duration; w.channels = pr.audio->channels;
  RawDecode raw; raw.file = TempRaw();
  Args a = {L"-i", P(path), L"-map", L"0:" + std::to_wstring(pr.audio->index), L"-vn", L"-ac", std::to_wstring(ch), L"-ar", L"8000", L"-c:a", L"pcm_s16le", L"-f", L"s16le", P(raw.file)};
  FfOut r = RunFfmpegSimple(a, cancel, INFINITE);
  if (!r.ok) { w.error = r.error; return w; }
  std::ifstream f(raw.file, std::ios::binary | std::ios::ate);
  if (!f) { w.error = L"The waveform could not be read."; return w; }
  uint64_t frames = (uint64_t)f.tellg() / 2 / ch;
  f.seekg(0);
  if (frames == 0) { w.error = L"The audio is empty."; return w; }
  w.peaks.assign(buckets, 0); w.minima.assign(buckets, 0); w.maxima.assign(buckets, 0);
  std::vector<int16_t> buf((size_t)ch * 16384);
  uint64_t idx = 0;
  while (f) {
    f.read((char*)buf.data(), buf.size() * 2);
    size_t n = (size_t)f.gcount() / 2 / ch;
    if (!n) break;
    for (size_t i = 0; i < n; ++i, ++idx) {
      size_t b = (size_t)(idx * (uint64_t)buckets / frames);
      if (b >= (size_t)buckets) b = buckets - 1;
      for (int c = 0; c < ch; ++c) {
        float v = buf[i * ch + c] / 32768.0f;
        if (v < w.minima[b]) w.minima[b] = v;
        if (v > w.maxima[b]) w.maxima[b] = v;
      }
    }
    if (cancel && cancel->load()) { w.error = L"Cancelled"; return w; }
  }
  for (int b = 0; b < buckets; ++b) w.peaks[b] = std::max(std::fabs(w.minima[b]), std::fabs(w.maxima[b]));
  w.ok = true;
  return w;
}

WaveformPerChannel ComputeWaveformPerChannel(const fs::path& path, int buckets, const std::atomic<bool>* cancel) {
  WaveformPerChannel w;
  Probed pr; std::wstring err;
  if (!NeedAudio(path, pr, err)) { w.error = err; return w; }
  buckets = std::max(1, std::min(200000, buckets));
  int ch = std::max(1, std::min(8, pr.audio->channels));
  w.duration = pr.duration; w.channels = ch;
  RawDecode raw; raw.file = TempRaw();
  Args a = {L"-i", P(path), L"-map", L"0:" + std::to_wstring(pr.audio->index), L"-vn", L"-ac", std::to_wstring(ch), L"-ar", L"8000", L"-c:a", L"pcm_s16le", L"-f", L"s16le", P(raw.file)};
  FfOut r = RunFfmpegSimple(a, cancel, INFINITE);
  if (!r.ok) { w.error = r.error; return w; }
  std::ifstream f(raw.file, std::ios::binary | std::ios::ate);
  if (!f) { w.error = L"The waveform could not be read."; return w; }
  uint64_t frames = (uint64_t)f.tellg() / 2 / ch;
  f.seekg(0);
  if (frames == 0) { w.error = L"The audio is empty."; return w; }
  w.peaks.assign(ch, std::vector<float>(buckets, 0));
  std::vector<int16_t> buf((size_t)ch * 16384);
  uint64_t idx = 0;
  while (f) {
    f.read((char*)buf.data(), buf.size() * 2);
    size_t n = (size_t)f.gcount() / 2 / ch;
    if (!n) break;
    for (size_t i = 0; i < n; ++i, ++idx) {
      size_t b = (size_t)(idx * (uint64_t)buckets / frames);
      if (b >= (size_t)buckets) b = buckets - 1;
      for (int c = 0; c < ch; ++c) { float v = std::fabs(buf[i * ch + c] / 32768.0f); if (v > w.peaks[c][b]) w.peaks[c][b] = v; }
    }
    if (cancel && cancel->load()) { w.error = L"Cancelled"; return w; }
  }
  w.ok = true;
  return w;
}

} // namespace filo::media
