#include "tools/audio/audio_common.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>

namespace filo::tools::audio {

// ------------------------------------------------------------------------------------------------ WaveData
void WaveData::Set(media::Waveform&& w) {
  wf = std::move(w);
  float m = 0.f;
  for (float v : wf.peaks) m = std::max(m, v);
  norm = std::max(m, 0.02f);
}
float WaveData::PeakRange(double t0, double t1) const {
  if (!Ok() || wf.duration <= 0) return 0.f;
  const int n = (int)wf.peaks.size();
  int b0 = std::clamp((int)std::floor(t0 / wf.duration * n), 0, n - 1);
  int b1 = std::clamp((int)std::ceil(t1 / wf.duration * n), b0 + 1, n);
  float pk = 0.f;
  for (int k = b0; k < b1; ++k) pk = std::max(pk, wf.peaks[k]);
  return std::min(1.f, pk / norm);
}
float WaveData::PeakAt(double t) const { return PeakRange(t, t + wf.duration / (double)std::max<size_t>(1, wf.peaks.size())); }

// ------------------------------------------------------------------------------------------------ AudioDialog
AudioDialog::~AudioDialog() {
  cancel_->store(true);
  player_.reset();
  for (auto& f : temps_) { std::error_code ec; fs::remove(f, ec); }
}

void AudioDialog::OnOpened() {
  ToolDialog::OnOpened();
  // visual tests: FILO_TEST_PRESET applies a preset (same JSON as --tool-run) before the loads start
  if (const wchar_t* e = _wgetenv(L"FILO_TEST_PRESET")) { if (auto j = Json::Parse(Narrow(e))) LoadPreset(*j); }
  if (!Path().empty()) Begin();
  if (_wgetenv(L"FILO_TEST_DPI")) {                     // headless renders wait for the async loads so the screenshot shows real data
    Settle(20.0);
    if (const wchar_t* ms = _wgetenv(L"FILO_TEST_PLAY")) {
      TestPlay();
      Settle(20.0);
      const double until = NowSeconds() + _wtof(ms) / 1000.0;
      MSG m;
      while (NowSeconds() < until) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(10); }
    }
  }
}

void AudioDialog::Settle(double timeoutSec) {
  const double until = NowSeconds() + timeoutSec;
  MSG m;
  while (pending_ > 0 && NowSeconds() < until) {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Sleep(10);
  }
}

void AudioDialog::OnTick(double) {
  if (!player_ || !player_->IsOpen()) return;
  const bool was = player_->Playing();
  player_->Update();
  if (was && !player_->Playing()) { playTag_.clear(); Invalidate(); }
}

LRESULT AudioDialog::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_AUDIO_ASYNC) {
    std::unique_ptr<std::function<void()>> f(reinterpret_cast<std::function<void()>*>(lp));
    if (pending_ > 0) --pending_;
    if (f && *f) (*f)();
    Invalidate(); SetHandled(true); return 0;
  }
  return ToolDialog::OnMessage(msg, wp, lp);
}

void AudioDialog::RunAsync(std::function<std::function<void()>()> work) {
  HWND hw = Hwnd();
  ++pending_;
  std::thread([hw, work = std::move(work)]() mutable {
    std::function<void()> done = work();
    auto* heap = new std::function<void()>(std::move(done));
    if (!PostMessageW(hw, WM_AUDIO_ASYNC, 0, (LPARAM)heap)) delete heap;
  }).detach();
}

void AudioDialog::LoadWave(int buckets) {
  waveLoading_ = true; waveError_.clear();
  const fs::path p = Path();
  auto cancel = cancel_;
  RunAsync([this, p, buckets, cancel]() -> std::function<void()> {
    media::Waveform w = media::ComputeWaveform(p, buckets, cancel.get());
    return [this, w = std::move(w)]() mutable {
      waveLoading_ = false;
      if (w.ok) { wave_.Set(std::move(w)); OnWaveLoaded(); }
      else waveError_ = w.error.empty() ? L"This audio file cannot be read." : w.error;
    };
  });
}

