#include "media_internal.h"
#include "registry/filetype.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace filo::media::detail {

// ============================================================================================================================
// process helpers
// ============================================================================================================================
static bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

std::wstring FriendlyFfmpegError(const std::string& e) {
  if (Contains(e, "No space left")) return L"There is not enough free disk space to write the result.";
  if (Contains(e, "Permission denied") || Contains(e, "Access is denied")) return L"Access to the file or the output folder was denied.";
  if (Contains(e, "No such file or directory") && !Contains(e, "Invalid data")) return L"A file could not be found.";
  if (Contains(e, "Invalid data found") || Contains(e, "moov atom not found") || Contains(e, "Header missing") || Contains(e, "End of file") ||
      Contains(e, "could not find codec parameters") || Contains(e, "Invalid argument") && Contains(e, "Error opening input"))
    return L"The file appears to be damaged, incomplete or not a media file.";
  if (Contains(e, "Unknown encoder") || Contains(e, "Encoder") && Contains(e, "not found")) return L"This build of the media engine lacks an encoder required for this conversion.";
  if (Contains(e, "matches no streams") || Contains(e, "does not contain any stream") || Contains(e, "Stream map") && Contains(e, "matches no streams"))
    return L"The file does not contain the audio or video stream this operation needs.";
  if (Contains(e, "Error while opening encoder") || Contains(e, "Could not open encoder"))
    return L"The encoder could not be started with these settings (for example an unsupported size or sample rate).";
  if (Contains(e, "Output file does not contain any stream") || Contains(e, "Nothing was written"))
    return L"Nothing could be written: the file has no stream that fits the chosen format.";
  return L"The media engine could not process this file.";
}

static Args BaseArgs(bool verbose) {
  return {L"-nostdin", L"-hide_banner", L"-loglevel", verbose ? L"info" : L"error", L"-nostats", L"-y"};
}

FfOut RunFfmpeg(const Args& args, double durationSec, JobContext& ctx, double p0, double p1, bool verbose) {
  FfOut o;
  fs::path exe = ff::FfmpegPath();
  if (exe.empty()) { o.error = L"FFmpeg was not found. Reinstall Filo to restore its media components."; return o; }
  ProcessOptions po;
  po.exe = exe;
  po.args = BaseArgs(verbose);
  po.args.insert(po.args.end(), {L"-progress", L"pipe:1"});
  po.args.insert(po.args.end(), args.begin(), args.end());
  po.cancel = ctx.CancelFlag();
  po.captureLimit = 16u << 20;
  po.onStdoutLine = [&](const std::string& line) {
    if (durationSec > 0 && line.rfind("out_time_us=", 0) == 0) {
      double t = atof(line.c_str() + 12) / 1e6;
      if (t >= 0) ctx.SetProgress(p0 + (p1 - p0) * std::min(1.0, t / durationSec));
    }
  };
  ProcessResult r = RunProcess(po);
  o.stderrText = r.err;
  if (r.cancelled) { o.cancelled = true; o.error = L"Cancelled"; return o; }
  if (!r.started) { o.error = L"Could not start FFmpeg: " + r.startError; return o; }
  if (r.timedOut) { o.error = L"FFmpeg timed out."; return o; }
  if (r.exitCode != 0) {
    FILO_LOG(Warn, "media", "ffmpeg exit %lu: %s", r.exitCode, r.err.substr(r.err.size() > 800 ? r.err.size() - 800 : 0).c_str());
    o.error = FriendlyFfmpegError(r.err);
    return o;
  }
  ctx.SetProgress(p1);
  o.ok = true;
  return o;
}

FfOut RunFfmpegSimple(const Args& args, const std::atomic<bool>* cancel, DWORD timeoutMs, size_t captureLimit, bool verbose) {
  FfOut o;
  fs::path exe = ff::FfmpegPath();
  if (exe.empty()) { o.error = L"FFmpeg was not found. Reinstall Filo to restore its media components."; return o; }
  ProcessOptions po;
  po.exe = exe;
  po.args = BaseArgs(verbose);
  po.args.insert(po.args.end(), args.begin(), args.end());
  po.cancel = cancel;
  po.timeoutMs = timeoutMs;
  po.captureLimit = captureLimit;
  ProcessResult r = RunProcess(po);
  o.stderrText = r.err;
  o.stdoutText = std::move(r.out);
  if (r.cancelled) { o.cancelled = true; o.error = L"Cancelled"; return o; }
  if (!r.started) { o.error = L"Could not start FFmpeg: " + r.startError; return o; }
  if (r.timedOut) { o.error = L"FFmpeg timed out."; return o; }
  if (r.exitCode != 0) { o.error = FriendlyFfmpegError(r.err); return o; }
  o.ok = true;
  return o;
}

