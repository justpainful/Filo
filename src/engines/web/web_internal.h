// Helpers shared by the yt-dlp runner (ytdlp.cpp), the account code (account.cpp) and the gallery-dl runner (gallerydl.cpp).
// Not part of the public web engine API.
#pragma once
#include "engines/web/web.h"

namespace filo::web::internal {

std::vector<std::wstring> BaseArgs();                                           // arguments every yt-dlp run starts with (plugins, FFmpeg, cookies)
std::wstring ErrorText(const std::string& stderrText, const std::wstring& startError);   // yt-dlp's last ERROR line as a sentence
void PrepareEnvironment();                                                      // UTF-8 output, no colours

struct CookieChoice { CookieSource source = CookieSource::None; fs::path file; };
CookieChoice CurrentCookies();                                                  // the sign-in the user picked (both downloaders use it)

// ---- gallery-dl: Instagram ---------------------------------------------------------------------------------------------------------
bool UsesGalleryDl(const Platform& platform);                                   // true for the sites gallery-dl handles (Instagram)

// One file of a post, as gallery-dl prints it (one JSON array per line, see kGalleryFormat in gallerydl.cpp).
struct GalleryFile {
  std::string shortcode, user, type;               // type: "post" or "reel"
  std::wstring fullName, text;
  int num = 0, count = 0, width = 0, height = 0;   // num of count: the file's place in a carousel
  int64_t timestamp = 0, likes = -1;
  std::string url, ext;
};
bool ParseGalleryLine(const std::string& line, GalleryFile& out);
// Adds a file to the post it belongs to (starts the post when `e` is empty).
void AddGalleryFile(AccountEntry& e, const GalleryFile& f);

// Fills `ai` (already classified) with the posts of an Instagram profile. Honours `cancel`; `onEntry` is called per post.
void ListGallery(AccountInfo& ai, const ListOptions& opt, const std::atomic<bool>* cancel, const std::function<void(const AccountEntry&)>& onEntry);
// One post, reel or story address as a LinkInfo whose items carry direct addresses.
LinkInfo ProbeGallery(const std::wstring& url, const std::atomic<bool>* cancel);
std::wstring GalleryErrorText(const std::string& stderrText, const std::wstring& startError, bool& needsLogin);

}  // namespace filo::web::internal
