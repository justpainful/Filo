// Lightweight audio/video preview player for the tool dialogs. FFmpeg decodes to raw BGRA frames and 16-bit PCM through pipes
// (so every container/codec Filo can convert can also be previewed); video frames are paced by the audio device clock
// (waveOut), or by the wall clock for silent media. Playback never touches the original file beyond reading it.
#pragma once
#include "core/util.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace filo::tools {

class MediaPlayer {
 public:
  struct Options {
    bool wantVideo = true, wantAudio = true;
    double speed = 1.0;                    // playback rate (pitch preserved); the position still reports SOURCE time
    int maxW = 960, maxH = 540;            // decode size cap for the preview frames
    std::wstring videoFilter;              // extra ffmpeg -vf chain applied before scaling (e.g. crop preview)
    std::wstring audioFilter;              // extra ffmpeg -af chain (e.g. pan for channel gains)
  };

  MediaPlayer();
  ~MediaPlayer();
  MediaPlayer(const MediaPlayer&) = delete;
  MediaPlayer& operator=(const MediaPlayer&) = delete;

  bool Open(const fs::path& path, const Options& o, std::wstring* error = nullptr);
  void Close();
  bool IsOpen() const { return open_; }
  double Duration() const { return duration_; }
  bool HasVideo() const { return hasVideo_; }
  bool HasAudio() const { return hasAudio_; }
  int FrameW() const { return outW_; }        // decoded preview size
  int FrameH() const { return outH_; }
  int SourceW() const { return srcW_; }       // display size of the source (rotation applied)
  int SourceH() const { return srcH_; }
  double Fps() const { return fps_; }

  void Play();
  void Pause();
  void Toggle() { if (Playing()) Pause(); else Play(); }
  bool Playing() const { return playing_; }
  // Source-time seek. While paused this shows a still frame (fast keyframe frame while `scrubbing`, exact otherwise).
  void Seek(double t, bool scrubbing = false);
  // Plays [start, end] and pauses at `end` (loop=true restarts at `start`).
  void PlayRange(double start, double end, bool loop = false);
  void ClearRange() { rangeEnd_ = -1; loop_ = false; }
  double Position();                          // source seconds
  bool AtEnd() const { return atEnd_; }

  // UI thread, once per frame. Returns true when `Frame()` holds a new picture since the last call.
  bool Update();
  const std::vector<uint8_t>& Frame() const { return frame_; }       // outW*outH*4 BGRA (opaque)
  uint64_t FrameSerial() const { return frameSerial_; }

 private:
  void StopStreams();
  void StartStreams(double startSrc);
  void VideoLoop(HANDLE pipe, uint64_t gen);
  void AudioLoop(HANDLE pipe, uint64_t gen);
  double Clock();                             // output-timeline seconds since the current streams started
  struct PendingFrame { double pts; std::vector<uint8_t> px; };

  fs::path path_;
  Options opt_;
  bool open_ = false, hasVideo_ = false, hasAudio_ = false;
  double duration_ = 0, fps_ = 30;
  int srcW_ = 0, srcH_ = 0, outW_ = 0, outH_ = 0;

  std::atomic<bool> playing_{false}, atEnd_{false};
  std::atomic<uint64_t> gen_{0};
  double startSrc_ = 0;                       // source time at which the running streams began
  double rangeEnd_ = -1, rangeStart_ = 0; bool loop_ = false;
  bool streamsRunning_ = false;
  double pausedClock_ = 0;                    // clock value when paused
  double wallStart_ = 0;                      // wall time (NowSeconds) matching clock 0 for silent media

  // decode threads
  std::thread vthread_, athread_;
  HANDLE vproc_ = nullptr, aproc_ = nullptr;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<PendingFrame> ring_;
  std::vector<uint8_t> frame_;
  uint64_t frameSerial_ = 0;
  bool stillPending_ = false;

  // audio device
  HWAVEOUT wave_ = nullptr;
  std::atomic<uint64_t> samplesWritten_{0};
  std::mutex audioMu_;
  bool audioStarted_ = false;
};

} // namespace filo::tools
