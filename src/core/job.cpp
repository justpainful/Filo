#include "core/job.h"
#include "core/log.h"
#include <rpc.h>
#include <algorithm>

#pragma comment(lib, "rpcrt4.lib")

namespace filo {

const wchar_t* JobStateName(JobState s) {
  switch (s) {
    case JobState::Pending: return L"Pending";
    case JobState::Preparing: return L"Preparing";
    case JobState::Running: return L"Running";
    case JobState::Finalizing: return L"Finalizing";
    case JobState::Completed: return L"Completed";
    case JobState::Failed: return L"Failed";
    case JobState::Cancelled: return L"Cancelled";
  }
  return L"?";
}

void JobContext::SetProgress(double p) {
  progress_.store(progLo_ + (progHi_ - progLo_) * std::clamp(p, 0.0, 1.0));
  if (notify_) notify_();
}
void JobContext::SetStage(JobState s, const std::wstring& text) {
  { std::lock_guard lk(mu_); state_ = s; stage_ = text; }
  if (notify_) notify_();
}
void JobContext::AddWarning(std::wstring w) { std::lock_guard lk(mu_); warnings_.push_back(std::move(w)); }

fs::path JobContext::TempDir() {
  std::lock_guard lk(mu_);
  if (temp_.empty()) {
    UUID u; UuidCreate(&u);
    wchar_t* s = nullptr; UuidToStringW(&u, (RPC_WSTR*)&s);
    temp_ = ::filo::TempDir() / (std::wstring(L"job-") + (s ? s : L"x"));
    if (s) RpcStringFreeW((RPC_WSTR*)&s);
    std::error_code ec; fs::create_directories(temp_, ec);
  }
  return temp_;
}

JobManager& JobManager::Get() { static JobManager* m = new JobManager(); return *m; }

void JobManager::EnsureWorkers() {
  // called with mu_ held
  while ((int)workers_.size() < maxConcurrent_) workers_.emplace_back([this] { WorkerLoop(); });
}

void JobManager::SetMaxConcurrent(int n) {
  std::lock_guard lk(mu_);
  maxConcurrent_ = std::clamp(n, 1, 16);
  EnsureWorkers();
  cv_.notify_all();
}

uint64_t JobManager::Submit(JobSpec spec) {
  std::lock_guard lk(mu_);
  auto e = std::make_unique<Entry>();
  e->info.id = nextId_++;
  e->info.operation = spec.operation; e->info.title = spec.title; e->info.inputs = spec.inputs;
  e->ctx = std::make_unique<JobContext>();
  e->ctx->id_ = e->info.id;
  e->spec = std::move(spec);
  uint64_t id = e->info.id;
  Entry* raw = e.get();
  raw->ctx->notify_ = [this, raw] { Notify(*raw, false); };
  jobs_[id] = std::move(e);
  queue_.push_back(id);
  EnsureWorkers();
  cv_.notify_one();
  return id;
}

void JobManager::Cancel(uint64_t id) {
  std::lock_guard lk(mu_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return;
  it->second->ctx->cancel_ = true;
  // if still queued, finish it right away
  auto q = std::find(queue_.begin(), queue_.end(), id);
  if (q != queue_.end()) {
    queue_.erase(q);
    it->second->info.state = JobState::Cancelled;
    it->second->info.finishedAt = NowSeconds();
    Notify(*it->second, true);
    doneCv_.notify_all();
  }
}
void JobManager::CancelAll() {
  std::vector<uint64_t> ids;
  { std::lock_guard lk(mu_); for (auto& [id, e] : jobs_) if (!e->info.Finished()) ids.push_back(id); }
  for (auto id : ids) Cancel(id);
}

std::optional<JobInfo> JobManager::Info(uint64_t id) {
  std::lock_guard lk(mu_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::nullopt;
  JobInfo i = it->second->info;
  if (!i.Finished()) { i.progress = it->second->ctx->progress_.load(); std::lock_guard l2(it->second->ctx->mu_); i.state = it->second->ctx->state_ == JobState::Pending ? i.state : it->second->ctx->state_; i.stage = it->second->ctx->stage_; }
  return i;
}
std::vector<JobInfo> JobManager::All() {
  std::vector<uint64_t> ids;
  { std::lock_guard lk(mu_); for (auto& [id, e] : jobs_) ids.push_back(id); }
  std::vector<JobInfo> out;
  for (auto id : ids) if (auto i = Info(id)) out.push_back(*i);
  return out;
}

bool JobManager::WaitFor(uint64_t id, DWORD timeoutMs) {
  std::unique_lock lk(mu_);
  auto pred = [&] { auto it = jobs_.find(id); return it == jobs_.end() || it->second->info.Finished(); };
  if (timeoutMs == INFINITE) { doneCv_.wait(lk, pred); return true; }
  return doneCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
}

int JobManager::AddListener(std::function<void(const JobInfo&)> cb) {
  std::lock_guard lk(mu_); listeners_.emplace_back(nextListener_, std::move(cb)); return nextListener_++;
}
void JobManager::RemoveListener(int id) {
  std::lock_guard lk(mu_);
  listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](auto& p) { return p.first == id; }), listeners_.end());
}

