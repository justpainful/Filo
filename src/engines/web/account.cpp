// Accounts: list every post of a profile or channel with yt-dlp, then download the chosen posts one by one into a folder of their own.
// Posts already saved by an earlier run are remembered in the account folder, so running it again only fetches what is new.
#include "engines/web/web.h"
#include "engines/web/web_internal.h"
#include "core/log.h"
#include "core/process.h"
#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace filo::web {

namespace {

// The channel id of a YouTube channel ("UCLA_DiR1FfKNvjuUpBHmylQ") makes an ugly folder name: prefer the @handle or the channel name.
bool LooksLikeChannelId(const std::wstring& h) { return h.size() == 24 && h[0] == L'U' && h[1] == L'C'; }

}  // namespace

// ------------------------------------------------------------------------------------------------ listing
AccountInfo ListAccount(const std::wstring& url, const ListOptions& opt, const std::atomic<bool>* cancel,
                        const std::function<void(const AccountEntry&)>& onEntry) {
  AccountInfo ai;
  ai.sourceUrl = url;
  ai.ref = ClassifyAccount(url);
  ai.platform = ai.ref.platform;
  ai.handle = ai.ref.handle;
  ai.name = ai.handle;
  if (!ai.ref.is) { ai.error = L"This is not the address of an account or channel."; return ai; }
  if (!Available()) { ai.error = L"The downloader (yt-dlp) is missing from this Filo installation."; return ai; }
  internal::PrepareEnvironment();

  std::set<std::string> seen;
  std::atomic<bool> stop{false}, finished{false}, hitLimit{false};
  std::thread watcher([&] {                                  // the process runner polls one flag: fold the caller's cancel into it
    while (!finished.load()) { if (cancel && cancel->load()) stop = true; Sleep(40); }
  });
  std::wstring lastError;
  bool nameFromListing = false;
  const int cap = std::max(1, opt.cap);

  for (const std::string& listUrl : ai.ref.listUrls) {
    if (stop) break;
    const int want = opt.limit > 0 ? std::min(opt.limit, cap) : cap;
    const int left = want - (int)ai.entries.size();
    if (left <= 0) break;
    std::vector<std::wstring> a = internal::BaseArgs();
    a.insert(a.end(), {L"-j", L"--flat-playlist", L"--ignore-errors", L"--playlist-end", std::to_wstring(left)});
    if (opt.reposts) a.insert(a.end(), {L"--extractor-args", L"filotwitteruser:reposts=1"});
    a.push_back(L"--");
    a.push_back(Widen(listUrl));

    ProcessOptions po;
    po.exe = YtDlpPath();
    po.args = std::move(a);
    po.cancel = &stop;
    po.lowPriority = false;
    po.timeoutMs = 2 * 60 * 60 * 1000;
    po.captureLimit = 256u << 10;                           // the entries arrive through onStdoutLine; keep only error text
    po.onStdoutLine = [&](const std::string& line) {
      if (line.empty() || line[0] != '{' || stop) return;
      auto j = Json::Parse(line);
      if (!j) return;
      AccountEntry e;
      if (!ParseAccountEntry(*j, e) || !seen.insert(e.id).second) return;
      if (e.repost && !opt.reposts) return;
      if (!nameFromListing) {
        nameFromListing = true;
        std::wstring nm = Widen(j->Get("playlist_channel").StringOr(j->Get("playlist_uploader").StringOr("")));
        if (nm.empty()) nm = e.authorName;
        if (!nm.empty()) ai.name = nm;
        std::wstring pid = Widen(j->Get("playlist_uploader_id").StringOr(""));
        if (!pid.empty() && pid[0] == L'@') pid.erase(0, 1);
        if (LooksLikeChannelId(ai.handle)) ai.handle = !pid.empty() && !LooksLikeChannelId(pid) ? pid : SafePart(ai.name, 24);
      }
      ai.entries.push_back(e);
      if (onEntry) onEntry(ai.entries.back());
      if ((int)ai.entries.size() >= want) { hitLimit = true; if ((int)ai.entries.size() >= cap && opt.limit <= 0) ai.capped = true; stop = true; }
    };
    ProcessResult pr = RunProcess(po);
    if (pr.startError.size()) { lastError = internal::ErrorText({}, pr.startError); break; }
    if (!pr.err.empty()) {
      const std::string low = pr.err;
      std::string l2 = low; for (auto& ch : l2) ch = (char)tolower((unsigned char)ch);
      if (l2.find("login") != std::string::npos || l2.find("log in") != std::string::npos || l2.find("sign in") != std::string::npos ||
          l2.find("cookies") != std::string::npos || l2.find("rate-limit") != std::string::npos || l2.find("empty media response") != std::string::npos)
        ai.needsLogin = true;
      if (l2.find("unsupported url") != std::string::npos) lastError = L"Filo cannot read the posts of this kind of address yet.";
      else lastError = internal::ErrorText(pr.err, {});
    }
  }
  finished = true;
  watcher.join();

  ai.interrupted = cancel && cancel->load() && !hitLimit;
  ai.ok = !ai.entries.empty();
  if (!ai.ok && !ai.interrupted) {
    if (ai.needsLogin) ai.error = L"This account can only be read when you are signed in. Choose a browser under Sign-in and read it again.";
    else if (!lastError.empty()) ai.error = lastError;
    else ai.error = L"No posts were found on this account.";
  }
  return ai;
}

