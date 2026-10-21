// Headless media operations used by the tool dialogs (compress, trim, crop, speed, join, split, frames, redact, normalise,
// visualiser, channels, bleep, waveform, previews). Everything runs ffmpeg as a separate process, reports real progress
// through JobContext::SetProgress and stops promptly when JobContext::Cancelled() becomes true.
//
// Conventions
//  * Functions that create a file never write the final path: they use OutputTransaction / OutputDirTransaction (core/output.h).
//    `outputPath` / `outputDir` empty => "<stem><suffix>.<ext>" beside the input according to Settings (UniqueName policy).
//  * The input file is never modified.
//  * Result structs derive from JobResult (ok / outputs / error). Warnings are reported through JobContext::AddWarning (the job
//    manager copies them to JobResult::warnings), exactly like RunConversion does.
//  * Times are seconds (double). Rectangles are in DISPLAY pixels: the picture as the user sees it after the rotation
//    metadata has been applied (a phone video recorded in portrait is 1080x1920 here even if coded 1920x1080 + rotate 90).
//  * Re-encoded video output has the rotation baked into the pixels (no rotation metadata); stream-copy outputs keep it.
#pragma once
#include "core/job.h"
#include "registry/capability.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace filo::media {

struct TimeRange { double start = 0, end = 0; };

// ================================================================================================================================
// 1. Compress
// ================================================================================================================================
enum class CompressMode { Preset, TargetSize, Percent };
enum class CompressPreset { High, Balanced, Small, Smallest };

struct CompressRequest {
  CompressMode mode = CompressMode::Preset;
  CompressPreset preset = CompressPreset::Balanced;   // Preset mode: High crf 23, Balanced 27, Small 31, Smallest 35 (+ 720p cap)
  uint64_t targetBytes = 0;                           // TargetSize mode: aim for <= this many bytes (two-pass ABR)
  double percentSmaller = 50;                         // Percent mode: result is (100 - x)% of the original size
  int maxHeight = 0;                                  // 0 keeps the size; taller videos are scaled down (never up)
  int audioBitrateKbps = 0;                           // 0 = automatic (video: 96..128, audio: preset based)
  bool hardware = false;                              // NVENC (Preset mode only; target-size modes always use software two-pass)
  bool preserveMetadata = true;
  fs::path outputPath;
};
struct CompressResult : JobResult {
  bool notSmaller = false;       // the re-encode was not smaller than the original: nothing was kept (ok == false)
  bool missedTarget = false;     // target-size modes: result stayed above the requested size (still saved, warning added)
  uint64_t inputBytes = 0, outputBytes = 0;
  int crfUsed = 0;               // Preset mode
  int videoKbps = 0;             // target-size modes
};
// Video: same container as the input (mp4/mov/mkv/webm/avi/wmv; anything else becomes mp4). Audio: same format, lower bitrate.
CompressResult CompressMedia(const fs::path& input, const CompressRequest& req, JobContext& ctx);

struct SizeEstimate {
  uint64_t bytes = 0, lowBytes = 0, highBytes = 0;
  bool exact = false;            // target modes are exact targets; presets are heuristics
  bool feasible = true;          // false when the target is too small for the audio + a watchable video
  std::wstring note;
};
SizeEstimate EstimateCompressedSize(const fs::path& input, const CompressRequest& req);

// ================================================================================================================================
// 2. Trim
// ================================================================================================================================
struct TrimVideoRequest {
  double start = 0;
  double end = 0;                // <= start or >= duration: until the end
  bool copyMode = false;         // stream copy: starts at the keyframe at/before `start` (see TrimResult)
  bool preserveMetadata = true;
  bool hardware = false;
  int crf = 0;                   // 0 = Settings.videoCrf
  fs::path outputPath;
};
struct TrimAudioRequest {
  double start = 0;
  double end = 0;
  bool copyMode = false;         // stream copy (mp3/aac/flac/wav... cut at packet boundaries)
  bool trimSilentEnds = false;   // ignore start/end and cut leading/trailing silence instead (see DetectSilentEnds)
  double silenceThresholdDb = -50;
  double minSilenceSec = 0.15;
  double fadeInSec = 0, fadeOutSec = 0;
  bool preserveMetadata = true;
  int audioBitrateKbps = 0;      // 0 = keep the source bitrate (or 192)
  fs::path outputPath;
};
struct TrimResult : JobResult {
  double requestedStart = 0, requestedEnd = 0;
  double actualStart = 0, actualEnd = 0;     // what the output really contains, in source time (differs only in copy mode)
  double outputDuration = 0;                 // measured with ffprobe on the produced file
  bool snappedToKeyframes = false;
};
TrimResult TrimVideo(const fs::path& input, const TrimVideoRequest& req, JobContext& ctx);
TrimResult TrimAudio(const fs::path& input, const TrimAudioRequest& req, JobContext& ctx);

