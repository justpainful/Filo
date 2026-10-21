#include "tools/video/vid_common.h"
#include <algorithm>
#include <cmath>

namespace filo::tools::vid {

// ================================================================================================================================
// StillWorker
// ================================================================================================================================
void StillWorker::Start(const fs::path& p, HWND notify) {
  Stop();
  path_ = p; hwnd_ = notify;
  { std::lock_guard lk(mu_); quit_ = false; have_ = false; ready_ = false; }
  latest_ = 0; done_ = 0; cancel_ = false;
  th_ = std::thread([this] { Loop(); });
}

void StillWorker::Stop() {
  { std::lock_guard lk(mu_); quit_ = true; }
  cancel_ = true;
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

uint64_t StillWorker::Request(double t, bool accurate, int maxW, int maxH) {
  std::lock_guard lk(mu_);
  req_ = {t, accurate, maxW, maxH, latest_.load() + 1};
  latest_ = req_.seq;
  have_ = true;
  cancel_ = true;             // abort a stale decode that is still running
  cv_.notify_all();
  return req_.seq;
}

bool StillWorker::Take(media::FrameImage& out, uint64_t& seq) {
  std::lock_guard lk(mu_);
  if (!ready_) return false;
  out = std::move(res_); seq = resSeq_; ready_ = false;
  return true;
}

void StillWorker::Loop() {
  media::FrameScrubber scr(path_);
  for (;;) {
    Req r;
    {
      std::unique_lock lk(mu_);
      cv_.wait(lk, [&] { return quit_ || have_; });
      if (quit_) return;
      r = req_; have_ = false; cancel_ = false;
    }
    media::FrameImage img = scr.Get(r.t, r.w, r.h, r.accurate, &cancel_);
    {
      std::lock_guard lk(mu_);
      if (quit_) return;
      if (img.ok && r.seq == latest_.load()) { res_ = std::move(img); resSeq_ = r.seq; ready_ = true; }
      done_ = r.seq;
    }
    if (hwnd_) PostMessageW(hwnd_, WM_VID_STILL, 0, 0);
  }
}

// ================================================================================================================================
// VideoView
// ================================================================================================================================
bool VideoView::Open(const fs::path& path, HWND notify, std::wstring* err) {
  Close();
  path_ = path; hwnd_ = notify;
  MediaPlayer::Options o; o.maxW = kPreviewMaxW; o.maxH = kPreviewMaxH; o.speed = speed_;
  std::wstring e;
  if (!player_.Open(path, o, &e)) { if (err) *err = e; return false; }
  if (!player_.HasVideo()) { player_.Close(); if (err) *err = L"This file has no video track."; return false; }
  dur_ = player_.Duration(); fps_ = player_.Fps() > 1 ? player_.Fps() : 30.0;
  srcW_ = player_.SourceW(); srcH_ = player_.SourceH(); hasAudio_ = player_.HasAudio();
  player_.Update();                 // consumes the player's "first still" request (one short, one-time wait at open)
  still_.Start(path, notify);
  open_ = true; playing_ = false; resumable_ = false; speedDirty_ = false; needExact_ = false;
  pos_ = 0; rangeEndUsed_ = -1; lastPlayerSerial_ = 0; forceUpload_ = false;
  return true;
}

void VideoView::Close() {
  still_.Stop();
  player_.Close();
  open_ = false; playing_ = false;
  bmp_.Reset(); bw_ = bh_ = 0;
}

int VideoView::FrameIndex(double t) const { return (int)std::floor(std::max(0.0, t) * fps_ + 1e-3); }

void VideoView::RequestStill(double t, bool accurate) {
  double tr = t;
  if (accurate) tr = std::max(0.0, FrameIndex(t) / fps_ - std::min(0.003, 0.25 / fps_));   // land just before the frame's timestamp
  still_.Request(tr, accurate, kPreviewMaxW, kPreviewMaxH);
}

void VideoView::Seek(double t, bool scrubbing) {
  if (!open_) return;
  t = std::clamp(t, 0.0, dur_);
  if (playing_) { player_.Seek(t); pos_ = t; resumable_ = false; return; }
  pos_ = t; resumable_ = false;
  RequestStill(t, !scrubbing);
  needExact_ = scrubbing;
}

void VideoView::Step(int n) {
  if (!open_) return;
  if (playing_) Pause();
  const int last = std::max(0, FrameIndex(dur_ - 0.5 / fps_));
  const int idx = std::clamp(FrameIndex(pos_) + n, 0, last);
  pos_ = idx / fps_; resumable_ = false; needExact_ = false;
  RequestStill(pos_, true);
}

void VideoView::ReopenPlayer(bool audio) {
  player_.Close();
  MediaPlayer::Options o; o.maxW = kPreviewMaxW; o.maxH = kPreviewMaxH; o.speed = speed_; o.wantAudio = audio;
  playerAudio_ = audio;
  player_.Open(path_, o, nullptr);
  player_.Update();
  lastPlayerSerial_ = player_.FrameSerial();
  speedDirty_ = false; resumable_ = false;
}

void VideoView::Play(double end, double from) {
  if (!open_) return;
  double e = end > 0 ? std::min(end, dur_) : dur_;
  double s = pos_;
  if (s >= e - 0.06 || s < from - 0.001) s = std::min(from, std::max(0.0, e - 0.1));
  // keep audible playback away from the end of the audio stream (see kAudioTailGuard)
  const double cap = dur_ - kAudioTailGuard;
  const bool inTail = hasAudio_ && kAudioTailGuard > 0 && s >= cap;
  if (hasAudio_ && kAudioTailGuard > 0 && !inTail && e > cap) e = std::max(cap, s + 0.1);
  if (speedDirty_ || playerAudio_ == inTail) ReopenPlayer(!inTail);
  if (resumable_ && std::fabs(e - rangeEndUsed_) < 1e-6 && s == pos_) player_.Play();
  else player_.PlayRange(s, e);
  rangeEndUsed_ = e; pos_ = s; playing_ = true; resumable_ = false; needExact_ = false;
}

void VideoView::Pause() {
  if (!playing_) return;
  player_.Pause();
  pos_ = player_.Position();
  playing_ = false; resumable_ = true;
}

void VideoView::Upload(ID2D1DeviceContext* dc, const uint8_t* data, int w, int h) {
  if (!dc || w <= 0 || h <= 0 || !data) return;
  if (!bmp_ || bw_ != w || bh_ != h) {
    bmp_.Reset();
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), data, w * 4, &bp, &bmp_))) { bmp_.Reset(); bw_ = bh_ = 0; return; }
    bw_ = w; bh_ = h;
  } else {
    bmp_->CopyFromMemory(nullptr, data, w * 4);
  }
  ++serial_;
}

