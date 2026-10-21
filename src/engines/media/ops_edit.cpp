// Trim, crop, speed, remove-audio, redact and split operations.
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace filo::media {
using namespace detail;

namespace {

template <class R> R Failed(const std::wstring& why) { R r; static_cast<JobResult&>(r) = JobResult::Fail(why); return r; }

bool NeedVideo(const fs::path& input, Probed& pr, std::wstring& err) {
  if (!ProbeFile(input, pr, err)) return false;
  if (!pr.video) { err = L"This file has no video track."; return false; }
  return true;
}

double ProbeDuration(const fs::path& p) { auto mi = ff::Probe(fs::path(P(p))); return mi ? mi->duration : 0; }

// Cuts [start,end) of `input` into `out`. Re-encode: frame-accurate. Copy: starts at the keyframe at/before `start`.
EngineResult CutSegment(const Probed& pr, const fs::path& input, const std::wstring& container, double start, double end, bool copy, bool preserve, bool hw, int crf,
                        const fs::path& out, JobContext& ctx, double p0, double p1, double* actualStart, double* outDuration) {
  std::vector<std::wstring> warnings;
  double seek = start;
  if (copy) {
    seek = KeyframeAtOrBefore(input, pr.video->index, start);
    if (seek > start) seek = start;
  }
  double len = end - seek;
  if (len <= 0.01) return EngineResult::Fail(L"The selected range is too short.");
  if (actualStart) *actualStart = seek;
  Args a = {L"-ss", N(seek, 6), L"-i", P(input), L"-t", N(len, 6)};
  if (copy) {
    a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(pr.video->index)});
    for (auto& s : pr.mi.streams) if (s.type == "audio") a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(s.index)});
    a.insert(a.end(), {L"-map", L"0:s?", L"-c", L"copy", L"-avoid_negative_ts", L"make_zero"});
    Args md = MetadataArgs(preserve, true);
    a.insert(a.end(), md.begin(), md.end());
    a.insert(a.end(), {L"-f", MuxerFor(container)});
  } else {
    StdEncode e; e.container = container; e.crf = crf; e.hw = hw; e.preserve = preserve; e.allowAudioCopy = false;
    e.codec = CodecLike(container, pr.video->codec);
    Args rest = BuildStdOutputArgs(pr, e, L"", L"", warnings);
    a.insert(a.end(), rest.begin(), rest.end());
  }
  a.push_back(P(out));
  FfOut r = RunFfmpeg(a, len, ctx, p0, p1);
  EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
  er.warnings = warnings;
  if (r.ok && outDuration) *outDuration = ProbeDuration(out);
  return er;
}

} // namespace

// ================================================================================================================================
// Trim
// ================================================================================================================================
TrimResult TrimVideo(const fs::path& input, const TrimVideoRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedVideo(input, pr, err)) return Failed<TrimResult>(err);
  TrimResult res;
  double dur = pr.duration;
  double start = std::max(0.0, std::min(req.start, dur));
  double end = (req.end <= start || req.end > dur) ? dur : req.end;
  if (end - start < 0.04) return Failed<TrimResult>(L"The selected range is too short.");
  std::wstring container = OutputContainerFor(input);
  if (req.copyMode && !VideoCopyOk(container, pr.video->codec))
    return Failed<TrimResult>(L"This video's codec cannot be cut without re-encoding. Turn off the fast copy mode.");
  res.requestedStart = start; res.requestedEnd = end;
  double actualStart = start, outDur = 0;
  JobResult jr = WithTransaction(input, container, L" (trimmed)", req.outputPath, ctx, [&](const fs::path& out) {
    return CutSegment(pr, input, container, start, end, req.copyMode, req.preserveMetadata, req.hardware, EffectiveCrf(req.crf), out, ctx, 0, 1, &actualStart, &outDur);
  }, VerifyHasVideo);
  static_cast<JobResult&>(res) = jr;
  if (jr.ok) {
    res.actualStart = actualStart;
    res.outputDuration = outDur;
    res.actualEnd = actualStart + (outDur > 0 ? outDur : end - actualStart);
    res.snappedToKeyframes = req.copyMode && std::fabs(actualStart - start) > 0.002;
    if (req.copyMode && res.snappedToKeyframes)
      ctx.AddWarning(L"Fast copy mode can only start at a keyframe: the clip begins at " + FormatTime(actualStart) + L" instead of " + FormatTime(start) + L".");
  }
  return res;
}

