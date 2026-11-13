#include "core/http.h"
#include <winhttp.h>
#include <fstream>
#include <memory>

namespace filo {

namespace {

struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET v = nullptr) : h(v) {}
  ~Handle() { if (h) WinHttpCloseHandle(h); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

constexpr wchar_t kUserAgent[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";

// Core transfer: `sink` receives body chunks and returns false to abort.
HttpResult Fetch(const std::wstring& url, const HttpOptions& o, const std::function<bool(const char*, size_t)>& sink) {
  HttpResult r;
  URL_COMPONENTS uc{sizeof uc};
  uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc) || !uc.lpszHostName) { r.error = L"The address is not valid."; return r; }
  const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP) { r.error = L"Only http and https addresses are supported."; return r; }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath ? uc.lpszUrlPath : L"/", uc.dwUrlPathLength + uc.dwExtraInfoLength);
  if (path.empty()) path = L"/";

  Handle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
  if (!session.h) { r.error = LastErrorMessage(); return r; }
  WinHttpSetTimeouts(session.h, (int)o.timeoutMs, (int)o.timeoutMs, (int)o.timeoutMs, (int)o.timeoutMs);
  DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
  WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof decomp);
  Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
  if (!conn.h) { r.error = LastErrorMessage(); return r; }
  Handle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!req.h) { r.error = LastErrorMessage(); return r; }
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof redirectPolicy);
  for (const auto& [k, v] : o.headers) {
    std::wstring line = Widen(k) + L": " + Widen(v);
    WinHttpAddRequestHeaders(req.h, line.c_str(), (DWORD)line.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  }
  if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
    r.error = LastErrorMessage(); return r;
  }
  DWORD status = 0, sz = sizeof status;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
  r.status = (int)status;
  wchar_t ct[128] = {}; sz = sizeof ct;
  if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &sz, WINHTTP_NO_HEADER_INDEX)) r.contentType = Narrow(ct);
  if (status < 200 || status >= 300) { r.error = L"The server answered with HTTP " + std::to_wstring(status) + L"."; return r; }
  uint64_t total = 0;
  wchar_t cl[32] = {}; sz = sizeof cl;
  if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &sz, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(cl, nullptr, 10);

  std::vector<char> buf(64 * 1024);
  for (;;) {
    if (o.cancel && o.cancel->load()) { r.cancelled = true; r.error = L"Cancelled."; return r; }
    DWORD got = 0;
    if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) { r.error = LastErrorMessage(); return r; }
    if (got == 0) break;
    r.bytes += got;
    if (r.bytes > o.maxBytes) { r.error = L"The file is larger than the allowed size."; return r; }
    if (!sink(buf.data(), got)) { r.error = L"Could not write the file."; return r; }
    if (o.onProgress) o.onProgress(r.bytes, total);
  }
  r.ok = true;
  return r;
}

}  // namespace

HttpResult HttpGetFile(const std::wstring& url, const fs::path& dest, const HttpOptions& o) {
  std::ofstream f(dest, std::ios::binary | std::ios::trunc);
  if (!f) { HttpResult r; r.error = L"Could not create " + dest.filename().wstring(); return r; }
  HttpResult r = Fetch(url, o, [&](const char* p, size_t n) { f.write(p, (std::streamsize)n); return (bool)f; });
  f.close();
  if (!r.ok) { std::error_code ec; fs::remove(dest, ec); }
  return r;
}

HttpResult HttpGetMemory(const std::wstring& url, std::string& out, const HttpOptions& o) {
  out.clear();
  return Fetch(url, o, [&](const char* p, size_t n) { out.append(p, n); return true; });
}

}  // namespace filo
