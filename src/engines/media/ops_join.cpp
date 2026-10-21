// Join several clips into one MP4 through the concat filter (never raw byte concatenation).
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>

namespace filo::media {
using namespace detail;

namespace {
JoinResult Fail(const std::wstring& why) { JoinResult r; static_cast<JobResult&>(r) = JobResult::Fail(why); return r; }

// Common broadcast rates keep their exact rational form.
std::wstring FpsExpr(double fps) {
  struct R { double v; const wchar_t* e; };
  static const R rates[] = {{23.976, L"24000/1001"}, {29.97, L"30000/1001"}, {59.94, L"60000/1001"}, {47.952, L"48000/1001"}};
  for (auto& r : rates) if (std::fabs(fps - r.v) < 0.02) return r.e;
  return N(std::round(fps * 1000) / 1000.0, 3);
}
}

JoinResult JoinVideos(const JoinRequest& req, JobContext& ctx) {
  if (req.inputs.size() < 2) return Fail(L"Add at least two videos to join.");
  std::vector<Probed> clips(req.inputs.size());
  size_t cmdLen = 0;
  for (size_t i = 0; i < req.inputs.size(); ++i) {
    std::wstring err;
    if (!ProbeFile(req.inputs[i], clips[i], err)) return Fail(req.inputs[i].filename().wstring() + L": " + err);
    if (!clips[i].video) return Fail(req.inputs[i].filename().wstring() + L" has no video track, so it cannot be joined.");
    cmdLen += req.inputs[i].wstring().size() + 6;
    if (ctx.Cancelled()) return Fail(L"Cancelled");
  }
  if (cmdLen > 24000) return Fail(L"Too many clips (or paths that are too long) for a single join. Join them in smaller groups.");

  // target geometry = first clip as the user sees it
  int W = 0, H = 0; clips[0].mi.DisplaySize(W, H);
  W &= ~1; H &= ~1;
  if (W < 16 || H < 16) return Fail(L"The first clip is too small to use as the reference size.");
  double fps = clips[0].video->fps;
  if (!(fps >= 1 && fps <= 240)) fps = 30;
  if (fps > 60) fps = 60;
  const int kRate = 48000;

  std::vector<double> vdur(clips.size());
  double expected = 0;
  for (size_t i = 0; i < clips.size(); ++i) { vdur[i] = std::max(0.04, VideoDuration(clips[i])); expected += vdur[i]; }

  std::wstring fg;
  std::wstring concatIn;
  for (size_t i = 0; i < clips.size(); ++i) {
    std::wstring n = std::to_wstring(i);
    fg += L"[" + n + L":" + std::to_wstring(clips[i].video->index) + L"]scale=" + std::to_wstring(W) + L":" + std::to_wstring(H) +
          L":force_original_aspect_ratio=decrease:flags=bicubic,pad=" + std::to_wstring(W) + L":" + std::to_wstring(H) +
          L":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps=" + FpsExpr(fps) + L",format=yuv420p,setpts=PTS-STARTPTS[v" + n + L"];";
    if (clips[i].audio) {
      fg += L"[" + n + L":" + std::to_wstring(clips[i].audio->index) + L"]aresample=" + std::to_wstring(kRate) +
            L",aformat=sample_fmts=fltp:channel_layouts=stereo,apad,atrim=0:" + N(vdur[i], 4) + L",asetpts=PTS-STARTPTS[a" + n + L"];";
    } else {
      fg += L"anullsrc=r=" + std::to_wstring(kRate) + L":cl=stereo:d=" + N(vdur[i], 4) + L",aformat=sample_fmts=fltp,asetpts=PTS-STARTPTS[a" + n + L"];";
    }
    concatIn += L"[v" + n + L"][a" + n + L"]";
  }
  fg += concatIn + L"concat=n=" + std::to_wstring(clips.size()) + L":v=1:a=1[vout][aout]";

  JoinResult res;
  res.expectedDuration = expected; res.width = W; res.height = H; res.fps = fps;
  double outDur = 0;
  bool mismatch = false;
  JobResult jr = WithTransaction(req.inputs[0], L"mp4", L" (joined)", req.outputPath, ctx, [&](const fs::path& out) {
    std::vector<std::wstring> warnings;
    Args a;
    for (auto& p : req.inputs) { a.push_back(L"-i"); a.push_back(P(p)); }
    a.insert(a.end(), {L"-filter_complex", fg, L"-map", L"[vout]", L"-map", L"[aout]"});
    VideoEnc ve; ve.codec = VCodec::H264; ve.crf = EffectiveCrf(req.crf); ve.hw = req.hardware;
    Args va = VideoEncoderArgs(L"mp4", ve, &warnings);
    a.insert(a.end(), va.begin(), va.end());
    AudioEnc ae; ae.kbps = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : 192; ae.channels = 2; ae.sampleRate = kRate;
    ff::StreamInfo fake; fake.channels = 2; fake.sampleRate = kRate;
    Args aa = AudioEncoderArgs(L"mp4", ae, nullptr, &fake, &warnings);
    a.insert(a.end(), aa.begin(), aa.end());
    a.insert(a.end(), {L"-map_metadata", L"0", L"-map_chapters", L"-1", L"-movflags", L"+faststart", L"-f", L"mp4", P(out)});
    FfOut r = RunFfmpeg(a, expected, ctx);
    if (!r.ok) return EngineResult::Fail(r.error);
    auto mi = ff::Probe(out);
    outDur = mi ? mi->duration : 0;
    double tol = 0.3 + 1.5 / fps * clips.size();
    if (std::fabs(outDur - expected) > tol) { mismatch = true; return EngineResult::Fail(L"The joined video came out with an unexpected length, so it was not saved."); }
    for (size_t i = 0; i < clips.size(); ++i) {
      int w = 0, h = 0; clips[i].mi.DisplaySize(w, h);
      if (w != W || h != H) { warnings.push_back(req.inputs[i].filename().wstring() + L" was resized to fit the first clip's frame (" + std::to_wstring(W) + L"x" + std::to_wstring(H) + L")."); }
      if (!clips[i].audio) warnings.push_back(req.inputs[i].filename().wstring() + L" has no sound; silence was added.");
    }
    EngineResult er = EngineResult::Ok(); er.warnings = warnings; return er;
  }, VerifyHasVideo);
  (void)mismatch;
  static_cast<JobResult&>(res) = jr;
  res.outputDuration = outDur;
  return res;
}

} // namespace filo::media