// ============================================================================================================================
// probing
// ============================================================================================================================
bool ProbeFile(const fs::path& p, Probed& out, std::wstring& err) {
  fs::path lp = fs::path(P(p));     // \?\ prefix for very long paths
  std::error_code ec;
  if (!fs::exists(lp, ec)) { err = L"The file could not be found."; return false; }
  if (fs::is_regular_file(lp, ec) && fs::file_size(lp, ec) == 0) { err = L"The file is empty."; return false; }
  if (!ff::Available()) { err = L"FFmpeg was not found. Reinstall Filo to restore its media components."; return false; }
  auto mi = ff::Probe(lp);
  if (!mi || mi->streams.empty()) { err = L"This file could not be read as audio or video. It may be damaged or in an unsupported format."; return false; }
  out.mi = std::move(*mi);
  out.video = out.mi.FirstVideo();
  out.audio = out.mi.FirstAudio();
  out.duration = out.mi.duration;
  if (out.duration <= 0) for (auto& s : out.mi.streams) out.duration = std::max(out.duration, s.duration);
  return true;
}

const Json& RawStream(const ff::MediaInfo& mi, int index) {
  static const Json null;
  const auto& arr = mi.raw.Get("streams");
  for (size_t i = 0; i < arr.Size(); ++i) if (arr.At(i).Get("index").AsInt(-1) == index) return arr.At(i);
  return null;
}

static double RateOf(const std::string& s) {
  size_t sl = s.find('/');
  if (sl == std::string::npos) return atof(s.c_str());
  double n = atof(s.c_str()), d = atof(s.c_str() + sl + 1);
  return d > 0 ? n / d : 0;
}

bool IsVfr(const ff::MediaInfo& mi, const ff::StreamInfo& v) {
  const Json& r = RawStream(mi, v.index);
  double rf = RateOf(r.Get("r_frame_rate").AsString()), af = RateOf(r.Get("avg_frame_rate").AsString());
  if (rf <= 0 || af <= 0) return false;
  return std::fabs(rf - af) / rf > 0.03;
}

double VideoDuration(const Probed& pr) {
  if (pr.video && pr.video->duration > 0) return pr.video->duration;
  return pr.duration;
}

std::string ColorTransfer(const ff::MediaInfo& mi, const ff::StreamInfo& v) { return RawStream(mi, v.index).Get("color_transfer").AsString(); }
int BitsPerSample(const ff::MediaInfo& mi, const ff::StreamInfo& a) {
  const Json& r = RawStream(mi, a.index);
  int b = (int)r.Get("bits_per_raw_sample").AsInt(0);
  if (b <= 0) b = (int)r.Get("bits_per_sample").AsInt(0);
  return b;
}
std::string ChannelLayoutOf(const ff::MediaInfo& mi, const ff::StreamInfo& a) {
  std::string l = RawStream(mi, a.index).Get("channel_layout").AsString();
  if (!l.empty()) return l;
  return a.channels == 1 ? "mono" : a.channels == 2 ? "stereo" : "";
}

// ============================================================================================================================
// formats
// ============================================================================================================================
bool IsVideoContainer(const std::wstring& e) { return e == L"mp4" || e == L"mov" || e == L"mkv" || e == L"webm" || e == L"avi" || e == L"wmv"; }
bool IsAudioFormat(const std::wstring& e) {
  return e == L"mp3" || e == L"m4a" || e == L"wav" || e == L"flac" || e == L"ogg" || e == L"opus" || e == L"aiff" || e == L"wma";
}

