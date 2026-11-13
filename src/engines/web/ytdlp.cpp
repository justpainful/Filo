// yt-dlp runner: probe and download. yt-dlp is a separate process (Unlicense), started without a shell, killed with Filo.
#include "engines/web/web.h"
#include "engines/web/web_internal.h"
#include "engines/common/ffmpeg.h"
#include "core/log.h"
#include "core/process.h"
#include <algorithm>
#include <fstream>
#include <mutex>

namespace filo::web {

namespace {
std::mutex g_cookieMu;
CookieSource g_cookieSource = CookieSource::None;
fs::path g_cookieFile;
}  // namespace

void SetCookies(CookieSource source, const fs::path& file) {
  std::lock_guard lk(g_cookieMu);
  g_cookieSource = source;
  g_cookieFile = file;
}
CookieSource CookiesInUse() { std::lock_guard lk(g_cookieMu); return g_cookieSource; }

fs::path YtDlpPath() { return FindTool(L"yt-dlp.exe"); }
bool Available() { return !YtDlpPath().empty(); }

fs::path PluginDir() {
  const fs::path exe = ExeDir();
  for (const fs::path& c : {exe / L"tools" / L"yt-dlp-plugins", exe / L"yt-dlp-plugins"}) {
    std::error_code ec;
    if (fs::is_directory(c, ec)) return c;
  }
  // development builds: build\<cfg>\bin -> walk up to the source tree
  fs::path p = exe;
  for (int i = 0; i < 6 && p.has_parent_path(); ++i, p = p.parent_path()) {
    std::error_code ec;
    if (fs::is_directory(p / L"assets" / L"yt-dlp-plugins", ec)) return p / L"assets" / L"yt-dlp-plugins";
  }
  return {};
}

namespace {

void PrepareEnvironment() {
  static std::once_flag once;
  std::call_once(once, [] {
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");     // titles in any script reach us intact
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"NO_COLOR", L"1");
  });
}

std::vector<std::wstring> CommonArgs() {
  std::vector<std::wstring> a = {L"--ignore-config", L"--no-colors", L"--no-warnings", L"--socket-timeout", L"25", L"--retries", L"4",
                                 L"--extractor-retries", L"3"};
  if (const fs::path plugins = PluginDir(); !plugins.empty()) { a.push_back(L"--plugin-dirs"); a.push_back(plugins.wstring()); }
  // YouTube needs a JavaScript runtime to unlock its best formats; use Node when it is installed.
  if (const fs::path node = FindTool(L"node.exe"); !node.empty()) { a.push_back(L"--js-runtimes"); a.push_back(L"node:" + node.wstring()); }
  if (const fs::path ffmpeg = ff::FfmpegPath(); !ffmpeg.empty()) { a.push_back(L"--ffmpeg-location"); a.push_back(ffmpeg.parent_path().wstring()); }
  {
    std::lock_guard lk(g_cookieMu);
    std::error_code ec;
    switch (g_cookieSource) {
      case CookieSource::Firefox: a.insert(a.end(), {L"--cookies-from-browser", L"firefox"}); break;
      case CookieSource::Chrome: a.insert(a.end(), {L"--cookies-from-browser", L"chrome"}); break;
      case CookieSource::Edge: a.insert(a.end(), {L"--cookies-from-browser", L"edge"}); break;
      case CookieSource::File: if (!g_cookieFile.empty() && fs::is_regular_file(g_cookieFile, ec)) a.insert(a.end(), {L"--cookies", g_cookieFile.wstring()}); break;
      default: break;
    }
  }
  return a;
}

// "ERROR: [TikTok] 123: Unable to extract..." -> a sentence for people
std::wstring FriendlyError(const std::string& err, const std::wstring& startError) {
  if (!startError.empty()) return L"Filo could not start its downloader: " + startError;
  std::string line;
  size_t pos = 0;
  while (pos < err.size()) {
    size_t e = err.find('\n', pos);
    std::string l = err.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    if (l.rfind("ERROR:", 0) == 0) line = l.substr(6);
    pos = e == std::string::npos ? err.size() : e + 1;
  }
  while (!line.empty() && (line[0] == ' ')) line.erase(0, 1);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  // drop "[extractor] id:" prefixes
  while (line.rfind('[', 0) == 0) { size_t c = line.find(']'); if (c == std::string::npos) break; line.erase(0, c + 1); while (!line.empty() && line[0] == ' ') line.erase(0, 1); }
  { size_t c = line.find(": "); if (c != std::string::npos && c < 24 && line.find(' ') > c) line.erase(0, c + 2); }
  std::string low = line; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
  if (low.find("unsupported url") != std::string::npos) return L"This link is not supported.";
  if (low.find("cookie") != std::string::npos && (low.find("database") != std::string::npos || low.find("decrypt") != std::string::npos || low.find("could not") != std::string::npos))
    return L"Filo could not read the browser's saved sign-in. Close that browser and try again, or choose Firefox or a cookies file.";
  if (low.find("private") != std::string::npos) return L"This post is private.";
  if (low.find("sign in") != std::string::npos || low.find("log in") != std::string::npos || low.find("login") != std::string::npos)
    return L"This post needs a signed-in account, so it cannot be downloaded.";
  if (low.find("http error 429") != std::string::npos || low.find("too many requests") != std::string::npos) return L"The site is limiting requests right now. Try again in a few minutes.";
  if (low.find("http error 404") != std::string::npos || low.find("not available") != std::string::npos || low.find("removed") != std::string::npos || low.find("does not exist") != std::string::npos)
    return L"This post is not available (it may have been removed).";
  if (low.find("urlopen error") != std::string::npos || low.find("getaddrinfo") != std::string::npos || low.find("timed out") != std::string::npos)
    return L"The site could not be reached. Check the internet connection.";
  if (line.empty()) return L"The link could not be read.";
  if (line.size() > 220) line.resize(220);
  return Widen(line);
}

// One yt-dlp download of a single item. Returns the finished files.
struct RunResult { bool ok = false; bool cancelled = false; std::wstring error; std::vector<fs::path> files; };

RunResult RunDownload(const LinkInfo& info, int playlistIndex, const std::vector<std::wstring>& formatArgs, const fs::path& outTemplate,
                      const DownloadSink& sink, double lo, double hi, int expectedStreams) {
  RunResult rr;
  std::vector<std::wstring> a = CommonArgs();
  a.insert(a.end(), {info.isPlaylist ? L"--yes-playlist" : L"--no-playlist", L"--newline", L"--progress", L"--no-mtime", L"--no-overwrites",
                     L"--progress-template", L"download:FILO_PROG|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s",
                     L"--print", L"after_move:FILO_OUT|%(filepath)s", L"-o", outTemplate.wstring()});
  if (info.isPlaylist && playlistIndex > 0) { a.push_back(L"--playlist-items"); a.push_back(std::to_wstring(playlistIndex)); }
  a.insert(a.end(), formatArgs.begin(), formatArgs.end());
  a.push_back(L"--");
  a.push_back(Widen(info.pageUrl));

  PrepareEnvironment();
  ProcessOptions po;
  po.exe = YtDlpPath();
  po.args = std::move(a);
  po.cancel = sink.cancel;
  po.lowPriority = false;
  po.timeoutMs = 3 * 60 * 60 * 1000;
  int stream = 0;
  double last = 0;
  po.onStdoutLine = [&](const std::string& line) {
    if (line.rfind("FILO_PROG|", 0) == 0) {
      // downloaded|total|estimate; yt-dlp prints "NA" for unknown values, which atof reads as 0
      const char* p1 = strchr(line.c_str() + 10, '|');
      const char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
      const double d = atof(line.c_str() + 10), t = p1 ? atof(p1 + 1) : 0, e = p2 ? atof(p2 + 1) : 0;
      const double total = t > 0 ? t : e;
      if (total > 0) {
        if (d + 1024 < last) ++stream;                              // a new stream started (audio after video)
        last = d;
        const double unit = std::min(1.0, (stream + std::min(1.0, d / total)) / std::max(1, expectedStreams));
        if (sink.progress) sink.progress(lo + (hi - lo) * unit);
      }
    } else if (line.rfind("FILO_OUT|", 0) == 0) {
      fs::path p(Widen(line.substr(9)));
      std::error_code ec;
      if (fs::exists(p, ec)) rr.files.push_back(p);
    }
  };
  std::string errText;
  ProcessResult pr = RunProcess(po);
  errText = pr.err;
  if (pr.cancelled) { rr.cancelled = true; rr.error = L"Cancelled."; return rr; }
  if (rr.files.empty() || !pr.Ok()) {
    rr.error = FriendlyError(errText, pr.startError);
    if (pr.Ok() && rr.files.empty()) rr.error = L"Nothing was downloaded.";
    return rr;
  }
  if (sink.progress) sink.progress(hi);
  rr.ok = true;
  return rr;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ probe
static LinkInfo ProbeYtDlp(const std::wstring& url, const std::atomic<bool>* cancel) {
  LinkInfo bad;
  bad.sourceUrl = url;
  bad.platform = DetectPlatform(url);
  if (!Available()) { bad.error = L"The downloader (yt-dlp) is missing from this Filo installation."; return bad; }
  PrepareEnvironment();
  ProcessOptions po;
  po.exe = YtDlpPath();
  po.args = CommonArgs();
  po.args.insert(po.args.end(), {L"-J", L"--no-playlist", L"--flat-playlist", L"--playlist-end", L"80", L"--", url});
  po.cancel = cancel;
  po.lowPriority = false;
  po.timeoutMs = 120000;
  po.captureLimit = 96u << 20;
  ProcessResult pr = RunProcess(po);
  if (pr.cancelled) { bad.error = L"Cancelled."; return bad; }
  if (pr.timedOut) { bad.error = L"The site took too long to answer."; return bad; }
  if (!pr.Ok() || pr.out.empty()) { bad.error = FriendlyError(pr.err, pr.startError); return bad; }
  std::string jerr;
  auto json = Json::Parse(pr.out, &jerr);
  if (!json) { bad.error = L"The downloader returned something Filo could not read."; FILO_LOG(Warn, "web", "probe JSON: %s", jerr.c_str()); return bad; }
  // Flat playlist entries are unresolved links. Resolve up to 80 entries only when needed;
  // direct image entries produced by Filo's photo plugin are already complete.
  bool needsResolution = false;
  for (const Json& e : json->Get("entries").Elements()) {
    const std::string type = e.Get("_type").StringOr("");
    if ((type == "url" || type == "url_transparent") && e.Get("_filo_kind").StringOr("") != "image") needsResolution = true;
  }
  if (needsResolution) {
    po.args = CommonArgs();
    po.args.insert(po.args.end(), {L"-J", L"--yes-playlist", L"--no-flat-playlist", L"--playlist-end", L"80", L"--", url});
    pr = RunProcess(po);
    if (pr.cancelled) { bad.error = L"Cancelled."; return bad; }
    if (pr.timedOut) { bad.error = L"The site took too long to answer."; return bad; }
    if (!pr.Ok() || pr.out.empty()) { bad.error = FriendlyError(pr.err, pr.startError); return bad; }
    json = Json::Parse(pr.out, &jerr);
    if (!json) { bad.error = L"The downloader returned something Filo could not read."; return bad; }
  }
  return ParseProbe(*json, url);
}

LinkInfo Probe(const std::wstring& url, const std::atomic<bool>* cancel) {
  LinkInfo li = ProbeYtDlp(url, cancel);
  // Instagram: yt-dlp only knows video posts and wants a sign-in for most of them; gallery-dl also reads photos and carousels.
  if (!li.ok && li.platform.id == "instagram" && GalleryDlAvailable() && !(cancel && cancel->load())) {
    LinkInfo g = internal::ProbeGallery(url, cancel);
    if (g.ok || !g.error.empty()) return g;
  }
  return li;
}

// ------------------------------------------------------------------------------------------------ download
namespace {

struct Produced {
  fs::path path;
  ItemKind kind;
  int itemIndex;       // index into info.items
  int height;          // for names ("1080p"); 0 = unknown
  bool audioOfVideo;
};

std::vector<std::wstring> VideoFormatArgs(Quality q, VideoContainer container) {
  if (container == VideoContainer::Webm)
    return {L"-f", L"bv*[ext=webm]+ba[ext=webm]/b[ext=webm]", L"-S", L"res,fps,br", L"--merge-output-format", L"webm", L"--remux-video", L"webm"};
  if (container == VideoContainer::Mp4)
    return {L"-f", q == Quality::Compatible ? L"bv*[vcodec^=avc1][ext=mp4]+ba[ext=m4a]/b[vcodec^=avc1][ext=mp4]" : L"bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]", L"-S", L"res,fps,br", L"--merge-output-format", L"mp4", L"--remux-video", L"mp4"};
  if (container == VideoContainer::Mkv)
    return {L"-f", q == Quality::Compatible ? L"bv*[vcodec^=avc1]+ba[ext=m4a]/b[ext=mp4]/bv*+ba/b" : L"bv*+ba/b", L"-S", L"res,fps,br", L"--merge-output-format", L"mkv", L"--remux-video", L"mkv"};
  if (q == Quality::Compatible)
    return {L"-f", L"bv*[vcodec^=avc1]+ba[ext=m4a]/b[ext=mp4]/bv*+ba/b", L"-S", L"res,fps,br", L"--merge-output-format", L"mp4"};
  // resolution first, then frame rate, then bitrate; yt-dlp keeps the codecs the site serves and picks a container that can hold them
  return {L"-f", L"bv*+ba/b", L"-S", L"res,fps,br", L"--merge-output-format", L"mp4/mkv"};
}

std::wstring Lower(std::wstring s) { return ToLower(s); }

std::vector<std::wstring> AudioFormatArgs(AudioFormat format) {
  std::vector<std::wstring> args = {L"-f", L"ba/b"};
  if (format != AudioFormat::Original) {
    args.insert(args.end(), {L"-x", L"--audio-format", format == AudioFormat::Mp3 ? L"mp3" : format == AudioFormat::M4a ? L"m4a" : L"wav"});
  }
  return args;
}

}  // namespace

JobResult Download(const DownloadRequest& req, JobContext& ctx) {
  DownloadSink sink;
  sink.cancel = ctx.CancelFlag();
  sink.progress = [&ctx](double p) { ctx.SetProgress(p); };
  sink.stage = [&ctx](JobState st, const std::wstring& text) { ctx.SetStage(st, text); };
  sink.workDir = ctx.TempDir() / L"dl";
  return DownloadPost(req, sink);
}

JobResult DownloadPost(const DownloadRequest& req, const DownloadSink& sink) {
  const LinkInfo& info = req.info;
  const bool textWanted = req.saveText && info.HasText();
  if (req.items.empty() && !req.wantAudio && !textWanted) return JobResult::Fail(L"Nothing is selected.");
  std::error_code ec;
  fs::create_directories(req.destRoot, ec);
  if (!fs::is_directory(req.destRoot, ec)) return JobResult::Fail(L"The download folder cannot be used: " + req.destRoot.wstring());

  std::vector<int> videos, direct, tracks;
  for (int i : req.items) {
    if (i < 0 || i >= (int)info.items.size()) continue;
    (info.items[i].kind == ItemKind::Video && !info.items[i].direct ? videos : info.items[i].useYtDlp ? tracks : direct).push_back(i);
  }
  std::vector<int> audioOf;
  if (req.wantAudio) {
    audioOf = videos;
    if (audioOf.empty()) for (int i = 0; i < (int)info.items.size(); ++i) if (info.items[i].kind == ItemKind::Video) audioOf.push_back(i);
  }
  const int units = (int)videos.size() + (int)audioOf.size() + (int)direct.size() + (int)tracks.size();
  if (units == 0 && !textWanted) return JobResult::Fail(L"Nothing is selected.");
  if ((!videos.empty() || !audioOf.empty() || !tracks.empty()) && !Available()) return JobResult::Fail(L"The downloader (yt-dlp) is missing from this Filo installation.");

  if (sink.workDir.empty()) return JobResult::Fail(L"No scratch folder for the download.");
  const fs::path work = sink.workDir;
  fs::create_directories(work, ec);
  std::vector<Produced> produced;
  int done = 0;
  auto window = [&](double& lo, double& hi) { lo = (double)done / std::max(1, units) * 0.94; hi = (double)(done + 1) / std::max(1, units) * 0.94; };
  auto stage = [&](JobState st, const wchar_t* text) { if (sink.stage) sink.stage(st, text); };
  auto progress = [&](double p) { if (sink.progress) sink.progress(p); };
  stage(JobState::Running, L"Downloading");

  // ---- videos (video + audio streams merged by FFmpeg)
  for (int i : videos) {
    double lo, hi; window(lo, hi);
    RunResult rr = RunDownload(info, info.items[i].index, VideoFormatArgs(req.quality, req.videoContainer), work / (L"v" + std::to_wstring(i) + L".%(ext)s"), sink, lo, hi, 2);
    if (rr.cancelled) return JobResult::Fail(L"Cancelled.");
    if (!rr.ok) return JobResult::Fail(rr.error);
    for (auto& f : rr.files) produced.push_back({f, ItemKind::Video, i, info.items[i].height, false});
    ++done;
  }
  // ---- audio tracks, original codec (no re-encoding)
  for (int i : audioOf) {
    double lo, hi; window(lo, hi);
    RunResult rr = RunDownload(info, info.items[i].index, AudioFormatArgs(req.audioFormat), work / (L"a" + std::to_wstring(i) + L".%(ext)s"), sink, lo, hi, 1);
    if (rr.cancelled) return JobResult::Fail(L"Cancelled.");
    if (!rr.ok) return JobResult::Fail(rr.error);
    for (auto& f : rr.files) produced.push_back({f, ItemKind::Audio, i, 0, true});
    ++done;
  }
  for (int i : tracks) {
    double lo, hi; window(lo, hi);
    RunResult rr = RunDownload(info, info.items[i].index, AudioFormatArgs(req.audioFormat), work / (L"t" + std::to_wstring(i) + L".%(ext)s"), sink, lo, hi, 1);
    if (rr.cancelled) return JobResult::Fail(L"Cancelled.");
    if (!rr.ok) return JobResult::Fail(rr.error);
    for (auto& f : rr.files) produced.push_back({f, ItemKind::Audio, i, 0, false});
    ++done;
  }
  // ---- photos and slideshow sound: plain downloads of the original files
  for (int i : direct) {
    const Item& it = info.items[i];
    double lo, hi; window(lo, hi);
    if (it.url.empty()) return JobResult::Fail(L"A picture of this post has no address.");
    HttpOptions ho;
    ho.cancel = sink.cancel;
    ho.headers = it.headers;
    ho.onProgress = [&](uint64_t d, uint64_t t) { if (t) progress(lo + (hi - lo) * std::min(1.0, (double)d / (double)t)); };
    const fs::path file = work / (L"d" + std::to_wstring(i) + L"." + Widen(it.ext.empty() ? "bin" : it.ext));
    HttpResult hr = HttpGetFile(Widen(it.url), file, ho);
    if (hr.cancelled) return JobResult::Fail(L"Cancelled.");
    if (!hr.ok) return JobResult::Fail(L"Could not download " + (it.kind == ItemKind::Image ? std::wstring(L"picture ") + std::to_wstring(it.index ? it.index : 1) : it.kind == ItemKind::Video ? std::wstring(L"the video") : std::wstring(L"the sound")) + L": " + hr.error);
    fs::path finished = file;
    if (it.kind == ItemKind::Audio && req.audioFormat != AudioFormat::Original) {
      const fs::path ffmpeg = ff::FfmpegPath();
      if (ffmpeg.empty()) return JobResult::Fail(L"FFmpeg is required to convert audio.");
      const wchar_t* ext = req.audioFormat == AudioFormat::Mp3 ? L"mp3" : req.audioFormat == AudioFormat::M4a ? L"m4a" : L"wav";
      finished = work / (L"converted" + std::to_wstring(i) + L"." + ext);
      ProcessOptions convert;
      convert.exe = ffmpeg;
      convert.args = {L"-nostdin", L"-y", L"-i", file.wstring(), L"-vn", finished.wstring()};
      convert.cancel = sink.cancel;
      convert.timeoutMs = 3 * 60 * 60 * 1000;
      ProcessResult result = RunProcess(convert);
      if (result.cancelled) return JobResult::Fail(L"Cancelled.");
      if (!result.Ok() || !fs::exists(finished, ec)) return JobResult::Fail(L"Could not convert the sound to the selected format.");
    }
    produced.push_back({finished, it.kind, i, it.height, false});
    ++done;
  }
  if (produced.empty() && !textWanted) return JobResult::Fail(L"Nothing was downloaded.");

  // ---- naming and placement
  stage(JobState::Finalizing, L"Saving");
  const std::wstring base = BaseName(info);
  const bool folder = req.folderThreshold > 0 && (int)produced.size() >= req.folderThreshold;
  fs::path targetDir = req.destRoot;
  if (folder) {
    targetDir = UniquePath(req.destRoot / base);
    fs::create_directories(targetDir, ec);
    if (!fs::is_directory(targetDir, ec)) return JobResult::Fail(L"Could not create the folder " + targetDir.wstring());
  }
  int videoCount = (int)std::count_if(produced.begin(), produced.end(), [](const Produced& p) { return p.kind == ItemKind::Video; });
  int imageNo = 0, videoNo = 0;
  std::vector<fs::path> outputs;
  for (const Produced& p : produced) {
    std::wstring stem = base;
    const std::wstring ext = Lower(p.path.extension().wstring());
    if (p.kind == ItemKind::Image) {
      wchar_t n[16]; swprintf_s(n, L"_%02d", ++imageNo);
      stem += n;
    } else if (p.kind == ItemKind::Audio) {
      stem += (info.items[p.itemIndex].kind == ItemKind::Audio && !p.audioOfVideo) ? L"_sound" : L"_audio";
      if (!p.audioOfVideo && info.CountOf(ItemKind::Audio) > 1 && info.items[p.itemIndex].index) stem += L"_" + std::to_wstring(info.items[p.itemIndex].index);
      if (p.audioOfVideo && videoCount + (int)audioOf.size() > 1 && info.items[p.itemIndex].index) stem += L"_" + std::to_wstring(info.items[p.itemIndex].index);
    } else {
      ++videoNo;
      if (videoCount > 1) stem += L"_v" + std::to_wstring(videoNo);
      if (p.height > 0) stem += L"_" + std::to_wstring(p.height) + L"p";
    }
    const fs::path dest = UniquePath(targetDir / (stem + ext));
    if (!MoveFileExW(p.path.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
      return JobResult::Fail(L"Could not save " + dest.filename().wstring() + L": " + LastErrorMessage());
    outputs.push_back(dest);
  }
  if (textWanted) {
    const fs::path dest = UniquePath(targetDir / (base + L".txt"));
    const std::wstring text = FormatPostText(info);
    const std::string utf8 = Narrow(text);
    std::ofstream f(dest, std::ios::binary);
    if (f) { static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF}; f.write(reinterpret_cast<const char*>(bom), 3); f.write(utf8.data(), (std::streamsize)utf8.size()); f.close(); outputs.push_back(dest); }
  }
  progress(1.0);
  return JobResult::Ok(folder ? std::vector<fs::path>{targetDir} : outputs);
}

namespace internal {
std::vector<std::wstring> BaseArgs() { return CommonArgs(); }
std::wstring ErrorText(const std::string& stderrText, const std::wstring& startError) { return FriendlyError(stderrText, startError); }
void PrepareEnvironment() { web::PrepareEnvironment(); }
CookieChoice CurrentCookies() { std::lock_guard lk(g_cookieMu); return {g_cookieSource, g_cookieFile}; }
}  // namespace internal

}  // namespace filo::web
