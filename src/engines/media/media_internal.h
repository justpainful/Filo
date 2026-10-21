// Internal helpers shared by the media engine translation units. Not part of the public API.
#pragma once
#include "engines/common/ffmpeg.h"
#include "core/util.h"
#include "core/output.h"
#include "core/settings.h"
#include "core/log.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace filo::media::detail {

using Args = std::vector<std::wstring>;

// ---- process helpers -------------------------------------------------------------------------------------------------
struct FfOut {
  bool ok = false;
  bool cancelled = false;
  std::wstring error;        // user-facing sentence when !ok
  std::string stderrText;    // raw stderr (never shown to users)
  std::string stdoutText;
};
// Runs ffmpeg with progress (out_time -> [p0,p1]) and cancellation. `verbose` raises the log level to info so filters such
// as loudnorm / silencedetect / ebur128 print their reports into FfOut::stderrText.
FfOut RunFfmpeg(const Args& args, double durationSec, JobContext& ctx, double p0 = 0, double p1 = 1, bool verbose = false);
// Same but without a JobContext (UI helpers): cancel flag optional.
FfOut RunFfmpegSimple(const Args& args, const std::atomic<bool>* cancel, DWORD timeoutMs, size_t captureLimit = 8u << 20, bool verbose = false);
std::wstring FriendlyFfmpegError(const std::string& stderrText);

// ---- probing -----------------------------------------------------------------------------------------------------------
struct Probed {
  ff::MediaInfo mi;
  double duration = 0;                 // best duration estimate (seconds)
  const ff::StreamInfo* video = nullptr;
  const ff::StreamInfo* audio = nullptr;
};
// Probes `p`; on failure returns false and fills `err` with a user-facing sentence (empty file, corrupt, missing...).
bool ProbeFile(const fs::path& p, Probed& out, std::wstring& err);
const Json& RawStream(const ff::MediaInfo& mi, int index);
bool IsVfr(const ff::MediaInfo& mi, const ff::StreamInfo& v);
double VideoDuration(const Probed& pr);                 // video stream duration when known, else container duration
std::string ColorTransfer(const ff::MediaInfo& mi, const ff::StreamInfo& v);
int BitsPerSample(const ff::MediaInfo& mi, const ff::StreamInfo& a);   // 0 if unknown
std::string ChannelLayoutOf(const ff::MediaInfo& mi, const ff::StreamInfo& a);

// ---- formats -------------------------------------------------------------------------------------------------------------
bool IsVideoContainer(const std::wstring& ext);         // mp4 mov mkv webm avi wmv
bool IsAudioFormat(const std::wstring& ext);            // mp3 m4a wav flac ogg opus aiff wma
// Logical video codecs.
enum class VCodec { H264, HEVC, AV1, VP9, MPEG4, WMV2 };
const char* VCodecName(VCodec c);
VCodec DefaultVideoCodec(const std::wstring& container);
bool ContainerAllowsVideoCodec(const std::wstring& container, VCodec c);
std::string ChoiceId(VCodec c);                          // "h264", "hevc", "av1", "vp9", "mpeg4", "wmv2"
bool ParseVCodec(const std::string& s, VCodec& out);
// Is the (ffprobe) codec name valid to stream-copy into the container?
bool VideoCopyOk(const std::wstring& container, const std::string& codec);
bool AudioCopyOk(const std::wstring& ext, const std::string& codec);
// Container of an existing file from its detected format ("mp4","mov","mkv","webm","avi","wmv"); empty when not one of ours.
std::wstring VideoContainerFor(const fs::path& p);
std::wstring AudioFormatFor(const fs::path& p);
// Best container for an operation output derived from an input (same container when we can write it, else mp4).
std::wstring OutputContainerFor(const fs::path& input);

// ---- encoder argument builders --------------------------------------------------------------------------------------------
struct VideoEnc {
  VCodec codec = VCodec::H264;
  int crf = 22;                // x264-style scale; mapped per codec
  bool hw = false;             // NVENC when it works
  int bitrateKbps = 0;         // >0 => ABR instead of CRF (used by target-size mode)
  int pass = 0;                // 1 or 2 for two-pass
  fs::path passLog;            // -passlogfile base
  const char* preset = nullptr;// x264/x265 preset override
  bool fast = false;           // quick preview-class encode (ultrafast-ish)
};
// True when `ffmpeg` lists the NVENC encoder AND a one-second trial encode succeeded (cached per process).
bool NvencUsable(VCodec c);
// Arguments describing the video encoder (after -map): "-c:v libx264 -crf 22 ...". May append warnings.
Args VideoEncoderArgs(const std::wstring& container, VideoEnc e, std::vector<std::wstring>* warnings);
Args VideoContainerFlags(const std::wstring& container);              // -movflags +faststart etc.
bool EncoderAvailable(VCodec c, bool allowHw);

struct AudioEnc {
  int kbps = 192;
  int sampleRate = 0;          // 0 keep (adjusted when the codec cannot take the source rate)
  int channels = 0;            // 0 keep
  bool lossless = false;       // m4a -> ALAC
  int bitDepth = 0;            // 0 auto, 16 or 24 for pcm/flac
};
// "-c:a ... -b:a ... -ar .. -ac .." for the given output format/container. `src` may be null.
Args AudioEncoderArgs(const std::wstring& ext, const AudioEnc& a, const ff::MediaInfo* mi, const ff::StreamInfo* src, std::vector<std::wstring>* warnings);
std::string AudioEncoderName(const std::wstring& ext, bool lossless = false);   // ffmpeg encoder used by default for `ext`
bool CoverSupported(const std::wstring& audioExt);       // mp3 m4a flac

