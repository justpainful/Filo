// Shared plumbing for the operation APIs: transactions, standard re-encode argument builders, keyframe lookup.
#include "media_internal.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace filo::media::detail {

const wchar_t* MuxerFor(const std::wstring& e) {
  if (e == L"mp4") return L"mp4"; if (e == L"mov") return L"mov"; if (e == L"mkv") return L"matroska"; if (e == L"webm") return L"webm";
  if (e == L"avi") return L"avi"; if (e == L"wmv" || e == L"wma") return L"asf"; if (e == L"mp3") return L"mp3"; if (e == L"m4a") return L"ipod";
  if (e == L"wav") return L"wav"; if (e == L"flac") return L"flac"; if (e == L"ogg") return L"ogg"; if (e == L"opus") return L"opus"; if (e == L"aiff") return L"aiff";
  if (e == L"gif") return L"gif";
  return L"mp4";
}

bool IsLossyAudioFormat(const std::wstring& e) { return e == L"mp3" || e == L"m4a" || e == L"ogg" || e == L"opus" || e == L"wma"; }

std::wstring JoinFilters(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return a + L"," + b;
}

int EffectiveCrf(int requested) {
  if (requested > 0) return std::min(requested, 51);
  int c = SettingsStore::Get().Snapshot().videoCrf;
  return std::max(0, std::min(51, c > 0 ? c : 22));
}

bool CancelledNow(JobContext& ctx) { return ctx.Cancelled(); }

void RemoveWithRetry(const fs::path& p) {
  for (int i = 0; i < 40; ++i) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    if (fs::remove(p, ec)) return;
    Sleep(50);
  }
}

JobResult WithTransaction(const fs::path& naming, const std::wstring& ext, const std::wstring& suffix, const fs::path& explicitPath, JobContext& ctx,
                          const WorkFn& work, const VerifyFn2& verify) {
  if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
  Settings st = SettingsStore::Get().Snapshot();
  fs::path desired = explicitPath.empty() ? DesiredOutputPath(naming, ext, st, suffix) : explicitPath;
  if (!explicitPath.empty()) { std::error_code ec; fs::create_directories(desired.parent_path(), ec); }
  OutputTransaction tx(desired, st.overwritePolicy);
  ctx.SetStage(JobState::Running, L"");
  EngineResult er = work(tx.TempPath());
  if (ctx.Cancelled()) { tx.Rollback(); RemoveWithRetry(tx.TempPath()); return JobResult::Fail(L"Cancelled"); }
  for (auto& w : er.warnings) ctx.AddWarning(w);
  if (!er.ok) { tx.Rollback(); RemoveWithRetry(tx.TempPath()); return JobResult::Fail(er.error.empty() ? L"The operation failed." : er.error); }
  ctx.SetStage(JobState::Finalizing, L"");
  std::wstring err;
  if (!tx.Commit(&err, verify)) return JobResult::Fail(err);
  return JobResult::Ok({tx.FinalPath()});
}

JobResult WithDirTransaction(const fs::path& naming, const std::wstring& suffix, const fs::path& explicitDir, JobContext& ctx, const WorkFn& work) {
  if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
  Settings st = SettingsStore::Get().Snapshot();
  fs::path desired = explicitDir;
  if (desired.empty()) desired = ResolveOutputDir(naming, st) / (naming.stem().wstring() + st.filenameSuffix + suffix);
  else { std::error_code ec; fs::create_directories(desired.parent_path(), ec); }
  OutputDirTransaction tx(desired);
  ctx.SetStage(JobState::Running, L"");
  EngineResult er = work(tx.TempDir());
  if (ctx.Cancelled()) return JobResult::Fail(L"Cancelled");
  for (auto& w : er.warnings) ctx.AddWarning(w);
  if (!er.ok) return JobResult::Fail(er.error.empty() ? L"The operation failed." : er.error);
  ctx.SetStage(JobState::Finalizing, L"");
  std::wstring err;
  if (!tx.Commit(&err)) return JobResult::Fail(err);
  return JobResult::Ok({tx.FinalDir()});
}

bool VerifyHasVideo(const fs::path& p, std::wstring* why) {
  auto mi = ff::Probe(p);
  if (!mi || !mi->FirstVideo() || mi->duration <= 0) { if (why) *why = L"The result could not be opened for verification."; return false; }
  return true;
}
bool VerifyHasAudio(const fs::path& p, std::wstring* why) {
  auto mi = ff::Probe(p);
  if (!mi || !mi->FirstAudio() || mi->duration <= 0) { if (why) *why = L"The result could not be opened for verification."; return false; }
  return true;
}