SilenceInfo DetectSilentEnds(const fs::path& input, double thresholdDb, double minSilence, const std::atomic<bool>* cancel) {
  SilenceInfo si;
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) { si.error = err; return si; }
  if (!pr.audio) { si.error = L"This file has no audio track."; return si; }
  si.duration = pr.duration;
  Args a = {L"-i", P(input), L"-map", L"0:" + std::to_wstring(pr.audio->index), L"-af",
            L"silencedetect=noise=" + N(thresholdDb, 1) + L"dB:d=" + N(std::max(0.02, minSilence), 3), L"-f", L"null", L"-"};
  FfOut r = RunFfmpegSimple(a, cancel, INFINITE, 8u << 20, true);
  if (!r.ok) { si.error = r.error; return si; }
  struct Seg { double s = 0, e = -1; };
  std::vector<Seg> segs;
  std::istringstream is(r.stderrText); std::string line;
  while (std::getline(is, line)) {
    size_t p = line.find("silence_start:");
    if (p != std::string::npos) { Seg s; s.s = atof(line.c_str() + p + 14); segs.push_back(s); continue; }
    p = line.find("silence_end:");
    if (p != std::string::npos && !segs.empty()) segs.back().e = atof(line.c_str() + p + 12);
  }
  double d = si.duration, lead = 0, trail = d;
  const double eps = 0.06;
  for (auto& s : segs) {
    double e = s.e < 0 ? d : s.e;
    if (s.s <= eps) lead = std::max(lead, e);
    if (e >= d - 0.12) trail = std::min(trail, std::max(0.0, s.s));
  }
  if (lead >= d - 0.05 || trail <= lead + 0.001) { si.allSilent = true; si.ok = true; si.start = 0; si.end = 0; si.leadingSilence = d; return si; }
  si.start = lead; si.end = trail;
  si.leadingSilence = lead; si.trailingSilence = d - trail;
  si.ok = true;
  return si;
}

TrimResult TrimAudio(const fs::path& input, const TrimAudioRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) return Failed<TrimResult>(err);
  if (!pr.audio) return Failed<TrimResult>(L"This file has no audio track.");
  std::vector<std::wstring> warnings;
  std::wstring ext = AudioFormatFor(input);
  if (ext.empty()) { ext = L"wav"; warnings.push_back(L"This audio format cannot be written directly; the trimmed clip was saved as WAV."); }
  double dur = pr.duration;
  double start = std::max(0.0, std::min(req.start, dur));
  double end = (req.end <= start || req.end > dur) ? dur : req.end;
  if (req.trimSilentEnds) {
    SilenceInfo si = DetectSilentEnds(input, req.silenceThresholdDb, req.minSilenceSec, ctx.CancelFlag());
    if (!si.ok) return Failed<TrimResult>(si.error);
    if (si.allSilent) return Failed<TrimResult>(L"The whole file is silent, so there is nothing to keep.");
    start = si.start; end = si.end;
  }
  if (end - start < 0.02) return Failed<TrimResult>(L"The selected range is too short.");
  double len = end - start;
  bool fades = req.fadeInSec > 0 || req.fadeOutSec > 0;
  std::wstring af;
  if (req.fadeInSec > 0) af = L"afade=t=in:st=0:d=" + N(std::min(req.fadeInSec, len), 3);
  if (req.fadeOutSec > 0) { double f = std::min(req.fadeOutSec, len); af = JoinFilters(af, L"afade=t=out:st=" + N(len - f, 3) + L":d=" + N(f, 3)); }
  // FLAC is always re-encoded: stream copy would keep the source STREAMINFO (total length) and report the wrong duration; the re-encode is lossless anyway
  bool copy = req.copyMode && !fades && ext != L"flac" && AudioCopyOk(ext, pr.audio->codec);
  if (req.copyMode && !copy && ext != L"flac") warnings.push_back(L"Copy mode was not possible here (fades or a format that must be re-encoded), so the audio was re-encoded.");
  TrimResult res;
  res.requestedStart = start; res.requestedEnd = end;
  double outDur = 0;
  JobResult jr = WithTransaction(input, ext, L" (trimmed)", req.outputPath, ctx, [&](const fs::path& out) {
    AudioEnc ae; ae.kbps = LikeSourceKbps(pr, req.audioBitrateKbps);
    Args a = {L"-ss", N(start, 6), L"-i", P(input), L"-t", N(len, 6)};
    Args rest = AudioFileOutputArgs(pr, ext, ae, req.preserveMetadata, copy, af, L"", L"", warnings);
    a.insert(a.end(), rest.begin(), rest.end());
    a.push_back(P(out));
    FfOut r = RunFfmpeg(a, len, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    if (r.ok) outDur = ProbeDuration(out);
    return er;
  }, VerifyHasAudio);
  static_cast<JobResult&>(res) = jr;
  if (jr.ok) { res.actualStart = start; res.outputDuration = outDur; res.actualEnd = start + (outDur > 0 ? outDur : len); }
  return res;
}