void JobManager::Notify(Entry& e, bool force) {
  // Throttle progress chatter; state changes (force) always go out.
  static thread_local double last = 0;
  double now = NowSeconds();
  if (!force && now - last < 0.05) return;
  last = now;
  JobInfo info = e.info;
  if (!info.Finished()) {
    info.progress = e.ctx->progress_.load();
    std::lock_guard lk(e.ctx->mu_);
    if (e.ctx->state_ != JobState::Pending) info.state = e.ctx->state_;
    info.stage = e.ctx->stage_;
  }
  std::vector<std::function<void(const JobInfo&)>> cbs;
  { std::lock_guard lk(mu_); for (auto& l : listeners_) cbs.push_back(l.second); }
  for (auto& cb : cbs) cb(info);
}

void JobManager::WorkerLoop() {
  for (;;) {
    Entry* e = nullptr;
    {
      std::unique_lock lk(mu_);
      cv_.wait(lk, [&] { return stop_ || (!queue_.empty() && running_ < maxConcurrent_); });
      if (stop_) return;
      uint64_t id = queue_.front(); queue_.pop_front();
      e = jobs_[id].get();
      e->info.state = JobState::Preparing; e->info.startedAt = NowSeconds();
      ++running_;
    }
    e->ctx->SetStage(JobState::Preparing);
    Notify(*e, true);
    JobResult result;
    try {
      e->ctx->SetStage(JobState::Running);
      result = e->spec.work(*e->ctx);
    } catch (const std::exception& ex) {
      result = JobResult::Fail(L"Unexpected error: " + Widen(ex.what()));
      FILO_LOG(Error, "job", "job %llu threw: %s", (unsigned long long)e->info.id, ex.what());
    } catch (...) {
      result = JobResult::Fail(L"Unexpected error");
    }
    {
      std::lock_guard lk(e->ctx->mu_);
      for (auto& w : e->ctx->warnings_) result.warnings.push_back(w);
    }
    JobState final = e->ctx->Cancelled() ? JobState::Cancelled : (result.ok ? JobState::Completed : JobState::Failed);
    if (final == JobState::Cancelled) { result.ok = false; if (result.error.empty()) result.error = L"Cancelled"; }
    // cleanup scratch dir
    fs::path tmp;
    { std::lock_guard lk(e->ctx->mu_); tmp = e->ctx->temp_; }
    if (!tmp.empty()) { std::error_code ec; fs::remove_all(tmp, ec); }
    {
      std::lock_guard lk(mu_);
      e->info.result = std::move(result);
      e->info.state = final;
      e->info.progress = final == JobState::Completed ? 1.0 : e->ctx->progress_.load();
      e->info.finishedAt = NowSeconds();
      --running_;
    }
    Notify(*e, true);
    doneCv_.notify_all();
    cv_.notify_all();
  }
}

void JobManager::PurgeFinished(size_t keep) {
  std::lock_guard lk(mu_);
  std::vector<uint64_t> done;
  for (auto& [id, e] : jobs_) if (e->info.Finished()) done.push_back(id);
  if (done.size() <= keep) return;
  for (size_t i = 0; i + keep < done.size(); ++i) jobs_.erase(done[i]);
}

void JobManager::Shutdown() {
  CancelAll();
  std::vector<std::thread> ws;
  { std::lock_guard lk(mu_); stop_ = true; ws.swap(workers_); }
  cv_.notify_all();
  for (auto& t : ws) if (t.joinable()) t.join();
  { std::lock_guard lk(mu_); stop_ = false; }
}

} // namespace filo