// ---- video filters ------------------------------------------------------------------------------------------------------------
std::wstring BuildVideoFilter(const Probed& pr, const std::wstring& target, std::vector<std::wstring>& warnings, bool& forceCfr) {
  std::vector<std::wstring> f;
  int w = 0, h = 0; pr.mi.DisplaySize(w, h);
  forceCfr = false;
  if (pr.video && (target == L"avi" || target == L"wmv")) {
    forceCfr = true;
    if (IsVfr(pr.mi, *pr.video)) {
      double fps = pr.video->fps > 0 ? pr.video->fps : 25;
      f.push_back(L"fps=" + N(std::round(fps * 100) / 100.0, 2));
      warnings.push_back(L"The video had a variable frame rate; it was converted to a constant " + N(fps, 2) + L" fps because ." + target + L" requires it.");
    }
  }
  if ((w % 2) || (h % 2)) {
    f.push_back(L"crop=trunc(iw/2)*2:trunc(ih/2)*2");
    warnings.push_back(L"The frame size was odd (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"); one pixel row/column was trimmed because video formats need even dimensions.");
  }
  std::wstring s;
  for (size_t i = 0; i < f.size(); ++i) { if (i) s += L","; s += f[i]; }
  return s;
}

VCodec CodecLike(const std::wstring& container, const std::string& src) {
  VCodec want = DefaultVideoCodec(container);
  if (src == "hevc") want = VCodec::HEVC;
  else if (src == "av1") want = VCodec::AV1;
  else if (src == "vp9" && container != L"mp4" && container != L"mov") want = VCodec::VP9;
  if (!ContainerAllowsVideoCodec(container, want) || !EncoderAvailable(want, false)) want = DefaultVideoCodec(container);
  return want;
}

Args BuildStdOutputArgs(const Probed& pr, const StdEncode& e, const std::wstring& vf, const std::wstring& af, std::vector<std::wstring>& warnings) {
  Args a;
  const std::wstring& c = e.container;
  a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(pr.video->index)});
  std::vector<const ff::StreamInfo*> audios;
  if (!e.dropAudio) for (auto& s : pr.mi.streams) if (s.type == "audio") audios.push_back(&s);
  if (!e.filterComplex.empty()) {
    a.insert(a.end(), {L"-filter_complex", e.filterComplex});
    if (!e.dropAudio) a.insert(a.end(), {L"-map", e.audioLabel});
  } else {
    for (auto* s : audios) a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(s->index)});
  }
  Args subCodec;
  if (!e.dropSubs) SubtitleArgs(c, pr.mi, a, subCodec, &warnings);

  bool forceCfr = false;
  if (e.copyVideo) {
    a.insert(a.end(), {L"-c:v", L"copy"});
    if (pr.video->codec == "hevc" && (c == L"mp4" || c == L"mov")) a.insert(a.end(), {L"-tag:v", L"hvc1"});
  } else {
    std::wstring pre = BuildVideoFilter(pr, c, warnings, forceCfr);
    std::wstring chain = JoinFilters(vf, pre);   // user filters first (they know the display geometry), then the safety filters
    if (!chain.empty()) a.insert(a.end(), {L"-vf", chain});
    VideoEnc ve; ve.codec = e.codec; ve.crf = e.crf; ve.hw = e.hw; ve.bitrateKbps = e.videoKbps; ve.pass = e.pass; ve.passLog = e.passLog; ve.preset = e.preset;
    Args va = VideoEncoderArgs(c, ve, &warnings);
    a.insert(a.end(), va.begin(), va.end());
    if (forceCfr) a.insert(a.end(), {L"-fps_mode", L"cfr"});
  }
  if (!e.dropAudio && (!audios.empty() || !e.filterComplex.empty())) {
    bool filtered = !af.empty() || !e.filterComplex.empty();
    bool copyOk = !filtered && e.allowAudioCopy && !audios.empty() &&
                  std::all_of(audios.begin(), audios.end(), [&](const ff::StreamInfo* s) { return AudioCopyOk(c, s->codec); });
    if (copyOk) a.insert(a.end(), {L"-c:a", L"copy"});
    else {
      AudioEnc ae; ae.kbps = e.audioKbps > 0 ? e.audioKbps : 160;
      Args aa = AudioEncoderArgs(c, ae, &pr.mi, audios.empty() ? nullptr : audios[0], &warnings);
      a.insert(a.end(), aa.begin(), aa.end());
      if (!af.empty()) a.insert(a.end(), {L"-af", af});
    }
  }
  a.insert(a.end(), subCodec.begin(), subCodec.end());
  bool chapters = c == L"mkv" || c == L"mp4" || c == L"mov" || c == L"webm";
  Args md = MetadataArgs(e.preserve, chapters);
  a.insert(a.end(), md.begin(), md.end());
  Args fl = VideoContainerFlags(c);
  a.insert(a.end(), fl.begin(), fl.end());
  a.insert(a.end(), {L"-max_muxing_queue_size", L"2048", L"-f", MuxerFor(c)});
  return a;
}

