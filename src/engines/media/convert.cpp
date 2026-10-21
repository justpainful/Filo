// Video <-> video, video -> audio and audio <-> audio conversions (registered as capabilities) built on ffmpeg.
#include "media_internal.h"
#include "media_convert.h"
#include <algorithm>
#include <cmath>

namespace filo::media {
using namespace detail;

namespace {

// ---- option parsing ------------------------------------------------------------------------------------------------------
struct Opts {
  int crf = 22;
  int kbps = 192;
  bool preserve = true;
  bool hw = false;
  bool copy = false;
  VCodec codec = VCodec::H264;
  bool codecChosen = false;
  int sampleRate = 0, channels = 0, bitDepth = 0;
  bool lossless = false;
  int gifFps = 12, gifWidth = 480;
  bool gifLoop = true;
};

Opts ParseOpts(const Json& j, const std::wstring& out, std::vector<std::wstring>& warnings) {
  Opts o;
  o.crf = (int)std::lround(j.Get("crf").AsNumber(22));
  o.crf = std::max(0, std::min(51, o.crf));
  o.kbps = (int)std::lround(j.Get("audioBitrateKbps").AsNumber(192));
  o.preserve = j.Has("preserveMetadata") ? j.Get("preserveMetadata").AsBool(true) || j.Get("preserveMetadata").AsNumber(0) != 0 : true;
  o.hw = j.Get("hardwareAcceleration").AsBool(false) || (j.Get("hardwareAcceleration").IsNumber() && j.Get("hardwareAcceleration").AsNumber() != 0);
  o.copy = j.Get("copyStreams").AsBool(false) || (j.Get("copyStreams").IsNumber() && j.Get("copyStreams").AsNumber() != 0);
  o.codec = DefaultVideoCodec(out);
  if (j.Has("codec") && j.Get("codec").IsString()) {
    VCodec c;
    if (ParseVCodec(j.Get("codec").AsString(), c)) {
      if (ContainerAllowsVideoCodec(out, c)) { o.codec = c; o.codecChosen = true; }
      else warnings.push_back(std::wstring(L"The ") + Widen(VCodecName(c)) + L" codec cannot be stored in ." + out + L"; " + Widen(VCodecName(o.codec)) + L" was used instead.");
    }
  }
  if (!EncoderAvailable(o.codec, o.hw)) {
    // fall back to the container default when the chosen encoder is missing from this ffmpeg build
    VCodec d = DefaultVideoCodec(out);
    if (EncoderAvailable(d, false)) { warnings.push_back(std::wstring(Widen(VCodecName(o.codec))) + L" encoding is not available in this build; " + Widen(VCodecName(d)) + L" was used."); o.codec = d; }
  }
  auto numOrZero = [&](const char* k) { const Json& v = j.Get(k); return v.IsString() && v.AsString() == "keep" ? 0 : (int)std::lround(v.AsNumber(0)); };
  o.sampleRate = numOrZero("sampleRate");
  o.channels = numOrZero("channels");
  o.bitDepth = numOrZero("bitDepth");
  o.lossless = j.Get("lossless").AsBool(false) || (j.Get("lossless").IsNumber() && j.Get("lossless").AsNumber() != 0);
  o.gifFps = std::max(1, std::min(60, (int)std::lround(j.Get("gifFps").AsNumber(12))));
  o.gifWidth = std::max(0, (int)std::lround(j.Get("gifWidth").AsNumber(480)));
  o.gifLoop = j.Has("gifLoop") ? (j.Get("gifLoop").AsBool(true) || j.Get("gifLoop").AsNumber(0) != 0) : true;
  return o;
}

EngineResult Finish(FfOut& r, std::vector<std::wstring>& warnings) {
  if (!r.ok) { EngineResult er = EngineResult::Fail(r.error); er.warnings = warnings; return er; }
  EngineResult er = EngineResult::Ok();
  er.warnings = warnings;
  return er;
}

void HdrWarning(const Probed& pr, std::vector<std::wstring>& warnings) {
  if (!pr.video) return;
  std::string t = ColorTransfer(pr.mi, *pr.video);
  if (t == "smpte2084" || t == "arib-std-b67") warnings.push_back(L"This is an HDR video. It was converted to standard (SDR) range without tone mapping, so colours may look washed out.");
}

// ---- video outputs -------------------------------------------------------------------------------------------------------
EngineResult ToVideo(const ConvertTask& t, const Probed& pr, JobContext& ctx) {
  std::vector<std::wstring> warnings;
  const std::wstring out = t.outExt;
  Opts o = ParseOpts(t.options, out, warnings);
  if (!pr.video) return EngineResult::Fail(L"This file has no video track to convert.");
  if (!EncoderAvailable(o.codec, o.hw)) return EngineResult::Fail(L"This build of the media engine cannot encode " + Widen(VCodecName(o.codec)) + L" video.");

  Args a = {L"-i", P(t.input)};
  Args maps = {L"-map", L"0:" + std::to_wstring(pr.video->index)};
  // audio: keep all tracks (every supported container stores several)
  std::vector<const ff::StreamInfo*> audios;
  for (auto& s : pr.mi.streams) if (s.type == "audio") audios.push_back(&s);
  for (auto* s : audios) { maps.push_back(L"-map"); maps.push_back(L"0:" + std::to_wstring(s->index)); }
  Args subCodec;
  SubtitleArgs(out, pr.mi, maps, subCodec, &warnings);
  HdrWarning(pr, warnings);

  bool copyVideo = o.copy && VideoCopyOk(out, pr.video->codec);
  if (o.copy && !copyVideo) warnings.push_back(L"The video codec (" + Widen(pr.video->codec) + L") cannot be copied into ." + out + L", so the video was re-encoded.");
  bool copyAudio = false;
  if (o.copy && !audios.empty()) {
    copyAudio = std::all_of(audios.begin(), audios.end(), [&](const ff::StreamInfo* s) { return AudioCopyOk(out, s->codec); });
    if (!copyAudio) warnings.push_back(L"The audio could not be copied into ." + out + L", so it was re-encoded.");
  }

  a.insert(a.end(), maps.begin(), maps.end());
  bool forceCfr = false;
  if (copyVideo) {
    a.insert(a.end(), {L"-c:v", L"copy"});
    if (pr.video->codec == "hevc" && (out == L"mp4" || out == L"mov")) a.insert(a.end(), {L"-tag:v", L"hvc1"});
  } else {
    std::wstring vf = BuildVideoFilter(pr, out, warnings, forceCfr);
    if (!vf.empty()) a.insert(a.end(), {L"-vf", vf});
    VideoEnc ve; ve.codec = o.codec; ve.crf = o.crf; ve.hw = o.hw;
    Args va = VideoEncoderArgs(out, ve, &warnings);
    a.insert(a.end(), va.begin(), va.end());
    if (forceCfr) a.insert(a.end(), {L"-fps_mode", L"cfr"});
  }
  if (!audios.empty()) {
    if (copyAudio) a.insert(a.end(), {L"-c:a", L"copy"});
    else {
      AudioEnc ae; ae.kbps = o.kbps; ae.sampleRate = o.sampleRate; ae.channels = o.channels;
      Args aa = AudioEncoderArgs(out, ae, &pr.mi, audios[0], &warnings);
      a.insert(a.end(), aa.begin(), aa.end());
    }
  }
  a.insert(a.end(), subCodec.begin(), subCodec.end());
  bool chapters = out == L"mkv" || out == L"mp4" || out == L"mov" || out == L"webm";
  Args md = MetadataArgs(o.preserve, chapters);
  a.insert(a.end(), md.begin(), md.end());
  Args fl = VideoContainerFlags(out);
  a.insert(a.end(), fl.begin(), fl.end());
  a.insert(a.end(), {L"-max_muxing_queue_size", L"2048", L"-f", MuxerFor(out), P(t.output)});

  if (copyVideo && (audios.empty() || copyAudio)) warnings.push_back(L"Streams were copied without re-encoding; picture and sound quality are identical to the original.");
  FfOut r = RunFfmpeg(a, pr.duration, ctx);
  return Finish(r, warnings);
}

// ---- GIF ---------------------------------------------------------------------------------------------------------------------
EngineResult ToGif(const ConvertTask& t, const Probed& pr, JobContext& ctx) {
  std::vector<std::wstring> warnings;
  Opts o = ParseOpts(t.options, L"gif", warnings);
  if (!pr.video) return EngineResult::Fail(L"This file has no video track to convert.");
  int w = 0, h = 0; pr.mi.DisplaySize(w, h);
  double srcFps = pr.video->fps > 0 ? pr.video->fps : 25;
  double fps = std::min<double>(o.gifFps, std::max(1.0, srcFps));
  std::wstring chain = L"fps=" + N(fps, 3);
  if (o.gifWidth > 0 && w > o.gifWidth) chain += L",scale=" + std::to_wstring(o.gifWidth) + L":-1:flags=lanczos";
  if (pr.duration > 30) warnings.push_back(L"Long clips make very large GIFs; consider trimming the video first.");
  if (pr.audio) warnings.push_back(L"GIF images have no sound; the audio was left out.");
  if (pr.mi.streams.size() && std::any_of(pr.mi.streams.begin(), pr.mi.streams.end(), [](auto& s) { return s.type == "subtitle"; })) warnings.push_back(L"Subtitles were left out of the GIF.");
  const std::wstring src = L"[0:" + std::to_wstring(pr.video->index) + L"]";
  fs::path pal = ctx.TempDir() / L"palette.png";

  Args p1 = {L"-i", P(t.input), L"-filter_complex", src + chain + L",split[a][b];[a]palettegen=stats_mode=diff[p]",
             L"-map", L"[p]", L"-frames:v", L"1", L"-update", L"1", P(pal), L"-map", L"[b]", L"-f", L"null", L"-"};
  FfOut r = RunFfmpeg(p1, pr.duration, ctx, 0.0, 0.4);
  if (!r.ok) return Finish(r, warnings);
  if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
  Args p2 = {L"-i", P(t.input), L"-i", P(pal), L"-filter_complex", src + chain + L"[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle[v]",
             L"-map", L"[v]", L"-loop", o.gifLoop ? L"0" : L"-1", L"-f", L"gif", P(t.output)};
  r = RunFfmpeg(p2, pr.duration, ctx, 0.4, 1.0);
  return Finish(r, warnings);
}

// ---- audio outputs ---------------------------------------------------------------------------------------------------------
EngineResult ToAudio(const ConvertTask& t, const Probed& pr, JobContext& ctx) {
  std::vector<std::wstring> warnings;
  const std::wstring out = t.outExt;
  Opts o = ParseOpts(t.options, out, warnings);
  if (!pr.audio) return EngineResult::Fail(L"This file has no audio track.");
  if (pr.mi.AudioCount() > 1) warnings.push_back(L"The file has " + std::to_wstring(pr.mi.AudioCount()) + L" audio tracks; only the first one was converted.");
  bool copyAudio = o.copy && AudioCopyOk(out, pr.audio->codec);
  if (o.copy && !copyAudio) warnings.push_back(L"The audio could not be copied as-is into ." + out + L", so it was re-encoded.");
  AudioEnc ae; ae.kbps = o.kbps; ae.sampleRate = o.sampleRate; ae.channels = o.channels; ae.lossless = o.lossless; ae.bitDepth = o.bitDepth;
  Args a = {L"-i", P(t.input)};
  Args rest = AudioFileOutputArgs(pr, out, ae, o.preserve, copyAudio, L"", L"", L"", warnings);
  a.insert(a.end(), rest.begin(), rest.end());
  a.push_back(P(t.output));
  FfOut r = RunFfmpeg(a, pr.duration, ctx);
  return Finish(r, warnings);
}

} // namespace

// ============================================================================================================================
EngineResult ConvertMedia(const ConvertTask& t, JobContext& ctx) {
  if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
  Probed pr; std::wstring err;
  if (!ProbeFile(t.input, pr, err)) return EngineResult::Fail(err);
  const std::wstring& out = t.outExt;
  if (IsAudioFormat(out)) return ToAudio(t, pr, ctx);
  if (out == L"gif") return ToGif(t, pr, ctx);
  if (IsVideoContainer(out)) return ToVideo(t, pr, ctx);
  return EngineResult::Fail(L"This conversion is not supported.");
}

bool VerifyMediaOutput(const fs::path& produced, const std::wstring& ext, std::wstring* why) {
  auto mi = ff::Probe(produced);
  auto fail = [&](const wchar_t* m) { if (why) *why = m; return false; };
  if (!mi) return fail(L"The converted file could not be opened for verification.");
  if (mi->duration <= 0 && ext != L"gif") return fail(L"The converted file has no playable duration.");
  if (IsAudioFormat(ext)) {
    const ff::StreamInfo* a = mi->FirstAudio();
    if (!a) return fail(L"The converted file contains no audio.");
    std::string want = AudioEncoderName(ext);
    (void)want;
    return true;
  }
  const ff::StreamInfo* v = mi->FirstVideo();
  if (!v) return fail(L"The converted file contains no video.");
  if (ext == L"gif") return v->codec == "gif" ? true : fail(L"The converted file is not a GIF.");
  if (v->width <= 0 || v->height <= 0) return fail(L"The converted video has an invalid size.");
  return true;
}

} // namespace filo::media
