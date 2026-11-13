// Link understanding: platform names, address extraction, probe-JSON parsing and file naming. No process or network access here
// (everything in this file is unit-tested without yt-dlp).
#include "engines/web/web.h"
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <ctime>

namespace filo::web {

// ------------------------------------------------------------------------------------------------ links
static std::wstring HostOf(const std::wstring& url) {
  size_t s = url.find(L"://");
  s = s == std::wstring::npos ? 0 : s + 3;
  size_t e = url.find_first_of(L"/?#:", s);
  std::wstring host = ToLower(url.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s));
  const size_t at = host.rfind(L'@');
  if (at != std::wstring::npos) host = host.substr(at + 1);
  return host;
}

static bool HostIs(const std::wstring& host, const wchar_t* domain) {
  const std::wstring d = domain;
  return host == d || (host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0 && host[host.size() - d.size() - 1] == L'.');
}

Platform DetectPlatform(const std::wstring& url) {
  const std::wstring h = HostOf(url);
  struct Row { const wchar_t* domain; const char* id; const wchar_t* name; };
  static const Row rows[] = {
    {L"tiktok.com", "tiktok", L"TikTok"}, {L"youtube.com", "youtube", L"YouTube"}, {L"youtu.be", "youtube", L"YouTube"},
    {L"x.com", "x", L"X"}, {L"twitter.com", "x", L"X"}, {L"t.co", "x", L"X"},
    {L"instagram.com", "instagram", L"Instagram"}, {L"facebook.com", "facebook", L"Facebook"}, {L"fb.watch", "facebook", L"Facebook"},
    {L"reddit.com", "reddit", L"Reddit"}, {L"redd.it", "reddit", L"Reddit"}, {L"pinterest.com", "pinterest", L"Pinterest"},
    {L"pin.it", "pinterest", L"Pinterest"}, {L"twitch.tv", "twitch", L"Twitch"}, {L"vimeo.com", "vimeo", L"Vimeo"},
    {L"snapchat.com", "snapchat", L"Snapchat"}, {L"threads.net", "threads", L"Threads"}, {L"threads.com", "threads", L"Threads"},
    {L"soundcloud.com", "soundcloud", L"SoundCloud"}, {L"bilibili.com", "bilibili", L"Bilibili"},
    {L"dailymotion.com", "dailymotion", L"Dailymotion"}, {L"telegram.org", "telegram", L"Telegram"}, {L"t.me", "telegram", L"Telegram"},
    {L"linkedin.com", "linkedin", L"LinkedIn"}, {L"tumblr.com", "tumblr", L"Tumblr"}, {L"kick.com", "kick", L"Kick"},
    {L"bandcamp.com", "bandcamp", L"Bandcamp"}, {L"bsky.app", "bluesky", L"Bluesky"}, {L"streamable.com", "streamable", L"Streamable"},
    {L"archive.org", "archive", L"Internet Archive"}, {L"rumble.com", "rumble", L"Rumble"}, {L"mixcloud.com", "mixcloud", L"Mixcloud"},
    {L"ok.ru", "okru", L"OK.ru"}, {L"vk.com", "vk", L"VK"},
  };
  for (const Row& r : rows) if (HostIs(h, r.domain)) return {r.id, r.name, true};
  // any other site: its registrable-looking name ("cdn.example.com" -> "example")
  std::vector<std::wstring> parts = Split(h, L'.');
  std::wstring label = parts.size() >= 2 ? parts[parts.size() - 2] : (parts.empty() ? L"web" : parts[0]);
  if (label.empty()) label = L"web";
  return {"web", SafePart(label, 14), false};
}