const char* VCodecName(VCodec c) {
  switch (c) {
    case VCodec::H264: return "H.264"; case VCodec::HEVC: return "HEVC"; case VCodec::AV1: return "AV1";
    case VCodec::VP9: return "VP9"; case VCodec::MPEG4: return "MPEG-4"; case VCodec::WMV2: return "WMV2";
  }
  return "";
}
std::string ChoiceId(VCodec c) {
  switch (c) {
    case VCodec::H264: return "h264"; case VCodec::HEVC: return "hevc"; case VCodec::AV1: return "av1";
    case VCodec::VP9: return "vp9"; case VCodec::MPEG4: return "mpeg4"; case VCodec::WMV2: return "wmv2";
  }
  return "";
}
bool ParseVCodec(const std::string& s, VCodec& o) {
  for (VCodec c : {VCodec::H264, VCodec::HEVC, VCodec::AV1, VCodec::VP9, VCodec::MPEG4, VCodec::WMV2}) if (ChoiceId(c) == s) { o = c; return true; }
  if (s == "x264") { o = VCodec::H264; return true; }
  if (s == "h265" || s == "x265") { o = VCodec::HEVC; return true; }
  return false;
}
VCodec DefaultVideoCodec(const std::wstring& c) {
  if (c == L"webm") return VCodec::VP9;
  if (c == L"avi") return VCodec::MPEG4;
  if (c == L"wmv") return VCodec::WMV2;
  return VCodec::H264;
}
bool ContainerAllowsVideoCodec(const std::wstring& c, VCodec v) {
  if (c == L"mp4") return v == VCodec::H264 || v == VCodec::HEVC || v == VCodec::AV1;
  if (c == L"mov") return v == VCodec::H264 || v == VCodec::HEVC;
  if (c == L"mkv") return v == VCodec::H264 || v == VCodec::HEVC || v == VCodec::AV1 || v == VCodec::VP9;
  if (c == L"webm") return v == VCodec::VP9 || v == VCodec::AV1;
  if (c == L"avi") return v == VCodec::MPEG4 || v == VCodec::H264;
  if (c == L"wmv") return v == VCodec::WMV2;
  return false;
}

static bool In(const std::string& c, std::initializer_list<const char*> l) { for (auto s : l) if (c == s) return true; return false; }
bool VideoCopyOk(const std::wstring& c, const std::string& v) {
  if (c == L"mp4") return In(v, {"h264", "hevc", "av1", "mpeg4"});
  if (c == L"mov") return In(v, {"h264", "hevc", "mpeg4", "prores", "mjpeg"});
  if (c == L"mkv") return In(v, {"h264", "hevc", "vp8", "vp9", "av1", "mpeg4", "mpeg2video", "theora", "mjpeg"});
  if (c == L"webm") return In(v, {"vp8", "vp9", "av1"});
  if (c == L"avi") return In(v, {"mpeg4", "mjpeg", "msmpeg4v3", "msmpeg4v2"});
  if (c == L"wmv") return In(v, {"wmv1", "wmv2", "wmv3", "msmpeg4v3", "vc1"});
  return false;
}
bool AudioCopyOk(const std::wstring& e, const std::string& a) {
  if (e == L"mp4") return In(a, {"aac", "mp3", "ac3", "eac3", "alac"});
  if (e == L"mov") return In(a, {"aac", "mp3", "alac", "pcm_s16le", "pcm_s16be", "pcm_s24le", "ac3"});
  if (e == L"mkv") return In(a, {"aac", "mp3", "opus", "vorbis", "flac", "ac3", "eac3", "dts", "alac", "pcm_s16le", "pcm_s24le"});
  if (e == L"webm") return In(a, {"opus", "vorbis"});
  if (e == L"avi") return In(a, {"mp3", "ac3", "pcm_s16le", "aac"});
  if (e == L"wmv" || e == L"wma") return In(a, {"wmav1", "wmav2", "wmapro"});
  if (e == L"mp3") return a == "mp3";
  if (e == L"m4a") return In(a, {"aac", "alac"});
  if (e == L"flac") return a == "flac";
  if (e == L"opus") return a == "opus";
  if (e == L"ogg") return a == "vorbis";
  return false;
}

std::wstring VideoContainerFor(const fs::path& p) {
  FileKind k = DetectFile(p);
  if (IsVideoContainer(k.format)) return k.format;
  return L"";
}
std::wstring AudioFormatFor(const fs::path& p) {
  FileKind k = DetectFile(p);
  if (IsAudioFormat(k.format)) return k.format;
  return L"";
}
std::wstring OutputContainerFor(const fs::path& input) {
  std::wstring c = VideoContainerFor(input);
  return c.empty() ? L"mp4" : c;
}