bool VideoView::Update(ID2D1DeviceContext* dc, bool mouseDown) {
  if (!open_) return false;
  bool changed = false;
  if (playing_ && speedDirty_ && !mouseDown) {            // speed changed while playing: restart the streams at the new rate
    const double e = rangeEndUsed_;
    player_.Pause(); pos_ = player_.Position(); playing_ = false;
    Play(e, 0);
  }
  if (playing_) {
    player_.Update();
    if (player_.FrameSerial() != lastPlayerSerial_) {
      lastPlayerSerial_ = player_.FrameSerial();
      px_ = player_.Frame(); pxW_ = player_.FrameW(); pxH_ = player_.FrameH();
      Upload(dc, px_.data(), pxW_, pxH_);
      changed = true;
    }
    if (!player_.Playing()) { playing_ = false; resumable_ = false; pos_ = player_.AtEnd() ? dur_ : player_.Position(); }
    else pos_ = player_.Position();
  } else if (lastPlayerSerial_ == 0 && player_.FrameSerial() != 0) {
    lastPlayerSerial_ = player_.FrameSerial();
    px_ = player_.Frame(); pxW_ = player_.FrameW(); pxH_ = player_.FrameH();
    Upload(dc, px_.data(), pxW_, pxH_);
    changed = true;
  }
  media::FrameImage img; uint64_t seq = 0;
  if (still_.Take(img, seq) && !playing_) {
    Upload(dc, img.bgra.data(), img.width, img.height);
    pxW_ = img.width; pxH_ = img.height;
    px_ = std::move(img.bgra);
    changed = true;
  }
  if (!playing_ && needExact_ && !mouseDown) { RequestStill(pos_, true); needExact_ = false; }
  if (forceUpload_ && !px_.empty() && !bmp_) { forceUpload_ = false; Upload(dc, px_.data(), pxW_, pxH_); changed = true; }
  return changed;
}

