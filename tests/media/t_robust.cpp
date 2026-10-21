// Robustness: corrupt / empty / mislabelled files, unusual names, cancellation, inputs never modified, no temp leftovers.
#include "media_test_util.h"
#include "engines/media/media_ops.h"
#include "engines/media/media_info.h"
#include <algorithm>
#include <cmath>
#include <fstream>

using namespace mt;
namespace m = filo::media;

static Json Opts() { Json j = Json::MakeObject(); j["hardwareAcceleration"] = false; return j; }
static bool HasWarning(const JobResult& r, const wchar_t* needle) {
  for (auto& w : r.warnings) if (w.find(needle) != std::wstring::npos) return true;
  return false;
}

TEST(corrupt_empty_and_wrong_extension) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("robust_bad");
  std::error_code ec;
  // truncated mp4 (the moov atom is at the end of an mp4 written by ffmpeg without faststart -> unreadable)
  fs::path trunc = d / L"truncated.mp4";
  {
    std::ifstream in(fx.big, std::ios::binary); std::vector<char> buf(20000); in.read(buf.data(), buf.size());
    std::ofstream out(trunc, std::ios::binary); out.write(buf.data(), in.gcount());
  }
  JobResult r1 = RunCap("mp4>mkv", trunc, Opts(), d / L"t.mkv");
  CHECK(!r1.ok && !r1.error.empty());
  std::printf("  truncated: %ls\n", r1.error.c_str());
  CHECK(!fs::exists(d / L"t.mkv"));
  // random garbage with a media extension
  fs::path junk = d / L"junk.mp3";
  { std::ofstream o(junk, std::ios::binary); for (int i = 0; i < 5000; ++i) o.put((char)((i * 131 + 17) & 0xFF)); }
  JobResult r2 = RunCap("mp3>wav", junk, Opts(), d / L"j.wav");
  CHECK(!r2.ok && !r2.error.empty());
  std::printf("  garbage: %ls\n", r2.error.c_str());
  // zero-byte file
  fs::path zero = d / L"zero.mp4";
  { std::ofstream o(zero, std::ios::binary); }
  JobResult r3 = RunCap("mp4>mkv", zero, Opts(), d / L"z.mkv");
  CHECK(!r3.ok && r3.error.find(L"empty") != std::wstring::npos);
  std::printf("  zero-byte: %ls\n", r3.error.c_str());
  // a text file named .mp4
  fs::path txt = d / L"text.mp4";
  { std::ofstream o(txt, std::ios::binary); o << "this is not a video, just text\n"; }
  JobResult r4 = RunCap("mp4>mkv", txt, Opts(), d / L"tx.mkv");
  CHECK(!r4.ok && !r4.error.empty());
  // missing file
  JobResult r5 = RunCap("mp4>mkv", d / L"nope.mp4", Opts(), d / L"n.mkv");
  CHECK(!r5.ok);
  // a real mp4 that was renamed .mp3 (wrong extension): converted by content, with a warning
  fs::path wrong = d / L"actually_video.mp3";
  fs::copy_file(fx.mp4, wrong, fs::copy_options::overwrite_existing, ec);
  JobResult r6 = RunCap("mp4>mkv", wrong, Opts(), d / L"w.mkv");
  CHECK(r6.ok && HasWarning(r6, L"extension"));
  if (r6.ok) CHECK(Pr(r6.outputs[0])->FirstVideo()->codec == "h264");
  // a real mp3 without any extension
  fs::path noext = d / L"audiofile";
  fs::copy_file(fx.mp3, noext, fs::copy_options::overwrite_existing, ec);
  JobResult r7 = RunCap("mp3>flac", noext, Opts(), d / L"ne.flac");
  CHECK(r7.ok);
  // a directory
  JobResult r8 = RunCap("mp4>mkv", d, Opts(), d / L"dir.mkv");
  CHECK(!r8.ok);
  // operations on garbage fail cleanly too
  CHECK(!RunJob([&](JobContext& c) { m::TrimVideoRequest q; q.start = 0; q.end = 1; return m::TrimVideo(junk, q, c); }).ok);
  CHECK(!RunJob([&](JobContext& c) { m::RemoveAudioRequest q; return m::RemoveAudio(zero, q, c); }).ok);
  CHECK(!m::ComputeWaveform(junk, 10).ok);
  CHECK(!m::ExtractFrame(zero, 0, 10, 10).ok);
  CHECK(!m::SummarizeMedia(trunc).ok);
  CHECK(Leftovers(d).empty());
}