// ============================================================================================================================
// encoders
// ============================================================================================================================
static const char* HwName(VCodec c) {
  switch (c) { case VCodec::H264: return "h264_nvenc"; case VCodec::HEVC: return "hevc_nvenc"; case VCodec::AV1: return "av1_nvenc"; default: return nullptr; }
}

bool NvencUsable(VCodec c) {
  const char* name = HwName(c);
  if (!name || !ff::HasEncoder(name)) return false;
  static std::mutex mu;
  static std::map<int, bool> cache;
  std::lock_guard lk(mu);
  auto it = cache.find((int)c);
  if (it != cache.end()) return it->second;
  Args a = {L"-f", L"lavfi", L"-i", L"testsrc2=s=256x256:r=25:d=1", L"-t", L"1", L"-c:v", Widen(name), L"-f", L"null", L"-"};
  FfOut r = RunFfmpegSimple(a, nullptr, 20000);
  cache[(int)c] = r.ok;
  FILO_LOG(Info, "media", "NVENC trial %s: %s", name, r.ok ? "ok" : "unavailable");
  return r.ok;
}

bool EncoderAvailable(VCodec c, bool allowHw) {
  if (allowHw && HwName(c) && NvencUsable(c)) return true;
  switch (c) {
    case VCodec::H264: return ff::HasEncoder("libx264");
    case VCodec::HEVC: return ff::HasEncoder("libx265");
    case VCodec::AV1: return ff::HasEncoder("libsvtav1") || ff::HasEncoder("libaom-av1");
    case VCodec::VP9: return ff::HasEncoder("libvpx-vp9");
    case VCodec::MPEG4: return ff::HasEncoder("mpeg4");
    case VCodec::WMV2: return ff::HasEncoder("wmv2");
  }
  return false;
}

static int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

Args VideoEncoderArgs(const std::wstring& container, VideoEnc e, std::vector<std::wstring>* warnings) {
  Args a;
  auto add = [&](std::initializer_list<const wchar_t*> l) { for (auto s : l) a.push_back(s); };
  const bool abr = e.bitrateKbps > 0;
  const std::wstring br = std::to_wstring(std::max(8, e.bitrateKbps)) + L"k";
  bool hw = e.hw && e.pass == 0 && !abr && HwName(e.codec) && NvencUsable(e.codec) && (container == L"mp4" || container == L"mov" || container == L"mkv") &&
            !(e.codec == VCodec::AV1 && container == L"mov");
  auto passArgs = [&] {
    if (e.pass > 0) { a.push_back(L"-pass"); a.push_back(std::to_wstring(e.pass)); a.push_back(L"-passlogfile"); a.push_back(e.passLog.wstring()); }
  };
  if (hw) {
    // NVENC "cq" is not the same scale as x264 crf: +5 / +9 gives comparable file sizes and quality on the same footage
    int cq = e.codec == VCodec::HEVC ? Clamp(e.crf + 9, 0, 51) : e.codec == VCodec::AV1 ? Clamp((int)std::lround(e.crf * 1.45) + 3, 0, 63) : Clamp(e.crf + 5, 0, 51);
    a.push_back(L"-c:v"); a.push_back(Widen(HwName(e.codec)));
    add({L"-preset", L"p5", L"-tune", L"hq", L"-rc", L"vbr", L"-b:v", L"0"});
    a.push_back(L"-cq"); a.push_back(std::to_wstring(cq));
    if (e.codec == VCodec::H264) add({L"-profile:v", L"high"});
    if (e.codec == VCodec::HEVC && (container == L"mp4" || container == L"mov")) add({L"-tag:v", L"hvc1"});
    add({L"-pix_fmt", L"yuv420p"});
    return a;
  }
  switch (e.codec) {
    case VCodec::H264:
      add({L"-c:v", L"libx264", L"-preset"});
      a.push_back(e.preset ? Widen(e.preset) : (e.fast ? L"ultrafast" : L"medium"));
      if (abr) { add({L"-b:v"}); a.push_back(br); passArgs(); }
      else { add({L"-crf"}); a.push_back(std::to_wstring(Clamp(e.crf, 0, 51))); }
      add({L"-pix_fmt", L"yuv420p", L"-profile:v", L"high"});
      if (container == L"avi") add({L"-bf", L"0"});
      break;
    case VCodec::HEVC:
      add({L"-c:v", L"libx265", L"-preset"});
      a.push_back(e.preset ? Widen(e.preset) : (e.fast ? L"ultrafast" : L"medium"));
      if (abr) { add({L"-b:v"}); a.push_back(br); }
      else { add({L"-crf"}); a.push_back(std::to_wstring(Clamp(e.crf + 5, 0, 51))); }
      add({L"-pix_fmt", L"yuv420p", L"-x265-params", L"log-level=error"});
      if (container == L"mp4" || container == L"mov") add({L"-tag:v", L"hvc1"});
      break;
    case VCodec::AV1:
      if (ff::HasEncoder("libsvtav1")) {
        add({L"-c:v", L"libsvtav1", L"-preset"});
        a.push_back(e.fast ? L"12" : L"8");
        if (abr) { add({L"-b:v"}); a.push_back(br); } else { add({L"-crf"}); a.push_back(std::to_wstring(Clamp((int)std::lround(e.crf * 1.45), 0, 63))); }
        add({L"-pix_fmt", L"yuv420p"});
      } else {
        add({L"-c:v", L"libaom-av1", L"-cpu-used", L"6", L"-row-mt", L"1"});
        if (abr) { add({L"-b:v"}); a.push_back(br); } else { add({L"-crf"}); a.push_back(std::to_wstring(Clamp((int)std::lround(e.crf * 1.45), 0, 63))); add({L"-b:v", L"0"}); }
        add({L"-pix_fmt", L"yuv420p"});
      }
      break;
    case VCodec::VP9:
      add({L"-c:v", L"libvpx-vp9", L"-row-mt", L"1", L"-deadline"});
      a.push_back(e.fast ? L"realtime" : L"good");
      add({L"-cpu-used"}); a.push_back(e.fast ? L"8" : L"4");
      if (abr) { add({L"-b:v"}); a.push_back(br); passArgs(); }
      else { add({L"-crf"}); a.push_back(std::to_wstring(Clamp(e.crf + 9, 0, 63))); add({L"-b:v", L"0"}); }
      add({L"-pix_fmt", L"yuv420p"});
      break;
    case VCodec::MPEG4:
    case VCodec::WMV2: {
      add({L"-c:v", e.codec == VCodec::MPEG4 ? L"mpeg4" : L"wmv2"});
      if (abr) { add({L"-b:v"}); a.push_back(br); passArgs(); }
      else { double q = 2 + (e.crf - 18) * 0.5; add({L"-q:v"}); a.push_back(std::to_wstring(Clamp((int)std::lround(q), 1, 31))); }
      add({L"-g", L"250", L"-pix_fmt", L"yuv420p"});
      if (e.codec == VCodec::MPEG4 && container == L"avi") add({L"-vtag", L"xvid"});
      break;
    }
  }
  (void)warnings;
  return a;
}