// ================================================================================================================================
// drag helpers
// ================================================================================================================================
FRect RectDrag::Update(float mx, float my, int W, int H, double aspect, float minS) const {
  auto cl = [](float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); };
  const float dx = mx - mx0, dy = my - my0;
  FRect r = r0;
  if (mode == 1) { r.x = cl(r0.x + dx, 0, W - r0.w); r.y = cl(r0.y + dy, 0, H - r0.h); return r; }
  if (mode == 2) {
    float l = r0.x, t = r0.y, rr = r0.x + r0.w, b = r0.y + r0.h;
    const bool horiz = (flags & (kL | kR)) != 0, vert = (flags & (kT | kB)) != 0;
    if (aspect <= 0) {
      if (flags & kL) l = cl(l + dx, 0, rr - minS);
      if (flags & kR) rr = cl(rr + dx, l + minS, (float)W);
      if (flags & kT) t = cl(t + dy, 0, b - minS);
      if (flags & kB) b = cl(b + dy, t + minS, (float)H);
      return {l, t, rr - l, b - t};
    }
    const float a = (float)aspect;
    const float minW = minS * std::max(1.f, a);
    if (horiz && vert) {
      const float ax = (flags & kL) ? rr : l, ay = (flags & kT) ? b : t;
      const float sx = (flags & kL) ? -1.f : 1.f, sy = (flags & kT) ? -1.f : 1.f;
      const float tx = ((flags & kL) ? l : rr) + dx, ty = ((flags & kT) ? t : b) + dy;
      const float dw = sx * (tx - ax), dh = sy * (ty - ay);
      float w = std::max(dw, dh * a);
      const float maxW = std::max(minW, std::min(sx > 0 ? W - ax : ax, (sy > 0 ? H - ay : ay) * a));
      w = cl(w, minW, maxW);
      const float h = w / a;
      r.w = w; r.h = h; r.x = sx > 0 ? ax : ax - w; r.y = sy > 0 ? ay : ay - h;
      return r;
    }
    if (horiz) {
      const float ax = (flags & kL) ? rr : l, sx = (flags & kL) ? -1.f : 1.f;
      const float cy = r0.y + r0.h / 2;
      const float target = ((flags & kL) ? l : rr) + dx;
      const float maxW = std::max(minW, std::min(sx > 0 ? W - ax : ax, 2 * std::min(cy, H - cy) * a));
      const float w = cl(sx * (target - ax), minW, maxW);
      const float h = w / a;
      r.w = w; r.h = h; r.x = sx > 0 ? ax : ax - w; r.y = cy - h / 2;
      return r;
    }
    if (vert) {
      const float ay = (flags & kT) ? b : t, sy = (flags & kT) ? -1.f : 1.f;
      const float cx = r0.x + r0.w / 2;
      const float target = ((flags & kT) ? t : b) + dy;
      const float minH = minW / a;
      const float maxH = std::max(minH, std::min(sy > 0 ? H - ay : ay, 2 * std::min(cx, W - cx) / a));
      const float h = cl(sy * (target - ay), minH, maxH);
      const float w = h * a;
      r.w = w; r.h = h; r.y = sy > 0 ? ay : ay - h; r.x = cx - w / 2;
      return r;
    }
    return r0;
  }
  if (mode == 3) {
    const float ax = mx0, ay = my0;
    const float cx = cl(mx, 0, (float)W), cy = cl(my, 0, (float)H);
    const float sx = cx >= ax ? 1.f : -1.f, sy = cy >= ay ? 1.f : -1.f;
    float dw = std::fabs(cx - ax), dh = std::fabs(cy - ay);
    if (aspect > 0) {
      const float a = (float)aspect;
      float w = std::max(dw, dh * a);
      w = std::min(w, std::min(sx > 0 ? W - ax : ax, (sy > 0 ? H - ay : ay) * a));
      dw = w; dh = w / a;
    }
    r.x = sx > 0 ? ax : ax - dw; r.y = sy > 0 ? ay : ay - dh; r.w = dw; r.h = dh;
    return r;
  }
  return r0;
}

