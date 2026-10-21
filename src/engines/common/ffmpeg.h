// Shared FFmpeg/FFprobe helpers used by the image, media and doc engines. FFmpeg runs as a separate process (never
// linked), which keeps Filo's own licence independent of the GPL codecs inside the bundled ffmpeg build.
#pragma once
#include "core/json.h"
#include "core/job.h"
#include "core/process.h"
#include "registry/capability.h"
#include <optional>
#include <string>
#include <vector>

namespace filo::ff {

fs::path FfmpegPath();               // tools\ffmpeg.exe next to Filo.exe, else PATH. Empty when missing.
fs::path FfprobePath();
bool Available();                    // both found
// Version string ("9.0-full_build...") and whether an encoder/decoder/muxer exists - cached after the first call.
std::string Version();
bool HasEncoder(const std::string& name);       // e.g. "libwebp", "libaom-av1", "libx265"
bool HasDecoder(const std::string& name);
bool HasMuxer(const std::string& name);

struct StreamInfo {
  int index = 0;
  std::string type;                  // "video" | "audio" | "subtitle" | "data"
  std::string codec;                 // "h264", "aac", ...
  int width = 0, height = 0;
  double fps = 0;                    // avg_frame_rate (0 when unknown / audio)
  std::string pixFmt;
  int channels = 0, sampleRate = 0;
  int64_t bitRate = 0;
  int rotation = 0;                  // degrees from side data / tags (0, 90, 180, 270)
  bool attachedPic = false;          // cover art stream
  double duration = 0;
  std::string language;
};

struct MediaInfo {
  double duration = 0;               // seconds (format level)
  int64_t bitRate = 0;
  std::string formatName;            // ffprobe format_name ("mov,mp4,m4a,3gp,3g2,mj2")
  std::vector<StreamInfo> streams;
  Json raw;                          // full ffprobe JSON (format + streams) for metadata tools
  const StreamInfo* FirstVideo() const;
  const StreamInfo* FirstAudio() const;
  int AudioCount() const;
  bool HasVideo() const { return FirstVideo() != nullptr; }     // excludes attached pictures
  bool HasAudio() const { return FirstAudio() != nullptr; }
  // Display size after applying rotation metadata.
  void DisplaySize(int& w, int& h) const;
};

std::optional<MediaInfo> Probe(const fs::path& file, DWORD timeoutMs = 30000);

// Runs ffmpeg with `-nostdin -hide_banner -loglevel error -progress pipe:1 -y` prepended. `durationSec` > 0 maps
// out_time to progress (0..1 scaled into [p0,p1] of the job). Honours ctx cancellation. On failure the returned error
// carries the last stderr lines (sanitised, no file contents).
EngineResult Run(const std::vector<std::wstring>& args, double durationSec, JobContext& ctx, double p0 = 0.0, double p1 = 1.0);

// Common option builders
std::wstring Num(double v, int decimals = 3);                // locale-independent number -> wide string
std::vector<std::wstring> HwDecodeArgs(bool allowHw);        // {} unless hardware decode is enabled & safe

} // namespace filo::ff