fs::path AudioDialog::NewTempWav() {
  std::error_code ec;
  fs::path dir = TempDir();
  fs::create_directories(dir, ec);
  fs::path f = dir / (L"filo-audio-preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring((uintptr_t)this & 0xFFFFF) + L"-" + std::to_wstring(++tempCounter_) + L".wav");
  temps_.push_back(f);
  return f;
}

void AudioDialog::PreparePlayer(const fs::path& file) {
  RunAsync([this, file]() -> std::function<void()> {
    auto mp = std::make_unique<MediaPlayer>();
    MediaPlayer::Options o; o.wantVideo = false;
    std::wstring err;
    if (!mp->Open(file, o, &err)) mp.reset();
    auto holder = std::make_shared<std::unique_ptr<MediaPlayer>>(std::move(mp));
    return [this, holder, err]() {
      if (!*holder) { notice_ = err.empty() ? L"This file cannot be played." : err; return; }
      player_ = std::move(*holder);
    };
  });
}

bool AudioDialog::PlayRange(double from, double to, double posOffset, const std::wstring& tag) {
  if (!PlayerReady()) return false;
  notice_.clear();
  playOffset_ = posOffset; playTag_ = tag;
  const double end = to > from ? to : std::max(from + 0.05, player_->Duration());
  player_->PlayRange(from, end);
  Invalidate();
  return true;
}

void AudioDialog::StopPlayback() {
  if (player_) player_->Pause();
  playTag_.clear();
  Invalidate();
}

void AudioDialog::RenderAndPlay(const std::wstring& tag, double posOffset, std::function<EngineResult(JobContext&, const fs::path&)> render) {
  if (Rendering()) return;
  StopPlayback();
  renderTag_ = tag; notice_.clear();
  const fs::path wav = NewTempWav();
  RunAsync([render = std::move(render), wav, tag, posOffset, this]() -> std::function<void()> {
    JobContext ctx;
    EngineResult r = render(ctx, wav);
    std::unique_ptr<MediaPlayer> mp;
    std::wstring err = r.error;
    if (r.ok) {
      mp = std::make_unique<MediaPlayer>();
      MediaPlayer::Options o; o.wantVideo = false;
      if (!mp->Open(wav, o, &err)) { mp.reset(); if (err.empty()) err = L"The preview could not be played."; }
    }
    auto holder = std::make_shared<std::unique_ptr<MediaPlayer>>(std::move(mp));
    return [this, holder, err, tag, posOffset]() {
      renderTag_.clear();
      if (!*holder) { notice_ = err.empty() ? L"The preview could not be rendered." : err; return; }
      player_ = std::move(*holder);
      PlayRange(0, 0, posOffset, tag);
    };
  });
}

// ------------------------------------------------------------------------------------------------ footer / buttons
bool AudioDialog::Footer(Ui& ui, const Rect& body, const std::wstring& status, bool err, const std::wstring& action, bool enabled) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const float fh = status.empty() ? 67.f : 100.f;
  const float y0 = body.bottom() - fh;
  p.FillRect({body.x, y0, body.w, 1.f}, pal.divider);
  ui.PushId(0xA0F007);
  if (!status.empty()) {
    TextStyle st; st.size = dialog::bodyFontSize; st.wrap = true; st.ellipsis = true;
    Rect sr = Mir(ui, body, {body.x + 21.f, body.bottom() - 68.5f - 12.f, body.w - 42.f, 24.f});
    p.Text(sr, status, err ? Color::Hex(0xC2410C) : Info(pal), st);
  }
  TextStyle ts; ts.size = dialog::bodyFontSize; ts.weight = 500;
  const float bw = std::max(64.f, p.TextWidth(action, ts) + 28.f);
  Rect br = Mir(ui, body, {body.right() - 20.5f - bw, body.bottom() - 33.f - kBtnH / 2, bw, kBtnH});
  const bool pressed = ui.Button(br, action, ButtonStyle::Primary, enabled && !Locked());
  ui.PopId();
  return pressed;
}

