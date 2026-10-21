// Media-specific immediate-mode widgets shared by the video and audio tool dialogs: seek timeline with range handles and markers,
// waveform trim view (the Trim Audio window), waveform region editor (Bleep), thumbnails strip helpers.
#pragma once
#include "ui/ui.h"
#include "engines/media/media_ops.h"
#include <vector>

namespace filo::tools {

using ui::Rect;
using ui::Ui;

struct TimelineResult {
  bool seeked = false;        // the user moved the playhead (pos updated)
  bool scrubbing = false;     // the playhead is being dragged right now
  bool rangeChanged = false;  // a start/end handle moved
  int  handle = 0;            // 1 = start handle being dragged, 2 = end handle, 0 = none
  int  markerClicked = -1;    // index of a marker the user clicked (select / remove)
};

// Seek bar. `a`/`b` (optional) are the trim handles in seconds; `markers` (optional) are cut points; `wf` (optional) draws a
// faint waveform behind the track. Colours come from the palette; the accent colour marks the selection.
TimelineResult Timeline(Ui& ui, const Rect& r, double duration, double& pos, double* a = nullptr, double* b = nullptr,
                        const std::vector<double>* markers = nullptr, const media::Waveform* wf = nullptr, double frameStep = 0);

// The Trim Audio control: bars of the waveform inside an accent-bordered box, start/end blocks with a white grip at the ends,
// optional playhead. Returns true when a handle moved (start/end in seconds).
bool WaveformTrim(Ui& ui, const Rect& r, const media::Waveform& wf, double duration, double& start, double& end, double playhead, bool enabled = true);

struct WaveRegion { double start = 0, end = 0; };
// Waveform with independent editable regions (Bleep): drag on empty space to add, drag edges/body to adjust, Delete removes the
// selected one. `selected` is the selected region index or -1. Returns true when regions changed.
bool WaveformRegions(Ui& ui, const Rect& r, const media::Waveform& wf, double duration, std::vector<WaveRegion>& regions, int& selected, double playhead, double minLen = 0.05);

// m:ss.cc text used by every time readout (monospace in the reference).
std::wstring TimeText(double seconds);

} // namespace filo::tools