Args VideoContainerFlags(const std::wstring& c) {
  if (c == L"mp4" || c == L"mov") return {L"-movflags", L"+faststart"};
  return {};
}

// ---- audio ---------------------------------------------------------------------------------------------------------------
static std::wstring Family(const std::wstring& ext) {
  if (ext == L"mp4" || ext == L"mov" || ext == L"mkv" || ext == L"m4a") return L"aac";
  if (ext == L"webm" || ext == L"opus") return L"opus";
  if (ext == L"avi" || ext == L"mp3") return L"mp3";
  if (ext == L"wmv" || ext == L"wma") return L"wma";
  if (ext == L"ogg") return L"vorbis";
  return ext;   // wav, aiff, flac
}
std::string AudioEncoderName(const std::wstring& ext, bool lossless) {
  std::wstring f = Family(ext);
  if (f == L"aac") return lossless && ext == L"m4a" ? "alac" : "aac";
  if (f == L"opus") return "libopus";
  if (f == L"mp3") return "libmp3lame";
  if (f == L"wma") return "wmav2";
  if (f == L"vorbis") return "libvorbis";
  if (f == L"flac") return "flac";
  if (f == L"wav") return "pcm_s16le";
  if (f == L"aiff") return "pcm_s16be";
  return "";
}
bool CoverSupported(const std::wstring& e) { return e == L"mp3" || e == L"m4a" || e == L"flac"; }

static int NearestRate(int r, std::initializer_list<int> allowed) {
  int best = *allowed.begin(); double bd = 1e18;
  for (int a : allowed) { double d = std::fabs(std::log((double)r / a)) + (a > r ? 0.05 : 0); if (d < bd) { bd = d; best = a; } }
  return best;
}
static bool RateIn(int r, std::initializer_list<int> allowed) { for (int a : allowed) if (a == r) return true; return false; }

