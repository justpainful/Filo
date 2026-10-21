// OCR through Windows.Media.Ocr (C++/WinRT). Local only; uses the recognizer languages installed on this PC.
#pragma once
#include "core/util.h"
#include <cstdint>
#include <string>
#include <vector>

namespace filo::pdf {

struct OcrLineOut { std::wstring text; float x = 0, y = 0, w = 0, h = 0; };   // pixels of the bitmap passed in
struct OcrResult {
  std::vector<OcrLineOut> lines;
  std::wstring language;        // BCP-47 tag of the engine used
  int chars = 0;                // recognised non-space characters
};
struct OcrInfo {
  bool available = false;                   // at least one recognizer language installed
  std::vector<std::wstring> languages;      // installed recognizer language tags (e.g. "en-US", "ar-SA")
  std::wstring profileLanguage;             // first user-profile language that has a recognizer, else ""
  std::wstring message;                     // user-facing sentence when unavailable
  int maxImageDimension = 0;
};
OcrInfo QueryOcr();

// Recognises one page bitmap (BGRA, top-down). `lang`: "auto" (user profile languages, falling back to any installed one;
// when several recognizers are installed every one is tried and the best result wins) or an explicit BCP-47 tag.
// Images larger than the engine limit are scaled down internally; coordinates are mapped back to the input size.
bool OcrRecognize(const uint8_t* bgra, int w, int h, const std::wstring& lang, OcrResult& out, std::wstring* err);

} // namespace filo::pdf
