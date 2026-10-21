#include "media_info.h"
#include "media_internal.h"
#include <cmath>

namespace filo::media {
using namespace detail;

std::wstring FriendlyCodecName(const std::string& c) {
  static const std::map<std::string, std::wstring> m = {
    {"h264", L"H.264"}, {"hevc", L"HEVC (H.265)"}, {"av1", L"AV1"}, {"vp9", L"VP9"}, {"vp8", L"VP8"}, {"mpeg4", L"MPEG-4"}, {"mpeg2video", L"MPEG-2"},
    {"wmv1", L"WMV1"}, {"wmv2", L"WMV2"}, {"wmv3", L"WMV3"}, {"vc1", L"VC-1"}, {"msmpeg4v3", L"MS-MPEG4v3"}, {"mjpeg", L"Motion JPEG"}, {"prores", L"ProRes"},
    {"gif", L"GIF"}, {"theora", L"Theora"}, {"png", L"PNG"},
    {"aac", L"AAC"}, {"mp3", L"MP3"}, {"opus", L"Opus"}, {"vorbis", L"Vorbis"}, {"flac", L"FLAC"}, {"alac", L"ALAC"}, {"ac3", L"AC-3"}, {"eac3", L"E-AC-3"},
    {"dts", L"DTS"}, {"wmav1", L"WMA"}, {"wmav2", L"WMA"}, {"wmapro", L"WMA Pro"}, {"mp2", L"MP2"},
    {"pcm_s16le", L"PCM 16-bit"}, {"pcm_s16be", L"PCM 16-bit"}, {"pcm_s24le", L"PCM 24-bit"}, {"pcm_s24be", L"PCM 24-bit"}, {"pcm_s32le", L"PCM 32-bit"},
    {"pcm_f32le", L"PCM float"}, {"pcm_u8", L"PCM 8-bit"},
    {"subrip", L"SRT"}, {"ass", L"ASS"}, {"mov_text", L"Timed text"}, {"webvtt", L"WebVTT"}};
  auto it = m.find(c);
  if (it != m.end()) return it->second;
  std::wstring w = Widen(c);
  for (auto& ch : w) ch = (wchar_t)towupper(ch);
  return w;
}

std::wstring FriendlyLayout(int ch, const std::string& l) {
  if (ch == 1) return L"mono";
  if (ch == 2) return L"stereo";
  if (ch == 6) return L"5.1";
  if (ch == 8) return L"7.1";
  if (!l.empty()) return Widen(l);
  return std::to_wstring(ch) + L" channels";
}

static std::wstring Khz(int hz) {
  if (hz <= 0) return L"";
  return ff::Num(hz / 1000.0, hz % 1000 ? 1 : 0) + L" kHz";
}

std::wstring MediaSummary::Describe() const {
  std::wstring s = FormatTime(duration);
  auto add = [&](const std::wstring& x) { if (!x.empty()) s += L" · " + x; };
  add(videoLine); add(audioLine);
  if (sizeBytes) add(FormatBytes(sizeBytes));
  return s;
}

MediaSummary SummarizeMedia(const fs::path& path) {
  MediaSummary s;
  s.path = path;
  Probed pr; std::wstring err;
  if (!ProbeFile(path, pr, err)) { s.error = err; return s; }
  std::error_code ec;
  s.sizeBytes = fs::file_size(path, ec);
  s.duration = pr.duration;
  s.totalBitRate = pr.mi.bitRate;
  s.container = Widen(pr.mi.formatName);
  size_t comma = s.container.find(L',');
  if (comma != std::wstring::npos) s.container = s.container.substr(0, comma);
  for (auto& c : s.container) c = (wchar_t)towupper(c);
  s.hasVideo = pr.video != nullptr;
  s.hasAudio = pr.audio != nullptr;
  s.isAudioOnly = !s.hasVideo && s.hasAudio;
  if (pr.video) {
    const auto& v = *pr.video;
    s.codedWidth = v.width; s.codedHeight = v.height; s.rotation = v.rotation;
    pr.mi.DisplaySize(s.width, s.height);
    s.fps = v.fps;
    s.variableFrameRate = IsVfr(pr.mi, v);
    s.videoCodec = v.codec; s.videoCodecName = FriendlyCodecName(v.codec);
    s.pixelFormat = v.pixFmt; s.videoBitRate = v.bitRate;
    std::string tr = ColorTransfer(pr.mi, v);
    s.hdr = tr == "smpte2084" || tr == "arib-std-b67";
    s.videoLine = std::to_wstring(s.width) + L"x" + std::to_wstring(s.height);
    if (s.fps > 0) s.videoLine += L" · " + ff::Num(s.fps, 2) + L" fps" + (s.variableFrameRate ? L" (variable)" : L"");
    s.videoLine += L" · " + s.videoCodecName;
  }
  for (auto& st : pr.mi.streams) {
    if (st.type == "audio") {
      AudioTrackInfo t;
      t.streamIndex = st.index; t.codec = st.codec; t.codecName = FriendlyCodecName(st.codec);
      t.channels = st.channels; t.sampleRate = st.sampleRate; t.bitRate = st.bitRate;
      t.layout = FriendlyLayout(st.channels, ChannelLayoutOf(pr.mi, st));
      t.language = Widen(st.language);
      t.description = t.codecName + L" " + t.layout;
      if (t.sampleRate) t.description += L" · " + Khz(t.sampleRate);
      if (t.bitRate) t.description += L" · " + std::to_wstring((t.bitRate + 500) / 1000) + L" kbps";
      s.audioTracks.push_back(t);
    } else if (st.type == "subtitle") ++s.subtitleTracks;
    else if (st.type == "video" && st.attachedPic) s.hasCoverArt = true;
  }
  if (pr.audio) {
    s.audioCodec = pr.audio->codec; s.audioCodecName = FriendlyCodecName(pr.audio->codec);
    s.channels = pr.audio->channels; s.sampleRate = pr.audio->sampleRate; s.audioBitRate = pr.audio->bitRate;
    s.audioLine = s.audioTracks.empty() ? L"" : s.audioTracks[0].description;
  }
  const Json& tags = pr.mi.raw.Get("format").Get("tags");
  auto tag = [&](const char* k) {
    for (auto& [key, v] : tags.Items()) { std::string lk = key; for (auto& c : lk) c = (char)tolower((unsigned char)c); if (lk == k) return Widen(v.AsString()); }
    return std::wstring();
  };
  s.title = tag("title"); s.artist = tag("artist"); s.album = tag("album");
  s.ok = true;
  return s;
}

} // namespace filo::media