// Subtitle handling for a video container: appends -map/-c:s args for the input's subtitle streams; returns number kept.
int SubtitleArgs(const std::wstring& container, const ff::MediaInfo& mi, Args& maps, Args& codecArgs, std::vector<std::wstring>* warnings);

// Metadata args: -map_metadata / -map_chapters
Args MetadataArgs(bool preserve, bool chapters);

// ---- path / misc -----------------------------------------------------------------------------------------------------------
std::wstring P(const fs::path& p);                       // path as ffmpeg argument (adds \\?\ prefix for very long paths)
std::wstring N(double v, int decimals = 3);              // number -> wstring
std::wstring FilterEscape(const std::wstring& s);        // escape for use inside a filtergraph option value
std::wstring Hex6(uint32_t rgb);                         // "0xRRGGBB"
fs::path DesiredOutput(const fs::path& input, const std::wstring& ext, const std::wstring& suffix, const fs::path& explicitPath);
// Joins a vector of warnings into a JobContext.
void AddWarnings(JobContext& ctx, const std::vector<std::wstring>& w);
bool FileExists(const fs::path& p);


// ---- transactions / shared output builders ---------------------------------------------------------------------------------
using WorkFn = std::function<EngineResult(const fs::path& tempOutput)>;
using VerifyFn2 = std::function<bool(const fs::path&, std::wstring*)>;
// Creates an OutputTransaction for "<stem><suffix>.<ext>" (or explicitPath), runs `work` on the temp path, commits atomically.
// Warnings returned by `work` go to ctx.AddWarning. Cancellation => JobResult::Fail("Cancelled") and no temp file is left behind.
JobResult WithTransaction(const fs::path& naming, const std::wstring& ext, const std::wstring& suffix, const fs::path& explicitPath, JobContext& ctx,
                          const WorkFn& work, const VerifyFn2& verify = {});
// Folder variant (OutputDirTransaction); explicitDir empty => "<stem><suffix>" beside the input.
JobResult WithDirTransaction(const fs::path& naming, const std::wstring& suffix, const fs::path& explicitDir, JobContext& ctx, const WorkFn& work);
bool VerifyHasVideo(const fs::path& p, std::wstring* why);
bool VerifyHasAudio(const fs::path& p, std::wstring* why);
void RemoveWithRetry(const fs::path& p);
int EffectiveCrf(int requested);                          // 0 => Settings.videoCrf
bool CancelledNow(JobContext& ctx);

struct StdEncode {
  std::wstring container;            // output container
  int crf = 22;
  bool hw = false;
  bool preserve = true;
  VCodec codec = VCodec::H264;
  bool copyVideo = false;            // -c:v copy
  bool allowAudioCopy = true;        // copy audio when the container takes it as-is (ignored when an audio filter is present)
  bool dropAudio = false;
  bool dropSubs = false;
  int audioKbps = 0;                 // 0 => 160
  int videoKbps = 0;                 // >0 => ABR
  int pass = 0;
  fs::path passLog;
  const char* preset = nullptr;
  std::wstring filterComplex;        // when set, the single audio output is the label `audioLabel` (e.g. "[aout]")
  std::wstring audioLabel;
};
// Maps first video + all audio (+ subtitles), then codec/metadata/container args. Does NOT include -i or the output path.
Args BuildStdOutputArgs(const Probed& pr, const StdEncode& e, const std::wstring& videoFilter, const std::wstring& audioFilter, std::vector<std::wstring>& warnings);
// Picks the encoder family matching the source (hevc stays hevc, av1 stays av1...) when the container allows it.
VCodec CodecLike(const std::wstring& container, const std::string& srcCodec);
// Audio-file output arguments: maps first audio (+ cover art), codec, optional -af / filter_complex, metadata, muxer. No -i / output path.
Args AudioFileOutputArgs(const Probed& pr, const std::wstring& ext, const AudioEnc& ae, bool preserve, bool copy, const std::wstring& audioFilter,
                         const std::wstring& filterComplex, const std::wstring& mapLabel, std::vector<std::wstring>& warnings);
const wchar_t* MuxerFor(const std::wstring& ext);
// Even-size crop and (avi/wmv) constant-frame-rate handling for the first video stream; sets forceCfr for avi/wmv.
std::wstring BuildVideoFilter(const Probed& pr, const std::wstring& target, std::vector<std::wstring>& warnings, bool& forceCfr);
std::wstring JoinFilters(const std::wstring& a, const std::wstring& b);
// Closest keyframe at or before `t` (seconds) in video stream `streamIndex`; returns 0 if none found.
double KeyframeAtOrBefore(const fs::path& p, int streamIndex, double t);
// Audio encoder defaults: bitrate to use when re-encoding a lossy file "like the source".
int LikeSourceKbps(const Probed& pr, int requested);
bool IsLossyAudioFormat(const std::wstring& ext);

} // namespace filo::media::detail
