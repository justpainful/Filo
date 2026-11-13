// Web engine: probes a link (TikTok, YouTube, X, Instagram, ... anything yt-dlp understands) and downloads the chosen items in
// the best quality the site offers. yt-dlp and FFmpeg run as separate processes (never linked); photos are fetched directly.
// The Filo plugin for yt-dlp (assets/yt-dlp-plugins) adds the photos of TikTok slideshows and X posts, which upstream skips.
#pragma once
#include "core/http.h"
#include "core/job.h"
#include "core/json.h"
#include "core/util.h"
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace filo::web {

// ---- links ---------------------------------------------------------------------------------------------------------
struct Platform {
  std::string id;          // "tiktok", "youtube", "x", ... ("web" for any other site)
  std::wstring name;       // "TikTok", "YouTube", "X"; the site name for other hosts. Safe to use in file names.
  bool known = false;
};
Platform DetectPlatform(const std::wstring& url);
// First http(s) address inside `text` (a dragged link, "Title - https://...", a URL file's contents). Empty when there is none.
std::wstring ExtractUrl(const std::wstring& text);

// ---- what a link offers --------------------------------------------------------------------------------------------
enum class ItemKind { Video, Image, Audio };

struct Item {
  ItemKind kind = ItemKind::Video;
  int index = 0;                     // 1-based position in the post's playlist (0: the link is a single item)
  std::string id;
  std::wstring title;
  std::string ext;                   // the extension the best source has ("jpg", "mp4", ...)
  int width = 0, height = 0;         // best available size
  double duration = 0;               // seconds
  int64_t size = 0;                  // best guess in bytes (0 = unknown)
  std::string thumbUrl;
  std::string url;                   // direct address (photos and slideshow sound); empty for videos (yt-dlp resolves them)
  bool useYtDlp = false;             // audio from an extractor may be a segmented stream, not a direct file
  bool direct = false;               // a video that has a direct address too (X posts listed by Filo's plugin): fetched like a picture
  std::vector<std::pair<std::string, std::string>> headers;   // headers the direct address needs (Referer)
  std::wstring detail;               // "1920 x 1080 - 0:09 - 11.3 MB"
};

struct LinkInfo {
  bool ok = false;
  std::wstring error;                // user-facing, set when !ok
  std::wstring sourceUrl;            // what was probed
  std::string pageUrl;               // canonical address (what yt-dlp is given for the download)
  Platform platform;
  std::string id;
  std::wstring title, uploader;
  std::string thumbUrl;
  bool isPlaylist = false;           // the items are entries of one post (yt-dlp needs --playlist-items)
  std::wstring authorName;           // display name of the author ("NASA"); `uploader` is the handle used in file names
  std::wstring description;          // the post text / caption / video description, line breaks kept
  int64_t timestamp = 0;             // unix seconds the post was published (0 = unknown)
  int64_t likes = -1, reposts = -1, comments = -1, views = -1;   // -1 = unknown
  bool textOnly = false;             // a post with no picture or video (a tweet): only its text can be saved
  std::vector<Item> items;
  int CountOf(ItemKind k) const { int n = 0; for (auto& i : items) n += i.kind == k; return n; }
  bool HasAudioChoice() const { return CountOf(ItemKind::Video) > 0; }   // "audio only" makes sense when a video exists
  bool HasText() const { return !description.empty() || !title.empty(); }
};

// The text file saved next to a post: title, author, address, date, counts, then the text itself (CRLF, UTF-8).
std::wstring FormatPostText(const LinkInfo& info);
// "2026-11-12 20:16 UTC"; empty for 0.
std::wstring FormatUtcTime(int64_t unixSeconds);

// ---- running yt-dlp --------------------------------------------------------------------------------------------------
fs::path YtDlpPath();                // tools\yt-dlp.exe next to Filo.exe, else PATH. Empty when missing.
fs::path PluginDir();                // the Filo plugin folder (tools\yt-dlp-plugins, or the source tree in development builds)
bool Available();

// Asks yt-dlp what the link contains. Blocking (seconds); run it on a worker thread. Honours `cancel`.
LinkInfo Probe(const std::wstring& url, const std::atomic<bool>* cancel = nullptr);
// Same parsing from yt-dlp's JSON (unit tests, fixtures).
LinkInfo ParseProbe(const Json& root, const std::wstring& sourceUrl);

enum class Quality { Best, Compatible };       // Best: highest resolution/bitrate whatever the codec; Compatible: H.264 + AAC in MP4
enum class VideoContainer { Auto, Mp4, Mkv, Webm };
enum class AudioFormat { Original, Mp3, M4a, Wav };

struct DownloadRequest {
  LinkInfo info;
  std::vector<int> items;            // indices into info.items to save
  bool wantAudio = false;            // also save the audio track of the chosen videos (all videos when none is chosen)
  Quality quality = Quality::Best;
  VideoContainer videoContainer = VideoContainer::Auto;
  AudioFormat audioFormat = AudioFormat::Original;
  fs::path destRoot;                 // normally the Downloads folder
  int folderThreshold = 4;           // this many files or more go into a folder of their own (0 = never)
  bool saveText = false;             // also save the post's text as a .txt next to the files (or alone for a text-only post)
};

// Where a download reports to. Jobs fill it from their JobContext; the account download gives every worker its own scratch folder.
struct DownloadSink {
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(double)> progress;                          // 0..1 for this post (optional)
  std::function<void(JobState, const std::wstring&)> stage;      // optional
  fs::path workDir;                                              // scratch folder, empty = a fresh one under the job's temp dir
};
JobResult DownloadPost(const DownloadRequest& req, const DownloadSink& sink);

// Downloads into a scratch folder, then moves the files into `destRoot` under their final names. Outputs: the files, or the
// folder when they were put in one.
JobResult Download(const DownloadRequest& req, JobContext& ctx);

// ---- sign-in (cookies) for sites that only show accounts to signed-in visitors --------------------------------------------------
enum class CookieSource { None, Firefox, Chrome, Edge, File };
void SetCookies(CookieSource source, const fs::path& file = {});     // used by every yt-dlp run that follows
CookieSource CookiesInUse();

// ---- accounts: every post of a TikTok / YouTube / X / ... profile --------------------------------------------------------------------
struct AccountRef {
  bool is = false;
  Platform platform;
  std::wstring handle;                   // "nasa" (no @)
  std::vector<std::string> listUrls;     // what yt-dlp lists, in order (a YouTube channel has Videos, Shorts and Live tabs)
};
// Recognises profile / channel addresses: tiktok.com/@name, youtube.com/@name (/channel/, /c/, /user/, playlist?list=), x.com/name,
// instagram.com/name, twitch.tv/name, soundcloud.com/name, ... Posts and unknown sites give is == false.
AccountRef ClassifyAccount(const std::wstring& url);
inline bool IsAccountUrl(const std::wstring& url) { return ClassifyAccount(url).is; }

struct AccountEntry {
  std::string id;
  std::string url;                       // the post's address
  std::wstring title;
  std::wstring text;                     // caption / tweet text when the listing carries it
  std::wstring authorName;
  std::string authorHandle;
  double duration = 0;
  int64_t timestamp = 0;
  int64_t likes = -1, reposts = -1, comments = -1, views = -1;
  bool repost = false;
  bool hasMedia = false;                 // `media` is complete (X): the post is saved without asking the site again
  std::vector<Item> media;
};
// One line of `yt-dlp -j --flat-playlist`. Returns false when the line holds nothing usable.
bool ParseAccountEntry(const Json& e, AccountEntry& out);
// A post whose listing entry already holds everything (X): the LinkInfo the download needs, no probing.
LinkInfo LinkInfoFromEntry(const AccountEntry& e, const Platform& platform);

struct AccountInfo {
  bool ok = false;                       // entries were found
  bool interrupted = false;              // the listing was stopped; `entries` is what was found until then
  bool capped = false;                   // stopped at ListOptions::cap
  bool needsLogin = false;
  std::wstring error;
  std::wstring sourceUrl;
  AccountRef ref;
  Platform platform;
  std::wstring handle, name;
  std::vector<AccountEntry> entries;     // newest first, as the site lists them
};
struct ListOptions {
  int limit = 0;                         // stop after this many posts (0 = all)
  int cap = 5000;                        // never list more than this
  bool reposts = false;                  // X: also list what the account reposted
};
// Lists the posts of an account (blocking; run on a worker thread). `onEntry` is called for every post as it is found.
AccountInfo ListAccount(const std::wstring& url, const ListOptions& opt, const std::atomic<bool>* cancel = nullptr,
                        const std::function<void(const AccountEntry&)>& onEntry = {});

// An account from a saved listing (yt-dlp -J style: {"_filo_account":{"name":..},"entries":[...]}); fixtures and screenshots.
AccountInfo AccountFromJson(const Json& root, const std::wstring& url);

struct AccountDownloadRequest {
  AccountInfo account;
  std::vector<int> entries;              // indices into account.entries, in download order
  bool videos = true, pictures = true, text = true, sounds = false;
  Quality quality = Quality::Best;
  VideoContainer videoContainer = VideoContainer::Auto;
  fs::path destRoot;                     // the account gets its own folder inside: Filo_<Platform>_<handle>
  int folderThreshold = 4;               // a post with this many files or more gets a folder of its own inside the account folder
  bool skipDone = true;                  // posts saved by an earlier run (listed in the account folder) are not fetched again
  int parallel = 3;                      // posts downloaded at the same time (1..4)
};
JobResult DownloadAccount(const AccountDownloadRequest& req, JobContext& ctx);
std::wstring AccountFolderName(const Platform& platform, const std::wstring& handle);   // "Filo_TikTok_nasa"

// ---- naming ------------------------------------------------------------------------------------------------------------
fs::path DefaultDownloadsDir();      // the user's Downloads known folder
// "Filo_TikTok_dram_i_0633255174": app, platform, author (max 18 chars) and the tail of the post id. Unique per post.
std::wstring BaseName(const LinkInfo& info);
std::wstring SafePart(std::wstring s, size_t maxLen);

}  // namespace filo::web
