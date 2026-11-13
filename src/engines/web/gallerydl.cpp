// gallery-dl runner: Instagram profiles, posts, reels and stories. gallery-dl (GPL-2.0) is a separate program, started without a shell and
// killed with Filo. It is only asked what a post contains: it prints one JSON array per file (see kGalleryFormat), this file groups the
// files into posts, and Filo fetches the files itself from the addresses it reports. Naming, resume, the post-text file and parallel
// downloads therefore work exactly like for every other site.
#include "engines/web/web.h"
#include "engines/web/web_internal.h"
#include "core/log.h"
#include "core/process.h"
#include <algorithm>
#include <ctime>
#include <set>
#include <thread>

namespace filo::web {

fs::path GalleryDlPath() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(L"FILO_GALLERY_DL", buf, (DWORD)std::size(buf));
  if (n > 0 && n < std::size(buf)) {
    std::error_code ec;
    fs::path p(buf);
    if (fs::is_regular_file(p, ec)) return p;
  }
  return FindTool(L"gallery-dl.exe");
}
bool GalleryDlAvailable() { return !GalleryDlPath().empty(); }

namespace internal {

namespace {

// Printed once per file, before gallery-dl would download it (-N: nothing is downloaded). Every value is JSON, so captions with line
// breaks, quotes or any script arrive intact; fields a post lacks print as null.
const wchar_t* const kGalleryFormat =
    L"prepare:[{post_shortcode!j},{post_id!j},{num!j},{count!j},\"{date:%Y-%m-%dT%H:%M:%S}\",{username!j},{fullname!j},{likes!j},"
    L"{description!j},{width!j},{height!j},{_url!j},{extension!j},{type!j}]";

int64_t ParseIsoUtc(const std::string& s) {
  int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
  if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6 || Y < 1970) return 0;
  tm t{};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = sec;
  const __time64_t v = _mkgmtime64(&t);
  return v < 0 ? 0 : (int64_t)v;
}

std::wstring FirstLine(const std::wstring& text, size_t maxLen) {
  size_t b = 0;
  while (b < text.size() && (text[b] == L'\r' || text[b] == L'\n' || text[b] == L' ')) ++b;
  size_t e = text.find_first_of(L"\r\n", b);
  std::wstring s = text.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
  while (!s.empty() && s.back() == L' ') s.pop_back();
  if (s.size() > maxLen) { s.resize(maxLen); s += L"\x2026"; }
  return s;
}

bool IsVideoExt(const std::string& ext) { return ext == "mp4" || ext == "mov" || ext == "webm" || ext == "m4v" || ext == "mkv"; }
bool IsAudioExt(const std::string& ext) { return ext == "m4a" || ext == "mp3" || ext == "aac" || ext == "ogg" || ext == "opus"; }

std::vector<std::wstring> GalleryArgs() {
  std::vector<std::wstring> a = {L"--config-ignore", L"-w"};
  const CookieChoice c = CurrentCookies();
  std::error_code ec;
  switch (c.source) {
    case CookieSource::Firefox: a.insert(a.end(), {L"--cookies-from-browser", L"firefox"}); break;
    case CookieSource::Chrome: a.insert(a.end(), {L"--cookies-from-browser", L"chrome"}); break;
    case CookieSource::Edge: a.insert(a.end(), {L"--cookies-from-browser", L"edge"}); break;
    case CookieSource::File: if (!c.file.empty() && fs::is_regular_file(c.file, ec)) a.insert(a.end(), {L"--cookies", c.file.wstring()}); break;
    default: break;
  }
  return a;
}

const wchar_t* const kNeedsSignIn = L"Instagram only shows this to signed-in visitors. Sign in to Instagram in Firefox (Chrome and Edge often cannot be read), choose it under Sign-in and read it again.";

}  // namespace

bool UsesGalleryDl(const Platform& platform) { return platform.id == "instagram"; }

bool ParseGalleryLine(const std::string& line, GalleryFile& f) {
  if (line.size() < 24 || line[0] != '[') return false;
  auto j = Json::Parse(line);
  if (!j || !j->IsArray() || j->Size() < 14) return false;
  const auto& a = j->Elements();
  f = GalleryFile();
  f.shortcode = a[0].StringOr("");
  f.num = (int)a[2].AsInt(0);
  f.count = (int)a[3].AsInt(0);
  f.timestamp = ParseIsoUtc(a[4].StringOr(""));
  f.user = a[5].StringOr("");
  f.fullName = Widen(a[6].StringOr(""));
  f.likes = a[7].IsNumber() ? a[7].AsInt(-1) : -1;
  f.text = Widen(a[8].StringOr(""));
  f.width = (int)a[9].AsInt(0);
  f.height = (int)a[10].AsInt(0);
  f.url = a[11].StringOr("");
  f.ext = a[12].StringOr("");
  for (auto& ch : f.ext) ch = (char)tolower((unsigned char)ch);
  f.type = a[13].StringOr("");
  return !f.shortcode.empty() && f.url.rfind("http", 0) == 0;
}