Args AudioFileOutputArgs(const Probed& pr, const std::wstring& ext, const AudioEnc& ae, bool preserve, bool copy, const std::wstring& af,
                         const std::wstring& fc, const std::wstring& mapLabel, std::vector<std::wstring>& warnings) {
  Args a;
  if (!fc.empty()) a.insert(a.end(), {L"-filter_complex", fc, L"-map", mapLabel});
  else a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(pr.audio->index)});
  const ff::StreamInfo* cover = nullptr;
  for (auto& s : pr.mi.streams) if (s.type == "video" && s.attachedPic) { cover = &s; break; }
  bool coverKept = false;
  if (cover && preserve) {
    if (CoverSupported(ext)) { a.insert(a.end(), {L"-map", L"0:" + std::to_wstring(cover->index)}); coverKept = true; }
    else warnings.push_back(L"The cover art was left out (." + ext + L" files cannot carry a picture).");
  }
  if (copy && af.empty() && fc.empty()) a.insert(a.end(), {L"-c:a", L"copy"});
  else {
    Args aa = AudioEncoderArgs(ext, ae, &pr.mi, pr.audio, &warnings);
    a.insert(a.end(), aa.begin(), aa.end());
    if (!af.empty()) a.insert(a.end(), {L"-af", af});
  }
  if (coverKept) {
    if (cover->codec == "mjpeg" || cover->codec == "png") a.insert(a.end(), {L"-c:v", L"copy"});
    else a.insert(a.end(), {L"-c:v", L"mjpeg", L"-q:v", L"2"});
    a.insert(a.end(), {L"-disposition:v:0", L"attached_pic"});
  }
  Args md = MetadataArgs(preserve, false);
  a.insert(a.end(), md.begin(), md.end());
  if (ext == L"m4a") a.insert(a.end(), {L"-movflags", L"+faststart"});
  if (ext == L"wav") a.insert(a.end(), {L"-rf64", L"auto"});
  a.insert(a.end(), {L"-f", MuxerFor(ext)});
  return a;
}

int LikeSourceKbps(const Probed& pr, int requested) {
  if (requested > 0) return requested;
  if (pr.audio && pr.audio->bitRate > 0) return std::max(64, std::min(320, (int)((pr.audio->bitRate + 500) / 1000)));
  if (pr.mi.bitRate > 0 && !pr.video) return std::max(64, std::min(320, (int)((pr.mi.bitRate + 500) / 1000)));
  return 192;
}

// ---- keyframes -------------------------------------------------------------------------------------------------------------------
double KeyframeAtOrBefore(const fs::path& p, int streamIndex, double t) {
  if (t <= 0.0005) return 0;
  auto scan = [&](double from) -> double {
    std::wstring interval = N(std::max(0.0, from), 3) + L"%" + N(t + 0.5, 3);
    int code = 0;
    std::string out = RunCapture(ff::FfprobePath(), {L"-v", L"error", L"-select_streams", std::to_wstring(streamIndex), L"-show_entries", L"packet=pts_time,flags",
                                                       L"-read_intervals", interval, L"-of", L"csv=p=0", L"--", P(p)}, 120000, &code);
    double best = -1;
    std::istringstream is(out); std::string line;
    while (std::getline(is, line)) {
      size_t comma = line.find(',');
      if (comma == std::string::npos) continue;
      if (line.find('K', comma) == std::string::npos) continue;
      double pts = atof(line.c_str());
      if (pts <= t + 0.0005 && pts > best) best = pts;
    }
    return best;
  };
  double k = scan(t - 60);
  if (k < 0) k = scan(0);
  return k < 0 ? 0 : k;
}

} // namespace filo::media::detail