// ================================================================================================================================
// Crop
// ================================================================================================================================
CropRect AspectCropRect(int sw, int sh, AspectPreset p, double cx, double cy, double zoom) {
  double rw = 0, rh = 0;
  switch (p) {
    case AspectPreset::Square1x1: rw = 1; rh = 1; break; case AspectPreset::R4x3: rw = 4; rh = 3; break; case AspectPreset::R3x4: rw = 3; rh = 4; break;
    case AspectPreset::R16x9: rw = 16; rh = 9; break; case AspectPreset::R9x16: rw = 9; rh = 16; break; case AspectPreset::R3x2: rw = 3; rh = 2; break;
    case AspectPreset::R2x3: rw = 2; rh = 3; break; case AspectPreset::R21x9: rw = 21; rh = 9; break; case AspectPreset::R4x5: rw = 4; rh = 5; break;
    default: break;
  }
  CropRect r;
  double w = sw, h = sh;
  if (rw > 0) {
    double ratio = rw / rh;
    if ((double)sw / sh > ratio) { h = sh; w = h * ratio; } else { w = sw; h = w / ratio; }
  }
  zoom = std::max(1.0, zoom);
  w /= zoom; h /= zoom;
  r.w = std::max(2, (int)std::floor(w / 2) * 2); r.h = std::max(2, (int)std::floor(h / 2) * 2);
  r.w = std::min(r.w, sw & ~1); r.h = std::min(r.h, sh & ~1);
  double x = cx * sw - r.w / 2.0, y = cy * sh - r.h / 2.0;
  r.x = std::max(0, std::min((int)std::lround(x), sw - r.w)) & ~1;
  r.y = std::max(0, std::min((int)std::lround(y), sh - r.h)) & ~1;
  return r;
}

CropResult CropVideo(const fs::path& input, const CropRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedVideo(input, pr, err)) return Failed<CropResult>(err);
  int W = 0, H = 0; pr.mi.DisplaySize(W, H);
  CropRect c = req.rect;
  c.x &= ~1; c.y &= ~1; c.w &= ~1; c.h &= ~1;
  if (c.x < 0 || c.y < 0 || c.w < 2 || c.h < 2 || c.x + c.w > W || c.y + c.h > H)
    return Failed<CropResult>(L"The crop area is outside the picture (" + std::to_wstring(W) + L"x" + std::to_wstring(H) + L").");
  std::wstring container = OutputContainerFor(input);
  CropResult res;
  res.applied = c; res.outWidth = c.w; res.outHeight = c.h;
  JobResult jr = WithTransaction(input, container, L" (cropped)", req.outputPath, ctx, [&](const fs::path& out) {
    std::vector<std::wstring> warnings;
    StdEncode e; e.container = container; e.crf = EffectiveCrf(req.crf); e.hw = req.hardware; e.preserve = req.preserveMetadata; e.codec = CodecLike(container, pr.video->codec);
    std::wstring vf = L"crop=" + std::to_wstring(c.w) + L":" + std::to_wstring(c.h) + L":" + std::to_wstring(c.x) + L":" + std::to_wstring(c.y);
    Args a = {L"-i", P(input)};
    Args rest = BuildStdOutputArgs(pr, e, vf, L"", warnings);
    a.insert(a.end(), rest.begin(), rest.end());
    a.push_back(P(out));
    FfOut r = RunFfmpeg(a, pr.duration, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    return er;
  }, VerifyHasVideo);
  static_cast<JobResult&>(res) = jr;
  return res;
}