std::wstring ExtractUrl(const std::wstring& text) {
  size_t p = std::wstring::npos;
  for (const wchar_t* scheme : {L"https://", L"http://"}) {
    size_t q = text.find(scheme);
    if (q != std::wstring::npos && (p == std::wstring::npos || q < p)) p = q;
  }
  if (p == std::wstring::npos) {
    const std::wstring t = Trim(text);
    if (StartsWithI(t, L"www.") && t.find_first_of(L" \t\r\n") == std::wstring::npos) return L"https://" + t;
    if (t.find_first_of(L" \t\r\n\"'<>") == std::wstring::npos) {
      const size_t end = t.find_first_of(L"/?#");
      const std::wstring host = t.substr(0, end);
      const size_t dot = host.rfind(L'.');
      if (dot != std::wstring::npos && dot > 0 && dot + 1 < host.size() && host.find(L'@') == std::wstring::npos &&
          std::all_of(host.begin(), host.end(), [](wchar_t c) { return iswalnum(c) || c == L'.' || c == L'-'; })) return L"https://" + t;
    }
    return {};
  }
  size_t e = text.find_first_of(L" \t\r\n\"'<>", p);
  std::wstring url = text.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
  while (!url.empty() && wcschr(L".,;:!?)]}", url.back())) url.pop_back();
  return url.size() > 10 ? url : std::wstring();
}

// ------------------------------------------------------------------------------------------------ naming
std::wstring SafePart(std::wstring s, size_t maxLen) {
  std::wstring out;
  bool lastUnderscore = true;                       // swallows leading separators
  for (wchar_t c : s) {
    const bool keep = iswalnum(c) || c == L'-' || c > 0x7F;
    if (keep && !(c >= 0x2000 && c <= 0x206F) && c != 0x00A0) { out += c; lastUnderscore = false; }
    else if (!lastUnderscore) { out += L'_'; lastUnderscore = true; }
    if (out.size() >= maxLen) break;
  }
  while (!out.empty() && out.back() == L'_') out.pop_back();
  return out;
}

std::wstring BaseName(const LinkInfo& info) {
  std::wstring id = Widen(info.id);
  const bool numeric = !id.empty() && std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
  if (numeric && id.size() > 10) id = id.substr(id.size() - 10);       // long numeric post ids (TikTok, X): the last 10 digits stay unique
  id = SafePart(id, 12);
  std::wstring name = L"Filo_" + (info.platform.name.empty() ? std::wstring(L"Web") : info.platform.name);
  std::wstring who = SafePart(info.uploader, 18);
  if (!who.empty()) name += L"_" + who;
  if (!id.empty()) name += L"_" + id;
  return name;
}

fs::path DefaultDownloadsDir() {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) out = p;
  CoTaskMemFree(p);
  if (out.empty()) { PWSTR h = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &h)) && h) out = fs::path(h) / L"Downloads"; CoTaskMemFree(h); }
  return out;
}