TEST(unicode_spaces_emoji_and_dash_names) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("robust_names");
  std::error_code ec;
  const wchar_t* names[] = {L"فيديو تجربة مميز.mp4", L"my  video (copy) [1].mp4", L"emoji \U0001F600\U0001F3AC clip.mp4", L"-leading dash.mp4", L"quote's and ; & % ^ ! $ name.mp4", L"日本語のファイル.mp4"};
  for (auto n : names) {
    fs::path in = d / n;
    fs::copy_file(fx.mp4, in, fs::copy_options::overwrite_existing, ec);
    CHECK(fs::exists(in));
    uint64_t h = FileHash(in);
    // default output location (next to the input): the name is derived from the input
    JobResult r = RunCap("mp4>mkv", in, Opts(), fs::path());
    CHECK(r.ok);
    if (!r.ok) { std::printf("  %ls: %ls\n", n, r.error.c_str()); continue; }
    CHECK_EQ(r.outputs[0].parent_path().wstring(), d.wstring());
    CHECK(r.outputs[0].stem().wstring() == in.stem().wstring());
    CHECK(Pr(r.outputs[0]) && Pr(r.outputs[0])->FirstVideo());
    JobResult a = RunCap("mp4>mp3", in, Opts(), fs::path());
    CHECK(a.ok && Pr(a.outputs[0])->FirstAudio()->codec == "mp3");
    // operations with the same input
    m::SaveFramesRequest fr; fr.timestamps = {0.5};
    JobResult f = RunJob([&](JobContext& c) { return m::SaveVideoFrames(in, fr, c); });
    CHECK(f.ok);
    m::TrimVideoRequest tr; tr.start = 0.2; tr.end = 1.0;
    CHECK(RunJob([&](JobContext& c) { return m::TrimVideo(in, tr, c); }).ok);
    CHECK_EQ(FileHash(in), h);
  }
  // a long path (> 260 chars)
  fs::path longDir = d;
  for (int i = 0; i < 6; ++i) longDir /= std::wstring(45, L'x') + std::to_wstring(i);
  fs::create_directories(LongPath(longDir), ec);
  fs::path lin = longDir / L"clip.mp4";
  CHECK(CopyFileW(LongPath(fx.mp4).c_str(), LongPath(lin).c_str(), FALSE) != 0);
  std::printf("  long path length: %zu\n", lin.wstring().size());
  JobResult lr = RunCap("mp4>mkv", lin, Opts(), fs::path());
  CHECK(lr.ok);
  if (lr.ok) CHECK(GetFileAttributesW(LongPath(lr.outputs[0]).c_str()) != INVALID_FILE_ATTRIBUTES);
  else std::printf("  long path: %ls\n", lr.error.c_str());
  // an existing output is never overwritten (unique name)
  JobResult u1 = RunCap("mp4>mkv", fx.mp4, Opts(), d / L"same.mkv"), u2 = RunCap("mp4>mkv", fx.mp4, Opts(), d / L"same.mkv");
  CHECK(u1.ok && u2.ok && u1.outputs[0] != u2.outputs[0] && fs::exists(u1.outputs[0]) && fs::exists(u2.outputs[0]));
  CHECK(Leftovers(d).empty());
}

TEST(inputs_are_never_modified) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("robust_hash");
  std::vector<fs::path> inputs = {fx.mp4, fx.mkv, fx.webm, fx.avi, fx.wmv, fx.gif, fx.mp3, fx.m4a, fx.wav, fx.flac, fx.ogg, fx.opus, fx.aiff, fx.wma, fx.coverMp3, fx.portrait, fx.twoAudio};
  std::vector<uint64_t> before;
  for (auto& p : inputs) before.push_back(FileHash(p));
  int i = 0;
  for (auto& cap : CapabilityRegistry::Get().All()) {
    if (cap.engine != "ffmpeg") continue;
    if (++i % 7) continue;     // a spread of the matrix; the full matrix is run in matrix_every_capability
    fs::path in;
    for (size_t k = 0; k < inputs.size(); ++k) if (DetectFile(inputs[k]).format == cap.in) { in = inputs[k]; break; }
    if (in.empty()) continue;
    JobResult r = RunCap(cap.id, in, Opts(), d / Widen("o" + std::to_string(i) + "." + Narrow(cap.out)));
    CHECK(r.ok);
  }
  m::TrimVideoRequest tr; tr.start = 0.5; tr.end = 2;
  RunJob([&](JobContext& c) { return m::TrimVideo(fx.mp4, tr, c); });
  m::NormalizeRequest nr;
  RunJob([&](JobContext& c) { return m::NormalizeVolume(fx.wav, nr, c); });
  for (size_t k = 0; k < inputs.size(); ++k) CHECK_EQ(FileHash(inputs[k]), before[k]);
}

