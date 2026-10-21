// Job system: every long operation runs off the UI thread as a Job with progress, cancellation and cleanup.
#pragma once
#include "core/util.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace filo {

enum class JobState { Pending, Preparing, Running, Finalizing, Completed, Failed, Cancelled };
const wchar_t* JobStateName(JobState s);

struct JobResult {
  bool ok = false;
  std::vector<fs::path> outputs;
  std::wstring error;                 // user-facing, never contains file contents
  std::vector<std::wstring> warnings; // e.g. "Transparency was flattened (JPG has no alpha)"
  static JobResult Ok(std::vector<fs::path> outs = {}) { JobResult r; r.ok = true; r.outputs = std::move(outs); return r; }
  static JobResult Fail(std::wstring why) { JobResult r; r.error = std::move(why); return r; }
};

class JobContext {
 public:
  uint64_t Id() const { return id_; }
  bool Cancelled() const { return cancel_.load(std::memory_order_relaxed); }
  const std::atomic<bool>* CancelFlag() const { return &cancel_; }
  void SetProgress(double p);                        // 0..1 (mapped into the current progress window), thread-safe
  // Multi-step jobs: later SetProgress(p) calls report lo + (hi-lo)*p, so engines that count 0..1 per file compose cleanly.
  void SetProgressWindow(double lo, double hi) { progLo_ = lo; progHi_ = hi; }
  void SetStage(JobState s, const std::wstring& text = L"");
  void AddWarning(std::wstring w);
  fs::path TempDir();                                // lazily created per-job scratch dir (deleted when the job ends)
 private:
  friend class JobManager;
  uint64_t id_ = 0;
  std::atomic<bool> cancel_{false};
  std::atomic<double> progress_{0};
  double progLo_ = 0, progHi_ = 1;
  std::mutex mu_;
  JobState state_ = JobState::Pending;
  std::wstring stage_;
  std::vector<std::wstring> warnings_;
  fs::path temp_;
  std::function<void()> notify_;
};

struct JobSpec {
  std::wstring operation;                            // "convert", "compress", "tool:trim-video", ...
  std::wstring title;                                // shown in notifications: "photo.png -> WEBP"
  std::vector<fs::path> inputs;
  std::function<JobResult(JobContext&)> work;
};

struct JobInfo {
  uint64_t id = 0;
  JobState state = JobState::Pending;
  double progress = 0;
  std::wstring stage;
  std::wstring operation, title;
  std::vector<fs::path> inputs;
  JobResult result;
  double startedAt = 0, finishedAt = 0;              // NowSeconds()
  bool Finished() const { return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled; }
};

class JobManager {
 public:
  static JobManager& Get();
  uint64_t Submit(JobSpec spec);
  void Cancel(uint64_t id);
  void CancelAll();
  std::optional<JobInfo> Info(uint64_t id);
  std::vector<JobInfo> All();
  bool WaitFor(uint64_t id, DWORD timeoutMs = INFINITE);   // for tests / CLI
  void SetMaxConcurrent(int n);
  // Listener is invoked on worker threads whenever a job changes (state/progress throttled to ~20/s).
  int AddListener(std::function<void(const JobInfo&)> cb);
  void RemoveListener(int id);
  void Shutdown();                                        // cancel everything and join workers
  void PurgeFinished(size_t keep = 50);

 private:
  JobManager() = default;
  struct Entry {
    JobSpec spec;
    JobInfo info;
    std::unique_ptr<JobContext> ctx;
  };
  void WorkerLoop();
  void EnsureWorkers();
  void Notify(Entry& e, bool force);

  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable doneCv_;
  std::map<uint64_t, std::unique_ptr<Entry>> jobs_;
  std::deque<uint64_t> queue_;
  std::vector<std::thread> workers_;
  std::vector<std::pair<int, std::function<void(const JobInfo&)>>> listeners_;
  uint64_t nextId_ = 1;
  int nextListener_ = 1;
  int maxConcurrent_ = 2;
  int running_ = 0;
  bool stop_ = false;
};

} // namespace filo
