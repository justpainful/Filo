// Minimal PPTX (PresentationML) writer and text reader. Used by PDF -> PPTX and as a fixture generator for the tests.
#pragma once
#include "engines/doc/docmodel.h"

namespace filo::doc {

struct SlideRun {
  std::string text;
  bool bold = false, italic = false, underline = false;
  double sizePt = 18;
  uint32_t rgb = 0x000000;
  std::string font;                // "" = theme font
  bool rtl = false;
  bool hidden = false;             // fully transparent text (searchable OCR text laid over a page picture)
};
struct SlidePara {
  std::vector<SlideRun> runs;
  char align = 'l';                // l c r j
  bool rtl = false;
  bool bullet = false;
};
struct SlideShape {
  enum class Kind { Title, Text, Image } kind = Kind::Text;
  double x = 0, y = 0, w = 0, h = 0;      // points, top-left origin
  std::vector<SlidePara> paras;           // Title / Text
  std::shared_ptr<ImageData> image;       // Image
  bool wrap = true;                       // false: a single line that must not wrap
  double rotationDeg = 0;                 // text rotation
};
struct Slide {
  std::vector<SlideShape> shapes;
  std::string notes;
};
struct Deck {
  double widthPt = 720, heightPt = 405;   // slide size (PowerPoint maximum is 4032 pt)
  std::vector<Slide> slides;
  std::string title;
};

bool WritePptx(const Deck& d, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings = nullptr);

struct PptxInfo { int slides = 0; double widthPt = 0, heightPt = 0; std::vector<std::string> slideText; std::vector<int> imagesPerSlide; };
bool ReadPptxInfo(const fs::path& file, PptxInfo& info, std::wstring* err);
bool VerifyPptx(const fs::path& file, std::wstring* why);

} // namespace filo::doc