int HitHandle(const Rect& r, float mx, float my, float tol, bool* inside) {
  const bool nearL = std::fabs(mx - r.x) <= tol, nearR = std::fabs(mx - r.right()) <= tol;
  const bool nearT = std::fabs(my - r.y) <= tol, nearB = std::fabs(my - r.bottom()) <= tol;
  const bool withinX = mx >= r.x - tol && mx <= r.right() + tol, withinY = my >= r.y - tol && my <= r.bottom() + tol;
  int f = 0;
  if (withinY) { if (nearL && (!nearR || std::fabs(mx - r.x) <= std::fabs(mx - r.right()))) f |= kL; else if (nearR) f |= kR; }
  if (withinX) { if (nearT && (!nearB || std::fabs(my - r.y) <= std::fabs(my - r.bottom()))) f |= kT; else if (nearB) f |= kB; }
  if (inside) *inside = r.Contains(mx, my);
  return f;
}

int CursorForFlags(int f) {
  const bool l = f & kL, r = f & kR, t = f & kT, b = f & kB;
  if ((l && t) || (r && b)) return (int)(intptr_t)IDC_SIZENWSE;
  if ((r && t) || (l && b)) return (int)(intptr_t)IDC_SIZENESW;
  if (l || r) return (int)(intptr_t)IDC_SIZEWE;
  if (t || b) return (int)(intptr_t)IDC_SIZENS;
  return 0;
}

IRect Quantize(const FRect& r, int W, int H, bool even) {
  IRect q;
  q.x = (int)std::floor(r.x + 0.5f); q.y = (int)std::floor(r.y + 0.5f);
  q.w = (int)std::floor(r.w + 0.5f); q.h = (int)std::floor(r.h + 0.5f);
  if (even) { q.x &= ~1; q.y &= ~1; q.w &= ~1; q.h &= ~1; }
  const int minS = even ? 2 : 1;
  q.w = std::clamp(q.w, minS, std::max(minS, even ? (W & ~1) : W)); q.h = std::clamp(q.h, minS, std::max(minS, even ? (H & ~1) : H));
  if (q.x + q.w > W) q.x = even ? ((W - q.w) & ~1) : W - q.w;
  if (q.y + q.h > H) q.y = even ? ((H - q.h) & ~1) : H - q.h;
  q.x = std::max(0, q.x); q.y = std::max(0, q.y);
  return q;
}

