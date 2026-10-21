// ToolDialog: base class of every Filo tool window. Adds to DialogWindow: running a Job with live progress / cancel /
// success / error states, async shell thumbnails, saved presets, and the standard footer (status text + action button).
#pragma once
#include "ui/dialog_window.h"
#include "core/job.h"
#include "core/json.h"
#include "registry/selection.h"
#include <functional>
#include <unordered_map>

namespace filo::tools {

using ui::Rect;
using ui::Ui;

constexpr UINT WM_FILO_TOOL_JOB = WM_APP + 0x50;      // wParam = job id
constexpr UINT WM_FILO_TOOL_THUMB = WM_APP + 0x51;    // lParam = heap ThumbPayload*

class ToolDialog : public ui::DialogWindow {
 public:
  ~ToolDialog() override;
  // Called by tool registrations: creates the dialog (self-owning; deletes itself when closed) near the cursor.
  template <class T, class... A> static T* Launch(A&&... a) {
    T* d = new T(std::forward<A>(a)...);
    return OpenDialog(d) ? d : nullptr;
  }
  // Takes ownership of `d`: positions it near `anchor` (default: the cursor), shows it (or keeps it hidden for visual tests)
  // and deletes it when closed. Returns false (and deletes `d`) when the window could not be created.
  // Headless end-to-end hook: applies `preset` (same JSON the dialog saves/loads), presses the primary action and pumps messages until
  // the job finishes. Used by `Filo.exe --tool-run` and by tests. Returns false when no job was started or on timeout.
  bool RunForTest(const Json& preset, double timeoutSec, JobResult* result);
  static bool OpenDialog(ToolDialog* d, bool show = true, const POINT* anchor = nullptr) {
    d->onClosed = [d] { delete d; };
    POINT p{}; if (anchor) p = *anchor; else GetCursorPos(&p);
    ui::DialogOptions o = d->Options(); o.glassCloseButton=true;
    if (!d->Open(o, p, show)) { delete d; return false; }
    return true;
  }

 protected:
  virtual ui::DialogOptions Options() const = 0;
  virtual std::string ToolId() const = 0;           // key into Settings.toolPresets
  virtual Json SavePreset() const { return Json::MakeObject(); }
  virtual void LoadPreset(const Json&) {}
  virtual void TestApply() {}                       // press the primary action (start the job) without any UI
  void OnOpened() override;
  void OnCloseRequested() override;
  bool CanClose() const override { return !Busy(); }

  // ---- jobs
  enum class JobPhase { Idle, Running, Done, Failed };
  void RunJob(const std::wstring& title, std::function<JobResult(JobContext&)> work);
  bool Busy() const { return phase_ == JobPhase::Running; }
  JobPhase Phase() const { return phase_; }
  const JobResult& LastResult() const { return result_; }
  // Draws the progress / done / error panel over `area` when a job is active or finished. Returns true while it owns input
  // (the caller should skip drawing its own widgets for that frame).
  bool DrawJobPanel(Ui& ui, const Rect& area);
  void ResetJob() { phase_ = JobPhase::Idle; result_ = {}; }

  // ---- footer: divider + status text (left) + action button (right). Returns true when the action button was pressed.
  struct FooterInfo {
    std::wstring status;               // e.g. "Add at least one bleep range."
    bool statusIsError = false;
    std::wstring action;               // primary button label ("Apply", "Join Videos")
    bool actionEnabled = true;
    std::wstring secondary;            // optional second button on the left of the action ("Reset", "Original")
    bool secondaryEnabled = true;
  };
  struct FooterResult { bool action = false, secondary = false; };
  FooterResult DrawFooter(Ui& ui, const Rect& content, const FooterInfo& f);
  static Rect FooterRect(const Rect& content);        // the footer area (bottom of `content`)
  static Rect BodyAbove(const Rect& content);         // content minus the footer

  // ---- thumbnails (async shell thumbnails; falls back to the file icon)
  ID2D1Bitmap* Thumb(const std::filesystem::path& p, int sidePx = 160);
  void SetThumbOverride(const std::filesystem::path& p, int w, int h, const uint8_t* bgraPremul);   // e.g. a decoded video frame

  LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp) override;
  void OnDeviceReset(bool rebuilt) override;
  bool NeedsContinuousFrames() const override { return phase_ == JobPhase::Running; }

  // Path -> "photo.png" style display name
  static std::wstring NameOf(const std::filesystem::path& p) { return p.filename().wstring(); }
  void Reveal(const std::filesystem::path& p);

  Selection selection_;

 private:
  struct ThumbEntry { std::vector<uint8_t> px; int w = 0, h = 0; ComPtr<ID2D1Bitmap> bmp; bool requested = false; bool isIcon = false; };
  std::unordered_map<std::wstring, ThumbEntry> thumbs_;
  JobPhase phase_ = JobPhase::Idle;
  uint64_t jobId_ = 0;
  int jobListener_ = 0;
  JobResult result_;
  double progress_ = 0;
  std::wstring stage_, jobTitle_;
  double doneAt_ = 0;
};

} // namespace filo::tools