// ------------------------------------------------------------------------------------------------ probe JSON
namespace {

bool IsImageExt(const std::string& e) { return e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "heic" || e == "avif" || e == "gif" || e == "bmp"; }

std::wstring SizeText(int64_t b) { return b > 0 ? FormatBytes((uint64_t)b) : std::wstring(); }

std::wstring Duration(double s) {
  if (s <= 0) return {};
  const int t = (int)(s + 0.5);
  wchar_t buf[32];
  if (t >= 3600) swprintf_s(buf, L"%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
  else swprintf_s(buf, L"%d:%02d", t / 60, t % 60);
  return buf;
}

std::wstring Detail(const Item& it) {
  std::wstring d;
  auto add = [&](const std::wstring& s) { if (s.empty()) return; if (!d.empty()) d += L"  \x00B7  "; d += s; };
  if (it.width > 0 && it.height > 0) add(std::to_wstring(it.width) + L" \x00D7 " + std::to_wstring(it.height));
  else if (it.height > 0) add(std::to_wstring(it.height) + L"p");
  add(Duration(it.duration));
  add(SizeText(it.size));
  return d;
}

Item ParseEntry(const Json& e, int index, bool inPlaylist) {
  Item it;
  it.index = inPlaylist ? index : 0;
  it.id = e.Get("id").StringOr("");
  it.title = Widen(e.Get("title").StringOr(""));
  it.ext = e.Get("ext").StringOr("");
  it.duration = e.Get("duration").AsNumber(0);
  it.thumbUrl = e.Get("thumbnail").StringOr("");
  if (it.thumbUrl.empty()) { const auto& th = e.Get("thumbnails").Elements(); if (!th.empty()) it.thumbUrl = th.back().Get("url").StringOr(""); }

  // best video format: tallest, then widest bitrate; the audio sizes are added so "size" approximates the finished file
  int bestH = (int)e.Get("height").AsInt(0), bestW = (int)e.Get("width").AsInt(0);
  int64_t vsize = 0, asize = 0;
  bool anyVideo = false, anyAudio = false;
  double bestTbr = 0;
  for (const Json& f : e.Get("formats").Elements()) {
    const std::string vc = f.Get("vcodec").StringOr("none"), ac = f.Get("acodec").StringOr("none");
    const int64_t sz = f.Get("filesize").AsInt(0) ? f.Get("filesize").AsInt(0) : f.Get("filesize_approx").AsInt(0);
    if (vc != "none") {
      anyVideo = true;
      const int h = (int)f.Get("height").AsInt(0);
      const double tbr = f.Get("tbr").AsNumber(0);
      if (h > bestH || (h == bestH && tbr > bestTbr)) { bestH = h; bestW = (int)f.Get("width").AsInt(bestW); bestTbr = tbr; vsize = sz; }
    } else if (ac != "none") {
      anyAudio = true;
      if (sz > asize) asize = sz;
    }
  }
  it.width = bestW; it.height = bestH;
  it.size = e.Get("filesize").AsInt(0) ? e.Get("filesize").AsInt(0) : (vsize ? vsize + asize : e.Get("filesize_approx").AsInt(0));

  const std::string marked = e.Get("_filo_kind").StringOr("");
  if (marked == "image") it.kind = ItemKind::Image;
  else if (marked == "audio") it.kind = ItemKind::Audio;
  else if (!anyVideo && e.Get("formats").Size() == 0 && IsImageExt(it.ext)) it.kind = ItemKind::Image;
  else if (!anyVideo && (anyAudio || (e.Get("vcodec").StringOr("") == "none" && e.Get("acodec").StringOr("none") != "none"))) it.kind = ItemKind::Audio;
  else it.kind = ItemKind::Video;
  it.useYtDlp = it.kind == ItemKind::Audio && marked != "audio";

  if (it.kind != ItemKind::Video) {
    it.url = e.Get("url").StringOr("");
    for (const auto& [k, v] : e.Get("http_headers").Items()) if (v.IsString() && (k == "Referer" || k == "referer")) it.headers.emplace_back("Referer", v.AsString());
    if (it.url.empty()) {                                  // a plain format list: take its best direct address
      const auto& fm = e.Get("formats").Elements();
      if (!fm.empty()) it.url = fm.back().Get("url").StringOr("");
    }
    if (it.thumbUrl.empty() && it.kind == ItemKind::Image) it.thumbUrl = it.url;
    if (it.ext.empty()) it.ext = it.kind == ItemKind::Image ? "jpg" : "m4a";
  }
  it.detail = Detail(it);
  if (it.kind == ItemKind::Audio && it.title.empty()) it.title = L"Sound";
  return it;
}

}  // namespace

// The author's handle: uploader_id when it reads like a name (X, YouTube), else uploader (TikTok's uploader_id is a numeric id).
static std::wstring AuthorOf(const Json& j) {
  const std::string id = j.Get("uploader_id").StringOr(""), name = j.Get("uploader").StringOr(j.Get("channel").StringOr(""));
  const bool numeric = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
  std::wstring who = Widen(id.empty() || numeric ? name : id);
  if (!who.empty() && who[0] == L'@') who.erase(0, 1);
  return who;
}

static int64_t OptInt(const Json& j) { return j.IsNumber() ? j.AsInt(-1) : -1; }

static int64_t TimestampOf(const Json& j) {
  if (const int64_t t = j.Get("timestamp").AsInt(0); t > 0) return t;
  const std::string d = j.Get("upload_date").StringOr("");                 // "20261109"
  if (d.size() == 8 && std::all_of(d.begin(), d.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    struct tm tmv = {};
    tmv.tm_year = std::stoi(d.substr(0, 4)) - 1900; tmv.tm_mon = std::stoi(d.substr(4, 2)) - 1; tmv.tm_mday = std::stoi(d.substr(6, 2));
    const __time64_t t = _mkgmtime64(&tmv);
    return t > 0 ? (int64_t)t : 0;
  }
  return 0;
}

static void FillPostFields(const Json& j, LinkInfo& li) {
  li.authorName = Widen(j.Get("channel").StringOr(j.Get("uploader").StringOr("")));
  li.description = Widen(j.Get("description").StringOr(""));
  li.timestamp = TimestampOf(j);
  li.likes = OptInt(j.Get("like_count")); li.reposts = OptInt(j.Get("repost_count"));
  li.comments = OptInt(j.Get("comment_count")); li.views = OptInt(j.Get("view_count"));
}

LinkInfo ParseProbe(const Json& root, const std::wstring& sourceUrl) {
  LinkInfo li;
  li.sourceUrl = sourceUrl;
  li.platform = DetectPlatform(sourceUrl);
  li.pageUrl = root.Get("webpage_url").StringOr(Narrow(sourceUrl));
  if (li.pageUrl.empty()) li.pageUrl = Narrow(sourceUrl);
  li.id = root.Get("id").StringOr("");
  li.title = Widen(root.Get("title").StringOr(""));
  li.uploader = AuthorOf(root);
  li.thumbUrl = root.Get("thumbnail").StringOr("");
  FillPostFields(root, li);

  if (root.Get("_type").StringOr("") == "playlist") {
    const auto& entries = root.Get("entries").Elements();
    li.isPlaylist = true;
    int n = 0;
    for (const Json& e : entries) {
      ++n;
      if (!e.IsObject()) continue;
      const std::string t = e.Get("_type").StringOr("video");
      const bool directImage = t == "url" && e.Get("_filo_kind").StringOr("") == "image"
                               && !e.Get("url").StringOr("").empty();
      if (!directImage && (t == "url" || t == "playlist" || t == "url_transparent")) {
        li.error = L"This link is a playlist or a channel. Open a single video or post instead.";
        return li;
      }
      Item it = ParseEntry(e, n, true);
      if (li.uploader.empty()) li.uploader = AuthorOf(e);
      li.items.push_back(std::move(it));
    }
    if (li.thumbUrl.empty() && !li.items.empty()) li.thumbUrl = li.items[0].thumbUrl;
  } else {
    li.items.push_back(ParseEntry(root, 0, false));
    if (li.thumbUrl.empty()) li.thumbUrl = li.items[0].thumbUrl;
  }
  if (li.items.empty() && li.description.empty()) { li.error = L"Nothing to download was found at this link."; return li; }
  if (li.items.empty()) li.textOnly = true;                      // a tweet with words only: its text is the post
  if (li.platform.id == "web" && !li.uploader.empty() && li.uploader.size() > 40) li.uploader.resize(40);
  li.ok = true;
  return li;
}


// ------------------------------------------------------------------------------------------------ post text
std::wstring FormatUtcTime(int64_t unixSeconds) {
  if (unixSeconds <= 0) return {};
  const __time64_t t = (__time64_t)unixSeconds;
  struct tm tmv = {};
  if (_gmtime64_s(&tmv, &t) != 0) return {};
  wchar_t buf[40];
  swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d UTC", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  return buf;
}

static std::wstring Grouped(int64_t n) {
  std::wstring d = std::to_wstring(n), out;
  for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) out += L','; out += d[i]; }
  return out;
}