Args AudioEncoderArgs(const std::wstring& ext, const AudioEnc& ae, const ff::MediaInfo* mi, const ff::StreamInfo* src, std::vector<std::wstring>* warnings) {
  Args a;
  std::wstring fam = Family(ext);
  int srcRate = src ? src->sampleRate : 0;
  int srcCh = src ? src->channels : 0;
  int ch = ae.channels > 0 ? ae.channels : srcCh;
  int rate = ae.sampleRate > 0 ? ae.sampleRate : srcRate;
  int outCh = ch;
  int kbps = std::max(8, ae.kbps);
  auto push = [&](const wchar_t* k, const std::wstring& v) { a.push_back(k); a.push_back(v); };
  auto warn = [&](const std::wstring& w) { if (warnings) warnings->push_back(w); };

  int bits = 16;
  if (ae.bitDepth == 24) bits = 24;
  else if (ae.bitDepth == 16) bits = 16;
  else if (src && mi) { int b = BitsPerSample(*mi, *src); if (b > 16) bits = 24; }

  if (fam == L"aac") {
    if (ae.lossless && ext == L"m4a") { push(L"-c:a", L"alac"); if (bits > 16) push(L"-sample_fmt", L"s32p"); else push(L"-sample_fmt", L"s16p"); }
    else {
      push(L"-c:a", L"aac");
      if (outCh > 8) outCh = 2;
      int k = outCh > 2 ? std::min(512, kbps * outCh / 2) : kbps;
      push(L"-b:a", std::to_wstring(Clamp(k, 32, 512)) + L"k");
      if (rate > 96000) rate = 48000;
    }
  } else if (fam == L"mp3") {
    push(L"-c:a", L"libmp3lame");
    push(L"-b:a", std::to_wstring(Clamp(kbps, 32, 320)) + L"k");
    if (rate > 0 && !RateIn(rate, {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000})) rate = NearestRate(rate, {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000});
    if (outCh > 2) { outCh = 2; warn(L"The audio was mixed down to stereo (MP3 supports at most 2 channels)."); }
    if (ext == L"mp3") { a.push_back(L"-id3v2_version"); a.push_back(L"3"); }
  } else if (fam == L"opus") {
    push(L"-c:a", L"libopus");
    push(L"-b:a", std::to_wstring(Clamp(kbps, 8, std::max(64, 128 * std::max(1, std::min(outCh, 8))))) + L"k");
    push(L"-vbr", L"on");
    if (outCh > 8) outCh = 2;
    if (outCh > 2) push(L"-mapping_family", L"1");
    if (rate <= 0 || !RateIn(rate, {8000, 12000, 16000, 24000, 48000})) rate = 48000;
  } else if (fam == L"vorbis") {
    push(L"-c:a", L"libvorbis");
    // quality mode (an ABR target fails for some rate/bitrate combinations, e.g. 192k at 96 kHz): kbps -> nominal vorbis quality 0..10
    {
      static const int kb[] = {64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 500};
      double q = 10;
      for (int i = 0; i < 11; ++i) if (kbps <= kb[i]) { q = i == 0 ? 0.0 : (i - 1) + (double)(kbps - kb[i - 1]) / (kb[i] - kb[i - 1]); break; }
      push(L"-q:a", N(std::max(0.0, std::min(10.0, q)), 2));
    }
    if (outCh > 8) outCh = 2;
  } else if (fam == L"wma") {
    push(L"-c:a", L"wmav2");
    push(L"-b:a", std::to_wstring(Clamp(kbps, 32, 320)) + L"k");
    if (rate > 0 && !RateIn(rate, {8000, 11025, 16000, 22050, 32000, 44100, 48000})) rate = NearestRate(rate, {8000, 11025, 16000, 22050, 32000, 44100, 48000});
    if (outCh > 2) { outCh = 2; warn(L"The audio was mixed down to stereo (WMA supports at most 2 channels)."); }
  } else if (fam == L"flac") {
    push(L"-c:a", L"flac");
    push(L"-compression_level", L"8");
    if (bits > 16) { push(L"-sample_fmt", L"s32"); push(L"-bits_per_raw_sample", L"24"); } else push(L"-sample_fmt", L"s16");
  } else if (fam == L"wav") {
    push(L"-c:a", bits > 16 ? L"pcm_s24le" : L"pcm_s16le");
  } else if (fam == L"aiff") {
    push(L"-c:a", bits > 16 ? L"pcm_s24be" : L"pcm_s16be");
  }
  if (rate > 0 && rate != srcRate) push(L"-ar", std::to_wstring(rate));
  else if (rate > 0 && (fam == L"opus" || fam == L"mp3" || fam == L"wma")) push(L"-ar", std::to_wstring(rate));
  if (outCh > 0 && outCh != srcCh) push(L"-ac", std::to_wstring(outCh));
  return a;
}

