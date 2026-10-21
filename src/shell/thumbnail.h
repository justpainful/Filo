// Asynchronous Windows Shell thumbnails (uses the system thumbnail cache). Never blocks the UI thread: a single STA worker
// thread asks IShellItemImageFactory for a bitmap and posts the result back.
#pragma once
#include "core/util.h"
#include <functional>
#include <vector>

namespace filo::shell {

struct ThumbnailImage {
  int w = 0, h = 0;
  std::vector<uint8_t> bgraPremul;   // w*h*4
  bool isIcon = false;               // true when no real thumbnail existed (file-type icon)
};

class ThumbnailService {
 public:
  static ThumbnailService& Get();
  // `done` is invoked on the worker thread; marshal to the UI thread yourself (PostMessage).
  // Returns a ticket; a newer Request() supersedes older queued ones from the same owner (stale wheel).
  void Request(const fs::path& p, int maxSide, uint64_t owner, std::function<void(const ThumbnailImage&)> done);
  void CancelOwner(uint64_t owner);
  void Shutdown();
 private:
  ThumbnailService();
  struct Impl; Impl* impl_;
};

} // namespace filo::shell