struct SilenceInfo {
  bool ok = false;
  std::wstring error;
  double duration = 0;
  double start = 0, end = 0;                 // range that contains sound
  double leadingSilence = 0, trailingSilence = 0;
  bool allSilent = false;
};
SilenceInfo DetectSilentEnds(const fs::path& input, double thresholdDb = -50, double minSilenceSec = 0.15, const std::atomic<bool>* cancel = nullptr);

// ================================================================================================================================
// 3. Crop
// ================================================================================================================================
struct CropRect { int x = 0, y = 0, w = 0, h = 0; };
enum class AspectPreset { Free, Square1x1, R4x3, R3x4, R16x9, R9x16, R3x2, R2x3, R21x9, R4x5 };
// Largest centred (or offset) rectangle of the given aspect inside srcW x srcH, even-sized. centerX/Y in 0..1, zoom >= 1 shrinks it.
CropRect AspectCropRect(int srcW, int srcH, AspectPreset preset, double centerX = 0.5, double centerY = 0.5, double zoom = 1.0);
struct CropRequest {
  CropRect rect;                 // display pixels; w/h are rounded down to even numbers, x/y down to even
  bool preserveMetadata = true;
  bool hardware = false;
  int crf = 0;
  fs::path outputPath;
};
struct CropResult : JobResult { CropRect applied; int outWidth = 0, outHeight = 0; };
CropResult CropVideo(const fs::path& input, const CropRequest& req, JobContext& ctx);

// ================================================================================================================================
// 4. Speed
// ================================================================================================================================
struct SpeedRequest {
  double factor = 2.0;           // 0.25 .. 8 (anything > 0 is accepted; atempo is chained)
  bool changePitch = false;      // true: audio pitch follows the speed (tape style); false: pitch preserved
  bool interpolate = false;      // slow motion: blend extra frames instead of repeating (video only, slow)
  bool preserveMetadata = true;
  bool hardware = false;
  int crf = 0;
  fs::path outputPath;           // video or audio file (audio files keep their format)
};
struct SpeedResult : JobResult { double expectedDuration = 0, outputDuration = 0; };
SpeedResult ChangeSpeed(const fs::path& input, const SpeedRequest& req, JobContext& ctx);
double SpeedDurationPreview(double inputDuration, double factor);       // inputDuration / factor

// ================================================================================================================================
// 5. Join
// ================================================================================================================================
struct JoinRequest {
  std::vector<fs::path> inputs;  // in order; every clip needs a video track
  int crf = 0;
  bool hardware = false;
  int audioBitrateKbps = 192;
  fs::path outputPath;           // .mp4
};
// Output: MP4 (H.264 + AAC), frame size and frame rate of the FIRST clip; all other clips are scaled/padded to it, rotation applied,
// audio normalised to 48 kHz stereo, clips without audio get silence.
struct JoinResult : JobResult { double expectedDuration = 0, outputDuration = 0; int width = 0, height = 0; double fps = 0; };
JoinResult JoinVideos(const JoinRequest& req, JobContext& ctx);

// ================================================================================================================================
// 6. Split
// ================================================================================================================================
enum class SplitMode { Markers, EqualParts, Segments };
struct SplitRequest {
  SplitMode mode = SplitMode::EqualParts;
  std::vector<double> markers;       // Markers: cut points; N markers => N+1 files
  int parts = 2;                     // EqualParts
  std::vector<TimeRange> segments;   // Segments: custom, may leave gaps
  bool copyMode = false;             // keyframe-snapped stream copy (fast, no quality loss)
  bool preserveMetadata = true;
  bool hardware = false;
  int crf = 0;
  fs::path outputDir;                // desired folder; empty => "<stem> (split)" beside the input
};
struct SplitSegment { double start = 0, end = 0; double actualStart = 0; fs::path path; };
struct SplitResult : JobResult { std::vector<SplitSegment> segments; fs::path folder; };
SplitResult SplitVideo(const fs::path& input, const SplitRequest& req, JobContext& ctx);

