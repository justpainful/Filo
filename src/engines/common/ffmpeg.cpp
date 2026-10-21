#include "engines/common/ffmpeg.h"
#include "core/log.h"
#include <mutex>
#include <set>
#include <sstream>
#include <cmath>

namespace filo::ff {

static fs::path g_ffmpeg, g_ffprobe;
static std::once_flag g_once, g_capOnce;
static std::set<std::string> g_enc, g_dec, g_mux;
static std::string g_version;

static void Locate() {
  std::call_once(g_once, [] { g_ffmpeg = FindTool(L"ffmpeg.exe"); g_ffprobe = FindTool(L"ffprobe.exe"); });
}
fs::path FfmpegPath() { Locate(); return g_ffmpeg; }
fs::path FfprobePath() { Locate(); return g_ffprobe; }
bool Available() { return !FfmpegPath().empty() && !FfprobePath().empty(); }

static void LoadCaps() {
  std::call_once(g_capOnce, [] {
    if (FfmpegPath().empty()) return;
    auto parse = [](const std::string& text, std::set<std::string>& out, bool skipHeader) {
      std::istringstream is(text); std::string line; bool body = !skipHeader;
      while (std::getline(is, line)) {
        if (!body) { if (line.find("--") != std::string::npos) body = true; continue; }
        std::istringstream ls(line); std::string flags, name; ls >> flags >> name;
        if (!name.empty()) {
          // muxers/demuxers can be "mp4,mov" style? ffmpeg lists single names; split on commas anyway
          size_t p = 0; while (true) { size_t c = name.find(',', p); out.insert(name.substr(p, c == std::string::npos ? c : c - p)); if (c == std::string::npos) break; p = c + 1; }
        }
      }
    };
    parse(RunCapture(FfmpegPath(), {L"-hide_banner", L"-encoders"}, 20000), g_enc, true);
    parse(RunCapture(FfmpegPath(), {L"-hide_banner", L"-decoders"}, 20000), g_dec, true);
    parse(RunCapture(FfmpegPath(), {L"-hide_banner", L"-muxers"}, 20000), g_mux, true);
    std::string v = RunCapture(FfmpegPath(), {L"-version"}, 10000);
    g_version = v.substr(0, v.find('\n'));
  });
}
std::string Version() { LoadCaps(); return g_version; }
bool HasEncoder(const std::string& n) { LoadCaps(); return g_enc.count(n) > 0; }
bool HasDecoder(const std::string& n) { LoadCaps(); return g_dec.count(n) > 0; }
bool HasMuxer(const std::string& n) { LoadCaps(); return g_mux.count(n) > 0; }

const StreamInfo* MediaInfo::FirstVideo() const { for (auto& s : streams) if (s.type == "video" && !s.attachedPic) return &s; return nullptr; }
const StreamInfo* MediaInfo::FirstAudio() const { for (auto& s : streams) if (s.type == "audio") return &s; return nullptr; }
int MediaInfo::AudioCount() const { int n = 0; for (auto& s : streams) if (s.type == "audio") ++n; return n; }
void MediaInfo::DisplaySize(int& w, int& h) const {
  w = h = 0;
  if (auto* v = FirstVideo()) { w = v->width; h = v->height; if (v->rotation == 90 || v->rotation == 270) std::swap(w, h); }
}

static double ParseRate(const std::string& s) {
  size_t sl = s.find('/');
  if (sl == std::string::npos) return atof(s.c_str());
  double n = atof(s.c_str()), d = atof(s.c_str() + sl + 1);
  return d > 0 ? n / d : 0;
}

std::optional<MediaInfo> Probe(const fs::path& file, DWORD timeoutMs) {
  if (FfprobePath().empty()) return std::nullopt;
  int code = 0;
  std::string out = RunCapture(FfprobePath(), {L"-v", L"error", L"-print_format", L"json", L"-show_format", L"-show_streams", L"--", file.wstring()}, timeoutMs, &code);
  if (code != 0 || out.empty()) return std::nullopt;
  auto j = Json::Parse(out);
  if (!j) return std::nullopt;
  MediaInfo mi; mi.raw = *j;
  const Json& fmt = j->Get("format");
  mi.duration = fmt.Get("duration").AsNumber(0);
  mi.bitRate = fmt.Get("bit_rate").AsInt(0);
  mi.formatName = fmt.Get("format_name").AsString();
  for (auto& s : j->Get("streams").Elements()) {
    StreamInfo si;
    si.index = (int)s.Get("index").AsInt();
    si.type = s.Get("codec_type").AsString();
    si.codec = s.Get("codec_name").AsString();
    si.width = (int)s.Get("width").AsInt(); si.height = (int)s.Get("height").AsInt();
    si.fps = ParseRate(s.Get("avg_frame_rate").AsString());
    if (si.fps <= 0) si.fps = ParseRate(s.Get("r_frame_rate").AsString());
    si.pixFmt = s.Get("pix_fmt").AsString();
    si.channels = (int)s.Get("channels").AsInt(); si.sampleRate = (int)s.Get("sample_rate").AsInt();
    si.bitRate = s.Get("bit_rate").AsInt(0);
    si.duration = s.Get("duration").AsNumber(0);
    si.language = s.Get("tags").Get("language").AsString();
    si.attachedPic = s.Get("disposition").Get("attached_pic").AsInt() == 1;
    // rotation: side data (display matrix) or the legacy rotate tag
    for (auto& sd : s.Get("side_data_list").Elements()) if (sd.Has("rotation")) si.rotation = (((int)std::lround(-sd.Get("rotation").AsNumber())) % 360 + 360) % 360;
    if (si.rotation == 0 && s.Get("tags").Has("rotate")) si.rotation = (((int)s.Get("tags").Get("rotate").AsInt()) % 360 + 360) % 360;
    mi.streams.push_back(std::move(si));
  }
  if (mi.duration <= 0) for (auto& s : mi.streams) mi.duration = std::max(mi.duration, s.duration);
  return mi;
}

std::wstring Num(double v, int decimals) {
  wchar_t b[64];
  swprintf_s(b, L"%.*f", decimals, v);
  // swprintf follows the C locale ('.') because the process never calls setlocale; strip trailing zeros
  std::wstring s = b;
  if (s.find(L'.') != std::wstring::npos) { while (!s.empty() && s.back() == L'0') s.pop_back(); if (!s.empty() && s.back() == L'.') s.pop_back(); }
  return s.empty() ? L"0" : s;
}

std::vector<std::wstring> HwDecodeArgs(bool allowHw) {
  // Hardware decode is only a speed-up for long videos; correctness first, so it is off unless explicitly enabled
  // and the engine decides per task. Returning {} keeps behaviour deterministic.
  (void)allowHw;
  return {};
}

static std::wstring LastLines(const std::string& err, size_t maxLines = 4) {
  std::vector<std::string> lines; std::istringstream is(err); std::string l;
  while (std::getline(is, l)) { while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back(); if (!l.empty()) lines.push_back(l); }
  std::string out;
  for (size_t i = lines.size() > maxLines ? lines.size() - maxLines : 0; i < lines.size(); ++i) { out += lines[i]; out += "\n"; }
  return Widen(out);
}

EngineResult Run(const std::vector<std::wstring>& args, double durationSec, JobContext& ctx, double p0, double p1) {
  fs::path exe = FfmpegPath();
  if (exe.empty()) return EngineResult::Fail(L"FFmpeg was not found. Reinstall Filo to restore its media components.");
  ProcessOptions po;
  po.exe = exe;
  po.args = {L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-progress", L"pipe:1", L"-y"};
  po.args.insert(po.args.end(), args.begin(), args.end());
  po.cancel = ctx.CancelFlag();
  po.onStdoutLine = [&](const std::string& line) {
    if (durationSec > 0 && line.rfind("out_time_us=", 0) == 0) {
      double t = atof(line.c_str() + 12) / 1e6;
      ctx.SetProgress(p0 + (p1 - p0) * std::min(1.0, std::max(0.0, t / durationSec)));
    }
  };
  ProcessResult r = RunProcess(po);
  if (r.cancelled) return EngineResult::Fail(L"Cancelled");
  if (!r.started) return EngineResult::Fail(L"Could not start FFmpeg: " + r.startError);
  if (r.timedOut) return EngineResult::Fail(L"FFmpeg timed out.");
  if (r.exitCode != 0) {
    FILO_LOG(Warn, "ffmpeg", "exit %lu: %s", r.exitCode, r.err.substr(0, 600).c_str());
    return EngineResult::Fail(L"The media engine could not process this file.\n" + LastLines(r.err));
  }
  ctx.SetProgress(p1);
  return EngineResult::Ok();
}

} // namespace filo::ff