static void BlurPass(const uint8_t* src, uint8_t* dst, int len, int count, int pixStep, int lineStep, int r) {
  const int win = 2 * r + 1;
  for (int l = 0; l < count; ++l) {
    const uint8_t* s = src + (size_t)l * lineStep * 4;
    uint8_t* d = dst + (size_t)l * lineStep * 4;
    int sum[3] = {0, 0, 0};
    for (int k = -r; k <= r; ++k) {
      const uint8_t* q = s + (size_t)std::clamp(k, 0, len - 1) * pixStep * 4;
      sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2];
    }
    for (int i = 0; i < len; ++i) {
      uint8_t* o = d + (size_t)i * pixStep * 4;
      o[0] = (uint8_t)(sum[0] / win); o[1] = (uint8_t)(sum[1] / win); o[2] = (uint8_t)(sum[2] / win); o[3] = 255;
      const uint8_t* qa = s + (size_t)std::clamp(i + r + 1, 0, len - 1) * pixStep * 4;
      const uint8_t* qs = s + (size_t)std::clamp(i - r, 0, len - 1) * pixStep * 4;
      sum[0] += qa[0] - qs[0]; sum[1] += qa[1] - qs[1]; sum[2] += qa[2] - qs[2];
    }
  }
}

void BoxBlurBGRA(std::vector<uint8_t>& px, int w, int h, int radius) {
  if (w < 3 || h < 3 || px.size() < (size_t)w * h * 4) return;
  const int r = std::clamp(radius, 1, std::max(1, std::min(w, h) / 2 - 1));
  std::vector<uint8_t> tmp(px.size());
  for (int it = 0; it < 3; ++it) {
    BlurPass(px.data(), tmp.data(), w, h, 1, w, r);
    BlurPass(tmp.data(), px.data(), h, w, w, 1, r);
  }
}

// ================================================================================================================================
// VideoToolDialog
// ================================================================================================================================
VideoToolDialog::VideoToolDialog(const Selection& sel) {
  selection_ = sel;
  path_ = sel.items.empty() ? fs::path() : sel.items[0].path;
  info_ = media::SummarizeMedia(path_);
}

VideoToolDialog::~VideoToolDialog() { view_.Close(); }

std::wstring VideoToolDialog::ProblemText() const {
  if (!info_.ok) return info_.error.empty() ? L"This file cannot be opened." : info_.error;
  if (!info_.hasVideo) return L"This file has no video track.";
  if (info_.duration <= 0) return L"This video has no duration.";
  return L"";
}

bool UiRtl() {
  wchar_t b[8] = {};
  if (GetEnvironmentVariableW(L"FILO_VID_RTL", b, 8) && b[0] == L'1') return true;      // test hook: render the layout mirrored
  return SettingsStore::Get().Snapshot().language == L"ar";
}

void VideoToolDialog::OnOpened() {
  if (Usable()) {
    std::wstring e;
    if (!view_.Open(path_, Hwnd(), &e)) viewError_ = e.empty() ? L"The preview could not be started." : e;
  }
  ToolDialog::OnOpened();
  // test hook: FILO_VID_PRESET='{"..."}' applies a preset after the saved one (used for screenshots)
  wchar_t buf[4096] = {};
  if (GetEnvironmentVariableW(L"FILO_VID_PRESET", buf, 4096)) {
    if (auto j = Json::Parse(Narrow(buf))) LoadPreset(*j);
  }
}

LRESULT VideoToolDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_VID_STILL) { Invalidate(); SetHandled(true); return 0; }
  return ToolDialog::OnMessage(msg, wp, lp);
}

void VideoToolDialog::OnDeviceReset(bool rebuilt) {
  ToolDialog::OnDeviceReset(rebuilt);
  view_.ResetDevice();
}