void AddGalleryFile(AccountEntry& e, const GalleryFile& f) {
  if (e.id.empty()) {
    e = AccountEntry();
    e.id = f.shortcode;
    e.url = std::string("https://www.instagram.com/") + (f.type == "reel" ? "reel" : "p") + "/" + f.shortcode + "/";
    e.authorHandle = f.user;
    e.authorName = f.fullName.empty() ? Widen(f.user) : f.fullName;
    e.text = f.text;
    e.title = FirstLine(f.text, 80);
    e.timestamp = f.timestamp;
    e.likes = f.likes;
    e.hasMedia = true;
    e.expires = true;
  }
  Item it;
  it.kind = IsVideoExt(f.ext) ? ItemKind::Video : IsAudioExt(f.ext) ? ItemKind::Audio : ItemKind::Image;
  it.index = (int)e.media.size() + 1;
  it.id = e.id + "_" + std::to_string(it.index);
  it.url = f.url;
  it.ext = f.ext.empty() ? (it.kind == ItemKind::Video ? "mp4" : "jpg") : f.ext;
  it.width = f.width; it.height = f.height;
  it.thumbUrl = it.kind == ItemKind::Image ? f.url : "";
  it.direct = it.kind == ItemKind::Video;                         // fetched from its address, no yt-dlp run
  if (it.kind == ItemKind::Video) it.headers.emplace_back("User-Agent", "Mozilla/5.0");
  if (f.width > 0 && f.height > 0) it.detail = std::to_wstring(f.width) + L" \x00D7 " + std::to_wstring(f.height);
  e.media.push_back(std::move(it));
}

std::wstring GalleryErrorText(const std::string& err, const std::wstring& startError, bool& needsLogin) {
  if (!startError.empty()) return L"Filo could not start its Instagram reader: " + startError;
  std::string line, pos_line;
  size_t pos = 0;
  while (pos < err.size()) {
    size_t e = err.find('\n', pos);
    std::string l = err.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.find("[error]") != std::string::npos) line = l;
    else if (!l.empty() && l.find("[warning]") == std::string::npos && pos_line.empty() && l[0] != ' ') pos_line = l;
    pos = e == std::string::npos ? err.size() : e + 1;
  }
  if (line.empty()) line = pos_line;
  while (line.rfind('[', 0) == 0) {                                // "[instagram][error] NotFoundError: ..." -> "NotFoundError: ..."
    const size_t c = line.find(']');
    if (c == std::string::npos) break;
    line.erase(0, c + 1);
    while (!line.empty() && line[0] == ' ') line.erase(0, 1);
  }
  std::string low = line;
  for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
  const bool signedIn = CurrentCookies().source != CookieSource::None;
  auto has = [&](const char* s) { return low.find(s) != std::string::npos; };

  if (has("cookie") && (has("failed") || has("decrypt") || has("could not") || has("database") || has("permission") || has("locked")))
    return L"Filo could not read the browser's saved sign-in. Close that browser and try again, or choose Firefox or a cookies file.";
  if (has("429") || has("too many requests") || has("rate limit") || has("rate-limit"))
    return L"Instagram is limiting requests right now. Try again in a few minutes.";
  if (has("could not be found") || has("notfounderror") || has("404")) {
    if (!signedIn) { needsLogin = true; return kNeedsSignIn; }
    return L"Instagram did not show this account or post. Check the address; a private account can only be read when the signed-in account follows it.";
  }
  if (has("authorization") || has("private") || has("403")) {
    if (!signedIn) { needsLogin = true; return kNeedsSignIn; }
    return L"This account or post is private or restricted for the account you are signed in with.";
  }
  if (has("authentication") || has("login") || has("log in") || has("401") || has("checkpoint") || has("challenge") || has("consent")) {
    needsLogin = true;
    return signedIn ? L"Instagram rejected the saved sign-in. Sign in to Instagram in that browser again, then read the account again."
                    : std::wstring(kNeedsSignIn);
  }
  if (has("timed out") || has("getaddrinfo") || has("name resolution") || has("connectionerror") || has("connection aborted"))
    return L"Instagram could not be reached. Check the internet connection.";
  if (line.empty()) return L"";
  if (line.size() > 220) line.resize(220);
  return Widen(line);
}

// The standalone gallery-dl is a 64-bit Python bundle that needs Microsoft's Visual C++ runtime; Windows reports its absence as this exit code.
static std::wstring RunErrorText(const ProcessResult& pr, bool& needsLogin) {
  if (pr.started && pr.exitCode == 0xC0000135u)
    return L"The Instagram reader needs the Microsoft Visual C++ Redistributable. Install it from https://aka.ms/vs/17/release/vc_redist.x64.exe and try again.";
  return GalleryErrorText(pr.err, pr.startError, needsLogin);
}

