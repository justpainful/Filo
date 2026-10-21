// Shared pieces of the Video tool dialogs (Trim, Crop, Speed, Split, Frames, Redact, Mute):
//  * VideoView: the preview. Playback goes through the shared MediaPlayer; paused stills (scrubbing, frame stepping) are fetched
//    on a worker thread (media::FrameScrubber) so the UI thread never waits for ffmpeg. The newest picture is uploaded to one
//    reusable Direct2D bitmap.
//  * VideoToolDialog: ToolDialog base with the dark rounded preview well, the transport row (play / time / frame step),
//    keyboard handling (Space, arrows, ',' '.') and the "done" note drawn under the job panel.
//  * RectDrag helpers shared by Crop and Redact (move / resize / create a rectangle in video pixels, optional aspect lock).
#pragma once
#include "tools/tools.h"
#include "tools/tool_dialog.h"
#include "tools/media_player.h"
#include "tools/widgets_media.h"
#include "core/output.h"
#include "core/log.h"
#include "engines/media/media_info.h"
#include "engines/media/media_ops.h"
#include "engines/common/ffmpeg.h"
#include <atomic>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace filo::tools {

ToolDialog* MakeTrimVideo(const Selection& sel);
ToolDialog* MakeCropVideo(const Selection& sel);
ToolDialog* MakeChangeSpeed(const Selection& sel);
ToolDialog* MakeSplitVideo(const Selection& sel);
ToolDialog* MakeSaveFrames(const Selection& sel);
ToolDialog* MakeRedactVideo(const Selection& sel);
ToolDialog* MakeRemoveAudio(const Selection& sel);

namespace vid {

using namespace filo::ds;
using namespace filo::ui;

constexpr UINT WM_VID_STILL = WM_APP + 0x71;
constexpr int kPreviewMaxW = 1120, kPreviewMaxH = 630;
// MediaPlayer's audio thread frees its wave buffers when the audio stream hits EOF while the last ones are still queued in waveOut
// (crash). EOF is reached about 0.6 s before the end of playback, so audible playback stops this long before the end and the last
// stretch is previewed silently. Set to 0 once MediaPlayer::AudioLoop waits for its buffers (see the report).
constexpr double kAudioTailGuard = 0.9;

struct IRect { int x = 0, y = 0, w = 0, h = 0; };
struct FRect { float x = 0, y = 0, w = 0, h = 0; };

// Layout direction (Settings language; FILO_VID_RTL=1 forces RTL for visual tests).
bool UiRtl();

// ------------------------------------------------------------------------------------------------ async stills
class StillWorker {
 public:
  ~StillWorker() { Stop(); }
  void Start(const fs::path& p, HWND notify);
  void Stop();
  uint64_t Request(double t, bool accurate, int maxW, int maxH);
  bool Take(media::FrameImage& out, uint64_t& seq);       // UI thread: newest finished picture (once)
  bool Pending() const { std::lock_guard lk(mu_); return done_.load() < latest_.load() || ready_; }
 private:
  struct Req { double t = 0; bool accurate = true; int w = 0, h = 0; uint64_t seq = 0; };
  void Loop();
  fs::path path_;
  HWND hwnd_ = nullptr;
  std::thread th_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false, have_ = false, ready_ = false;
  Req req_;
  media::FrameImage res_;
  uint64_t resSeq_ = 0;
  std::atomic<bool> cancel_{false};
  std::atomic<uint64_t> latest_{0}, done_{0};
};

// ------------------------------------------------------------------------------------------------ preview
class VideoView {
 public:
  ~VideoView() { Close(); }
  bool Open(const fs::path& path, HWND notify, std::wstring* err);
  void Close();
  bool IsOpen() const { return open_; }
  double Duration() const { return dur_; }
  double Fps() const { return fps_; }
  int SrcW() const { return srcW_; }
  int SrcH() const { return srcH_; }
  bool HasAudio() const { return hasAudio_; }

  double Pos() const { return pos_; }
  bool Playing() const { return playing_; }
  void Seek(double t, bool scrubbing = false);
  void Step(int frames);
  // Plays from the playhead up to `end` (<= 0: the end of the video). When the playhead is at `end` or before `from` it starts at `from`.
  void Play(double end = -1, double from = 0);
  void Pause();
  void Toggle(double end = -1, double from = 0) { if (playing_) Pause(); else Play(end, from); }
  void PlayRange(double a, double b) { pos_ = a; resumable_ = false; Play(b, a); }
  void SetSpeed(double s) { if (s != speed_) { speed_ = s; speedDirty_ = true; } }
  double Speed() const { return speed_; }

  // UI thread, once per frame (before drawing). Returns true when a new picture was uploaded.
  bool Update(ID2D1DeviceContext* dc, bool mouseDown);
  ID2D1Bitmap* Bitmap() const { return bmp_.Get(); }
  int FrameW() const { return bw_; }
  int FrameH() const { return bh_; }
  const std::vector<uint8_t>& Pixels() const { return px_; }     // BGRA of the picture currently shown
  uint64_t Serial() const { return serial_; }
  bool StillPending() const { return still_.Pending(); }
  void ResetDevice() { bmp_.Reset(); bw_ = bh_ = 0; forceUpload_ = true; }
  int FrameIndex(double t) const;                                  // frame number covering time t (0-based)
  double FrameTime(int idx) const { return idx / fps_; }

