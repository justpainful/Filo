#include "engines/engines.h"
#include "media_convert.h"
#include "media_internal.h"
#include "registry/capability.h"

namespace filo {
using namespace media;
using namespace media::detail;

namespace {

OptionSpec Slider(const char* id, const wchar_t* label, double mn, double mx, double step, double def, const wchar_t* unit = L"") {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Slider; o.min = mn; o.max = mx; o.step = step; o.def = def; o.unit = unit; return o;
}
OptionSpec Toggle(const char* id, const wchar_t* label, bool def) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Toggle; o.def = def ? 1 : 0; return o;
}
OptionSpec Choice(const char* id, const wchar_t* label, std::vector<std::pair<std::string, std::wstring>> ch, double defIndex = 0) {
  OptionSpec o; o.id = id; o.label = label; o.kind = OptionSpec::Kind::Choice; o.choices = std::move(ch); o.def = defIndex; return o;
}

std::vector<OptionSpec> VideoOptions(const std::wstring& out, bool copyOffered) {
  std::vector<OptionSpec> v;
  if (out == L"gif") {
    v.push_back(Slider("gifFps", L"Frames per second", 5, 30, 1, 12, L"fps"));
    v.push_back(Slider("gifWidth", L"Maximum width", 120, 1280, 10, 480, L"px"));
    v.push_back(Toggle("gifLoop", L"Loop forever", true));
    return v;
  }
  v.push_back(Slider("crf", L"Quality (lower number = better picture, bigger file)", 14, 40, 1, 22));
  v.push_back(Slider("audioBitrateKbps", L"Audio bitrate", 64, 320, 8, 192, L"kbps"));
  std::vector<std::pair<std::string, std::wstring>> codecs;
  int def = 0;
  for (VCodec c : {VCodec::H264, VCodec::HEVC, VCodec::AV1, VCodec::VP9, VCodec::MPEG4, VCodec::WMV2})
    if (ContainerAllowsVideoCodec(out, c)) { if (c == DefaultVideoCodec(out)) def = (int)codecs.size(); codecs.push_back({ChoiceId(c), Widen(VCodecName(c))}); }
  if (codecs.size() > 1) v.push_back(Choice("codec", L"Video codec", codecs, def));
  if (out == L"mp4" || out == L"mov" || out == L"mkv") v.push_back(Toggle("hardwareAcceleration", L"Use the graphics card (NVENC) when available", true));
  if (copyOffered) v.push_back(Toggle("copyStreams", L"Copy streams without re-encoding when the formats allow it", false));
  v.push_back(Toggle("preserveMetadata", L"Keep metadata (title, date, chapters...)", true));
  return v;
}

std::vector<OptionSpec> AudioOptions(const std::wstring& out, bool fromVideo) {
  std::vector<OptionSpec> v;
  bool lossy = out == L"mp3" || out == L"m4a" || out == L"ogg" || out == L"opus" || out == L"wma";
  if (lossy) v.push_back(Slider("audioBitrateKbps", L"Bitrate", 64, 320, 8, 192, L"kbps"));
  if (out == L"m4a") v.push_back(Toggle("lossless", L"Lossless (ALAC) instead of AAC", false));
  if (out == L"wav" || out == L"aiff" || out == L"flac") v.push_back(Choice("bitDepth", L"Bit depth", {{"0", L"Automatic"}, {"16", L"16-bit"}, {"24", L"24-bit"}}, 0));
  v.push_back(Choice("sampleRate", L"Sample rate", {{"0", L"Keep original"}, {"8000", L"8 kHz"}, {"16000", L"16 kHz"}, {"22050", L"22.05 kHz"}, {"32000", L"32 kHz"},
                                                     {"44100", L"44.1 kHz"}, {"48000", L"48 kHz"}, {"96000", L"96 kHz"}}, 0));
  v.push_back(Choice("channels", L"Channels", {{"0", L"Keep original"}, {"1", L"Mono"}, {"2", L"Stereo"}}, 0));
  if (fromVideo || out == L"mp3" || out == L"m4a") v.push_back(Toggle("copyStreams", L"Copy the audio as-is when the formats match", false));
  v.push_back(Toggle("preserveMetadata", L"Keep tags and cover art", true));
  return v;
}

bool VideoPathAvailable(const std::wstring& out) {
  if (!ff::Available()) return false;
  if (out == L"gif") return ff::HasEncoder("gif");
  VCodec c = DefaultVideoCodec(out);
  return EncoderAvailable(c, false) && !AudioEncoderName(out).empty() && ff::HasEncoder(AudioEncoderName(out));
}
bool AudioPathAvailable(const std::wstring& out) {
  if (!ff::Available()) return false;
  std::string e = AudioEncoderName(out);
  return !e.empty() && ff::HasEncoder(e);
}

const wchar_t* kVideoIn[] = {L"mp4", L"mov", L"mkv", L"webm", L"avi", L"wmv"};
const wchar_t* kAudio[] = {L"mp3", L"m4a", L"wav", L"flac", L"ogg", L"opus", L"aiff", L"wma"};

std::wstring VideoLimit(const std::wstring& out) {
  if (out == L"gif") return L"GIF images are limited to 256 colours and have no sound or subtitles; long or large clips make very large files.";
  if (out == L"wmv") return L"Uses the old Windows Media Video 8 codec for compatibility (large files, softer picture); subtitles are not stored.";
  if (out == L"avi") return L"AVI stores MPEG-4 video with MP3 audio by default; chapters and subtitles are not stored.";
  if (out == L"webm") return L"WebM stores VP9 (or AV1) video with Opus audio; only text subtitles (as WebVTT) are kept.";
  if (out == L"mp4" || out == L"mov") return L"Only text subtitles are kept (as mov_text); picture-based subtitles are dropped.";
  return L"";
}
std::wstring AudioLimit(const std::wstring& out, bool fromVideo) {
  std::wstring s;
  if (out == L"mp3" || out == L"wma") s = L"Mixes more than 2 channels down to stereo. ";
  if (out == L"wav" || out == L"opus" || out == L"ogg" || out == L"aiff" || out == L"wma") s += L"Cover art cannot be stored in this format. ";
  if (fromVideo) s += L"Extracts the first audio track. ";
  return Trim(s);
}
bool IsLossyAudio(const std::wstring& e) { return e == L"mp3" || e == L"m4a" || e == L"ogg" || e == L"opus" || e == L"wma"; }

void Add(const std::wstring& in, const std::wstring& out, Group g) {
  Capability c;
  c.id = Narrow(in) + ">" + Narrow(out);
  c.group = g; c.in = in; c.out = out; c.engine = "ffmpeg";
  bool outAudio = IsAudioFormat(out);
  bool inVideo = g == Group::Video || in == L"gif";
  if (outAudio) {
    c.lossy = IsLossyAudio(out);
    c.limitation = AudioLimit(out, inVideo);
    c.options = AudioOptions(out, inVideo);
    c.available = [out] { return AudioPathAvailable(out); };
  } else {
    c.lossy = true;
    c.limitation = VideoLimit(out);
    c.options = VideoOptions(out, out != L"gif");
    c.available = [out] { return VideoPathAvailable(out); };
  }
  c.run = [](const ConvertTask& t, JobContext& ctx) { return media::ConvertMedia(t, ctx); };
  c.verify = [out](const fs::path& p, std::wstring* why) { return media::VerifyMediaOutput(p, out, why); };
  CapabilityRegistry::Get().Add(std::move(c));
}

} // namespace

void RegisterMediaEngine() {
  // video -> video / gif
  for (auto in : kVideoIn)
    for (auto out : kVideoIn) if (std::wstring(in) != out) Add(in, out, Group::Video);
  for (auto in : kVideoIn) Add(in, L"gif", Group::Video);
  // animated gif -> video (the input is an image by content; ffmpeg decodes the animation)
  for (auto out : kVideoIn) Add(L"gif", out, Group::Image);
  // video -> audio extraction
  for (auto in : kVideoIn) for (auto out : {L"mp3", L"m4a", L"wav"}) Add(in, out, Group::Video);
  // audio <-> audio
  for (auto in : kAudio) for (auto out : kAudio) if (std::wstring(in) != out) Add(in, out, Group::Audio);
}

} // namespace filo