static std::wstring CrLf(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size() + in.size() / 16);
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == L'\r') { if (i + 1 < in.size() && in[i + 1] == L'\n') ++i; out += L"\r\n"; }
    else if (in[i] == L'\n') out += L"\r\n";
    else out += in[i];
  }
  return out;
}

std::wstring FormatPostText(const LinkInfo& info) {
  std::wstring t;
  auto line = [&](const std::wstring& s) { if (!s.empty()) { t += s; t += L"\r\n"; } };
  const std::wstring body = Trim(info.description.empty() ? info.title : info.description);
  const bool captionSite = info.platform.id == "x" || info.platform.id == "tiktok" || info.platform.id == "instagram" || info.platform.id == "threads";
  if (!captionSite && !info.title.empty() && info.title != body && !info.description.empty()) line(info.title);
  std::wstring who = info.authorName;
  if (!info.uploader.empty() && info.uploader != info.authorName) who += (who.empty() ? L"" : L" ") + std::wstring(L"(@") + info.uploader + L")";
  line(info.platform.name + (who.empty() ? L"" : L"  -  " + who));
  line(Widen(info.pageUrl));
  if (info.timestamp > 0) line(FormatUtcTime(info.timestamp));
  std::wstring stats;
  auto stat = [&](const wchar_t* name, int64_t v) { if (v < 0) return; if (!stats.empty()) stats += L"   "; stats += std::wstring(name) + L" " + Grouped(v); };
  stat(L"Likes", info.likes); stat(L"Reposts", info.reposts); stat(L"Comments", info.comments); stat(L"Views", info.views);
  line(stats);
  if (!body.empty()) { t += L"\r\n"; t += CrLf(body); t += L"\r\n"; }
  return t;
}