// ================================================================================================================================
// 7. Frames
// ================================================================================================================================
struct SaveFramesRequest {
  std::vector<double> timestamps;    // seconds; clamped into the video
  fs::path outputDir;                // desired folder; empty => "<stem> (frames)" beside the input
};
struct FramesResult : JobResult { fs::path folder; };
// PNG at native (rotation-applied) resolution, named "<stem> - frame 001.png", ...
FramesResult SaveVideoFrames(const fs::path& input, const SaveFramesRequest& req, JobContext& ctx);

struct FrameImage {
  bool ok = false;
  std::wstring error;
  int width = 0, height = 0;
  std::vector<uint8_t> bgra;         // width*height*4, premultiplication not applied (alpha = 255)
  double time = 0;                   // requested time (clamped)
};
// Fast single-frame decode for previews. accurate=false snaps to the previous keyframe (much faster for scrubbing).
FrameImage ExtractFrame(const fs::path& path, double seconds, int maxW, int maxH, bool accurate = true, const std::atomic<bool>* cancel = nullptr);

// Keeps the probe result between calls (no ffprobe per frame) and remembers recent frames.
class FrameScrubber {
 public:
  explicit FrameScrubber(fs::path path);
  ~FrameScrubber();
  bool Open(std::wstring* error = nullptr);       // probes the file
  double Duration() const;
  int Width() const;                              // display size
  int Height() const;
  double Fps() const;
  // accurate=false => nearest preceding keyframe (instant while dragging); call again with accurate=true when the user releases.
  FrameImage Get(double seconds, int maxW, int maxH, bool accurate = false, const std::atomic<bool>* cancel = nullptr);
 private:
  struct Impl;
  Impl* impl_;
};

// ================================================================================================================================
// 8. Redact
// ================================================================================================================================
enum class RedactStyle { Blur, Solid };
struct RedactRegion {
  int x = 0, y = 0, w = 0, h = 0;    // display pixels
  double start = 0, end = 0;         // seconds; end <= start => until the end of the video
  RedactStyle style = RedactStyle::Blur;
  uint32_t color = 0x000000;         // 0xRRGGBB for Solid
  int blurStrength = 24;             // box-blur radius in pixels (clamped to the region)
};
struct RedactRequest {
  std::vector<RedactRegion> regions;
  bool stripMetadata = false;
  bool hardware = false;
  int crf = 0;
  fs::path outputPath;
};
JobResult RedactVideo(const fs::path& input, const RedactRequest& req, JobContext& ctx);

// ================================================================================================================================
// 9. Remove audio
// ================================================================================================================================
struct RemoveAudioRequest { fs::path outputPath; };
JobResult RemoveAudio(const fs::path& input, const RemoveAudioRequest& req, JobContext& ctx);   // video stream is copied bit-exact

// ================================================================================================================================
// 10. Normalise volume (EBU R128)
// ================================================================================================================================
struct LoudnessInfo {
  bool valid = false;
  double integratedLufs = -70, truePeakDb = -70, lra = 0, thresholdLufs = -70;
};
LoudnessInfo MeasureLoudness(const fs::path& input, JobContext* ctx = nullptr, std::wstring* error = nullptr);
struct NormalizeRequest {
  double targetLufs = -16, truePeakDb = -1.5, lra = 11;
  bool preserveMetadata = true;
  int audioBitrateKbps = 0;          // 0 = source bitrate / 192
  fs::path outputPath;               // audio file => same format; video file => video copied, audio re-encoded
};
struct NormalizeResult : JobResult { LoudnessInfo input, output; bool linear = false; double gainDb = 0; };
NormalizeResult NormalizeVolume(const fs::path& input, const NormalizeRequest& req, JobContext& ctx);