void VideoToolDialog::MaybeSelfTest() {
  wchar_t logPath[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"FILO_VID_SELFTEST", logPath, MAX_PATH)) return;
  std::wstring log;
  auto Log = [&](const std::wstring& s) {
    log += s + L"\r\n";
    FILE* lf = nullptr;
    if (_wfopen_s(&lf, logPath, L"wb") == 0 && lf) { fwrite(log.data(), sizeof(wchar_t), log.size(), lf); fclose(lf); }
  };
  auto pump = [&](double seconds, const std::function<bool()>& until = {}) {
    const double end = NowSeconds() + seconds;
    MSG m;
    while (NowSeconds() < end) {
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
      RenderNow(false);
      if (until && until()) return true;
      Sleep(8);
    }
    return false;
  };
  wchar_t b[256];
  auto pos = [&] { return view_.Pos(); };
  const double t0 = NowSeconds();
  bool got = pump(4.0, [&] { return view_.Bitmap() != nullptr; });
  swprintf_s(b, L"open: bitmap=%d after %.0f ms size=%dx%d dur=%.2f fps=%.2f", got, (NowSeconds() - t0) * 1000, view_.FrameW(), view_.FrameH(), view_.Duration(), view_.Fps()); Log(b);
  // frame step (async still)
  uint64_t s0 = view_.Serial(); double ts = NowSeconds();
  view_.Step(1);
  got = pump(3.0, [&] { return view_.Serial() != s0 && !view_.StillPending(); });
  swprintf_s(b, L"step +1: pos=%.3f picture-updated=%d in %.0f ms", pos(), got, (NowSeconds() - ts) * 1000); Log(b);
  s0 = view_.Serial(); ts = NowSeconds();
  view_.Seek(5.0, false);
  got = pump(3.0, [&] { return view_.Serial() != s0 && !view_.StillPending(); });
  swprintf_s(b, L"seek 5.0 (paused): pos=%.3f picture-updated=%d in %.0f ms", pos(), got, (NowSeconds() - ts) * 1000); Log(b);
  // scrubbing: many rapid seeks, only the last may be shown
  s0 = view_.Serial(); ts = NowSeconds();
  for (int i = 0; i < 12; ++i) { view_.Seek(2.0 + i * 0.1, true); pump(0.02); }
  view_.Seek(3.1, false);
  got = pump(4.0, [&] { return !view_.StillPending() && view_.Serial() != s0; });
  swprintf_s(b, L"scrub x12 then exact 3.1: pos=%.3f settled=%d in %.0f ms (UI never blocked: pump loop kept running)", pos(), got, (NowSeconds() - ts) * 1000); Log(b);
  // playback
  view_.Seek(1.0, false); pump(1.0, [&] { return !view_.StillPending(); });
  s0 = view_.Serial(); const double p0 = pos(); ts = NowSeconds();
  view_.Play(-1, 0);
  pump(1.5);
  swprintf_s(b, L"play 1.5 s: pos %.3f -> %.3f (advanced %.3f s) pictures=%llu playing=%d", p0, pos(), pos() - p0, (unsigned long long)(view_.Serial() - s0), view_.Playing()); Log(b);
  view_.Pause(); const double pp = pos(); pump(0.5);
  swprintf_s(b, L"pause: pos=%.3f stable-after-0.5s=%d", pp, std::fabs(pos() - pp) < 0.001); Log(b);
  view_.Play(-1, 0); pump(0.8);
  swprintf_s(b, L"resume: pos=%.3f (expected about %.3f)", pos(), pp + 0.8); Log(b);
  view_.Seek(8.0, false); pump(0.6);
  swprintf_s(b, L"seek 8.0 while playing: pos=%.3f playing=%d", pos(), view_.Playing()); Log(b);
  view_.Pause();
  view_.SetSpeed(2.0); view_.Seek(2.0, false); pump(0.8, [&] { return !view_.StillPending(); });
  const double q0 = pos(); view_.Play(-1, 0); pump(1.5);
  swprintf_s(b, L"play at 2x for 1.5 s: pos %.3f -> %.3f (advanced %.3f source s, expected ~3)", q0, pos(), pos() - q0); Log(b);
  view_.Pause(); view_.SetSpeed(1.0);
  view_.PlayRange(10.0, 10.8); pump(2.0, [&] { return !view_.Playing(); });
  swprintf_s(b, L"play range 10.0-10.8: playing=%d pos=%.3f", view_.Playing(), pos()); Log(b);
  view_.Seek(Dur() - 0.2, false); pump(0.5, [&] { return !view_.StillPending(); });
  view_.Play(-1, 0); pump(2.5, [&] { return !view_.Playing(); });
  swprintf_s(b, L"play to the end: playing=%d pos=%.3f (duration %.2f)", view_.Playing(), pos(), Dur()); Log(b);
  FILE* f = nullptr;
  if (_wfopen_s(&f, logPath, L"wb") == 0 && f) { fwrite(log.data(), sizeof(wchar_t), log.size(), f); fclose(f); }
}

