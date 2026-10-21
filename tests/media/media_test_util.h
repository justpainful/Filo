// Shared helpers for the media engine tests: fixtures (generated with ffmpeg lavfi), job runner, ffprobe/ffmpeg checks.
#pragma once
#include "test.h"
#include "core/json.h"
#include "core/job.h"
#include "core/process.h"
#include "engines/common/ffmpeg.h"
#include "registry/capability.h"
#include <optional>
#include <string>
#include <vector>

namespace mt {
using namespace filo;
using Args = std::vector<std::wstring>;

void Register();                 // registers the media capabilities once
fs::path Root();                 // repository root
fs::path Work();                 // testdata/media/work (created on demand)
fs::path OutDir(const std::string& name);   // fresh empty subfolder of Work()
std::wstring W(const std::string& s);

// ffmpeg / ffprobe helpers (separate processes; results are decoded by the independent tool, not by our code)
bool Ff(const std::vector<std::string>& args, std::string* err = nullptr);          // runs ffmpeg -y -v error ... ; true when exit 0 and silent
std::string FfOutput(const std::vector<std::string>& args, bool useStderr = false, int* code = nullptr);
bool NullDecode(const fs::path& p, std::string* err = nullptr);                      // full decode of all streams; true when no errors
int CountVideoFrames(const fs::path& p);                                             // decoded frame count (ffprobe -count_frames)
std::string PcmMd5(const fs::path& p, const std::string& afilter = "");              // md5 of decoded PCM of the first audio stream
std::vector<float> DecodeMonoF32(const fs::path& p, int rate = 16000, const std::string& afilter = "", double start = -1, double dur = -1);
double Rms(const std::vector<float>& v, size_t a = 0, size_t b = (size_t)-1);
double ChannelFreq(const fs::path& p, int channel);                                  // dominant frequency estimate via zero crossings
std::optional<ff::MediaInfo> Pr(const fs::path& p);
// ffprobe single value helper (e.g. "-select_streams","v:0","-show_entries","stream=r_frame_rate")
std::string Probe1(const fs::path& p, const std::vector<std::wstring>& extra);
// Extracts frame at time t to a PNG with ffmpeg and returns mean RGB of a rectangle via python -I + PIL.
bool FramePng(const fs::path& video, double t, const fs::path& png);
struct Rgb { double r = 0, g = 0, b = 0; bool ok = false; };
Rgb MeanRgb(const fs::path& png, int x, int y, int w, int h);
double PngDiff(const fs::path& a, const fs::path& b, int x, int y, int w, int h);   // mean absolute channel difference
double PngDiffOffset(const fs::path& a, int ax, int ay, const fs::path& b, int bx, int by, int w, int h);   // region of a vs region of b
int PngCountNear(const fs::path& png, int x, int y, int w, int h, int r, int g, int b, int tol);            // pixels within L1 distance tol of the colour
bool PngSize(const fs::path& png, int& w, int& h);
uint64_t FileHash(const fs::path& p);                  // FNV-1a over the whole file
double MeasureLufs(const fs::path& p);                 // independent EBU R128 integrated loudness (ffmpeg ebur128)
std::vector<fs::path> Leftovers(const fs::path& dir);  // ".filo-*" temp files / dirs

// Fixtures
struct Fx {
  fs::path baseWav;
  fs::path mp4, mov, mkv, webm, avi, wmv, gif;
  fs::path mp3, m4a, wav, flac, ogg, opus, aiff, wma;
  fs::path portrait;      // 320x180 coded + 90 degree display matrix (shows as 180x320)
  fs::path vfr;           // variable frame rate mp4
  fs::path noAudio;       // video only
  fs::path odd;           // 853x479 yuv444 mkv
  fs::path twoAudio;      // mkv with 2 audio tracks (440 / 880 Hz)
  fs::path subMkv;        // mkv with an srt track
  fs::path tiny;          // 64x64, 0.5 s
  fs::path long30;        // 30 s
  fs::path coverMp3;      // mp3 with attached picture + tags
  fs::path joinA, joinB, joinC, joinD;   // different size/fps/audio
  fs::path speech;        // wav with silence at both ends
  fs::path burstsMp4, burstsWav;   // 6 s: tone bursts at 1-2 s and 4-5 s
  fs::path noise;         // 10 s noise wav (loudness tests)
  fs::path kf;            // 6 s, 320x240, a keyframe every second, with audio
  fs::path big;           // 640x360, 8 s, high bitrate (compress tests)
  fs::path mono;          // mono wav, 440 Hz
  fs::path wav24;         // 24-bit wav
};
const Fx& Fixtures();

// Jobs
JobResult RunJob(std::function<JobResult(JobContext&)> fn, double timeoutSec = 600);
JobResult RunCap(const std::string& id, const fs::path& in, const Json& options, const fs::path& out);

// Matrix results (testdata/media/matrix-results.json is rewritten on every call)
void Record(const std::string& id, bool pass, const std::wstring& note);
}
