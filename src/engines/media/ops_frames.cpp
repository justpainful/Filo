// Frame extraction: PNG export at native resolution and fast BGRA previews for the UI.
#include "media_ops.h"
#include "media_internal.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

namespace filo::media {
using namespace detail;

namespace {

double ClampTime(const Probed& pr, double t) {
  double fps = pr.video && pr.video->fps > 0 ? pr.video->fps : 25;
  double last = std::max(0.0, pr.duration - 1.5 / fps);
  return std::max(0.0, std::min(t, last));
}

bool ParsePam(const std::string& data, FrameImage& img) {
  // "P7\nWIDTH w\nHEIGHT h\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" + RGBA bytes
  if (data.size() < 16 || data.compare(0, 2, "P7") != 0) return false;
  size_t end = data.find("ENDHDR\n");
  if (end == std::string::npos) return false;
  int w = 0, h = 0;
  size_t p = data.find("WIDTH "); if (p != std::string::npos) w = atoi(data.c_str() + p + 6);
  p = data.find("HEIGHT "); if (p != std::string::npos) h = atoi(data.c_str() + p + 7);
  size_t off = end + 7;
  if (w <= 0 || h <= 0 || data.size() < off + (size_t)w * h * 4) return false;
  img.width = w; img.height = h;
  img.bgra.resize((size_t)w * h * 4);
  const uint8_t* s = (const uint8_t*)data.data() + off;
  for (size_t i = 0; i < (size_t)w * h; ++i) { img.bgra[i * 4 + 0] = s[i * 4 + 2]; img.bgra[i * 4 + 1] = s[i * 4 + 1]; img.bgra[i * 4 + 2] = s[i * 4 + 0]; img.bgra[i * 4 + 3] = 255; }
  return true;
}

FrameImage ExtractWith(const fs::path& path, const Probed& pr, double seconds, int maxW, int maxH, bool accurate, const std::atomic<bool>* cancel) {
  FrameImage img;
  if (!pr.video) { img.error = L"This file has no video track."; return img; }
  double t = ClampTime(pr, seconds);
  img.time = t;
  int W = 0, H = 0; pr.mi.DisplaySize(W, H);
  int tw = W, th = H;
  if (maxW > 0 && maxH > 0 && (W > maxW || H > maxH)) {
    double s = std::min((double)maxW / W, (double)maxH / H);
    tw = std::max(2, (int)std::lround(W * s)); th = std::max(2, (int)std::lround(H * s));
  }
  Args a = {L"-ss", N(t, 4)};
  if (!accurate) a.push_back(L"-noaccurate_seek");
  a.insert(a.end(), {L"-i", P(path), L"-map", L"0:" + std::to_wstring(pr.video->index), L"-frames:v", L"1", L"-an", L"-sn"});
  if (tw != W || th != H) a.insert(a.end(), {L"-vf", L"scale=" + std::to_wstring(tw) + L":" + std::to_wstring(th) + L":flags=bilinear"});
  a.insert(a.end(), {L"-f", L"image2pipe", L"-c:v", L"pam", L"-pix_fmt", L"rgba", L"pipe:1"});
  FfOut r = RunFfmpegSimple(a, cancel, 60000, (size_t)tw * th * 4 + 4096);
  if (!r.ok) { img.error = r.error; return img; }
  if (!ParsePam(r.stdoutText, img)) { img.error = L"No picture could be decoded at that position."; return img; }
  img.ok = true;
  return img;
}

} // namespace

FrameImage ExtractFrame(const fs::path& path, double seconds, int maxW, int maxH, bool accurate, const std::atomic<bool>* cancel) {
  Probed pr; std::wstring err;
  if (!ProbeFile(path, pr, err)) { FrameImage i; i.error = err; return i; }
  return ExtractWith(path, pr, seconds, maxW, maxH, accurate, cancel);
}

FramesResult SaveVideoFrames(const fs::path& input, const SaveFramesRequest& req, JobContext& ctx) {
  FramesResult res;
  Probed pr; std::wstring err;
  auto fail = [&](const std::wstring& m) { static_cast<JobResult&>(res) = JobResult::Fail(m); return res; };
  if (!ProbeFile(input, pr, err)) return fail(err);
  if (!pr.video) return fail(L"This file has no video track.");
  if (req.timestamps.empty()) return fail(L"Pick at least one moment to save.");
  JobResult jr = WithDirTransaction(input, L" (frames)", req.outputDir, ctx, [&](const fs::path& dir) {
    size_t n = req.timestamps.size();
    int digits = std::max(3, (int)std::to_string(n).size());
    std::wstring stem = input.stem().wstring();
    for (size_t i = 0; i < n; ++i) {
      if (ctx.Cancelled()) return EngineResult::Fail(L"Cancelled");
      double t = ClampTime(pr, req.timestamps[i]);
      wchar_t num[16]; swprintf_s(num, L"%0*d", digits, (int)i + 1);
      fs::path out = dir / (stem + L" - frame " + num + L".png");
      Args a = {L"-ss", N(t, 4), L"-i", P(input), L"-map", L"0:" + std::to_wstring(pr.video->index), L"-frames:v", L"1", L"-an", L"-sn", L"-update", L"1", P(out)};
      FfOut r = RunFfmpeg(a, 0, ctx);
      if (!r.ok) return EngineResult::Fail(r.error);
      std::error_code ec;
      if (!fs::exists(out, ec) || fs::file_size(out, ec) == 0) return EngineResult::Fail(L"No picture could be decoded at " + FormatTime(t) + L".");
      ctx.SetProgress((double)(i + 1) / n);
    }
    return EngineResult::Ok();
  });
  static_cast<JobResult&>(res) = jr;
  if (jr.ok) res.folder = jr.outputs[0];
  return res;
}

// ---- scrubber ---------------------------------------------------------------------------------------------------------------------
struct FrameScrubber::Impl {
  fs::path path;
  Probed pr;
  bool open = false;
  std::mutex mu;
  struct Entry { int64_t key; FrameImage img; };
  std::deque<Entry> cache;
};

FrameScrubber::FrameScrubber(fs::path path) : impl_(new Impl) { impl_->path = std::move(path); }
FrameScrubber::~FrameScrubber() { delete impl_; }
bool FrameScrubber::Open(std::wstring* error) {
  std::wstring err;
  if (!ProbeFile(impl_->path, impl_->pr, err)) { if (error) *error = err; return false; }
  if (!impl_->pr.video) { if (error) *error = L"This file has no video track."; return false; }
  impl_->open = true;
  return true;
}
double FrameScrubber::Duration() const { return impl_->pr.duration; }
int FrameScrubber::Width() const { int w = 0, h = 0; impl_->pr.mi.DisplaySize(w, h); return w; }
int FrameScrubber::Height() const { int w = 0, h = 0; impl_->pr.mi.DisplaySize(w, h); return h; }
double FrameScrubber::Fps() const { return impl_->pr.video ? impl_->pr.video->fps : 0; }

FrameImage FrameScrubber::Get(double seconds, int maxW, int maxH, bool accurate, const std::atomic<bool>* cancel) {
  if (!impl_->open && !Open()) { FrameImage i; i.error = L"The video could not be opened."; return i; }
  double fps = impl_->pr.video->fps > 0 ? impl_->pr.video->fps : 25;
  double t = ClampTime(impl_->pr, seconds);
  int64_t frameNo = (int64_t)std::llround(t * fps);
  int64_t key = (frameNo << 20) ^ ((int64_t)maxW << 8) ^ (int64_t)maxH ^ (accurate ? 1LL << 62 : 0);
  {
    std::lock_guard lk(impl_->mu);
    for (auto& e : impl_->cache) if (e.key == key) return e.img;
  }
  FrameImage img = ExtractWith(impl_->path, impl_->pr, t, maxW, maxH, accurate, cancel);
  if (img.ok) {
    std::lock_guard lk(impl_->mu);
    impl_->cache.push_back({key, img});
    if (impl_->cache.size() > 24) impl_->cache.pop_front();
  }
  return img;
}

} // namespace filo::media