// ------------------------------------------------------------------------------------------------ accounts
namespace {

struct UrlParts { std::wstring host, query; std::vector<std::wstring> seg; };

UrlParts SplitUrl(const std::wstring& url) {
  UrlParts u;
  u.host = HostOf(url);
  size_t s = url.find(L"://"); s = s == std::wstring::npos ? 0 : s + 3;
  const size_t slash = url.find(L'/', s), cut = url.find_first_of(L"?#", s);
  if (slash != std::wstring::npos && (cut == std::wstring::npos || slash < cut)) {
    const std::wstring path = url.substr(slash + 1, (cut == std::wstring::npos ? std::wstring::npos : cut - slash - 1));
    for (std::wstring& p : Split(path, L'/')) {
      if (p.empty()) continue;
      if (StartsWithI(p, L"%40")) p = L"@" + p.substr(3);
      u.seg.push_back(p);
    }
  }
  if (cut != std::wstring::npos && url[cut] == L'?') {
    const size_t end = url.find(L'#', cut);
    u.query = url.substr(cut + 1, end == std::wstring::npos ? std::wstring::npos : end - cut - 1);
  }
  return u;
}

std::wstring QueryValue(const std::wstring& query, const std::wstring& key) {
  for (const std::wstring& kv : Split(query, L'&')) {
    const size_t eq = kv.find(L'=');
    if (eq != std::wstring::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
  }
  return {};
}

bool OneOf(const std::wstring& v, std::initializer_list<const wchar_t*> set) {
  const std::wstring l = ToLower(v);
  for (const wchar_t* s : set) if (l == s) return true;
  return false;
}

bool IsNumber(const std::wstring& v) { return !v.empty() && std::all_of(v.begin(), v.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }); }

// letters, digits and . _ - only: the name is put back into an address
bool CleanName(const std::wstring& v, size_t maxLen = 60) {
  if (v.empty() || v.size() > maxLen) return false;
  return std::all_of(v.begin(), v.end(), [](wchar_t c) { return iswalnum(c) || c == L'.' || c == L'_' || c == L'-' || c > 0x7F; });
}

}  // namespace

