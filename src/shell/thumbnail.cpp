#include "shell/thumbnail.h"
#include "core/com.h"
#include "core/log.h"
#include <shobjidl.h>
#include <thumbcache.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace filo::shell {

struct ThumbnailService::Impl {
  struct Job { fs::path path; int side; uint64_t owner; std::function<void(const ThumbnailImage&)> done; };
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Job> q;
  bool stop = false;
  std::thread th;

  void Loop() {
    ComInit com(COINIT_APARTMENTTHREADED);
    for (;;) {
      Job j;
      {
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return stop || !q.empty(); });
        if (stop) return;
        j = std::move(q.front()); q.pop_front();
      }
      ThumbnailImage img;
      ComPtr<IShellItem> item;
      if (SUCCEEDED(SHCreateItemFromParsingName(j.path.c_str(), nullptr, __uuidof(IShellItem), item.AsVoid()))) {
        ComPtr<IShellItemImageFactory> f;
        if (SUCCEEDED(item->QueryInterface(__uuidof(IShellItemImageFactory), f.AsVoid()))) {
          HBITMAP hb = nullptr;
          SIZE sz{j.side, j.side};
          HRESULT hr = f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hb);
          if (FAILED(hr) || !hb) { hr = f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &hb); img.isIcon = true; }
          if (SUCCEEDED(hr) && hb) {
            BITMAP bm{};
            GetObjectW(hb, sizeof bm, &bm);
            if (bm.bmWidth > 0 && bm.bmHeight > 0) {
              BITMAPINFO bi{};
              bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
              img.w = bm.bmWidth; img.h = bm.bmHeight;
              img.bgraPremul.resize((size_t)img.w * img.h * 4);
              HDC dc = GetDC(nullptr);
              GetDIBits(dc, hb, 0, img.h, img.bgraPremul.data(), &bi, DIB_RGB_COLORS);
              ReleaseDC(nullptr, dc);
              // Shell bitmaps are premultiplied BGRA when they carry alpha; opaque ones have alpha 0 -> force 255
              bool anyAlpha = false;
              for (size_t i = 3; i < img.bgraPremul.size(); i += 4) if (img.bgraPremul[i]) { anyAlpha = true; break; }
              if (!anyAlpha) for (size_t i = 3; i < img.bgraPremul.size(); i += 4) img.bgraPremul[i] = 255;
            }
            DeleteObject(hb);
          }
        }
      }
      if (j.done) j.done(img);
    }
  }
};

ThumbnailService::ThumbnailService() : impl_(new Impl) { impl_->th = std::thread([this] { impl_->Loop(); }); }
ThumbnailService& ThumbnailService::Get() { static ThumbnailService* s = new ThumbnailService(); return *s; }

void ThumbnailService::Request(const fs::path& p, int side, uint64_t owner, std::function<void(const ThumbnailImage&)> done) {
  { std::lock_guard lk(impl_->mu);
    impl_->q.push_back({p, side, owner, std::move(done)}); }
  impl_->cv.notify_one();
}
void ThumbnailService::CancelOwner(uint64_t owner) {
  std::lock_guard lk(impl_->mu);
  for (auto it = impl_->q.begin(); it != impl_->q.end();) it = (it->owner == owner) ? impl_->q.erase(it) : it + 1;
}
void ThumbnailService::Shutdown() {
  { std::lock_guard lk(impl_->mu); impl_->stop = true; }
  impl_->cv.notify_all();
  if (impl_->th.joinable()) impl_->th.join();
}

} // namespace filo::shell