TEST(cancellation_is_prompt_and_leaves_nothing) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("robust_cancel");
  fs::path big = d / L"slow_source.mp4";
  // a source that takes a long time to encode as HEVC/AV1 (1080p, 20 s)
  CHECK(Ff({"-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=20", "-f", "lavfi", "-i", "sine=f=440:d=20", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "20", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", Narrow(big.wstring())}));
  uint64_t h = FileHash(big);
  mt::Register();
  const Capability* cap = nullptr;
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == "mp4>mkv") cap = &c;
  CHECK(cap != nullptr);
  if (!cap) return;
  Json o = Json::MakeObject(); o["codec"] = "hevc"; o["hardwareAcceleration"] = false;
  fs::path forced = d / L"cancelled.mkv";
  JobSpec spec; spec.operation = L"convert"; spec.title = L"cancel test"; spec.inputs = {big};
  spec.work = [&](JobContext& ctx) { return RunConversion(*cap, big, o, ctx, &forced); };
  uint64_t id = JobManager::Get().Submit(spec);
  Sleep(2500);
  auto mid = JobManager::Get().Info(id);
  CHECK(mid && !mid->Finished());
  if (mid) CHECK(mid->progress > 0.0 && mid->progress < 1.0);      // real progress was reported before the cancel
  double t0 = NowSeconds();
  JobManager::Get().Cancel(id);
  bool done = JobManager::Get().WaitFor(id, 15000);
  double took = NowSeconds() - t0;
  std::printf("  progress at cancel %.2f; stopped after %.2f s\n", mid ? mid->progress : -1, took);
  CHECK(done); CHECK(took < 3.0);
  auto info = JobManager::Get().Info(id);
  CHECK(info && info->state == JobState::Cancelled && !info->result.ok);
  Sleep(300);
  CHECK(Leftovers(d).empty());
  CHECK(!fs::exists(forced));
  // and an operation (two-pass compress) cancelled mid-way
  m::CompressRequest cr; cr.mode = m::CompressMode::TargetSize; cr.targetBytes = 2'000'000; cr.outputPath = d / L"c.mp4";
  JobSpec s2; s2.operation = L"compress"; s2.title = L"c"; s2.inputs = {big};
  s2.work = [&](JobContext& ctx) { return static_cast<JobResult>(m::CompressMedia(big, cr, ctx)); };
  uint64_t id2 = JobManager::Get().Submit(s2);
  Sleep(1500);
  double t1 = NowSeconds();
  JobManager::Get().Cancel(id2);
  CHECK(JobManager::Get().WaitFor(id2, 15000));
  std::printf("  compress stopped after %.2f s\n", NowSeconds() - t1);
  CHECK(NowSeconds() - t1 < 3.0);
  Sleep(300);
  CHECK(Leftovers(d).empty());
  CHECK(!fs::exists(cr.outputPath));
  CHECK_EQ(FileHash(big), h);
  // waveform cancel via flag
  (void)fx;
}

TEST(progress_is_real_and_monotonic) {
  const Fx& fx = Fixtures();
  fs::path d = OutDir("robust_progress");
  mt::Register();
  const Capability* cap = nullptr;
  for (auto& c : CapabilityRegistry::Get().All()) if (c.id == "mp4>webm") cap = &c;
  CHECK(cap != nullptr);
  if (!cap) return;
  std::vector<double> seen;
  int listener = JobManager::Get().AddListener([&](const JobInfo& i) { if (i.operation == L"progress-test") seen.push_back(i.progress); });
  Json o = Json::MakeObject(); o["hardwareAcceleration"] = false;
  fs::path forced = d / L"p.webm";
  JobSpec spec; spec.operation = L"progress-test"; spec.title = L"p"; spec.inputs = {fx.long30};
  spec.work = [&](JobContext& ctx) { return RunConversion(*cap, fx.long30, o, ctx, &forced); };
  uint64_t id = JobManager::Get().Submit(spec);
  CHECK(JobManager::Get().WaitFor(id, 300000));
  JobManager::Get().RemoveListener(listener);
  auto info = JobManager::Get().Info(id);
  CHECK(info && info->result.ok);
  int mid = 0; for (double p : seen) if (p > 0.05 && p < 0.95) ++mid;
  std::printf("  progress callbacks: %zu, mid-range: %d\n", seen.size(), mid);
  CHECK(mid >= 2);
}
