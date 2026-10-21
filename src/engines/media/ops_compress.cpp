// Compress: preset (CRF), target size and percent reduction for video and audio.
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>

namespace filo::media {
using namespace detail;

namespace {

int PresetCrf(CompressPreset p) {
  switch (p) { case CompressPreset::High: return 23; case CompressPreset::Balanced: return 27; case CompressPreset::Small: return 31; default: return 35; }
}
int PresetAudioKbps(CompressPreset p) {
  switch (p) { case CompressPreset::High: return 160; case CompressPreset::Balanced: return 128; case CompressPreset::Small: return 96; default: return 64; }
}
int PresetMaxHeight(const CompressRequest& r) {
  if (r.maxHeight > 0) return r.maxHeight;
  return r.mode == CompressMode::Preset && r.preset == CompressPreset::Smallest ? 720 : 0;
}
uint64_t FileBytes(const fs::path& p) { std::error_code ec; auto s = fs::file_size(p, ec); return ec ? 0 : (uint64_t)s; }

uint64_t TargetBytesFor(const CompressRequest& r, uint64_t inBytes) {
  if (r.mode == CompressMode::TargetSize) return r.targetBytes;
  if (r.mode == CompressMode::Percent) return (uint64_t)((double)inBytes * std::max(0.0, std::min(99.0, 100.0 - r.percentSmaller)) / 100.0);
  return 0;
}

std::wstring ScaleFilter(int maxH, int dispH) {
  if (maxH <= 0 || dispH <= maxH) return L"";
  return L"scale=-2:" + std::to_wstring(maxH);
}

CompressResult Fail(const std::wstring& why) { CompressResult r; static_cast<JobResult&>(r) = JobResult::Fail(why); return r; }

} // namespace

SizeEstimate EstimateCompressedSize(const fs::path& input, const CompressRequest& req) {
  SizeEstimate e;
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) { e.feasible = false; e.note = err; return e; }
  std::error_code ec;
  uint64_t inBytes = fs::file_size(fs::path(P(input)), ec);
  double dur = std::max(0.1, pr.duration);
  if (req.mode != CompressMode::Preset) {
    uint64_t t = TargetBytesFor(req, inBytes);
    e.bytes = e.lowBytes = e.highBytes = t; e.exact = true;
    int audioK = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : 128;
    double videoKbps = (double)t * 8 / 1000 / dur * 0.96 - (pr.audio ? audioK : 0);
    if (pr.video && videoKbps < 40) { e.feasible = false; e.note = L"The target is too small for a video of this length."; }
    if (!pr.video && (double)t * 8 / 1000 / dur < 32) { e.feasible = false; e.note = L"The target is too small for audio of this length."; }
    if (t >= inBytes) { e.feasible = false; e.note = L"The target is not smaller than the original."; }
    return e;
  }
  int audioK = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : PresetAudioKbps(req.preset);
  double bytes = 0;
  if (pr.video) {
    int W = 0, H = 0; pr.mi.DisplaySize(W, H);
    int maxH = PresetMaxHeight(req);
    if (maxH > 0 && H > maxH) { W = (int)std::lround((double)W * maxH / H); H = maxH; }
    double fps = pr.video->fps > 0 ? pr.video->fps : 25;
    int crf = PresetCrf(req.preset);
    double bpp = 0.075 * std::pow(2.0, (23.0 - crf) / 6.0);     // H.264 "medium" on typical footage
    double vbits = (double)W * H * fps * bpp * dur;
    bytes = vbits / 8 + (pr.audio ? audioK * 1000.0 / 8 * dur : 0);
  } else {
    bytes = audioK * 1000.0 / 8 * dur;
  }
  bytes = std::min<double>(bytes, (double)inBytes);
  e.bytes = (uint64_t)bytes; e.lowBytes = (uint64_t)(bytes * 0.5); e.highBytes = (uint64_t)std::min<double>(bytes * 2.0, (double)inBytes);
  e.exact = false;
  e.note = L"Estimate only; the real size depends on how complex the picture is.";
  return e;
}

