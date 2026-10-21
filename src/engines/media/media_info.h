// Friendly media summary built on ff::Probe (for the info panel, tool dialogs and previews).
#pragma once
#include "core/util.h"
#include <optional>
#include <string>
#include <vector>

namespace filo::media {

struct AudioTrackInfo {
  int streamIndex = 0;
  std::string codec;               // ffprobe codec_name ("aac")
  std::wstring codecName;          // "AAC"
  int channels = 0;
  int sampleRate = 0;
  int64_t bitRate = 0;
  std::wstring layout;             // "mono", "stereo", "5.1"...
  std::wstring language;
  std::wstring description;        // "AAC stereo 44.1 kHz 128 kbps"
};

struct MediaSummary {
  bool ok = false;
  std::wstring error;                // user-facing when !ok
  fs::path path;
  bool hasVideo = false, hasAudio = false;
  bool isAudioOnly = false;
  double duration = 0;               // seconds
  uint64_t sizeBytes = 0;
  int64_t totalBitRate = 0;          // bits per second (0 unknown)
  std::wstring container;            // "MP4", "Matroska/WebM"...
  // video (first non-cover video stream)
  int width = 0, height = 0;         // display size (rotation applied)
  int codedWidth = 0, codedHeight = 0;
  int rotation = 0;                  // clockwise degrees (0/90/180/270) the picture must be turned to appear upright
  double fps = 0;
  bool variableFrameRate = false;
  std::string videoCodec;            // "h264"
  std::wstring videoCodecName;       // "H.264"
  std::string pixelFormat;
  int64_t videoBitRate = 0;
  bool hdr = false;
  // audio (first audio stream)
  std::string audioCodec;
  std::wstring audioCodecName;
  int channels = 0, sampleRate = 0;
  int64_t audioBitRate = 0;
  std::vector<AudioTrackInfo> audioTracks;
  int subtitleTracks = 0;
  bool hasCoverArt = false;
  std::wstring title, artist, album;   // common tags when present
  // one-line summaries for UI lists
  std::wstring videoLine;            // "1920x1080 - 30 fps - H.264"
  std::wstring audioLine;            // "AAC stereo - 44.1 kHz - 128 kbps"
  std::wstring Describe() const;     // "0:41.74 - 1920x1080 - 30 fps - H.264 - AAC stereo - 2.2 MB"
};

// Probes `path`. Never throws; failures are reported through MediaSummary::error (empty / corrupt / missing file...).
MediaSummary SummarizeMedia(const fs::path& path);
// Friendly codec names ("h264" -> "H.264", "pcm_s16le" -> "PCM 16-bit").
std::wstring FriendlyCodecName(const std::string& ffprobeCodec);
std::wstring FriendlyLayout(int channels, const std::string& ffprobeLayout);

} // namespace filo::media