void ListGallery(AccountInfo& ai, const ListOptions& opt, const std::atomic<bool>* cancel, const std::function<void(const AccountEntry&)>& onEntry) {
  PrepareEnvironment();
  const int cap = std::max(1, opt.cap);
  const int want = opt.limit > 0 ? std::min(opt.limit, cap) : cap;

  std::set<std::string> seen;
  std::atomic<bool> stop{false}, finished{false}, hitLimit{false};
  std::thread watcher([&] {                                  // the process runner polls one flag: fold the caller's cancel into it
    while (!finished.load()) { if (cancel && cancel->load()) stop = true; Sleep(40); }
  });

  AccountEntry cur;                                           // the post being collected
  auto flush = [&] {
    if (cur.id.empty()) return;
    AccountEntry e = std::move(cur);
    cur = AccountEntry();
    if (e.media.empty() || !seen.insert(e.id).second) return;
    if (ai.entries.empty() && !e.authorName.empty()) ai.name = e.authorName;
    ai.entries.push_back(std::move(e));
    if (onEntry) onEntry(ai.entries.back());
    if ((int)ai.entries.size() >= want) { hitLimit = true; if ((int)ai.entries.size() >= cap && opt.limit <= 0) ai.capped = true; stop = true; }
  };

  std::vector<std::wstring> a = GalleryArgs();
  a.insert(a.end(), {L"-N", kGalleryFormat, L"--"});
  a.push_back(Widen(ai.ref.listUrls.empty() ? std::string() : ai.ref.listUrls[0] + "posts/"));

  ProcessOptions po;
  po.exe = GalleryDlPath();
  po.args = std::move(a);
  po.cancel = &stop;
  po.lowPriority = false;
  po.timeoutMs = 4 * 60 * 60 * 1000;                          // gallery-dl pauses between Instagram requests on purpose
  po.captureLimit = 256u << 10;
  po.onStdoutLine = [&](const std::string& line) {
    if (stop) return;
    GalleryFile f;
    if (!ParseGalleryLine(line, f)) return;
    if (!cur.id.empty() && cur.id != f.shortcode) flush();   // a new post began: the previous one is complete
    if (stop) return;
    AddGalleryFile(cur, f);
    if (f.count > 0 && f.num >= f.count) flush();            // the last file of the post: show it at once
  };
  ProcessResult pr = RunProcess(po);
  if (!stop) flush();                                         // the end of the listing completes the last post (not after a cancel or limit)
  finished = true;
  watcher.join();

  ai.interrupted = cancel && cancel->load() && !hitLimit;
  ai.ok = !ai.entries.empty();
  if (!ai.ok && !ai.interrupted) {
    bool login = false;
    std::wstring text = RunErrorText(pr, login);
    ai.needsLogin = login;
    ai.error = !text.empty() ? text : L"No posts were found on this account.";
  }
}

LinkInfo ProbeGallery(const std::wstring& url, const std::atomic<bool>* cancel) {
  LinkInfo bad;
  bad.sourceUrl = url;
  bad.platform = DetectPlatform(url);
  if (!GalleryDlAvailable()) { bad.error = L"The Instagram reader (gallery-dl) is missing from this Filo installation."; return bad; }
  PrepareEnvironment();

  AccountEntry e;                                             // every file gallery-dl reports joins one LinkInfo (a story link holds several)
  std::vector<std::wstring> a = GalleryArgs();
  a.insert(a.end(), {L"-N", kGalleryFormat, L"--", url});
  ProcessOptions po;
  po.exe = GalleryDlPath();
  po.args = std::move(a);
  po.cancel = cancel;
  po.lowPriority = false;
  po.timeoutMs = 3 * 60 * 1000;
  po.captureLimit = 256u << 10;
  po.onStdoutLine = [&](const std::string& line) {
    GalleryFile f;
    if (ParseGalleryLine(line, f)) AddGalleryFile(e, f);
  };
  ProcessResult pr = RunProcess(po);
  if (pr.cancelled) { bad.error = L"Cancelled."; return bad; }
  if (pr.timedOut) { bad.error = L"The site took too long to answer."; return bad; }
  if (e.media.empty()) {
    bool login = false;
    const std::wstring text = RunErrorText(pr, login);
    bad.error = login ? L"This post needs a signed-in account. Choose a browser under Sign-in and try again." : text;
    if (bad.error.empty()) bad.error = L"No pictures or videos were found in this post.";
    return bad;
  }
  LinkInfo li = LinkInfoFromEntry(e, bad.platform);
  li.sourceUrl = url;
  return li;
}

}  // namespace internal
}  // namespace filo::web