CompressResult CompressMedia(const fs::path& input, const CompressRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) return Fail(err);
  std::error_code ec;
  uint64_t inBytes = fs::file_size(fs::path(P(input)), ec);
  CompressResult res;
  res.inputBytes = inBytes;
  uint64_t target = TargetBytesFor(req, inBytes);
  if (req.mode != CompressMode::Preset) {
    if (target == 0) return Fail(L"Choose a target size.");
    if (target >= inBytes) { res.notSmaller = true; static_cast<JobResult&>(res) = JobResult::Fail(L"The target size is not smaller than the original file."); return res; }
  }
  double dur = std::max(0.1, pr.duration);

  // ------------------------------------------------------------------------------------------------ audio files
  if (!pr.video) {
    if (!pr.audio) return Fail(L"This file has no audio or video to compress.");
    std::wstring ext = AudioFormatFor(input);
    std::vector<std::wstring> warnings;
    if (ext.empty()) return Fail(L"This audio format cannot be compressed here. Convert it to MP3, M4A or Opus first.");
    int kbps = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : PresetAudioKbps(req.preset);
    if (req.mode != CompressMode::Preset) kbps = (int)std::floor((double)target * 8 / 1000 / dur * 0.98);
    if (IsLossyAudioFormat(ext) && kbps < 24) return Fail(L"The target size is too small for audio of this length.");
    AudioEnc ae; ae.kbps = std::max(24, kbps);
    if (!IsLossyAudioFormat(ext)) {
      // wav / aiff / flac: the only lossless saving is a lower bit depth
      int bits = BitsPerSample(pr.mi, *pr.audio);
      if (ext == L"flac" || bits <= 16) { res.notSmaller = true; static_cast<JobResult&>(res) = JobResult::Fail(L"This is already " + std::wstring(ext == L"flac" ? L"losslessly compressed" : L"uncompressed 16-bit") + L" audio, so it cannot get meaningfully smaller without changing format. Convert it to MP3, M4A or Opus."); return res; }
      ae.bitDepth = 16;
      warnings.push_back(L"The bit depth was reduced to 16-bit to save space.");
    }
    uint64_t outBytes = 0;
    JobResult jr = WithTransaction(input, ext, L" (compressed)", req.outputPath, ctx, [&](const fs::path& out) {
      Args a = {L"-i", P(input)};
      Args rest = AudioFileOutputArgs(pr, ext, ae, req.preserveMetadata, false, L"", L"", L"", warnings);
      a.insert(a.end(), rest.begin(), rest.end());
      a.push_back(P(out));
      FfOut r = RunFfmpeg(a, dur, ctx);
      if (!r.ok) return EngineResult::Fail(r.error);
      outBytes = FileBytes(out);
      if (outBytes >= inBytes) { res.notSmaller = true; return EngineResult::Fail(L"The compressed file is not smaller than the original, so nothing was saved. The audio is probably already compressed at a low bitrate."); }
      EngineResult er = EngineResult::Ok(); er.warnings = warnings; return er;
    }, VerifyHasAudio);
    bool ns = res.notSmaller;
    static_cast<JobResult&>(res) = jr;
    res.notSmaller = ns; res.outputBytes = outBytes;
    if (jr.ok && req.mode != CompressMode::Preset && outBytes > target * 1.05) { res.missedTarget = true; ctx.AddWarning(L"The result is a little larger than the requested size."); }
    return res;
  }

  // ------------------------------------------------------------------------------------------------ video files
  std::wstring container = OutputContainerFor(input);
  int W = 0, H = 0; pr.mi.DisplaySize(W, H);
  std::wstring scale = ScaleFilter(PresetMaxHeight(req), H);
  int audioK = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : (req.mode == CompressMode::Preset ? PresetAudioKbps(req.preset) : 96);
  bool hasAudio = pr.audio != nullptr;
  // keep the original audio when it is already at least as small as what we would produce and the container takes it
  bool copyAudio = false;
  if (hasAudio) {
    bool allOk = true; int64_t br = 0;
    for (auto& s : pr.mi.streams) if (s.type == "audio") { allOk = allOk && AudioCopyOk(container, s.codec); br += s.bitRate; }
    copyAudio = allOk && br > 0 && br <= (int64_t)audioK * 1000 * 1.05;
  }

  if (req.mode == CompressMode::Preset) {
    int crf = PresetCrf(req.preset);
    res.crfUsed = crf;
    uint64_t outBytes = 0;
    JobResult jr = WithTransaction(input, container, L" (compressed)", req.outputPath, ctx, [&](const fs::path& out) {
      std::vector<std::wstring> warnings;
      StdEncode e; e.container = container; e.crf = crf; e.hw = req.hardware; e.preserve = req.preserveMetadata; e.codec = CodecLike(container, pr.video->codec);
      e.allowAudioCopy = copyAudio; e.audioKbps = audioK;
      Args a = {L"-i", P(input)};
      Args rest = BuildStdOutputArgs(pr, e, scale, L"", warnings);
      a.insert(a.end(), rest.begin(), rest.end());
      a.push_back(P(out));
      FfOut r = RunFfmpeg(a, dur, ctx);
      if (!r.ok) return EngineResult::Fail(r.error);
      outBytes = FileBytes(out);
      if (outBytes >= inBytes) { res.notSmaller = true; return EngineResult::Fail(L"The compressed video is not smaller than the original, so nothing was saved. It is probably already well compressed; try a stronger preset."); }
      EngineResult er = EngineResult::Ok(); er.warnings = warnings; return er;
    }, VerifyHasVideo);
    bool ns = res.notSmaller; int crfU = res.crfUsed;
    static_cast<JobResult&>(res) = jr;
    res.notSmaller = ns; res.crfUsed = crfU; res.inputBytes = inBytes; res.outputBytes = outBytes;
    return res;
  }

  // ---- target size / percent: two-pass average-bitrate encode
  double overhead = 0.96;   // container + rounding
  double videoKbpsD = (double)target * 8 / 1000 / dur * overhead - (hasAudio ? audioK : 0);
  if (hasAudio && videoKbpsD < 40) {   // try a smaller audio budget before giving up
    audioK = 48; copyAudio = false;
    videoKbpsD = (double)target * 8 / 1000 / dur * overhead - audioK;
  }
  if (videoKbpsD < 30) return Fail(L"The target size is too small for a video of this length. Choose a larger size or shorten the video first.");
  int videoKbps = (int)videoKbpsD;
  res.videoKbps = videoKbps;
  VCodec codec = DefaultVideoCodec(container);
  if (container == L"mkv" || container == L"mp4" || container == L"mov") codec = VCodec::H264;
  if (!EncoderAvailable(codec, false)) return Fail(L"This build of the media engine cannot encode the video for that size target.");
  fs::path passLog = ctx.TempDir() / L"pass";
  uint64_t outBytes = 0;
  JobResult jr = WithTransaction(input, container, L" (compressed)", req.outputPath, ctx, [&](const fs::path& out) {
    std::vector<std::wstring> warnings;
    auto build = [&](int pass, int kbps) {
      StdEncode e; e.container = container; e.preserve = req.preserveMetadata; e.codec = codec; e.allowAudioCopy = copyAudio; e.audioKbps = audioK;
      e.videoKbps = kbps; e.pass = pass; e.passLog = passLog;
      Args a = {L"-i", P(input)};
      std::vector<std::wstring> w2;
      Args rest = BuildStdOutputArgs(pr, e, scale, L"", pass == 1 ? w2 : warnings);
      a.insert(a.end(), rest.begin(), rest.end());
      return a;
    };
    // pass 1: analysis only (no audio, no output file)
    {
      // analysis pass: video only, written to the null muxer
      Args b = {L"-i", P(input), L"-map", L"0:" + std::to_wstring(pr.video->index)};
      std::vector<std::wstring> w2;
      bool cfr = false;
      std::wstring vf = JoinFilters(scale, BuildVideoFilter(pr, container, w2, cfr));
      if (!vf.empty()) b.insert(b.end(), {L"-vf", vf});
      VideoEnc ve; ve.codec = codec; ve.bitrateKbps = videoKbps; ve.pass = 1; ve.passLog = passLog;
      Args va = VideoEncoderArgs(container, ve, nullptr);
      b.insert(b.end(), va.begin(), va.end());
      if (cfr) b.insert(b.end(), {L"-fps_mode", L"cfr"});
      b.insert(b.end(), {L"-an", L"-sn", L"-f", L"null", L"-"});
      FfOut r1 = RunFfmpeg(b, dur, ctx, 0.0, 0.4);
      if (!r1.ok) return EngineResult::Fail(r1.error);
    }
    int kbps = videoKbps;
    for (int attempt = 0; attempt < 2; ++attempt) {
      Args a = build(2, kbps);
      a.push_back(P(out));
      FfOut r = RunFfmpeg(a, dur, ctx, attempt == 0 ? 0.4 : 0.8, attempt == 0 ? (0.85) : 1.0);
      if (!r.ok) return EngineResult::Fail(r.error);
      outBytes = FileBytes(out);
      if (outBytes <= target * 1.03 || attempt == 1) break;
      double ratio = (double)target / (double)outBytes;
      kbps = std::max(30, (int)(kbps * ratio * 0.97));
      res.videoKbps = kbps;
    }
    if (outBytes >= inBytes) { res.notSmaller = true; return EngineResult::Fail(L"The compressed video is not smaller than the original, so nothing was saved."); }
    EngineResult er = EngineResult::Ok(); er.warnings = warnings; return er;
  }, VerifyHasVideo);
  bool ns = res.notSmaller; int vk = res.videoKbps;
  static_cast<JobResult&>(res) = jr;
  res.notSmaller = ns; res.videoKbps = vk; res.inputBytes = inBytes; res.outputBytes = outBytes;
  if (jr.ok && outBytes > target * 1.05) { res.missedTarget = true; ctx.AddWarning(L"The result is slightly larger than the requested size (" + FormatBytes(outBytes) + L" instead of " + FormatBytes(target) + L")."); }
  return res;
}

} // namespace filo::media
