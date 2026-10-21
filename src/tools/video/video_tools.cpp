// Registration of the Video group tool dialogs (wheel order = registration order; Join Videos is registered before this group).
#include "tools/video/vid_common.h"
#include <map>
#include <mutex>

using namespace filo;
using namespace filo::tools;

namespace {

bool OneVideo(const Selection& s, std::wstring* why) {
  if (s.count() != 1 || !SelAllGroup(s, Group::Video)) { SelReason(why, L"Select one video."); return false; }
  return true;
}

// "Does this video have an audio track?" - one short ffprobe per file, cached by path + size + modification time.
bool HasAudioTrack(const fs::path& p) {
  static std::mutex mu;
  static std::map<std::wstring, bool> cache;
  std::error_code ec;
  const uint64_t size = fs::file_size(p, ec);
  const auto mt = fs::last_write_time(p, ec).time_since_epoch().count();
  const std::wstring key = p.wstring() + L"|" + std::to_wstring(size) + L"|" + std::to_wstring((long long)mt);
  { std::lock_guard lk(mu); auto it = cache.find(key); if (it != cache.end()) return it->second; }
  auto mi = ff::Probe(p, 6000);
  const bool has = mi && mi->HasAudio();
  { std::lock_guard lk(mu); cache[key] = has; }
  return has;
}

bool OneVideoWithAudio(const Selection& s, std::wstring* why) {
  if (!OneVideo(s, why)) return false;
  if (!HasAudioTrack(s.items[0].path)) { SelReason(why, L"This video has no audio track."); return false; }
  return true;
}

ToolInfo Info(const char* id, const wchar_t* label, const wchar_t* wheel, const char* icon, bool (*accepts)(const Selection&, std::wstring*) = &OneVideo) {
  ToolInfo t; t.id = id; t.label = label; t.wheelLabel = wheel; t.icon = icon; t.category = "video"; t.accepts = accepts;
  return t;
}

} // namespace

// Order on the wheel: Trim, Crop, Speed, Split, Frames, Redact, Mute (after Join Videos).
FILO_TOOL_GROUP(Video) {
  add(Info("trim-video", L"Trim Video", L"Trim", "trim"), &MakeTrimVideo);
  add(Info("crop-video", L"Crop Video", L"Crop", "crop"), &MakeCropVideo);
  add(Info("change-speed", L"Change Speed", L"Speed", "speed"), &MakeChangeSpeed);
  add(Info("split-video", L"Split Video", L"Split", "split"), &MakeSplitVideo);
  add(Info("save-frames", L"Save Frames", L"Frames", "frames"), &MakeSaveFrames);
  add(Info("redact-video", L"Redact Video", L"Redact", "redact"), &MakeRedactVideo);
  add(Info("remove-audio", L"Remove Audio", L"Mute", "removeaudio", &OneVideoWithAudio), &MakeRemoveAudio);
}
