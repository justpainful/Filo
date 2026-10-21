// Audio visualiser: renders a real-time-reactive waveform / spectrum / vectorscope over a background into an MP4.
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>

namespace filo::media {
using namespace detail;

JobResult AudioVisualizer(const fs::path& input, const VisualizerRequest& req, JobContext& ctx) {
  Probed pr; std::wstring err;
  if (!ProbeFile(input, pr, err)) return JobResult::Fail(err);
  if (!pr.audio) return JobResult::Fail(L"This file has no audio track to visualise.");
  int W = 1920, H = 1080;
  if (req.layout == VizLayout::Portrait) { W = 1080; H = 1920; }
  else if (req.layout == VizLayout::Square) { W = 1080; H = 1080; }
  int fps = std::max(10, std::min(60, req.fps));
  double dur = std::max(0.1, pr.duration);
  std::error_code ec;
  bool haveImage = !req.backgroundImage.empty();
  if (haveImage && !fs::exists(req.backgroundImage, ec)) return JobResult::Fail(L"The background image could not be found.");
  const ff::StreamInfo* cover = nullptr;
  if (!haveImage && req.useCoverArt) for (auto& s : pr.mi.streams) if (s.type == "video" && s.attachedPic) { cover = &s; break; }

  const std::wstring Ws = std::to_wstring(W), Hs = std::to_wstring(H), fpss = std::to_wstring(fps);
  std::wstring rgb = Hex6(req.waveColor);
  int wh = H / 3;
  std::wstring g;
  // background -> [bg]
  if (haveImage) g += L"[1:v]scale=" + Ws + L":" + Hs + L":force_original_aspect_ratio=increase,crop=" + Ws + L":" + Hs + L",setsar=1,fps=" + fpss + L",format=yuv420p[bg];";
  else if (cover) g += L"[0:" + std::to_wstring(cover->index) + L"]scale=" + Ws + L":" + Hs + L":force_original_aspect_ratio=increase,crop=" + Ws + L":" + Hs +
                       L",setsar=1,format=yuv420p,loop=loop=-1:size=1:start=0,fps=" + fpss + L"[bg];";
  else g += L"color=c=" + Hex6(req.backgroundColor) + L":s=" + Ws + L"x" + Hs + L":r=" + fpss + L",format=yuv420p[bg];";
  // audio split
  std::wstring aIn = L"[0:" + std::to_wstring(pr.audio->index) + L"]";
  g += aIn + L"asplit=2[aw][ao];";
  std::wstring overlay;
  switch (req.style) {
    case VizStyle::Spectrum:
      g += L"[aw]showfreqs=s=" + Ws + L"x" + std::to_wstring(H / 2) + L":mode=bar:fscale=log:ascale=log:win_size=2048:colors=" + rgb + L":rate=" + fpss + L",format=rgba[wv];";
      overlay = L"(W-w)/2:(H-h)/2";
      break;
    case VizStyle::Vectorscope: {
      int sz = std::min(W, H) * 3 / 4;
      uint32_t c = req.waveColor;
      g += L"[aw]aformat=channel_layouts=stereo,avectorscope=s=" + std::to_wstring(sz) + L"x" + std::to_wstring(sz) + L":mode=lissajous_xy:rate=" + fpss +
           L":rc=" + std::to_wstring((c >> 16) & 255) + L":gc=" + std::to_wstring((c >> 8) & 255) + L":bc=" + std::to_wstring(c & 255) + L":zoom=1.5:draw=line,format=rgba[wv];";
      overlay = L"(W-w)/2:(H-h)/2";
      break;
    }
    default:
      g += L"[aw]showwaves=s=" + Ws + L"x" + std::to_wstring(wh) + L":mode=cline:rate=" + fpss + L":colors=" + rgb + L":draw=full,format=rgba[wv];";
      overlay = L"(W-w)/2:(H-h)/2";
  }
  g += L"[bg][wv]overlay=" + overlay + L":format=auto:shortest=1,format=yuv420p[v]";

  return WithTransaction(input, L"mp4", L" (visualizer)", req.outputPath, ctx, [&](const fs::path& out) {
    std::vector<std::wstring> warnings;
    Args a = {L"-i", P(input)};
    if (haveImage) a.insert(a.end(), {L"-loop", L"1", L"-framerate", fpss, L"-i", P(req.backgroundImage)});
    a.insert(a.end(), {L"-filter_complex", g, L"-map", L"[v]", L"-map", L"[ao]", L"-t", N(dur, 3)});
    VideoEnc ve; ve.codec = VCodec::H264; ve.crf = req.crf > 0 ? req.crf : 23; ve.hw = req.hardware; ve.preset = "veryfast";
    Args va = VideoEncoderArgs(L"mp4", ve, &warnings);
    a.insert(a.end(), va.begin(), va.end());
    AudioEnc ae; ae.kbps = req.audioBitrateKbps > 0 ? req.audioBitrateKbps : 192;
    Args aa = AudioEncoderArgs(L"mp4", ae, &pr.mi, pr.audio, &warnings);
    a.insert(a.end(), aa.begin(), aa.end());
    a.insert(a.end(), {L"-r", fpss, L"-map_metadata", L"-1", L"-movflags", L"+faststart", L"-f", L"mp4", P(out)});
    FfOut r = RunFfmpeg(a, dur, ctx);
    EngineResult er = r.ok ? EngineResult::Ok() : EngineResult::Fail(r.error);
    er.warnings = warnings;
    return er;
  }, VerifyHasVideo);
}

} // namespace filo::media
