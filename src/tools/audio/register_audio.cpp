// Registration of the Audio tool group. Wheel order = registration order: Normalize, Visualizer, Trim, Channels, Bleep.
#include "tools/audio/audio_common.h"

using namespace filo;
using namespace filo::tools;

FILO_TOOL_GROUP(Audio) {
  auto oneAudio = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Audio)) { SelReason(why, L"Select one audio file."); return false; }
    return true;
  };
  {
    ToolInfo t;
    t.id = "normalize-volume"; t.label = L"Normalize Volume"; t.wheelLabel = L"Normalize"; t.icon = "normalize"; t.category = "audio";
    t.accepts = [](const Selection& s, std::wstring* why) {
      if (s.count() != 1 || !SelAllIn(s, {Group::Audio, Group::Video})) { SelReason(why, L"Select one audio or video file."); return false; }
      return true;
    };
    add(t, &MakeNormalizeVolume);
  }
  {
    ToolInfo t;
    t.id = "audio-visualizer"; t.label = L"Audio Visualizer"; t.wheelLabel = L"Visualizer"; t.icon = "visualizer"; t.category = "audio";
    t.accepts = oneAudio;
    add(t, &MakeAudioVisualizer);
  }
  {
    ToolInfo t;
    t.id = "trim-audio"; t.label = L"Trim Audio"; t.wheelLabel = L"Trim"; t.icon = "trim"; t.category = "audio";
    t.accepts = oneAudio;
    add(t, &MakeTrimAudio);
  }
  {
    ToolInfo t;
    t.id = "convert-channels"; t.label = L"Convert Audio Channels"; t.wheelLabel = L"Channels"; t.icon = "channels"; t.category = "audio";
    t.accepts = oneAudio;
    add(t, &MakeConvertChannels);
  }
  {
    ToolInfo t;
    t.id = "bleep-audio"; t.label = L"Bleep Audio"; t.wheelLabel = L"Bleep"; t.icon = "bleep"; t.category = "audio";
    t.accepts = oneAudio;
    add(t, &MakeBleepAudio);
  }
}