void VideoToolDialog::BeginFrame(Ui& ui) { view_.Update(dc_.Get(), ui.In().down); }

VideoToolDialog::WellGeom VideoToolDialog::DrawWell(Ui& ui, const Rect& well) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  WellGeom g; g.well = well;
  p.FillRoundRect(well, dialog::wellRadius, pal.darkCanvas);
  const float inset = 14.f;
  Rect avail = well.Inset(inset);
  float sw = (float)SrcW(), sh = (float)SrcH();
  if (sw <= 0 || sh <= 0) { sw = 16; sh = 9; }
  const float s = std::max(0.01f, std::min(avail.w / sw, avail.h / sh));
  g.scale = s;
  g.image = {avail.cx() - sw * s / 2, avail.cy() - sh * s / 2, sw * s, sh * s};
  if (view_.Bitmap() && Usable()) {
    p.Bitmap(view_.Bitmap(), g.image, 1.f, 5.f);
  } else {
    TextStyle es; es.size = 14; es.align = Align::Center; es.wrap = true; es.ellipsis = false;
    const std::wstring prob = ProblemText();
    if (!prob.empty() || !viewError_.empty()) {
      p.Icon("warning", well.cx(), well.cy() - 26, 26, Color(1, 1, 1, 0.55f), 1.8f);
      p.Text({well.x + 40, well.cy() - 6, well.w - 80, 60}, prob.empty() ? viewError_ : prob, Color(1, 1, 1, 0.70f), es);
    } else {
      p.FillRoundRect(g.image, 5, Color(1, 1, 1, 0.05f));
      p.Icon("video", well.cx(), well.cy() - 14, 28, Color(1, 1, 1, 0.35f), 1.6f);
      p.Text({well.x, well.cy() + 8, well.w, 24}, L"Loading preview...", Color(1, 1, 1, 0.55f), es);
    }
  }
  return g;
}

bool VideoToolDialog::DrawWellSink(Ui& ui, const WellGeom& g, const SinkOptions& o) {
  const uint64_t id = ui.Id("wellsink");
  Interaction it = ui.Behavior(g.well, id, view_.IsOpen(), true);
  if (!kbFocusDone_) { ui.Focus(id); kbFocusDone_ = true; }
  const InputState& in = ui.In();
  bool keyClick = false;
  for (const auto& k : in.keys) if (k.vk == VK_SPACE || k.vk == VK_RETURN) keyClick = true;
  bool acted = false;
  if (o.clickToggles && it.clicked && !keyClick && !it.dragging) { view_.Toggle(o.end, o.from); acted = true; }
  if (it.focused) {
    for (const auto& k : in.keys) {
      const int n = k.shift ? 10 : 1;
      if (k.vk == VK_SPACE) { view_.Toggle(o.end, o.from); acted = true; }
      else if (o.arrowsStep && k.vk == VK_LEFT) { view_.Step(-n); acted = true; }
      else if (o.arrowsStep && k.vk == VK_RIGHT) { view_.Step(n); acted = true; }
      else if (k.vk == VK_OEM_COMMA) { view_.Step(-n); acted = true; }
      else if (k.vk == VK_OEM_PERIOD) { view_.Step(n); acted = true; }
      else if (k.vk == VK_HOME) { view_.Seek(0, false); acted = true; }
      else if (k.vk == VK_END) { view_.Seek(Dur(), false); acted = true; }
    }
  }
  if (it.hot && o.clickToggles && Usable()) ui.SetCursor((int)(intptr_t)IDC_HAND);
  return acted;
}