// ================================================================================================================================
// Speed
// ================================================================================================================================
double SpeedDurationPreview(double d, double f) { return f > 0 ? d / f : d; }

SpeedResult ChangeSpeed(const fs::path& input, const SpeedRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) return Failed<SpeedResult>(err);
  double f = req.factor;
  if (!(f >= 0.05 && f <= 100)) return Failed<SpeedResult>(L"The speed must be between 0.05x and 100x.");
  bool hasVideo = pr.video != nullptr;
  if (!hasVideo && !pr.audio) return Failed<SpeedResult>(L"This file has no audio or video to change.");
  SpeedResult res;
  res.expectedDuration = SpeedDurationPreview(pr.duration, f);

  // audio filter
  std::wstring af;
  if (pr.audio) {
    if (req.changePitch) {
      int sr = pr.audio->sampleRate > 0 ? pr.audio->sampleRate : 44100;
      af = L"asetrate=" + N(sr * f, 3) + L",aresample=" + std::to_wstring(sr);
    } else {
      double r = f;
      std::vector<double> parts;
      while (r > 2.0 + 1e-9) { parts.push_back(2.0); r /= 2.0; }
      while (r < 0.5 - 1e-9) { parts.push_back(0.5); r /= 0.5; }
      parts.push_back(r);
      for (size_t i = 0; i < parts.size(); ++i) { if (i) af += L","; af += L"atempo=" + N(parts[i], 6); }
    }
  }
  std::wstring container = hasVideo ? OutputContainerFor(input) : AudioFormatFor(input);
  std::vector<std::wstring> warnings;
  if (container.empty()) { container = L"wav"; warnings.push_back(L"This audio format cannot be written directly; the result was saved as WAV."); }
  double outDur = 0;
  std::wstring suffix = L" (" + N(f, 2) + L"x)";
  JobResult jr = WithTransaction(input, container, suffix, req.outputPath, ctx, [&](const fs::path& out) {
    Args a = {L"-i", P(input)};
    if (hasVideo) {
      double fps = pr.video->fps > 0 ? pr.video->fps : 25;
      std::wstring vf = L"setpts=PTS/" + N(f, 6);
      if (req.interpolate && f < 1.0) vf += L",minterpolate=fps=" + N(fps, 3) + L":mi_mode=blend";
      else vf += L",fps=" + N(fps, 3);
      StdEncode e; e.container = container; e.crf = EffectiveCrf(req.crf); e.hw = req.hardware; e.preserve = req.preserveMetadata;
      e.codec = CodecLike(container, pr.video->codec); e.allowAudioCopy = false;
      bool hadSubs = std::any_of(pr.mi.streams.begin(), pr.mi.streams.end(), [](auto& s) { return s.type == "subtitle"; });
      if (hadSubs) { e.dropSubs = true; warnings.push_back(L"Subtitles were left out because their timing would no longer match the changed speed."); }
      Args rest = BuildStdOutputArgs(pr, e, vf, af, warnings);
      a.insert(a.end(), rest.begin(), rest.end());
    } else {
      AudioEnc ae; ae.kbps = LikeSourceKbps(pr, 0);
      Args rest = AudioFileOutputArgs(pr, container, ae, req.preserveMetadata, false, af, L"", L"", warnings);
      a.insert(a.end(), rest.begin(), rest.end());
    }
    a.push_back(P(out));
    FfOut r = RunFfmpeg(a, res.expectedDuration, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    if (r.ok) outDur = ProbeDuration(out);
    return er;
  }, hasVideo ? VerifyFn2(VerifyHasVideo) : VerifyFn2(VerifyHasAudio));
  static_cast<JobResult&>(res) = jr;
  res.outputDuration = outDur;
  return res;
}