bool AudioDialog::GlyphButton(Ui& ui, const Rect& r, const std::wstring& label, const char* icon, bool enabled) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const uint64_t id = ui.Id(label + L"#gb");
  Interaction it = ui.Behavior(r, id, enabled, true);
  if (it.hot && enabled) ui.SetCursor((int)(intptr_t)IDC_HAND);
  const float hov = ui.Anim(id * 3 + 1, it.hot ? 1.f : 0.f), prs = ui.Anim(id * 3 + 2, it.active && it.hot ? 1.f : 0.f, 0.06f);
  Color fill = enabled ? Mix(Mix(pal.controlFill, accent::base.WithAlpha(0.22f), hov), accent::base.WithAlpha(0.36f), prs) : pal.controlFillDisabled;
  p.FillRoundRect(r, control::buttonRadius, fill);
  const Color txt = enabled ? pal.text : pal.textDisabled;
  TextStyle ts; ts.size = dialog::bodyFontSize; ts.weight = 400; ts.align = Align::Center;
  if (!icon || !*icon) { p.Text(r, label, txt, ts); return it.clicked && enabled; }
  const float box = 22.f, gw = box * 0.5f, gap = 11.f, tw = p.TextWidth(label, ts);
  const float total = gw + gap + tw, x0 = r.cx() - total / 2;
  const float off = std::string(icon) == "play" ? box * (2.f / 24.f) : 0.f;
  if (ui.Rtl()) { p.Icon(icon, x0 + total - gw / 2 + off, r.cy(), box, txt, 1.6f); p.Text({x0, r.y, tw + 2, r.h}, label, txt, ts); }
  else { p.Icon(icon, x0 + gw / 2 - off, r.cy(), box, txt, 1.6f); p.Text({x0 + gw + gap, r.y, tw + 2, r.h}, label, txt, ts); }
  return it.clicked && enabled;
}

bool AudioDialog::GlyphOnly(Ui& ui, const Rect& r, const char* icon, float size, bool enabled) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  const uint64_t id = ui.AutoId("glyph");
  Interaction it = ui.Behavior(r, id, enabled, true);
  if (it.hot && enabled) ui.SetCursor((int)(intptr_t)IDC_HAND);
  const float hov = ui.Anim(id * 3 + 1, it.hot ? 1.f : 0.f);
  const float off = std::string(icon) == "play" ? size * (2.f / 24.f) : 0.f;
  p.Icon(icon, r.cx() - off, r.cy(), size, enabled ? Mix(pal.text, accent::base, hov) : pal.textDisabled, 1.6f);
  return it.clicked && enabled;
}

// ------------------------------------------------------------------------------------------------ bars
void DrawPeakBars(Painter& p, const Rect& r, const std::vector<float>& peaks, float scale, float norm, const Color& c, const BarStyle& bs, float heightFrac) {
  if (peaks.empty() || r.w <= 1) return;
  const int n = std::max(1, (int)(r.w / bs.pitch));
  const float pitch = r.w / n;
  const float mx = std::max(norm, 0.02f);
  const int N = (int)peaks.size();
  for (int i = 0; i < n; ++i) {
    const int b0 = std::clamp((int)((double)i * N / n), 0, N - 1), b1 = std::clamp((int)std::ceil((double)(i + 1) * N / n), b0 + 1, N);
    float pk = 0; for (int k = b0; k < b1; ++k) pk = std::max(pk, peaks[k]);
    const float h = std::max(1.6f, std::min(1.f, pk * scale / mx) * r.h * heightFrac);
    p.FillRoundRect({r.x + i * pitch + (pitch - bs.barW) / 2, r.cy() - h / 2, bs.barW, h}, bs.radius, c);
  }
}