AccountInfo AccountFromJson(const Json& root, const std::wstring& url) {
  AccountInfo ai;
  ai.sourceUrl = url;
  ai.ref = ClassifyAccount(url);
  ai.platform = ai.ref.is ? ai.ref.platform : DetectPlatform(url);
  ai.handle = ai.ref.handle;
  ai.name = Widen(root.Get("_filo_account").Get("name").StringOr(Narrow(ai.handle)));
  for (const Json& e : root.Get("entries").Elements()) {
    AccountEntry en;
    if (ParseAccountEntry(e, en)) ai.entries.push_back(std::move(en));
  }
  ai.ok = !ai.entries.empty();
  return ai;
}

// ------------------------------------------------------------------------------------------------ downloading
namespace {

bool Transient(const std::wstring& err) {
  return err.find(L"limiting requests") != std::wstring::npos || err.find(L"could not be reached") != std::wstring::npos ||
         err.find(L"took too long") != std::wstring::npos;
}

// sleeps `seconds`, returning false when the job was cancelled meanwhile
bool Pause(double seconds, const std::atomic<bool>* cancel) {
  const double end = NowSeconds() + seconds;
  while (NowSeconds() < end) { if (cancel && cancel->load()) return false; Sleep(100); }
  return !(cancel && cancel->load());
}

}  // namespace

