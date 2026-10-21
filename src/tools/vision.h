// Local computer vision through built-in Windows APIs (no downloads, nothing leaves the PC): face detection
// (Windows.Media.FaceAnalysis) and text-line detection (Windows.Media.Ocr, needs an installed OCR language).
#pragma once
#include "engines/image/bitmap.h"
#include <vector>

namespace filo::tools {

struct VisionBox { float x = 0, y = 0, w = 0, h = 0; };   // pixels of the analysed bitmap

// Run on a worker thread (they wait on WinRT async operations). Return false + `err` when the capability is unavailable.
bool DetectFaces(const img::Bitmap& bm, std::vector<VisionBox>& out, std::wstring* err);
bool DetectTextLines(const img::Bitmap& bm, std::vector<VisionBox>& out, std::wstring* err);

} // namespace filo::tools
