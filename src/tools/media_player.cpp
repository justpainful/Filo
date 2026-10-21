#include "tools/media_player.h"
#include "core/log.h"
#include "core/process.h"
#include "engines/common/ffmpeg.h"
#include "engines/media/media_ops.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "winmm.lib")

namespace filo::tools {

namespace {
constexpr int kAudioRate = 44100, kAudioCh = 2, kBytesPerSample = 2;
constexpr int kAudioChunk = kAudioRate * kAudioCh * kBytesPerSample / 10;   // 100 ms
constexpr size_t kRingMax = 8;

bool ReadExact(HANDLE h, uint8_t* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    DWORD r = 0;
    if (!ReadFile(h, dst + got, (DWORD)std::min<size_t>(n - got, 1 << 20), &r, nullptr) || r == 0) return false;
    got += r;
  }
  return true;
}

// atempo accepts 0.5..100 per instance; chain for anything outside the comfortable range
std::wstring AtempoChain(double f) {
  std::wstring s;
  while (f < 0.5) { s += L"atempo=0.5,"; f /= 0.5; }
  while (f > 2.0) { s += L"atempo=2.0,"; f /= 2.0; }
  s += L"atempo=" + ff::Num(f, 5);
  return s;
}
} // namespace

MediaPlayer::MediaPlayer() = default;
MediaPlayer::~MediaPlayer() { Close(); }

bool MediaPlayer::Open(const fs::path& path, const Options& o, std::wstring* err) {
  Close();
  path_ = path; opt_ = o;
  auto mi = ff::Probe(path);
  if (!mi) { if (err) *err = L"This file cannot be previewed."; return false; }
  duration_ = mi->duration;
  hasVideo_ = o.wantVideo && mi->HasVideo();
  hasAudio_ = o.wantAudio && mi->HasAudio();
  if (!hasVideo_ && !hasAudio_) { if (err) *err = L"Nothing to preview in this file."; return false; }
  if (hasVideo_) {
    int w = 0, h = 0; mi->DisplaySize(w, h);
    srcW_ = w; srcH_ = h;
    fps_ = mi->FirstVideo()->fps > 1 ? std::min(mi->FirstVideo()->fps, 60.0) : 30.0;
    const double s = std::min({1.0, (double)o.maxW / std::max(w, 1), (double)o.maxH / std::max(h, 1)});
    outW_ = std::max(2, (int)std::lround(w * s)) & ~1;
    outH_ = std::max(2, (int)std::lround(h * s)) & ~1;
    frame_.assign((size_t)outW_ * outH_ * 4, 0);
  }
  open_ = true; playing_ = false; atEnd_ = false; startSrc_ = 0; pausedClock_ = 0; streamsRunning_ = false; stillPending_ = true;
  frameSerial_ = 0;
  return true;
}

void MediaPlayer::Close() {
  playing_ = false;
  StopStreams();
  open_ = false; ring_.clear();
}

void MediaPlayer::StopStreams() {
  gen_.fetch_add(1);
  if (vproc_) TerminateProcess(vproc_, 1);
  if (aproc_) TerminateProcess(aproc_, 1);
  cv_.notify_all();
  if (vthread_.joinable()) vthread_.join();
  if (athread_.joinable()) athread_.join();
  if (vproc_) { CloseHandle(vproc_); vproc_ = nullptr; }
  if (aproc_) { CloseHandle(aproc_); aproc_ = nullptr; }
  if (wave_) { waveOutReset(wave_); waveOutClose(wave_); wave_ = nullptr; }
  { std::lock_guard lk(mu_); ring_.clear(); }
  streamsRunning_ = false;
}