JobResult DownloadAccount(const AccountDownloadRequest& req, JobContext& ctx) {
  const AccountInfo& acc = req.account;
  std::error_code ec;
  fs::create_directories(req.destRoot, ec);
  if (!fs::is_directory(req.destRoot, ec)) return JobResult::Fail(L"The download folder cannot be used: " + req.destRoot.wstring());
  if (req.entries.empty()) return JobResult::Fail(L"Nothing is selected.");
  if (!req.videos && !req.pictures && !req.text && !req.sounds) return JobResult::Fail(L"Choose what to save: videos, pictures or text.");

  const fs::path folder = req.destRoot / AccountFolderName(acc.platform, acc.handle);
  fs::create_directories(folder, ec);
  if (!fs::is_directory(folder, ec)) return JobResult::Fail(L"Could not create the folder " + folder.wstring());

  // posts saved by an earlier run
  const fs::path archive = folder / L".filo-saved.txt";
  std::set<std::string> saved;
  if (req.skipDone) {
    std::ifstream in(archive);
    for (std::string line; std::getline(in, line);) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) saved.insert(line); }
  }
  std::vector<int> todo;
  int alreadySaved = 0;
  for (int i : req.entries) {
    if (i < 0 || i >= (int)acc.entries.size()) continue;
    if (req.skipDone && saved.count(acc.entries[i].id)) { ++alreadySaved; continue; }
    todo.push_back(i);
  }
  if (todo.empty()) {
    if (alreadySaved > 0) {
      JobResult r = JobResult::Ok({folder});
      r.warnings.push_back(L"All " + std::to_wstring(alreadySaved) + L" posts were saved before. Nothing new to download.");
      return r;
    }
    return JobResult::Fail(L"Nothing is selected.");
  }

  const int total = (int)todo.size();
  const int workers = std::clamp(req.parallel, 1, 4);
  const fs::path scratch = ctx.TempDir() / L"account";
  fs::create_directories(scratch, ec);

  std::mutex mu;                                              // guards everything below
  std::atomic<int> next{0};
  int done = 0, saveOk = 0, skippedKind = 0;
  std::vector<std::pair<std::string, std::wstring>> failures;
  bool anyCancelled = false;

  auto announce = [&] {                                       // callers hold `mu`
    ctx.SetProgress(std::min(0.99, (double)done / total));
    ctx.SetStage(JobState::Running, L"Downloading " + std::to_wstring(std::min(done + 1, total)) + L" of " + std::to_wstring(total) + L" posts");
  };
  { std::lock_guard lk(mu); announce(); }

  auto work = [&](int slot) {
    for (;;) {
      if (ctx.Cancelled()) return;
      const int k = next.fetch_add(1);
      if (k >= total) return;
      const AccountEntry& entry = acc.entries[todo[k]];
      std::wstring error;
      bool ok = false, nothingToSave = false;
      for (int attempt = 0; attempt < 3 && !ctx.Cancelled(); ++attempt) {
        LinkInfo li = entry.hasMedia ? LinkInfoFromEntry(entry, acc.platform) : Probe(Widen(entry.url), ctx.CancelFlag());
        if (ctx.Cancelled()) break;
        if (!li.ok) { error = li.error; if (Transient(error) && attempt < 2 && Pause(attempt ? 25 : 8, ctx.CancelFlag())) continue; break; }
        if (li.uploader.empty()) li.uploader = acc.handle;
        if (li.id.empty()) li.id = entry.id;

        DownloadRequest dr;
        dr.info = li;
        bool anyVisual = false;
        for (const Item& it : li.items) if (it.kind != ItemKind::Audio) anyVisual = true;
        for (size_t i = 0; i < li.items.size(); ++i) {
          const ItemKind kind = li.items[i].kind;
          const bool pick = kind == ItemKind::Video ? req.videos : kind == ItemKind::Image ? req.pictures : (req.sounds || !anyVisual);
          if (pick) dr.items.push_back((int)i);
        }
        dr.saveText = req.text;
        dr.quality = req.quality;
        dr.videoContainer = req.videoContainer;
        dr.destRoot = folder;
        dr.folderThreshold = req.folderThreshold;
        if (dr.items.empty() && !(dr.saveText && li.HasText())) { nothingToSave = true; break; }

        DownloadSink sink;
        sink.cancel = ctx.CancelFlag();
        sink.workDir = scratch / (L"w" + std::to_wstring(slot)) / (L"p" + std::to_wstring(k));
        JobResult r = DownloadPost(dr, sink);
        std::error_code rec; fs::remove_all(sink.workDir, rec);
        if (r.ok) { ok = true; break; }
        error = r.error;
        if (error == L"Cancelled.") break;
        if (Transient(error) && attempt < 2 && Pause(attempt ? 25 : 8, ctx.CancelFlag())) continue;
        break;
      }
      std::lock_guard lk(mu);
      ++done;
      if (ctx.Cancelled()) { anyCancelled = true; return; }
      if (ok) {
        ++saveOk;
        std::ofstream out(archive, std::ios::app);
        if (out) out << entry.id << "\n";
      } else if (nothingToSave) ++skippedKind;
      else failures.emplace_back(entry.id, error.empty() ? std::wstring(L"Unknown error.") : error);
      announce();
    }
  };

  std::vector<std::thread> pool;
  for (int w = 0; w < workers; ++w) pool.emplace_back(work, w);
  for (auto& t : pool) t.join();

  if (ctx.Cancelled() || anyCancelled) {
    JobResult r = JobResult::Fail(L"Cancelled. " + std::to_wstring(saveOk) + L" posts were saved; run it again to continue where it stopped.");
    return r;
  }
  ctx.SetStage(JobState::Finalizing, L"Saving");
  if (saveOk == 0 && skippedKind == 0 && !failures.empty()) return JobResult::Fail(failures.front().second);
  JobResult r = JobResult::Ok({folder});
  if (!failures.empty()) {
    std::wstring w = std::to_wstring(failures.size()) + (failures.size() == 1 ? L" post could not be saved: " : L" posts could not be saved. First problem: ") + failures.front().second;
    r.warnings.push_back(w);
  }
  if (skippedKind > 0) r.warnings.push_back(std::to_wstring(skippedKind) + L" posts had nothing of the chosen kinds.");
  if (alreadySaved > 0) r.warnings.push_back(std::to_wstring(alreadySaved) + L" posts were already saved before and were skipped.");
  ctx.SetProgress(1.0);
  FILO_LOG(Info, "web", "account %ls: %d saved, %zu failed, %d skipped", acc.handle.c_str(), saveOk, failures.size(), alreadySaved);
  return r;
}

}  // namespace filo::web
