// ffmpeg-backed image codecs: WebP (libwebp), AVIF (libaom-av1 + avif muxer), animated GIF <-> WebP, and a generic decoder
// for formats the OS cannot decode properly (AVIF with alpha / exotic colour matrices, animated WebP, HEIC fallback).
#include "engines/image/image_internal.h"
#include "engines/common/ffmpeg.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <mutex>

namespace filo::img {

fs::path StageForFfmpeg(const fs::path& in, const fs::path& scratchDir, const wchar_t* stagedName) {
  std::error_code ec;
  fs::path abs = fs::absolute(in, ec);
  std::wstring s = abs.wstring();
  bool simple = s.size() < 200;
  for (wchar_t c : s) if (c < 0x20 || c > 0x7e || wcschr(L"\"%|<>^&!;'`$", c)) { simple = false; break; }
  if (simple) return abs;
  fs::path dst = scratchDir / stagedName;
  if (!CopyFileW(LongPath(in).c_str(), dst.c_str(), FALSE)) return abs;
  return dst;
}

bool FfHasMuxer(const std::string& name) {
  static std::mutex mu; static std::map<std::string, bool> cache;
  std::lock_guard lk(mu);
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  bool ok = false;
  if (ff::Available()) {
    int code = -1;
    std::string out = RunCapture(ff::FfmpegPath(), {L"-hide_banner", L"-h", L"muxer=" + Widen(name)}, 20000, &code);
    ok = code == 0 && out.find("Muxer " + name) != std::string::npos;
  }
  cache[name] = ok;
  return ok;
}

static bool RunFf(const std::vector<std::wstring>& args, JobContext* ctx, std::wstring* err, const wchar_t* what) {
  JobContext local;
  JobContext& c = ctx ? *ctx : local;
  EngineResult r = ff::Run(args, 0, c);
  if (r.ok) return true;
  if (err) *err = (r.error == L"Cancelled") ? L"Cancelled" : (std::wstring(L"The ") + what + L" could not be completed.");
  FILO_LOG(Warn, "image", "ffmpeg step failed (%ls)", what);
  return false;
}

static std::wstring Q(int v) { return std::to_wstring(v); }

bool FfEncodeWebp(const fs::path& pngIn, const fs::path& out, const FfEncodeParams& p, JobContext* ctx, std::wstring* err) {
  if (!ff::Available() || !ff::HasEncoder("libwebp")) { if (err) *err = L"WebP output needs the FFmpeg libwebp encoder, which was not found."; return false; }
  std::vector<std::wstring> a = {L"-i", pngIn.wstring(), L"-frames:v", L"1", L"-an", L"-c:v", L"libwebp", L"-pix_fmt", L"bgra"};
  if (p.lossless) { a.insert(a.end(), {L"-lossless", L"1", L"-quality", L"100", L"-compression_level", L"6"}); }
  else a.insert(a.end(), {L"-lossless", L"0", L"-quality", Q(std::clamp(p.quality, 1, 100)), L"-compression_level", L"4"});
  a.push_back(out.wstring());
  return RunFf(a, ctx, err, L"WebP encoder");
}

bool FfEncodeAvif(const fs::path& pngIn, const fs::path& out, const FfEncodeParams& p, JobContext* ctx, std::wstring* err) {
  if (!ff::Available() || !ff::HasEncoder("libaom-av1") || !FfHasMuxer("avif")) { if (err) *err = L"AVIF output needs the FFmpeg AV1 encoder and AVIF muxer, which were not found."; return false; }
  int crf = (int)std::lround((100 - std::clamp(p.quality, 1, 100)) * 0.63);
  bool lossless = p.lossless;
  bool c444 = lossless || p.quality >= 90;
  const std::wstring tags = L"setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1:range=pc";
  std::wstring colorChain;
  if (lossless) colorChain = L"format=gbrp,setparams=colorspace=gbr:color_primaries=bt709:color_trc=iec61966-2-1:range=pc";
  else colorChain = L"scale=out_color_matrix=bt709:out_range=pc,format=" + std::wstring(c444 ? L"yuv444p" : L"yuv420p") + L"," + tags;
  std::vector<std::wstring> a = {L"-i", pngIn.wstring()};
  if (p.hasAlpha) {
    std::wstring fc = L"[0:v]format=rgba,split[c][a];[a]alphaextract,format=gray,setparams=colorspace=bt709:range=pc[al];[c]" + colorChain + L"[cc]";
    a.insert(a.end(), {L"-filter_complex", fc, L"-map", L"[cc]", L"-map", L"[al]"});
  } else {
    a.insert(a.end(), {L"-vf", colorChain});
  }
  a.insert(a.end(), {L"-frames:v", L"1", L"-c:v", L"libaom-av1", L"-cpu-used", Q(std::clamp(p.avifSpeed, 0, 8)), L"-row-mt", L"1", L"-still-picture", L"1", L"-b:v", L"0"});
  if (lossless) a.insert(a.end(), {L"-crf", L"0", L"-aom-params", L"lossless=1"});
  else {
    a.insert(a.end(), {L"-crf", Q(crf)});
    if (p.hasAlpha) a.insert(a.end(), {L"-crf:v:1", Q(std::min(crf, 12))});
  }
  a.push_back(out.wstring());
  return RunFf(a, ctx, err, L"AVIF encoder");
}

bool FfDecodeToPng(const fs::path& in, const fs::path& pngOut, int frame, bool avifAlphaMerge, JobContext* ctx, std::wstring* err) {
  if (!ff::Available()) { if (err) *err = L"This image format needs FFmpeg, which was not found."; return false; }
  std::vector<std::wstring> a = {L"-i", in.wstring()};
  if (avifAlphaMerge) a.insert(a.end(), {L"-filter_complex", L"[0:v:0][0:v:1]alphamerge[o]", L"-map", L"[o]"});
  else if (frame > 0) a.insert(a.end(), {L"-vf", L"select=eq(n\\," + Q(frame) + L")", L"-fps_mode", L"passthrough"});
  a.insert(a.end(), {L"-frames:v", L"1", L"-an", L"-pix_fmt", L"rgba", pngOut.wstring()});
  return RunFf(a, ctx, err, L"image decoder");
}

bool FfAnimatedConvert(const fs::path& in, const fs::path& out, bool toWebp, int quality, bool lossless, JobContext* ctx, std::wstring* err) {
  if (!ff::Available()) { if (err) *err = L"Animated images need FFmpeg, which was not found."; return false; }
  std::vector<std::wstring> a = {L"-i", in.wstring(), L"-an"};
  if (toWebp) {
    if (!ff::HasEncoder("libwebp_anim")) { if (err) *err = L"Animated WebP output needs the FFmpeg libwebp_anim encoder, which was not found."; return false; }
    a.insert(a.end(), {L"-fps_mode", L"passthrough", L"-c:v", L"libwebp_anim", L"-pix_fmt", L"bgra", L"-loop", L"0"});
    if (lossless) a.insert(a.end(), {L"-lossless", L"1", L"-quality", L"100", L"-compression_level", L"6"});
    else a.insert(a.end(), {L"-lossless", L"0", L"-quality", Q(std::clamp(quality, 1, 100)), L"-compression_level", L"4"});
  } else {
    a.insert(a.end(), {L"-filter_complex", L"[0:v]split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse=dither=bayer:bayer_scale=5", L"-loop", L"0"});
  }
  a.push_back(out.wstring());
  return RunFf(a, ctx, err, toWebp ? L"animated WebP encoder" : L"animated GIF encoder");
}

} // namespace filo::img