void MediaPlayer::StartStreams(double startSrc) {
  StopStreams();
  startSrc_ = std::clamp(startSrc, 0.0, std::max(0.0, duration_ - 0.05));
  const uint64_t gen = gen_.load();
  samplesWritten_ = 0; audioStarted_ = false; atEnd_ = false;
  const double speed = std::clamp(opt_.speed, 0.1, 16.0);
  fs::path ffm = ff::FfmpegPath();
  if (hasVideo_) {
    std::wstring vf;
    if (!opt_.videoFilter.empty()) vf += opt_.videoFilter + L",";
    if (speed != 1.0) vf += L"setpts=PTS/" + ff::Num(speed, 5) + L",";
    vf += L"scale=" + std::to_wstring(outW_) + L":" + std::to_wstring(outH_) + L":flags=fast_bilinear,fps=" + ff::Num(fps_, 3) + L",format=bgra";
    std::vector<std::wstring> a = {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-ss", ff::Num(startSrc_, 3), L"-i", path_.wstring(), L"-an", L"-sn", L"-vf", vf,
                                   L"-f", L"rawvideo", L"-pix_fmt", L"bgra", L"-"};
    PipedProcess pp;
    if (SpawnPiped(ffm, a, pp)) {
      vproc_ = pp.process;
      vthread_ = std::thread([this, h = pp.stdoutRead, gen] { VideoLoop(h, gen); });
    }
  }
  if (hasAudio_) {
    std::wstring af;
    if (!opt_.audioFilter.empty()) af += opt_.audioFilter + L",";
    if (speed != 1.0) af += AtempoChain(speed) + L",";
    af += L"aresample=" + std::to_wstring(kAudioRate);
    std::vector<std::wstring> a = {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-ss", ff::Num(startSrc_, 3), L"-i", path_.wstring(), L"-vn", L"-sn", L"-af", af,
                                   L"-ac", std::to_wstring(kAudioCh), L"-ar", std::to_wstring(kAudioRate), L"-f", L"s16le", L"-"};
    PipedProcess pp;
    if (SpawnPiped(ffm, a, pp)) {
      aproc_ = pp.process;
      WAVEFORMATEX wf{WAVE_FORMAT_PCM, kAudioCh, kAudioRate, kAudioRate * kAudioCh * kBytesPerSample, kAudioCh * kBytesPerSample, 16, 0};
      if (waveOutOpen(&wave_, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
        waveOutPause(wave_);          // starts paused; Play() restarts it once data is queued
        athread_ = std::thread([this, h = pp.stdoutRead, gen] { AudioLoop(h, gen); });
      } else { TerminateProcess(aproc_, 1); CloseHandle(pp.stdoutRead); CloseHandle(aproc_); aproc_ = nullptr; hasAudio_ = false; }
    }
  }
  wallStart_ = NowSeconds();
  streamsRunning_ = true;
  if (!playing_) pausedClock_ = 0;
}

void MediaPlayer::VideoLoop(HANDLE pipe, uint64_t gen) {
  const size_t fsz = (size_t)outW_ * outH_ * 4;
  uint64_t n = 0;
  for (;;) {
    PendingFrame f; f.px.resize(fsz);
    if (!ReadExact(pipe, f.px.data(), fsz)) break;
    f.pts = (double)n++ / fps_;
    std::unique_lock lk(mu_);
    cv_.wait(lk, [&] { return gen_.load() != gen || ring_.size() < kRingMax; });
    if (gen_.load() != gen) break;
    ring_.push_back(std::move(f));
  }
  CloseHandle(pipe);
  std::lock_guard lk(mu_);
  if (gen_.load() == gen) ring_.push_back(PendingFrame{-1.0, {}});       // end-of-stream marker
}

void MediaPlayer::AudioLoop(HANDLE pipe, uint64_t gen) {
  const int kBufs = 6;
  std::vector<std::vector<uint8_t>> bufs(kBufs, std::vector<uint8_t>(kAudioChunk));
  std::vector<WAVEHDR> hdr(kBufs);
  for (auto& h : hdr) h = {};
  int idx = 0;
  bool first = true;
  for (;;) {
    if (gen_.load() != gen) break;
    WAVEHDR& h = hdr[idx];
    while (h.dwFlags & WHDR_PREPARED && !(h.dwFlags & WHDR_DONE)) { if (gen_.load() != gen) goto out; Sleep(5); }
    if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave_, &h, sizeof h);
    size_t got = 0;
    while (got < (size_t)kAudioChunk) {
      DWORD r = 0;
      if (!ReadFile(pipe, bufs[idx].data() + got, (DWORD)(kAudioChunk - got), &r, nullptr) || r == 0) break;
      got += r;
    }
    if (got == 0) break;
    h = {}; h.lpData = (LPSTR)bufs[idx].data(); h.dwBufferLength = (DWORD)got;
    { std::lock_guard lk(audioMu_);
      if (gen_.load() != gen || !wave_) break;
      waveOutPrepareHeader(wave_, &h, sizeof h);
      waveOutWrite(wave_, &h, sizeof h);
      samplesWritten_ += got / (kAudioCh * kBytesPerSample);
      if (first) { first = false; audioStarted_ = true; if (playing_) waveOutRestart(wave_); }
    }
    idx = (idx + 1) % kBufs;
    if (got < (size_t)kAudioChunk) break;
  }
out:
  CloseHandle(pipe);
}

