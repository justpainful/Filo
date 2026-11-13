// Helpers shared by the yt-dlp runner (ytdlp.cpp) and the account code (account.cpp). Not part of the public web engine API.
#pragma once
#include "engines/web/web.h"

namespace filo::web::internal {

std::vector<std::wstring> BaseArgs();                                           // arguments every yt-dlp run starts with (plugins, FFmpeg, cookies)
std::wstring ErrorText(const std::string& stderrText, const std::wstring& startError);   // yt-dlp's last ERROR line as a sentence
void PrepareEnvironment();                                                      // UTF-8 output, no colours

}  // namespace filo::web::internal