void DrawBars(Painter& p, const Rect& r, const WaveData& wd, double dur, const Color& c, const BarStyle& bs, float heightFrac, float selX0, float selX1, const Color* outside) {
  if (!wd.Ok() || dur <= 0 || r.w <= 1) return;
  const int n = std::max(1, (int)(r.w / bs.pitch));
  const float pitch = r.w / n;
  for (int i = 0; i < n; ++i) {
    const float x = r.x + i * pitch + (pitch - bs.barW) / 2;
    const float pk = wd.PeakRange(dur * i / n, dur * (i + 1) / n);
    const float h = std::max(1.6f, pk * r.h * heightFrac);
    const float cx = x + bs.barW / 2;
    p.FillRoundRect({x, r.cy() - h / 2, bs.barW, h}, bs.radius, (outside && (cx < selX0 || cx > selX1)) ? *outside : c);
  }
}

// ------------------------------------------------------------------------------------------------ Trim box
static double XToT(float x0, float w, float x, double dur) { return std::clamp((double)((x - x0) / std::max(1.f, w)), 0.0, 1.0) * dur; }

TrimEdit TrimBox(Ui& ui, const Rect& box, const WaveData& wd, double dur, double& start, double& end, double playhead, bool enabled, TrimState& st) {
  Painter& p = ui.P();
  TrimEdit out;
  const float R = 10.f, bw = 19.5f, border = 2.f;
  const Rect bars{box.x + bw, box.y, box.w - 2 * bw, box.h};
  auto tx = [&](double t) { return bars.x + (float)(dur > 0 ? std::clamp(t / dur, 0.0, 1.0) : 0.0) * bars.w; };
  const uint64_t id = ui.AutoId("trimbox");
  Interaction it = ui.Behavior(box, id, enabled && dur > 0, true);
  const InputState& in = ui.In();
  float xa = tx(start), xb = tx(end);
  const double minGap = 0.05;
  if (enabled && dur > 0) {
    if (it.pressed) {
      const float ca = xa - bw / 2, cb = xb + bw / 2;
      const float da = std::fabs(in.mx - ca), db = std::fabs(in.mx - cb);
      const float reach = bw / 2 + 9.f;
      if (da <= reach || db <= reach) { st.drag = da <= db ? 1 : 2; st.last = st.drag; st.grab = in.mx - (st.drag == 1 ? xa : xb); }
      else { st.drag = 0; out.seek = XToT(bars.x, bars.w, in.mx, dur); }
    }
    if (!it.active) st.drag = 0;
    if (st.drag && it.active) {
      const double t = XToT(bars.x, bars.w, in.mx - st.grab, dur);
      if (st.drag == 1) { const double v = std::clamp(t, 0.0, end - minGap); if (v != start) { start = v; out.changed = true; } }
      else { const double v = std::clamp(t, start + minGap, dur); if (v != end) { end = v; out.changed = true; } }
      ui.RequestFrame();
    }
    if (it.focused) for (const auto& k : in.keys) {
      const double step = k.shift ? 1.0 : 0.05;
      double d = k.vk == VK_LEFT ? -step : (k.vk == VK_RIGHT ? step : 0.0);
      if (d == 0.0) continue;
      if (st.last == 1) { const double v = std::clamp(start + d, 0.0, end - minGap); if (v != start) { start = v; out.changed = true; } }
      else { const double v = std::clamp(end + d, start + minGap, dur); if (v != end) { end = v; out.changed = true; } }
    }
    xa = tx(start); xb = tx(end);
    if (it.hot || st.drag) {
      const float da = std::fabs(in.mx - (xa - bw / 2)), db = std::fabs(in.mx - (xb + bw / 2));
      if (st.drag || std::min(da, db) <= bw / 2 + 9.f) ui.SetCursor((int)(intptr_t)IDC_SIZEWE);
    }
  }
  // ---- draw
  p.FillRoundRect(box, R, accent::base.WithAlpha(0.13f));
  p.PushClipRoundRect(box, R);
  const Rect inner = bars.Inset(4.f, 0.f);
  const Color dim = accent::base.WithAlpha(0.32f);
  DrawBars(p, inner, wd, dur, accent::base, BarStyle{}, 0.80f, xa, xb, &dim);
  // trimmed-out parts are veiled
  p.FillRect({bars.x, box.y, std::max(0.f, xa - bw - bars.x), box.h}, Color(0.5f, 0.5f, 0.55f, 0.28f));
  p.FillRect({xb + bw, box.y, std::max(0.f, bars.right() - xb - bw), box.h}, Color(0.5f, 0.5f, 0.55f, 0.28f));
  const Rect ha{std::max(box.x, xa - bw), box.y, bw, box.h}, hb{xb, box.y, bw, box.h};
  p.FillRect({box.x, box.y, ha.right() - box.x, box.h}, accent::base);         // left block (extends to the box edge)
  p.FillRect({hb.x, box.y, box.right() - hb.x, box.h}, accent::base);          // right block
  const Color grip(1, 1, 1, 0.95f);
  p.FillRoundRect({ha.right() - bw / 2 - 1.5f, box.cy() - 14.f, 3.f, 28.f}, 1.5f, grip);
  p.FillRoundRect({hb.x + bw / 2 - 1.5f, box.cy() - 14.f, 3.f, 28.f}, 1.5f, grip);
  if (playhead >= 0 && dur > 0) p.FillRect({tx(playhead) - 1.f, box.y, 2.f, box.h}, Color(1, 1, 1, 0.95f));
  p.PopClip();
  p.StrokeRoundRect(box.Inset(border / 2), R - border / 2, accent::base, border);
  if (it.focused) p.StrokeRoundRect(box.Inset(-2.5f), R + 2.5f, accent::base.WithAlpha(0.45f), 1.5f);
  return out;
}