AccountRef ClassifyAccount(const std::wstring& url) {
  AccountRef r;
  const Platform plat = DetectPlatform(url);
  const UrlParts u = SplitUrl(url);
  const auto& g = u.seg;
  auto plain = [&](std::initializer_list<const wchar_t*> hosts) { for (const wchar_t* h : hosts) if (u.host == h) return true; return false; };
  auto done = [&](const std::wstring& handle, std::vector<std::string> urls) {
    r.is = true; r.platform = plat; r.handle = handle; r.listUrls = std::move(urls); return r;
  };

  if (plat.id == "tiktok" && plain({L"tiktok.com", L"www.tiktok.com", L"m.tiktok.com"})) {
    if (g.size() == 1 && g[0].size() > 1 && g[0][0] == L'@' && CleanName(g[0].substr(1))) return done(g[0].substr(1), {"https://www.tiktok.com/" + Narrow(g[0])});
  } else if (plat.id == "youtube" && plain({L"youtube.com", L"www.youtube.com", L"m.youtube.com", L"music.youtube.com"})) {
    if (!g.empty() && g[0] == L"playlist") {
      const std::wstring list = QueryValue(u.query, L"list");
      if (CleanName(list, 80)) { AccountRef p = done(L"playlist_" + SafePart(list, 14), {"https://www.youtube.com/playlist?list=" + Narrow(list)}); p.isPlaylist = true; return p; }
      return r;
    }
    std::string base; std::wstring handle, tab;
    if (!g.empty() && g[0].size() > 1 && g[0][0] == L'@' && CleanName(g[0].substr(1))) {
      base = "https://www.youtube.com/" + Narrow(g[0]); handle = g[0].substr(1); tab = g.size() >= 2 ? g[1] : L"";
    } else if (g.size() >= 2 && OneOf(g[0], {L"channel", L"c", L"user"}) && CleanName(g[1], 80)) {
      base = "https://www.youtube.com/" + Narrow(ToLower(g[0])) + "/" + Narrow(g[1]); handle = g[1]; tab = g.size() >= 3 ? g[2] : L"";
    } else return r;
    if (OneOf(tab, {L"videos", L"shorts", L"streams", L"live"})) return done(handle, {base + "/" + (OneOf(tab, {L"live"}) ? std::string("streams") : Narrow(ToLower(tab)))});
    return done(handle, {base + "/videos", base + "/shorts", base + "/streams"});
  } else if (plat.id == "x" && plain({L"x.com", L"www.x.com", L"twitter.com", L"www.twitter.com", L"mobile.twitter.com"})) {
    static const wchar_t* reserved[] = {L"i", L"home", L"explore", L"search", L"settings", L"intent", L"share", L"hashtag", L"messages", L"notifications", L"compose",
                                        L"login", L"signup", L"tos", L"privacy", L"about", L"jobs", L"download", L"oauth", L"account", L"status", L"app", L"help",
                                        L"support", L"lists", L"topics", L"communities", L"premium", L"verified", L"following", L"followers"};
    if (g.empty() || g.size() > 2 || !CleanName(g[0], 15) || g[0].find_first_of(L".-") != std::wstring::npos) return r;
    for (const wchar_t* w : reserved) if (ToLower(g[0]) == w) return r;
    if (g.size() == 2 && !OneOf(g[1], {L"media", L"with_replies", L"highlights", L"articles"})) return r;
    return done(g[0], {"https://x.com/" + Narrow(g[0]) + (g.size() == 2 && OneOf(g[1], {L"media"}) ? "/media" : "")});
  } else if (plat.id == "instagram" && plain({L"instagram.com", L"www.instagram.com"})) {
    if (g.empty() || g.size() > 2 || !CleanName(g[0], 40)) return r;
    if (OneOf(g[0], {L"p", L"reel", L"reels", L"tv", L"stories", L"explore", L"accounts", L"direct", L"about", L"legal", L"developer", L"web", L"challenge",
                     L"directory", L"emailsignup", L"login", L"oauth", L"api", L"graphql", L"static", L"privacy", L"terms"})) return r;
    if (g.size() == 2 && !OneOf(g[1], {L"reels", L"tagged"})) return r;
    return done(g[0], {"https://www.instagram.com/" + Narrow(g[0]) + "/"});
  } else if (plat.id == "twitch" && plain({L"twitch.tv", L"www.twitch.tv", L"m.twitch.tv"})) {
    if (g.empty() || g.size() > 2 || !CleanName(g[0], 30)) return r;
    if (OneOf(g[0], {L"videos", L"directory", L"p", L"settings", L"downloads", L"jobs", L"turbo", L"friends", L"subscriptions", L"inventory", L"wallet", L"drops",
                     L"search", L"store", L"prime", L"signup", L"login", L"payments", L"u", L"popout", L"embed", L"moderator"})) return r;
    if (g.size() == 2 && !OneOf(g[1], {L"videos", L"clips", L"schedule", L"about", L"home"})) return r;
    return done(g[0], {"https://www.twitch.tv/" + Narrow(g[0]) + "/videos"});
  } else if (plat.id == "vimeo" && plain({L"vimeo.com", L"www.vimeo.com"})) {
    if (g.size() == 2 && OneOf(g[0], {L"album", L"showcase"}) && IsNumber(g[1])) {
      AccountRef p = done(L"album_" + g[1], {"https://vimeo.com/" + Narrow(ToLower(g[0])) + "/" + Narrow(g[1])}); p.isPlaylist = true; return p;
    }
    if (g.empty() || g.size() > 2 || !CleanName(g[0], 40) || IsNumber(g[0])) return r;
    if (OneOf(g[0], {L"channels", L"groups", L"categories", L"ondemand", L"watch", L"search", L"stock", L"upload", L"features", L"about", L"help", L"log_in", L"join",
                     L"jobs", L"terms", L"privacy", L"couchmode", L"staffpicks", L"blog", L"solutions", L"pricing", L"enterprise", L"live", L"create", L"site", L"album", L"user"})) return r;
    if (g.size() == 2 && !OneOf(g[1], {L"videos"})) return r;
    return done(g[0], {"https://vimeo.com/" + Narrow(g[0]) + "/videos"});
  } else if (plat.id == "soundcloud" && plain({L"soundcloud.com", L"www.soundcloud.com", L"m.soundcloud.com"})) {
    if (g.size() == 3 && OneOf(g[1], {L"sets"}) && CleanName(g[0], 60) && CleanName(g[2], 100)) {
      AccountRef p = done(g[2], {"https://soundcloud.com/" + Narrow(g[0]) + "/sets/" + Narrow(g[2])}); p.isPlaylist = true; return p;
    }
    if (g.empty() || g.size() > 2 || !CleanName(g[0], 60)) return r;
    if (OneOf(g[0], {L"discover", L"stream", L"search", L"upload", L"you", L"pages", L"terms-of-use", L"jobs", L"charts", L"popular", L"feed", L"settings", L"notifications",
                     L"messages", L"mobile", L"imprint", L"community-guidelines", L"creators", L"people"})) return r;
    if (g.size() == 2 && !OneOf(g[1], {L"tracks", L"albums", L"reposts", L"popular-tracks", L"likes"})) return r;
    return done(g[0], {"https://soundcloud.com/" + Narrow(g[0]) + "/" + (g.size() == 2 ? Narrow(ToLower(g[1])) : std::string("tracks"))});
  } else if (plat.id == "bandcamp") {
    const size_t dot = u.host.find(L'.');
    if (dot != std::wstring::npos && dot > 0 && u.host.substr(dot) == L".bandcamp.com" && u.host != L"www.bandcamp.com" && (g.empty() || (g.size() == 1 && OneOf(g[0], {L"music"}))))
      return done(u.host.substr(0, dot), {"https://" + Narrow(u.host) + "/music"});
  } else if (plat.id == "bilibili" && u.host == L"space.bilibili.com") {
    if (!g.empty() && IsNumber(g[0])) return done(g[0], {"https://space.bilibili.com/" + Narrow(g[0]) + "/video"});
  } else if (plat.id == "dailymotion" && plain({L"dailymotion.com", L"www.dailymotion.com"})) {
    if (g.size() == 2 && OneOf(g[0], {L"user"}) && CleanName(g[1], 60)) return done(g[1], {"https://www.dailymotion.com/user/" + Narrow(g[1])});
    if (g.size() == 2 && OneOf(g[0], {L"playlist"}) && CleanName(g[1], 60)) { AccountRef p = done(L"playlist_" + g[1], {"https://www.dailymotion.com/playlist/" + Narrow(g[1])}); p.isPlaylist = true; return p; }
  } else if (plat.id == "rumble" && plain({L"rumble.com", L"www.rumble.com"})) {
    if (g.size() == 2 && OneOf(g[0], {L"c", L"user"}) && CleanName(g[1], 60)) return done(g[1], {"https://rumble.com/" + Narrow(ToLower(g[0])) + "/" + Narrow(g[1])});
  }
  return r;
}