// ---- subtitles / metadata ------------------------------------------------------------------------------------------------
static bool IsTextSub(const std::string& c) { return c == "subrip" || c == "srt" || c == "ass" || c == "ssa" || c == "mov_text" || c == "webvtt" || c == "text" || c == "microdvd" || c == "subviewer"; }
static bool IsBitmapSub(const std::string& c) { return c == "dvd_subtitle" || c == "hdmv_pgs_subtitle" || c == "dvb_subtitle" || c == "xsub" || c == "dvb_teletext"; }

int SubtitleArgs(const std::wstring& container, const ff::MediaInfo& mi, Args& maps, Args& codecArgs, std::vector<std::wstring>* warnings) {
  int kept = 0, dropped = 0;
  bool simplified = false;
  for (auto& s : mi.streams) {
    if (s.type != "subtitle") continue;
    std::wstring codec;
    if (container == L"mp4" || container == L"mov") { if (IsTextSub(s.codec)) { codec = L"mov_text"; if (s.codec == "ass" || s.codec == "ssa") simplified = true; } }
    else if (container == L"mkv") {
      if (s.codec == "ass" || s.codec == "ssa" || s.codec == "subrip" || s.codec == "srt" || IsBitmapSub(s.codec)) codec = L"copy";
      else if (IsTextSub(s.codec)) codec = L"srt";
    } else if (container == L"webm") { if (IsTextSub(s.codec)) { codec = L"webvtt"; if (s.codec == "ass" || s.codec == "ssa") simplified = true; } }
    if (codec.empty()) { ++dropped; continue; }
    maps.push_back(L"-map"); maps.push_back(L"0:" + std::to_wstring(s.index));
    codecArgs.push_back(L"-c:s:" + std::to_wstring(kept)); codecArgs.push_back(codec);
    ++kept;
  }
  if (warnings) {
    if (dropped) warnings->push_back(dropped == 1 ? L"One subtitle track could not be stored in this format and was left out." : std::to_wstring(dropped) + L" subtitle tracks could not be stored in this format and were left out.");
    if (simplified) warnings->push_back(L"Subtitle styling was simplified to plain text.");
  }
  return kept;
}

Args MetadataArgs(bool preserve, bool chapters) {
  if (preserve) return chapters ? Args{L"-map_metadata", L"0", L"-map_chapters", L"0"} : Args{L"-map_metadata", L"0", L"-map_chapters", L"-1"};
  return {L"-map_metadata", L"-1", L"-map_chapters", L"-1"};
}

// ---- misc --------------------------------------------------------------------------------------------------------------------
std::wstring P(const fs::path& p) {
  std::wstring w = p.wstring();
  return w.size() >= 240 ? LongPath(p) : w;
}
std::wstring N(double v, int d) { return ff::Num(v, d); }
std::wstring FilterEscape(const std::wstring& s) {
  std::wstring o;
  for (wchar_t c : s) { if (c == L'\\' || c == L'\'' || c == L':' || c == L',' || c == L'[' || c == L']' || c == L';' || c == L'=') o.push_back(L'\\'); o.push_back(c); }
  return o;
}
std::wstring Hex6(uint32_t rgb) {
  wchar_t b[16]; swprintf_s(b, L"0x%06X", rgb & 0xFFFFFF); return b;
}
fs::path DesiredOutput(const fs::path& input, const std::wstring& ext, const std::wstring& suffix, const fs::path& explicitPath) {
  if (!explicitPath.empty()) return explicitPath;
  return DesiredOutputPath(input, ext, SettingsStore::Get().Snapshot(), suffix);
}
void AddWarnings(JobContext& ctx, const std::vector<std::wstring>& w) { for (auto& s : w) ctx.AddWarning(s); }
bool FileExists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

} // namespace filo::media::detail