// ------------------------------------------------------------------------------------------------ Bleep lanes
BleepEdit BleepLanes(Ui& ui, const Rect& rl, const Rect& al, const WaveData& wd, double dur, std::vector<media::BleepRange>& ranges, BleepState& st, double playhead, bool enabled) {
  Painter& p = ui.P();
  const Palette& pal = ui.Pal();
  BleepEdit out;
  const float laneR = 8.f;
  const double minLen = 0.05;
  auto tx = [&](double t) { return rl.x + (float)(dur > 0 ? std::clamp(t / dur, 0.0, 1.0) : 0.0) * rl.w; };
  auto xt = [&](float x) { return XToT(rl.x, rl.w, x, dur); };
  const InputState& in = ui.In();
  // ---- interaction
  const uint64_t idR = ui.AutoId("bleeplane"), idA = ui.AutoId("audiolane");
  Interaction ir = ui.Behavior(rl, idR, enabled && dur > 0, true);
  Interaction ia = ui.Behavior(al, idA, enabled && dur > 0, true);
  if (enabled && dur > 0) {
    if (ir.pressed) {
      const double t = xt(in.mx);
      st.mode = 0; st.idx = -1;
      for (int i = (int)ranges.size() - 1; i >= 0 && !st.mode; --i) {
        const float xa = tx(ranges[i].start), xb = tx(ranges[i].end);
        if (std::fabs(in.mx - xa) < 6.f) { st.mode = 2; st.idx = i; }
        else if (std::fabs(in.mx - xb) < 6.f) { st.mode = 3; st.idx = i; }
        else if (in.mx > xa && in.mx < xb) { st.mode = 4; st.idx = i; st.grab = t - ranges[i].start; }
      }
      if (!st.mode) { st.mode = 1; st.anchor = t; ranges.push_back({t, t}); st.idx = (int)ranges.size() - 1; out.changed = true; }
      st.selected = st.idx;
    }
    if (st.mode && st.idx >= 0 && st.idx < (int)ranges.size()) {
      if (ir.active) {
        const double t = xt(in.mx);
        media::BleepRange& g = ranges[st.idx];
        const media::BleepRange before = g;
        if (st.mode == 1) { g.start = std::min(st.anchor, t); g.end = std::max(st.anchor, t); }
        else if (st.mode == 2) g.start = std::clamp(t, 0.0, g.end - minLen);
        else if (st.mode == 3) g.end = std::clamp(t, g.start + minLen, dur);
        else { const double len = g.end - g.start; g.start = std::clamp(t - st.grab, 0.0, dur - len); g.end = g.start + len; }
        if (g.start != before.start || g.end != before.end) out.changed = true;
        ui.RequestFrame();
      }
      if (ir.released || !ir.active) {   // finished
        if (st.mode == 1 && ranges[st.idx].end - ranges[st.idx].start < minLen) { ranges.erase(ranges.begin() + st.idx); st.selected = -1; out.changed = true; }
        st.mode = 0; st.idx = -1;
      }
    } else if (!ir.active) { st.mode = 0; st.idx = -1; }
    if (ia.pressed) st.audioDrag = 1;
    if (!ia.active) st.audioDrag = 0;
    if (st.audioDrag && ia.active) out.seek = xt(in.mx);
    if ((ir.focused || ia.focused) && st.selected >= 0 && st.selected < (int)ranges.size())
      for (const auto& k : in.keys) if (k.vk == VK_DELETE || k.vk == VK_BACK) { ranges.erase(ranges.begin() + st.selected); st.selected = -1; out.changed = true; break; }
    if (ir.hot) {
      int cur = (int)(intptr_t)IDC_IBEAM;
      for (auto& g : ranges) { if (std::fabs(in.mx - tx(g.start)) < 6.f || std::fabs(in.mx - tx(g.end)) < 6.f) { cur = (int)(intptr_t)IDC_SIZEWE; break; } }
      ui.SetCursor(cur);
    }
    if (ia.hot) ui.SetCursor((int)(intptr_t)IDC_IBEAM);
  }
  // ---- range lane
  const Color laneFill(pal.text.r, pal.text.g, pal.text.b, 0.055f);
  p.FillRoundRect(rl, laneR, laneFill);
  p.PushClipRoundRect(rl, laneR);
  for (size_t i = 0; i < ranges.size(); ++i) {
    const float xa = tx(ranges[i].start), xb = tx(ranges[i].end);
    const bool sel = (int)i == st.selected;
    Rect rr{xa, rl.y + 3, std::max(3.f, xb - xa), rl.h - 6};
    p.FillRoundRect(rr, 5.f, accent::base.WithAlpha(sel ? 1.f : 0.80f));
    p.FillRoundRect({rr.x + 2.f, rr.cy() - 6, 2.f, 12}, 1.f, Color(1, 1, 1, 0.85f));
    if (rr.w > 12) p.FillRoundRect({rr.right() - 4.f, rr.cy() - 6, 2.f, 12}, 1.f, Color(1, 1, 1, 0.85f));
  }
  p.PopClip();
  // ---- audio lane
  p.FillRoundRect(al, laneR, laneFill);
  p.PushClipRoundRect(al, laneR);
  if (wd.Ok()) DrawBars(p, al.Inset(4.f, 10.f), wd, dur, accent::base.WithAlpha(0.92f), BarStyle{}, 0.92f);
  for (size_t i = 0; i < ranges.size(); ++i) {
    const float xa = tx(ranges[i].start), xb = tx(ranges[i].end);
    p.FillRect({xa, al.y, std::max(2.f, xb - xa), al.h}, accent::base.WithAlpha((int)i == st.selected ? 0.30f : 0.20f));
  }
  p.PopClip();
  // playheads: dark hairline in the range lane, white rounded bar in the audio lane (reference)
  if (playhead >= 0 && dur > 0) {
    const float px = tx(playhead);
    p.FillRect({std::min(px, rl.right() - 1.f), rl.y, 1.f, rl.h}, pal.text);
    p.FillRoundRect({std::min(px, al.right() - 2.5f), al.y, 2.5f, al.h}, 1.25f, Color(1, 1, 1, 0.97f));
  }
  return out;
}

} // namespace filo::tools::audio