std::wstring AccountFolderName(const Platform& platform, const std::wstring& handle) {
  std::wstring name = L"Filo_" + (platform.name.empty() ? std::wstring(L"Web") : platform.name);
  const std::wstring who = SafePart(handle, 28);
  if (!who.empty()) name += L"_" + who;
  return name;
}

bool ParseAccountEntry(const Json& e, AccountEntry& out) {
  if (!e.IsObject()) return false;
  if (e.Get("_type").StringOr("video") == "playlist") return false;
  out = AccountEntry();
  out.url = e.Get("url").StringOr("");
  if (out.url.rfind("http", 0) != 0) out.url = e.Get("webpage_url").StringOr("");
  out.id = e.Get("id").StringOr("");
  if (out.id.empty()) out.id = e.Get("webpage_url_basename").StringOr("");
  if (out.url.empty() || out.id.empty()) return false;
  out.title = Widen(e.Get("title").StringOr(""));
  out.text = Widen(e.Get("description").StringOr(""));
  out.authorName = Widen(e.Get("channel").StringOr(e.Get("uploader").StringOr("")));
  const std::string uid = e.Get("uploader_id").StringOr("");
  const bool numeric = !uid.empty() && std::all_of(uid.begin(), uid.end(), [](char c) { return c >= '0' && c <= '9'; });
  out.authorHandle = uid.empty() || numeric ? e.Get("uploader").StringOr("") : uid;
  if (!out.authorHandle.empty() && out.authorHandle[0] == '@') out.authorHandle.erase(0, 1);
  out.duration = e.Get("duration").AsNumber(0);
  out.timestamp = TimestampOf(e);
  out.likes = OptInt(e.Get("like_count")); out.reposts = OptInt(e.Get("repost_count"));
  out.comments = OptInt(e.Get("comment_count")); out.views = OptInt(e.Get("view_count"));
  out.repost = e.Get("_filo_repost").AsBool(false);
  if (e.Get("_filo_media").IsArray()) {
    out.hasMedia = true;
    int n = 0;
    for (const Json& m : e.Get("_filo_media").Elements()) {
      const std::string u = m.Get("url").StringOr("");
      if (u.empty()) continue;
      Item it;
      it.kind = m.Get("kind").StringOr("image") == "video" ? ItemKind::Video : ItemKind::Image;
      it.index = ++n;
      it.id = out.id + "_" + std::to_string(n);
      it.url = u;
      it.ext = m.Get("ext").StringOr(it.kind == ItemKind::Video ? "mp4" : "jpg");
      it.width = (int)m.Get("width").AsInt(0); it.height = (int)m.Get("height").AsInt(0);
      it.duration = m.Get("duration").AsNumber(0);
      it.thumbUrl = m.Get("thumbnail").StringOr(it.kind == ItemKind::Image ? u : "");
      it.direct = it.kind == ItemKind::Video;
      it.detail = Detail(it);
      out.media.push_back(std::move(it));
    }
  }
  return true;
}

LinkInfo LinkInfoFromEntry(const AccountEntry& e, const Platform& platform) {
  LinkInfo li;
  li.ok = true;
  li.platform = platform;
  li.sourceUrl = Widen(e.url);
  li.pageUrl = e.url;
  li.id = e.id;
  li.title = e.title;
  li.uploader = Widen(e.authorHandle);
  li.authorName = e.authorName;
  li.description = e.text;
  li.timestamp = e.timestamp;
  li.likes = e.likes; li.reposts = e.reposts; li.comments = e.comments; li.views = e.views;
  li.isPlaylist = e.media.size() > 1;
  li.items = e.media;
  li.textOnly = li.items.empty();
  if (!li.items.empty()) li.thumbUrl = li.items[0].thumbUrl;
  return li;
}


}  // namespace filo::web