double MediaPlayer::Clock() {
  if (!streamsRunning_) return pausedClock_;
  if (!playing_) return pausedClock_;
  if (hasAudio_ && wave_ && audioStarted_) {
    MMTIME t{}; t.wType = TIME_SAMPLES;
    std::lock_guard lk(audioMu_);
    if (wave_ && waveOutGetPosition(wave_, &t, sizeof t) == MMSYSERR_NOERROR && t.wType == TIME_SAMPLES) return (double)t.u.sample / kAudioRate;
  }
  return pausedClock_ + (NowSeconds() - wallStart_);
}

double MediaPlayer::Position() {
  const double speed = std::clamp(opt_.speed, 0.1, 16.0);
  double p = startSrc_ + Clock() * speed;
  if (!streamsRunning_) p = startSrc_ + pausedClock_ * speed;
  return std::clamp(p, 0.0, duration_);
}

void MediaPlayer::Play() {
  if (!open_ || playing_) return;
  if (atEnd_ || Position() >= duration_ - 0.05) { StartStreams(rangeEnd_ > 0 ? rangeStart_ : 0.0); atEnd_ = false; }
  else if (!streamsRunning_) StartStreams(startSrc_ + pausedClock_ * opt_.speed);
  playing_ = true;
  wallStart_ = NowSeconds();
  if (wave_ && audioStarted_) { std::lock_guard lk(audioMu_); waveOutRestart(wave_); }
}

void MediaPlayer::Pause() {
  if (!playing_) return;
  pausedClock_ = Clock();
  playing_ = false;
  if (wave_) { std::lock_guard lk(audioMu_); waveOutPause(wave_); }
}

void MediaPlayer::Seek(double t, bool scrubbing) {
  if (!open_) return;
  t = std::clamp(t, 0.0, std::max(0.0, duration_ - 0.01));
  const bool wasPlaying = playing_;
  playing_ = false;
  atEnd_ = false;
  if (wasPlaying) {
    StartStreams(t);
    playing_ = true; wallStart_ = NowSeconds();
    if (wave_ && audioStarted_) { std::lock_guard lk(audioMu_); waveOutRestart(wave_); }
    return;
  }
  // paused: show a still frame and restart the streams lazily on Play()
  StopStreams();
  startSrc_ = t; pausedClock_ = 0; streamsRunning_ = false;
  if (hasVideo_) {
    media::FrameImage fi = media::ExtractFrame(path_, t, outW_, outH_, !scrubbing);
    if (fi.ok && fi.width > 0) {
      std::lock_guard lk(mu_);
      frame_.assign((size_t)outW_ * outH_ * 4, 0);
      // ExtractFrame preserves aspect inside maxW x maxH: centre it in our fixed-size frame
      const int ox = (outW_ - fi.width) / 2, oy = (outH_ - fi.height) / 2;
      for (int y = 0; y < std::min(fi.height, outH_); ++y)
        memcpy(&frame_[((size_t)(y + std::max(0, oy)) * outW_ + std::max(0, ox)) * 4], &fi.bgra[(size_t)y * fi.width * 4], (size_t)std::min(fi.width, outW_) * 4);
      ++frameSerial_;
    }
  }
}

void MediaPlayer::PlayRange(double start, double end, bool loop) {
  rangeStart_ = start; rangeEnd_ = end; loop_ = loop;
  StartStreams(start);
  playing_ = true; wallStart_ = NowSeconds();
  if (wave_ && audioStarted_) { std::lock_guard lk(audioMu_); waveOutRestart(wave_); }
}

bool MediaPlayer::Update() {
  if (!open_) return false;
  bool changed = false;
  const double pos = Position();
  if (rangeEnd_ > 0 && playing_ && pos >= rangeEnd_ - 0.01) {
    if (loop_) { PlayRange(rangeStart_, rangeEnd_, true); return false; }
    Pause(); startSrc_ = rangeEnd_; pausedClock_ = 0; streamsRunning_ = false; StopStreams(); rangeEnd_ = -1; atEnd_ = false;
    return false;
  }
  if (playing_ && hasVideo_) {
    const double clk = Clock();
    std::lock_guard lk(mu_);
    while (!ring_.empty()) {
      PendingFrame& f = ring_.front();
      if (f.pts < 0) {           // end of stream
        if (ring_.size() == 1) { atEnd_ = true; playing_ = false; ring_.clear(); }
        break;
      }
      if (f.pts <= clk + 0.001) {
        frame_.swap(f.px); ++frameSerial_; changed = true; ring_.pop_front(); cv_.notify_all();
      } else break;
    }
  } else if (playing_ && !hasVideo_ && hasAudio_) {
    if (pos >= duration_ - 0.05) { playing_ = false; atEnd_ = true; }
  }
  if (stillPending_) {
    stillPending_ = false;
    if (hasVideo_) { Seek(0, false); changed = true; }
  }
  return changed;
}

} // namespace filo::tools
