// Minimal HTTPS GET client on WinHTTP (system proxy, redirects, gzip). Used for link-download thumbnails and photo files.
// Never follows a redirect from https to http, never sends cookies, never stores anything but the requested body.
#pragma once
#include "core/util.h"
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace filo {

struct HttpOptions {
  std::vector<std::pair<std::string, std::string>> headers;      // extra request headers (e.g. Referer)
  DWORD timeoutMs = 30000;
  const std::atomic<bool>* cancel = nullptr;                      // polled between reads
  std::function<void(uint64_t done, uint64_t total)> onProgress;  // total == 0 when unknown
  uint64_t maxBytes = 1ull << 30;
};

struct HttpResult {
  bool ok = false;
  bool cancelled = false;
  int status = 0;
  std::wstring error;
  std::string contentType;
  uint64_t bytes = 0;
};

HttpResult HttpGetFile(const std::wstring& url, const fs::path& dest, const HttpOptions& o = {});
HttpResult HttpGetMemory(const std::wstring& url, std::string& out, const HttpOptions& o = {});

}  // namespace filo