// ================================================================================================================================
// Remove audio
// ================================================================================================================================
JobResult RemoveAudio(const fs::path& input, const RemoveAudioRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) return JobResult::Fail(err);
  if (!pr.video) return JobResult::Fail(L"This file has no video track.");
  if (!pr.audio) return JobResult::Fail(L"This video has no audio track to remove.");
  std::wstring container = OutputContainerFor(input);
  return WithTransaction(input, container, L" (no audio)", req.outputPath, ctx, [&](const fs::path& out) {
    Args a = {L"-i", P(input), L"-map", L"0:v", L"-map", L"0:s?", L"-c", L"copy", L"-map_metadata", L"0", L"-map_chapters", L"0", L"-f", MuxerFor(container), P(out)};
    FfOut r = RunFfmpeg(a, pr.duration, ctx);
    return r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
  }, [](const fs::path& p, std::wstring* why) {
    if (!VerifyHasVideo(p, why)) return false;
    auto mi = ff::Probe(p);
    if (mi && mi->FirstAudio()) { if (why) *why = L"The audio track was not removed."; return false; }
    return true;
  });
}

// ================================================================================================================================
// Redact
// ================================================================================================================================
JobResult RedactVideo(const fs::path& input, const RedactRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedVideo(input, pr, err)) return JobResult::Fail(err);
  if (req.regions.empty()) return JobResult::Fail(L"Add at least one area to hide.");
  int W = 0, H = 0; pr.mi.DisplaySize(W, H);
  std::wstring graph;
  int idx = 0;
  for (auto reg : req.regions) {
    int x0 = std::max(0, reg.x) & ~1, y0 = std::max(0, reg.y) & ~1;
    int x1 = std::min(W, (reg.x + reg.w + 1) & ~1), y1 = std::min(H, (reg.y + reg.h + 1) & ~1);
    int w = x1 - x0, h = y1 - y0;
    if (w < 8 || h < 8) return JobResult::Fail(L"A hidden area is too small or lies outside the picture.");
    double s = std::max(0.0, reg.start);
    std::wstring en = reg.end > reg.start ? L"between(t," + N(s, 3) + L"," + N(reg.end, 3) + L")" : L"gte(t," + N(s, 3) + L")";
    std::wstring in = idx == 0 ? L"" : L"[v" + std::to_wstring(idx) + L"]";
    std::wstring outLabel = idx + 1 == (int)req.regions.size() ? L"" : L"[v" + std::to_wstring(idx + 1) + L"]";
    if (idx) graph += L";";
    if (reg.style == RedactStyle::Solid) {
      graph += in + L"drawbox=x=" + std::to_wstring(x0) + L":y=" + std::to_wstring(y0) + L":w=" + std::to_wstring(w) + L":h=" + std::to_wstring(h) +
               L":color=" + Hex6(reg.color) + L":t=fill:enable='" + en + L"'" + outLabel;
    } else {
      int cap = std::max(1, std::min(w, h) / 2 - 1);
      int R = std::max(1, std::min(reg.blurStrength, cap));
      int Rc = std::max(1, std::min(R, std::min(w, h) / 4 - 1));
      std::wstring a = L"a" + std::to_wstring(idx), b = L"b" + std::to_wstring(idx), c = L"c" + std::to_wstring(idx);
      graph += in + L"split[" + a + L"][" + b + L"];[" + b + L"]crop=" + std::to_wstring(w) + L":" + std::to_wstring(h) + L":" + std::to_wstring(x0) + L":" + std::to_wstring(y0) +
               L",boxblur=luma_radius=" + std::to_wstring(R) + L":luma_power=3:chroma_radius=" + std::to_wstring(Rc) + L":chroma_power=3[" + c + L"];[" + a + L"][" + c +
               L"]overlay=" + std::to_wstring(x0) + L":" + std::to_wstring(y0) + L":enable='" + en + L"'" + outLabel;
    }
    ++idx;
  }
  std::wstring container = OutputContainerFor(input);
  return WithTransaction(input, container, L" (redacted)", req.outputPath, ctx, [&](const fs::path& out) {
    std::vector<std::wstring> warnings;
    StdEncode e; e.container = container; e.crf = EffectiveCrf(req.crf); e.hw = req.hardware; e.preserve = !req.stripMetadata; e.codec = CodecLike(container, pr.video->codec);
    Args a = {L"-i", P(input)};
    Args rest = BuildStdOutputArgs(pr, e, graph, L"", warnings);
    a.insert(a.end(), rest.begin(), rest.end());
    a.push_back(P(out));
    FfOut r = RunFfmpeg(a, pr.duration, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    return er;
  }, VerifyHasVideo);
}