 private:
  void RequestStill(double t, bool accurate);
  void Upload(ID2D1DeviceContext* dc, const uint8_t* data, int w, int h);
  void ReopenPlayer(bool audio);
  fs::path path_;
  HWND hwnd_ = nullptr;
  MediaPlayer player_;
  StillWorker still_;
  bool open_ = false, playing_ = false, resumable_ = false, speedDirty_ = false, needExact_ = false, forceUpload_ = false, playerAudio_ = true;
  double dur_ = 0, fps_ = 30, pos_ = 0, speed_ = 1.0, rangeEndUsed_ = -1;
  int srcW_ = 0, srcH_ = 0;
  bool hasAudio_ = false;
  uint64_t lastPlayerSerial_ = 0;
  ComPtr<ID2D1Bitmap1> bmp_;
  int bw_ = 0, bh_ = 0;
  std::vector<uint8_t> px_;
  int pxW_ = 0, pxH_ = 0;
  uint64_t serial_ = 0;
};

// ------------------------------------------------------------------------------------------------ drag helper (video pixels)
enum : int { kL = 1, kR = 2, kT = 4, kB = 8 };
struct RectDrag {
  int mode = 0;            // 0 idle, 1 move, 2 resize, 3 create
  int flags = 0;
  float mx0 = 0, my0 = 0;
  FRect r0;
  bool Active() const { return mode != 0; }
  void Begin(int m, int f, float mx, float my, const FRect& r) { mode = m; flags = f; mx0 = mx; my0 = my; r0 = r; }
  void End() { mode = 0; }
  // New rectangle for the mouse at (mx, my) in video pixels. aspect > 0 locks w/h.
  FRect Update(float mx, float my, int W, int H, double aspect, float minSize) const;
};
// Which handle of the screen rectangle `r` is under (mx, my)? Returns kL|kT... flags (0 = none); `inside` reports the body.
int HitHandle(const Rect& r, float mx, float my, float tol, bool* inside);
int CursorForFlags(int flags);
// Even-sized integer rectangle inside W x H.
IRect Quantize(const FRect& r, int W, int H, bool even = true);

// Soft blur used by the Redact preview (3-pass box blur on BGRA, in place).
void BoxBlurBGRA(std::vector<uint8_t>& px, int w, int h, int radius);

// ------------------------------------------------------------------------------------------------ dialog base
class VideoToolDialog : public ToolDialog {
 protected:
  explicit VideoToolDialog(const Selection& sel);
  ~VideoToolDialog() override;

  fs::path path_;
  media::MediaSummary info_;
  VideoView view_;
  std::wstring viewError_;
  bool kbFocusDone_ = false;
  bool enterPressed_ = false;

  double Dur() const { return info_.ok && info_.duration > 0 ? info_.duration : view_.Duration(); }
  double Fps() const { return info_.ok && info_.fps > 1 ? info_.fps : (view_.Fps() > 1 ? view_.Fps() : 30.0); }
  int SrcW() const { return info_.ok && info_.width > 0 ? info_.width : view_.SrcW(); }
  int SrcH() const { return info_.ok && info_.height > 0 ? info_.height : view_.SrcH(); }
  bool Usable() const { return info_.ok && info_.hasVideo && Dur() > 0; }
  static bool IsRtl() { return UiRtl(); }
  std::wstring ProblemText() const;       // why the file cannot be used ("" when fine)

  void OnOpened() override;
  bool OnEnter() override { enterPressed_ = true; Invalidate(); return true; }
  bool NeedsContinuousFrames() const override { return view_.Playing() || Busy(); }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
  void OnDeviceReset(bool rebuilt) override;
  bool ConsumeEnter() { const bool e = enterPressed_; enterPressed_ = false; return e; }

  // ---- drawing helpers
  struct WellGeom { Rect well, image; float scale = 1; };       // scale: screen DIP per video pixel
  void BeginFrame(Ui& ui);                                       // per-frame view update (call first in Draw)
  WellGeom DrawWell(Ui& ui, const Rect& well);
  Rect VideoToScreen(const WellGeom& g, float x, float y, float w, float h) const { return {g.image.x + x * g.scale, g.image.y + y * g.scale, w * g.scale, h * g.scale}; }
  // Keyboard sink over the whole well (Space, frame stepping). Call AFTER the overlay widgets: the first Behavior to see a press wins.
  struct SinkOptions { bool clickToggles = true; bool arrowsStep = true; double end = -1, from = 0; };
  bool DrawWellSink(Ui& ui, const WellGeom& g, const SinkOptions& o);
  bool SinkFocused(Ui& ui) { return ui.Focused(ui.Id("wellsink")); }
  // play / pause, time readout, frame counter and the two frame-step buttons
  void DrawTransport(Ui& ui, const Rect& row, double end = -1, double from = 0, bool showFrames = true);
  // timeline without handles: scrubbing + preview. Returns true when the playhead moved.
  bool DrawScrubber(Ui& ui, const Rect& r, const std::vector<double>* markers = nullptr);
  void DrawDoneNote(Ui& ui, const Rect& content, const std::vector<std::wstring>& lines);

  void ApplySpeedToView(double s) { view_.SetSpeed(s); }
  // Test hook (FILO_VID_SELFTEST=<log file>): drives the preview through seek / step / play / pause / speed and logs what happened.
  void MaybeSelfTest();
  std::wstring TimeFull(double t) const { return FormatTime(t, true); }
};

// "m:ss.cc" mono text helper
inline TextStyle MonoStyle(float size = 14.f, int weight = 500, Align a = Align::Start) { TextStyle s; s.size = size; s.weight = weight; s.mono = true; s.align = a; return s; }

} // namespace vid
} // namespace filo::tools