// ================================================================================================================================
// 11. Audio visualiser
// ================================================================================================================================
enum class VizLayout { Landscape, Portrait, Square };         // 1920x1080, 1080x1920, 1080x1080
enum class VizStyle { Waveform, Spectrum, Vectorscope };
struct VisualizerRequest {
  VizLayout layout = VizLayout::Landscape;
  VizStyle style = VizStyle::Waveform;
  fs::path backgroundImage;          // optional; scaled to cover the frame
  bool useCoverArt = true;           // when no image is given and the audio has embedded cover art
  uint32_t backgroundColor = 0x0B1220;
  uint32_t waveColor = 0x2DD4BF;
  int fps = 30;
  int crf = 23;
  int audioBitrateKbps = 192;
  bool hardware = false;
  fs::path outputPath;               // .mp4
};
JobResult AudioVisualizer(const fs::path& audioInput, const VisualizerRequest& req, JobContext& ctx);

// ================================================================================================================================
// 12. Channels
// ================================================================================================================================
struct ChannelsRequest {
  int outChannels = 2;               // 1 = mono, 2 = stereo
  double leftGain = 1.0, rightGain = 1.0;   // linear multipliers applied to the left / right input channel (0..4)
  std::wstring outputExt;            // audio format of the result; empty = same as the input (video inputs keep their container)
  bool preserveMetadata = true;
  int audioBitrateKbps = 0;
  fs::path outputPath;
};
JobResult ConvertChannels(const fs::path& input, const ChannelsRequest& req, JobContext& ctx);
// Short 16-bit wav for listening to the settings before committing. Writes exactly `outWav` (caller owns the temp file).
EngineResult PreviewChannelsToWav(const fs::path& input, const ChannelsRequest& req, double startSec, double durationSec, const fs::path& outWav, JobContext& ctx);

// ================================================================================================================================
// 13. Bleep
// ================================================================================================================================
struct BleepRange { double start = 0, end = 0; };
enum class BleepKind { Tone, Silence };
struct BleepRequest {
  std::vector<BleepRange> ranges;
  BleepKind kind = BleepKind::Tone;
  double toneHz = 1000;
  double toneLevelDb = -12;          // relative to full scale
  int fadeMs = 8;                    // cross-fade length at both edges of every range
  bool preserveMetadata = true;
  int audioBitrateKbps = 0;
  fs::path outputPath;               // audio file => same format; video file => video copied
};
JobResult BleepAudio(const fs::path& input, const BleepRequest& req, JobContext& ctx);
EngineResult RenderBleepPreview(const fs::path& input, const BleepRequest& req, double startSec, double durationSec, const fs::path& outWav, JobContext& ctx);

// ================================================================================================================================
// 14. Waveform
// ================================================================================================================================
struct Waveform {
  bool ok = false;
  std::wstring error;
  std::vector<float> peaks;          // absolute maximum per bucket, 0..1
  std::vector<float> minima, maxima; // signed extremes per bucket (-1..1) for filled waveform drawing
  double duration = 0;
  int channels = 0;
};
// Decodes with ffmpeg to 8 kHz (all channels, at most 8) and reduces to `buckets` values; peaks are the largest absolute sample over all channels.
Waveform ComputeWaveform(const fs::path& path, int buckets, const std::atomic<bool>* cancel = nullptr);
struct WaveformPerChannel {
  bool ok = false;
  std::wstring error;
  std::vector<std::vector<float>> peaks;   // [channel][bucket], at most 8 channels
  double duration = 0;
  int channels = 0;
};
WaveformPerChannel ComputeWaveformPerChannel(const fs::path& path, int buckets, const std::atomic<bool>* cancel = nullptr);

// ================================================================================================================================
// 15. Audio preview rendering (selection + channel gains + bleeps -> temp wav)
// ================================================================================================================================
struct AudioPreviewRequest {
  double start = 0;
  double end = 0;                    // <= start: start + maxSeconds
  double maxSeconds = 30;
  int outChannels = 0;               // 0 keep, 1 mono, 2 stereo
  double leftGain = 1.0, rightGain = 1.0;
  std::vector<BleepRange> bleeps;    // source time
  BleepKind bleepKind = BleepKind::Tone;
  double toneHz = 1000, toneLevelDb = -12;
  int fadeMs = 8;
  int sampleRate = 44100;
};
EngineResult RenderAudioPreviewWav(const fs::path& input, const AudioPreviewRequest& req, const fs::path& outWav, JobContext& ctx);

} // namespace filo::media
