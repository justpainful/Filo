#include "media_test_util.h"
#include "engines/engines.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

namespace mt {

static fs::path FindRoot() {
  fs::path p = ExeDir();
  for (int i = 0; i < 8; ++i) { if (fs::exists(p / L"docs" / L"ENGINE-CONTRACT.md")) return p; p = p.parent_path(); }
  return fs::current_path();
}
fs::path Root() { static fs::path r = FindRoot(); return r; }
fs::path Work() { fs::path w = Root() / L"testdata" / L"media" / L"work"; std::error_code ec; fs::create_directories(w, ec); return w; }
fs::path OutDir(const std::string& name) {
  fs::path d = Work() / Widen(name);
  std::error_code ec; fs::remove_all(fs::path(std::wstring(1, 92) + std::wstring(1, 92) + L"?" + std::wstring(1, 92) + d.wstring()), ec); fs::remove_all(d, ec); fs::create_directories(d, ec);   // the prefixed form also removes very long paths
  return d;
}
std::wstring W(const std::string& s) { return Widen(s); }

static Args ToArgs(const std::vector<std::string>& a) { Args o; for (auto& s : a) o.push_back(Widen(s)); return o; }

bool Ff(const std::vector<std::string>& args, std::string* err) {
  ProcessOptions po; po.exe = ff::FfmpegPath();
  po.args = {L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y"};
  for (auto& a : ToArgs(args)) po.args.push_back(a);
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  if (err) *err = r.err;
  return r.Ok() && r.err.empty();
}
std::string FfOutput(const std::vector<std::string>& args, bool useStderr, int* code) {
  ProcessOptions po; po.exe = ff::FfmpegPath();
  po.args = {L"-nostdin", L"-hide_banner", L"-nostats", L"-y"};
  for (auto& a : ToArgs(args)) po.args.push_back(a);
  po.lowPriority = false; po.captureLimit = 256u << 20;
  ProcessResult r = RunProcess(po);
  if (code) *code = (int)r.exitCode;
  return useStderr ? r.err : r.out;
}
bool NullDecode(const fs::path& p, std::string* err) {
  ProcessOptions po; po.exe = ff::FfmpegPath();
  po.args = {L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-i", p.wstring(), L"-map", L"0:v?", L"-map", L"0:a?", L"-f", L"null", L"-"};
  po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  if (err) *err = r.err;
  return r.Ok() && r.err.empty();
}
std::string Probe1(const fs::path& p, const std::vector<std::wstring>& extra) {
  std::vector<std::wstring> a = {L"-v", L"error"};
  a.insert(a.end(), extra.begin(), extra.end());
  a.push_back(L"-of"); a.push_back(L"csv=p=0");
  a.push_back(p.wstring());
  std::string o = RunCapture(ff::FfprobePath(), a, 120000);
  while (!o.empty() && (o.back() == '\n' || o.back() == '\r')) o.pop_back();
  return o;
}
int CountVideoFrames(const fs::path& p) {
  return atoi(Probe1(p, {L"-count_frames", L"-select_streams", L"v:0", L"-show_entries", L"stream=nb_read_frames"}).c_str());
}
std::string PcmMd5(const fs::path& p, const std::string& af) {
  std::vector<std::string> a = {"-i", Narrow(p.wstring()), "-map", "0:a:0"};
  if (!af.empty()) { a.push_back("-af"); a.push_back(af); }
  a.insert(a.end(), {"-c:a", "pcm_s16le", "-f", "md5", "-"});
  std::string o = FfOutput(a);
  size_t k = o.find("MD5=");
  return k == std::string::npos ? "" : o.substr(k + 4, 32);
}
std::vector<float> DecodeMonoF32(const fs::path& p, int rate, const std::string& af, double start, double dur) {
  std::vector<std::string> a;
  if (start >= 0) { a.push_back("-ss"); a.push_back(std::to_string(start)); }
  a.insert(a.end(), {"-i", Narrow(p.wstring()), "-map", "0:a:0"});
  if (dur >= 0) { a.push_back("-t"); a.push_back(std::to_string(dur)); }
  std::string f = af.empty() ? "" : af + ",";
  a.insert(a.end(), {"-af", f + "aformat=channel_layouts=mono", "-ar", std::to_string(rate), "-f", "f32le", "pipe:1"});
  std::string o = FfOutput(a);
  std::vector<float> v(o.size() / 4);
  if (!v.empty()) memcpy(v.data(), o.data(), v.size() * 4);
  return v;
}
double Rms(const std::vector<float>& v, size_t a, size_t b) {
  b = std::min(b, v.size()); if (a >= b) return 0;
  double s = 0; for (size_t i = a; i < b; ++i) s += (double)v[i] * v[i];
  return std::sqrt(s / (b - a));
}
double ChannelFreq(const fs::path& p, int ch) {
  std::string af = "pan=mono|c0=c" + std::to_string(ch);
  std::vector<std::string> a = {"-i", Narrow(p.wstring()), "-map", "0:a:0", "-af", af, "-ar", "16000", "-t", "2", "-f", "f32le", "pipe:1"};
  std::string o = FfOutput(a);
  size_t n = o.size() / 4; if (n < 100) return 0;
  const float* d = (const float*)o.data(); size_t zc = 0;
  for (size_t i = 1; i < n; ++i) if ((d[i - 1] < 0) != (d[i] < 0)) ++zc;
  return zc / 2.0 / (n / 16000.0);
}
std::optional<ff::MediaInfo> Pr(const fs::path& p) { return ff::Probe(p); }

// ---- python / PIL pixel helpers ---------------------------------------------------------------------------------------------
static fs::path PixScript() {
  fs::path s = Work() / L"pix.py";
  std::ofstream f(s, std::ios::binary);
  f << "import sys\nfrom PIL import Image\nimport numpy as np\n"
       "m=sys.argv[1]\n"
       "def load(p): return np.asarray(Image.open(p).convert('RGB')).astype(float)\n"
       "if m=='mean':\n"
       "  a=load(sys.argv[2]); x,y,w,h=map(int,sys.argv[3:7]); r=a[y:y+h,x:x+w].reshape(-1,3).mean(axis=0); print(r[0],r[1],r[2])\n"
       "elif m=='diff':\n"
       "  a=load(sys.argv[2]); b=load(sys.argv[3]); x,y,w,h=map(int,sys.argv[4:8]); print(abs(a[y:y+h,x:x+w]-b[y:y+h,x:x+w]).mean())\n"
       "elif m=='size':\n"
       "  im=Image.open(sys.argv[2]); print(im.size[0],im.size[1])\n"
       "elif m=='diffoff':\n"
       "  a=load(sys.argv[2]); b=load(sys.argv[3]); ax,ay,bx,by,w,h=map(int,sys.argv[4:10]); print(abs(a[ay:ay+h,ax:ax+w]-b[by:by+h,bx:bx+w]).mean())\n"
       "elif m=='count':\n"
       "  a=load(sys.argv[2]); x,y,w,h,r,g,b,t=map(int,sys.argv[3:11]); reg=a[y:y+h,x:x+w]; d=abs(reg-np.array([r,g,b])).sum(axis=2); print(int((d<=t).sum()))\n";
  return s;
}
static std::string Python(const std::vector<std::wstring>& args) {
  ProcessOptions po; po.exe = FindTool(L"python.exe"); po.args = {L"-I", PixScript().wstring()};
  po.args.insert(po.args.end(), args.begin(), args.end()); po.lowPriority = false;
  ProcessResult r = RunProcess(po);
  return r.Ok() ? r.out : std::string();
}
bool FramePng(const fs::path& v, double t, const fs::path& png) {
  return Ff({"-ss", std::to_string(t), "-i", Narrow(v.wstring()), "-frames:v", "1", Narrow(png.wstring())}) && fs::exists(png);
}
Rgb MeanRgb(const fs::path& png, int x, int y, int w, int h) {
  std::string o = Python({L"mean", png.wstring(), std::to_wstring(x), std::to_wstring(y), std::to_wstring(w), std::to_wstring(h)});
  Rgb r; if (sscanf(o.c_str(), "%lf %lf %lf", &r.r, &r.g, &r.b) == 3) r.ok = true; return r;
}
double PngDiff(const fs::path& a, const fs::path& b, int x, int y, int w, int h) {
  std::string o = Python({L"diff", a.wstring(), b.wstring(), std::to_wstring(x), std::to_wstring(y), std::to_wstring(w), std::to_wstring(h)});
  return o.empty() ? -1 : atof(o.c_str());
}

double PngDiffOffset(const fs::path& a, int ax, int ay, const fs::path& b, int bx, int by, int w, int h) {
  std::string o = Python({L"diffoff", a.wstring(), b.wstring(), std::to_wstring(ax), std::to_wstring(ay), std::to_wstring(bx), std::to_wstring(by), std::to_wstring(w), std::to_wstring(h)});
  return o.empty() ? -1 : atof(o.c_str());
}
int PngCountNear(const fs::path& png, int x, int y, int w, int h, int r, int g, int b, int tol) {
  std::string o = Python({L"count", png.wstring(), std::to_wstring(x), std::to_wstring(y), std::to_wstring(w), std::to_wstring(h), std::to_wstring(r), std::to_wstring(g), std::to_wstring(b), std::to_wstring(tol)});
  return o.empty() ? -1 : atoi(o.c_str());
}
bool PngSize(const fs::path& png, int& w, int& h) {
  std::string o = Python({L"size", png.wstring()});
  return sscanf(o.c_str(), "%d %d", &w, &h) == 2;
}
uint64_t FileHash(const fs::path& p) {
  HANDLE f = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  uint64_t h = 1469598103934665603ull; std::vector<uint8_t> buf(1 << 20); DWORD n;
  while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) for (DWORD i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ull; }
  CloseHandle(f);
  return h;
}
double MeasureLufs(const fs::path& p) {
  std::string err = FfOutput({"-i", Narrow(p.wstring()), "-map", "0:a:0", "-af", "ebur128=peak=true", "-f", "null", "-"}, true);
  size_t k = err.rfind("Integrated loudness:");
  if (k == std::string::npos) return -999;
  size_t i = err.find("I:", k);
  return i == std::string::npos ? -999 : atof(err.c_str() + i + 2);
}
std::vector<fs::path> Leftovers(const fs::path& dir) {
  std::vector<fs::path> v; std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  while (!ec && it != end) {
    if (it->path().filename().wstring().rfind(L".filo-", 0) == 0) v.push_back(it->path());
    it.increment(ec);
  }
  return v;
}

// ---- fixtures -----------------------------------------------------------------------------------------------------------------
void Register() { static std::once_flag f; std::call_once(f, [] { RegisterMediaEngine(); }); }
static std::string S(const fs::path& p) { return Narrow(p.wstring()); }

const Fx& Fixtures() {
  static Fx fx;
  static bool done = false;
  if (done) return fx;
  done = true;
  Register();
  fs::path d = Work() / L"fx";
  std::error_code ec; fs::create_directories(d, ec);
  auto gen = [&](const char* name, std::vector<std::string> args, const char* tag = "") -> fs::path {
    fs::path out = d / Widen(name);
    std::error_code e2;
    if (fs::exists(out, e2) && fs::file_size(out, e2) > 0) return out;
    std::vector<std::string> a = std::move(args);
    a.push_back(S(out));
    std::string err;
    if (!Ff(a, &err)) { std::printf("  fixture %s failed: %s\n", name, err.substr(0, 300).c_str()); ++ftest::Failures(); fs::remove(out, e2); }
    (void)tag;
    return out;
  };
  auto vsrc = [](const char* size, int fps, double dur) { return std::vector<std::string>{"-f", "lavfi", "-i", std::string("testsrc2=s=") + size + ":r=" + std::to_string(fps) + ":d=" + std::to_string(dur)}; };
  auto cat = [](std::vector<std::string> a, std::vector<std::string> b) { a.insert(a.end(), b.begin(), b.end()); return a; };

  fx.baseWav = gen("base.wav", {"-f", "lavfi", "-i", "sine=f=440:r=44100:d=3", "-f", "lavfi", "-i", "sine=f=880:r=44100:d=3", "-filter_complex",
                                "[0:a][1:a]amerge=inputs=2,volume=4,aformat=channel_layouts=stereo[a]", "-map", "[a]", "-c:a", "pcm_s16le"});
  auto withBase = [&](std::vector<std::string> codecs) { return cat(cat(vsrc("320x240", 25, 3), {"-i", S(fx.baseWav), "-shortest"}), codecs); };
  fx.mp4 = gen("base.mp4", withBase({"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k"}));
  fx.mov = gen("base.mov", withBase({"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k"}));
  fx.mkv = gen("base.mkv", withBase({"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k"}));
  fx.webm = gen("base.webm", withBase({"-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-b:v", "0", "-crf", "36", "-pix_fmt", "yuv420p", "-c:a", "libopus", "-b:a", "96k"}));
  fx.avi = gen("base.avi", withBase({"-c:v", "mpeg4", "-q:v", "4", "-vtag", "xvid", "-pix_fmt", "yuv420p", "-c:a", "libmp3lame", "-b:a", "128k"}));
  fx.wmv = gen("base.wmv", withBase({"-c:v", "wmv2", "-q:v", "4", "-pix_fmt", "yuv420p", "-c:a", "wmav2", "-b:a", "128k"}));
  fx.gif = gen("base.gif", cat(vsrc("160x120", 10, 2), {"-vf", "split[a][b];[a]palettegen[p];[b][p]paletteuse", "-loop", "0"}));

  auto aud = [&](const char* name, std::vector<std::string> codecs) { return gen(name, cat({"-i", S(fx.baseWav), "-map", "0:a"}, codecs)); };
  fx.wav = aud("base_copy.wav", {"-c:a", "pcm_s16le"});
  fx.mp3 = aud("base.mp3", {"-c:a", "libmp3lame", "-b:a", "192k"});
  fx.m4a = aud("base.m4a", {"-c:a", "aac", "-b:a", "160k"});
  fx.flac = aud("base.flac", {"-c:a", "flac"});
  fx.ogg = aud("base.ogg", {"-c:a", "libvorbis", "-b:a", "160k"});
  fx.opus = aud("base.opus", {"-c:a", "libopus", "-b:a", "128k"});
  fx.aiff = aud("base.aiff", {"-c:a", "pcm_s16be"});
  fx.wma = aud("base.wma", {"-c:a", "wmav2", "-b:a", "128k"});

  fx.portrait = gen("portrait169.mp4", {"-display_rotation:v:0", "90", "-i",
                                        S(gen("land169.mp4", cat(cat(vsrc("320x180", 25, 3), {"-i", S(fx.baseWav), "-shortest"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k"}))),
                                        "-c", "copy"});
  fx.vfr = gen("vfr.mp4", {"-f", "lavfi", "-i", "testsrc2=s=320x240:r=30:d=3", "-f", "lavfi", "-i", "sine=f=500:r=44100:d=4.5", "-vf",
                           "setpts='if(lt(N,45),N/30,(45+(N-45)*2)/30)/TB'", "-fps_mode", "passthrough", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
  fx.noAudio = gen("noaudio.mp4", cat(vsrc("320x240", 25, 3), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-an"}));
  fx.odd = gen("odd853x479.mkv", cat(cat(vsrc("854x480", 10, 2), {"-i", S(fx.baseWav), "-shortest", "-vf", "crop=853:479:0:0:exact=1"}), {"-c:v", "ffv1", "-level", "3", "-pix_fmt", "yuv420p", "-c:a", "aac"}));   // x264 cannot store odd sizes, FFV1 can
  fx.twoAudio = gen("twoaudio.mkv", cat(cat(vsrc("320x240", 25, 3), {"-f", "lavfi", "-i", "sine=f=440:d=3", "-f", "lavfi", "-i", "sine=f=880:d=3", "-map", "0:v", "-map", "1:a", "-map", "2:a"}),
                                        {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
  {
    fs::path srt = d / L"sub.srt";
    std::ofstream(srt, std::ios::binary) << "1\n00:00:00,500 --> 00:00:02,000\nHello Filo\n\n2\n00:00:02,100 --> 00:00:02,900\nSecond line\n";
    fx.subMkv = gen("sub.mkv", {"-i", S(fx.twoAudio), "-i", S(srt), "-map", "0", "-map", "1", "-c", "copy", "-c:s", "srt"});
  }
  fx.tiny = gen("tiny.mp4", cat(cat(vsrc("64x64", 25, 0.5), {"-f", "lavfi", "-i", "sine=f=300:d=0.5"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
  fx.long30 = gen("long30.mp4", cat(cat(vsrc("320x240", 25, 30), {"-f", "lavfi", "-i", "sine=f=330:d=30"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
  {
    fs::path cover = d / L"cover.jpg";
    if (!fs::exists(cover)) Ff({"-f", "lavfi", "-i", "testsrc2=s=200x200:d=1", "-frames:v", "1", S(cover)});
    fx.coverMp3 = gen("cover.mp3", {"-i", S(fx.baseWav), "-i", S(cover), "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame", "-b:a", "160k", "-c:v", "copy", "-disposition:v", "attached_pic",
                                    "-id3v2_version", "3", "-metadata", "title=Fixture Title", "-metadata", "artist=Filo Tests"});
  }
  fx.joinA = gen("joinA.mp4", cat(cat(vsrc("320x240", 25, 2), {"-f", "lavfi", "-i", "sine=f=440:r=44100:d=2", "-ac", "2"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
  fx.joinB = gen("joinB.mp4", cat(cat(vsrc("640x360", 30, 2), {"-f", "lavfi", "-i", "sine=f=660:r=48000:d=2"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "1", "-shortest"}));
  fx.joinC = gen("joinC.mp4", cat(vsrc("480x270", 24, 2), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-an"}));
  fx.joinD = gen("joinD.mp4", {"-display_rotation:v:0", "90", "-i", S(gen("joinD_src.mp4", cat(cat(vsrc("320x180", 25, 2), {"-f", "lavfi", "-i", "sine=f=880:r=44100:d=2"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}))), "-c", "copy"});
  fx.speech = gen("speech.wav", {"-filter_complex",
                                 "anullsrc=r=44100:cl=stereo,atrim=0:1[a];sine=f=300:r=44100:d=4,volume=4,aformat=channel_layouts=stereo[b];anullsrc=r=44100:cl=stereo,atrim=0:1[c];[a][b][c]concat=n=3:v=0:a=1[o]", "-map", "[o]", "-c:a", "pcm_s16le"});
  fx.burstsWav = gen("bursts.wav", {"-f", "lavfi", "-i", "sine=f=1000:r=44100:d=6", "-af", "volume='4*(between(t,1,2)+between(t,4,5))':eval=frame,aformat=channel_layouts=stereo", "-c:a", "pcm_s16le"});
  fx.burstsMp4 = gen("bursts.mp4", cat(cat(vsrc("320x240", 25, 6), {"-i", S(fx.burstsWav), "-shortest"}), {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac"}));
  fx.kf = gen("kf.mp4", cat(cat(vsrc("320x240", 25, 6), {"-f", "lavfi", "-i", "sine=f=440:r=44100:d=6"}), {"-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
  fx.big = gen("big.mp4", cat(cat(vsrc("640x360", 25, 8), {"-f", "lavfi", "-i", "sine=f=500:d=8"}), {"-c:v", "libx264", "-preset", "ultrafast", "-crf", "10", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "256k", "-shortest"}));
  fx.mono = gen("mono.wav", {"-f", "lavfi", "-i", "sine=f=440:r=44100:d=3", "-ac", "1", "-c:a", "pcm_s16le"});
  fx.wav24 = gen("base24.wav", {"-f", "lavfi", "-i", "sine=f=440:r=48000:d=2", "-af", "volume=4,aformat=channel_layouts=stereo", "-c:a", "pcm_s24le"});
  fx.noise = gen("noise.wav", {"-f", "lavfi", "-i", "anoisesrc=d=10:c=pink:r=44100:a=0.2:seed=7", "-af", "aformat=channel_layouts=stereo", "-c:a", "pcm_s16le"});
  return fx;
}

// ---- jobs -------------------------------------------------------------------------------------------------------------------
JobResult RunJob(std::function<JobResult(JobContext&)> fn, double timeoutSec) {
  JobSpec s; s.operation = L"test"; s.title = L"test"; s.work = std::move(fn);
  uint64_t id = JobManager::Get().Submit(std::move(s));
  if (!JobManager::Get().WaitFor(id, (DWORD)(timeoutSec * 1000))) { JobManager::Get().Cancel(id); JobResult r; r.error = L"timeout"; return r; }
  auto info = JobManager::Get().Info(id);
  return info ? info->result : JobResult::Fail(L"no job info");
}
JobResult RunCap(const std::string& id, const fs::path& in, const Json& options, const fs::path& out) {
  Register();
  const Capability* cap = nullptr;
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == id) cap = &c;
  if (!cap) return JobResult::Fail(L"capability not registered: " + Widen(id));
  fs::path forced = out;
  return RunJob([&](JobContext& ctx) { return RunConversion(*cap, in, options, ctx, out.empty() ? nullptr : &forced); });
}

// ---- matrix results ---------------------------------------------------------------------------------------------------------
static std::mutex g_mu;
static std::map<std::string, std::pair<bool, std::wstring>> g_results;
void Record(const std::string& id, bool pass, const std::wstring& note) {
  std::lock_guard lk(g_mu);
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    std::ifstream in(Root() / L"testdata" / L"media" / L"matrix-results.json", std::ios::binary);
    if (in) {
      std::stringstream ss; ss << in.rdbuf();
      if (auto j = Json::Parse(ss.str())) for (auto& [k, v] : j->Get("results").Items()) g_results[k] = {v.Get("status").AsString() == "pass", Widen(v.Get("note").AsString())};
    }
  }
  auto it = g_results.find(id);
  if (it == g_results.end()) g_results[id] = {pass, note};
  else if (!pass) it->second = {false, note};       // a failure sticks
  else if (it->second.first) it->second.second = note;
  Json root = Json::MakeObject(), res = Json::MakeObject();
  for (auto& [k, v] : g_results) { Json e = Json::MakeObject(); e["status"] = v.first ? "pass" : "fail"; e["note"] = Narrow(v.second); res[k] = e; }
  root["results"] = res;
  std::ofstream f(Root() / L"testdata" / L"media" / L"matrix-results.json", std::ios::binary);
  f << root.Dump(true);
}

} // namespace mt