// ================================================================================================================================
// Split
// ================================================================================================================================
SplitResult SplitVideo(const fs::path& input, const SplitRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!NeedVideo(input, pr, err)) return Failed<SplitResult>(err);
  double dur = pr.duration;
  std::vector<TimeRange> segs;
  if (req.mode == SplitMode::EqualParts) {
    int n = std::max(2, req.parts);
    if (dur / n < 0.1) return Failed<SplitResult>(L"The video is too short to split into that many parts.");
    for (int i = 0; i < n; ++i) segs.push_back({dur * i / n, dur * (i + 1) / n});
  } else if (req.mode == SplitMode::Markers) {
    std::vector<double> m;
    for (double v : req.markers) if (v > 0.04 && v < dur - 0.04) m.push_back(v);
    std::sort(m.begin(), m.end());
    m.erase(std::unique(m.begin(), m.end(), [](double a, double b) { return std::fabs(a - b) < 0.04; }), m.end());
    if (m.empty()) return Failed<SplitResult>(L"Add at least one split marker inside the video.");
    double prev = 0;
    for (double v : m) { segs.push_back({prev, v}); prev = v; }
    segs.push_back({prev, dur});
  } else {
    for (auto r : req.segments) {
      r.start = std::max(0.0, r.start); r.end = std::min(dur, r.end <= r.start ? dur : r.end);
      if (r.end - r.start >= 0.04) segs.push_back(r);
    }
    if (segs.empty()) return Failed<SplitResult>(L"Add at least one segment inside the video.");
  }
  std::wstring container = OutputContainerFor(input);
  if (req.copyMode && !VideoCopyOk(container, pr.video->codec))
    return Failed<SplitResult>(L"This video's codec cannot be split without re-encoding. Turn off the fast copy mode.");
  double total = 0; for (auto& s : segs) total += s.end - s.start;
  SplitResult res;
  std::vector<SplitSegment> done;
  std::vector<std::wstring> names;
  JobResult jr = WithDirTransaction(input, L" (split)", req.outputDir, ctx, [&](const fs::path& dir) {
    double acc = 0;
    std::wstring stem = input.stem().wstring();
    int digits = segs.size() >= 100 ? 3 : 2;
    for (size_t i = 0; i < segs.size(); ++i) {
      if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
      wchar_t num[16]; swprintf_s(num, L"%0*d", digits, (int)i + 1);
      std::wstring name = stem + L" - " + num + L"." + container;
      double len = segs[i].end - segs[i].start;
      double actual = segs[i].start, od = 0;
      EngineResult er = CutSegment(pr, input, container, segs[i].start, segs[i].end, req.copyMode, req.preserveMetadata, req.hardware, EffectiveCrf(req.crf), dir / name,
                                   ctx, acc / total, (acc + len) / total, &actual, &od);
      if (!er.ok) return er;
      for (auto& w : er.warnings) ctx.AddWarning(w);
      acc += len;
      SplitSegment ss; ss.start = segs[i].start; ss.end = segs[i].end; ss.actualStart = actual;
      done.push_back(ss); names.push_back(name);
    }
    return EngineResult::Ok();
  });
  static_cast<JobResult&>(res) = jr;
  if (jr.ok) {
    res.folder = jr.outputs[0];
    for (size_t i = 0; i < done.size(); ++i) { done[i].path = res.folder / names[i]; }
    res.segments = done;
    if (req.copyMode) ctx.AddWarning(L"Copy mode cuts at keyframes, so each part may start slightly earlier than the requested time.");
  }
  return res;
}

} // namespace filo::media
