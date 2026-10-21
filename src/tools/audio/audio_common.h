// Shared base class + widgets of the Audio tool group (Normalize, Visualizer, Trim, Channels, Bleep).
// Layout numbers come from reference/audio-tools.png (2x retina, dip = px / 2): header-row inset 16.5, inner section inset 23.5,
// footer = divider + a 26 dip button whose centre sits 33 dip above the window bottom (67 dip footer, 100 dip with a status line).
#pragma once
#include "tools/tools.h"
#include "tools/media_player.h"
#include "engines/media/media_ops.h"
#include "engines/media/media_info.h"
#include "core/output.h"
#include "core/settings.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace filo::tools {

ToolDialog* MakeNormalizeVolume(const Selection& sel);
ToolDialog* MakeAudioVisualizer(const Selection& sel);
ToolDialog* MakeTrimAudio(const Selection& sel);
ToolDialog* MakeConvertChannels(const Selection& sel);
ToolDialog* MakeBleepAudio(const Selection& sel);

namespace audio {

using namespace filo::ds;
using namespace filo::ui;

constexpr float kInset = 16.5f;     // title-row / lane inset
constexpr float kInner = 23.5f;     // inner section inset
constexpr float kBtnH = 26.f;

struct WaveData {
  media::Waveform wf;
  float norm = 1.f;                 // loudest bucket (bars are scaled so it reaches the lane's height fraction)
  bool Ok() const { return wf.ok && !wf.peaks.empty(); }
  void Set(media::Waveform&& w);
  float PeakAt(double t) const;     // normalised 0..1 peak at time t
  float PeakRange(double t0, double t1) const;
};

class AudioDialog : public ToolDialog {
 public:
  ~AudioDialog() override;

 protected:
  static constexpr UINT WM_AUDIO_ASYNC = WM_APP + 0x71;

  virtual bool CanApply() const = 0;
  virtual void Apply() = 0;
  virtual void OnWaveLoaded() {}
  virtual void TestPlay() {}                      // visual tests (FILO_TEST_PLAY=ms): start playback, wait, then the screenshot is taken
  virtual void Begin() {}                         // start the async loads (called once the window exists)
  void OnOpened() final;
  void TestApply() override { if (CanApply()) Apply(); }
  bool OnEnter() override { if (Phase() == JobPhase::Idle && CanApply()) { Apply(); return true; } return false; }
  void OnCloseRequested() override { StopPlayback(); ToolDialog::OnCloseRequested(); }
  void OnTick(double now) override;
  bool NeedsContinuousFrames() const override { return Busy() || (player_ && player_->Playing()); }
  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;

  static bool RtlSetting() { return _wgetenv(L"FILO_TEST_RTL") != nullptr || SettingsStore::Get().Snapshot().language == L"ar"; }
  fs::path Path() const { return selection_.items.empty() ? fs::path() : selection_.items[0].path; }
  bool Locked() const { return Phase() != JobPhase::Idle; }

  // ---- background work: `work` runs on a worker thread and returns the closure to execute on the UI thread.
  void RunAsync(std::function<std::function<void()>()> work);
  void LoadWave(int buckets);                     // decodes the waveform of Path() into wave_

  // ---- reference-style widgets
  // Footer exactly like the reference: divider, optional status line, small muted action button (right). Returns true on press.
  bool Footer(Ui& ui, const Rect& body, const std::wstring& status, bool statusIsError, const std::wstring& action, bool enabled);
  // Rounded button with a filled glyph (play triangle: 22 dip icon box = the reference's 11 x 13 dip triangle) + label.
  bool GlyphButton(Ui& ui, const Rect& r, const std::wstring& label, const char* icon, bool enabled = true);
  // Bare glyph (no background), e.g. the play triangles next to the channel preview rows.
  bool GlyphOnly(Ui& ui, const Rect& r, const char* icon, float size, bool enabled = true);
  // Info text colour used by the reference for hints / readouts (warm, more saturated than body text).
  static Color Info(const Palette& pal) { return pal.heading; }
  // Mirrors an LTR layout rect inside the window body for RTL.
  static Rect Mir(Ui& ui, const Rect& body, Rect r) { return ui.Mirror(r, body); }

  // ---- playback (the MediaPlayer is opened on worker threads: Open() probes the file with ffprobe)
  void PreparePlayer(const fs::path& file);          // async: opens `file` into player_ (source playback: Trim)
  bool PlayerReady() const { return player_ && player_->IsOpen(); }
  bool PlayRange(double from, double to, double posOffset, const std::wstring& tag);   // on the opened player
  void StopPlayback();
  bool Playing() const { return player_ && player_->Playing() && !playTag_.empty(); }
  bool PlayingTag(const std::wstring& tag) const { return Playing() && playTag_ == tag; }
  double PlayPos() { return player_ ? playOffset_ + player_->Position() : playOffset_; }
  fs::path NewTempWav();
  // Renders a preview wav on a worker thread (`render` gets the destination), opens it and plays it. `tag` marks the owning control.
  void RenderAndPlay(const std::wstring& tag, double posOffset, std::function<EngineResult(JobContext&, const fs::path&)> render);
  bool Rendering() const { return !renderTag_.empty(); }

  WaveData wave_;
  bool waveLoading_ = false;
  std::wstring waveError_, notice_;
  std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
  std::unique_ptr<MediaPlayer> player_;
  std::wstring playTag_, renderTag_;
  double playOffset_ = 0;

 private:
  void Settle(double timeoutSec);
  int pending_ = 0;
  std::vector<fs::path> temps_;
  int tempCounter_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------- widgets
struct BarStyle { float barW = 2.2f, pitch = 3.75f; float radius = 0.8f; };
// Thin vertical bars (peaks of `wd` over [0,dur] mapped on r.x..r.right). Bars outside [selX0, selX1] use `outside`.
void DrawBars(Painter& p, const Rect& r, const WaveData& wd, double dur, const Color& c, const BarStyle& bs, float heightFrac,
              float selX0 = -1e9f, float selX1 = 1e9f, const Color* outside = nullptr);
// Same bars for an arbitrary peak vector (Convert preview rows); `scale` multiplies the peaks.
void DrawPeakBars(Painter& p, const Rect& r, const std::vector<float>& peaks, float scale, float norm, const Color& c, const BarStyle& bs, float heightFrac);

struct TrimState { int drag = 0; float grab = 0; int last = 1; };
struct TrimEdit { bool changed = false; double seek = -1; };
// The Trim Audio control (accent-bordered rounded box, thin bars, solid end blocks with a white grip, white playhead).
TrimEdit TrimBox(Ui& ui, const Rect& box, const WaveData& wd, double dur, double& start, double& end, double playhead, bool enabled, TrimState& st);

struct BleepState { int mode = 0; int idx = -1; double anchor = 0, grab = 0; int selected = -1; int audioDrag = 0; };
struct BleepEdit { bool changed = false; double seek = -1; };
// The two Bleep lanes: range lane (drag to add / edges to resize / body to move / Delete removes) and the audio lane (click = seek).
BleepEdit BleepLanes(Ui& ui, const Rect& rangeLane, const Rect& audioLane, const WaveData& wd, double dur,
                     std::vector<media::BleepRange>& ranges, BleepState& st, double playhead, bool enabled);

} // namespace audio
} // namespace filo::tools