void VideoToolDialog::DrawTransport(Ui& ui, const Rect& row, double end, double from, bool showFrames) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const bool ok = view_.IsOpen();
  auto c = ui.Cols(row, {Fixed(42), Fixed(186), Flex(1), Fixed(30), Fixed(30)}, 8);
  ui.PushId(0x7A01);
  if (ui.IconButton({c[0].x, row.cy() - 15, 42, 30}, view_.Playing() ? "pause" : "play", ok, view_.Playing() ? L"Pause (Space)" : L"Play (Space)", true)) view_.Toggle(end, from);
  ui.PopId();
  // time readout: current in bold mono, total in tertiary
  const std::wstring cur = FormatTime(view_.IsOpen() ? view_.Pos() : 0.0, true), tot = L"/ " + FormatTime(Dur(), true);
  TextStyle cs = MonoStyle(15.f, 600), ts = MonoStyle(13.f, 400);
  const float w1 = p.TextWidth(cur, cs), w2 = p.TextWidth(tot, ts);
  if (!ui.Rtl()) {
    p.Text({c[1].x, row.y, w1 + 4, row.h}, cur, pal.text, cs);
    p.Text({c[1].x + w1 + 10, row.y, w2 + 4, row.h}, tot, pal.textTertiary, ts);
  } else {
    p.Text({c[1].right() - w1 - 4, row.y, w1 + 4, row.h}, cur, pal.text, cs);
    p.Text({c[1].right() - w1 - 10 - w2 - 4, row.y, w2 + 4, row.h}, tot, pal.textTertiary, ts);
  }
  if (showFrames) {
    TextStyle fst; fst.size = 12.5f; fst.align = Align::End;
    const int idx = view_.IsOpen() ? view_.FrameIndex(view_.Pos()) : 0;
    p.Text(c[2], L"Frame " + std::to_wstring(idx + 1), pal.textTertiary, fst);
    ui.PushId(0x7A02);
    if (ui.IconButton({c[3].x, row.cy() - 14, 30, 28}, "chevronleft", ok, L"Previous frame (Left arrow)", true)) view_.Step(-1);
    if (ui.IconButton({c[4].x, row.cy() - 14, 30, 28}, "chevronright", ok, L"Next frame (Right arrow)", true)) view_.Step(1);
    ui.PopId();
  }
}

bool VideoToolDialog::DrawScrubber(Ui& ui, const Rect& r, const std::vector<double>* markers) {
  double pos = view_.IsOpen() ? view_.Pos() : 0.0;
  TimelineResult tl = Timeline(ui, r, Dur(), pos, nullptr, nullptr, markers, nullptr, 1.0 / Fps());
  if (tl.seeked && view_.IsOpen()) { view_.Seek(pos, tl.scrubbing); return true; }
  return false;
}

void VideoToolDialog::DrawDoneNote(Ui& ui, const Rect& content, const std::vector<std::wstring>& lines) {
  if (Phase() != JobPhase::Done || lines.empty()) return;
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const float cw = std::min(content.w - 48, 380.f);
  float y = content.cy() + 110 + 12;
  TextStyle st; st.size = 12.5f; st.align = Align::Center; st.wrap = true; st.ellipsis = false; st.vcenter = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    TextStyle s2 = st; s2.weight = i == 0 ? 600 : 400;
    p.Text({content.cx() - cw / 2, y, cw, 20}, lines[i], i == 0 ? pal.text : pal.textSecondary, s2);
    y += 20;
  }
}

} // namespace filo::tools::vid
