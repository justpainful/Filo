// Test helpers for the PDF engine tests: fixtures, PDF construction with PDFium, WIC decode, python/PIL checks.
#pragma once
#include "core/process.h"
#include "core/util.h"
#include "engines/pdf/pdf_doc.h"
#include "test.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ptest {
using namespace filo;

fs::path Root();                     // repo root (found by walking up from the exe)
fs::path DataDir();                  // testdata/pdf
fs::path Scratch();                  // per-run scratch folder under the build dir (created)
fs::path Fixture(const std::string& name);   // testdata/pdf/<name>; LibreOffice fixtures are generated on demand

struct Img { int w = 0, h = 0; std::vector<uint8_t> bgra; };
bool DecodeImage(const fs::path& p, Img& out);                  // WIC -> straight BGRA
bool EncodePng(const fs::path& p, const Img& img);
bool EncodeJpeg(const fs::path& p, const Img& img, int quality);
double NonBlankRatio(const Img& i);                             // share of pixels that are not (near) white
double MeanAbsDiff(const Img& a, const Img& b);                 // 0..255 over RGB; needs equal size

// ---- PDF construction through PDFium -----------------------------------------------------------------------------------
struct TextSpec {
  float x = 72, y = 72;              // top-left origin, points (baseline = y + size)
  float size = 12;
  std::wstring text;
  std::string font = "Helvetica";    // PDF standard 14 name
  uint32_t rgb = 0;
  float rotateDeg = 0;
};
struct LineSpec { float x1, y1, x2, y2, width = 1; };
// x,y,w,h = displayed box (top-left origin); rot90 = quarter turns counter-clockwise applied to the stored image
struct ImageSpec { float x, y, w, h; int pw, ph; bool noise = false; bool jpeg = false; int jpegQuality = 90; int rot90 = 0; bool flipX = false; };
struct PageSpec {
  float w = 595, h = 842;
  int rotate = 0;
  std::vector<TextSpec> texts;
  std::vector<LineSpec> lines;
  std::vector<ImageSpec> images;
};
bool BuildPdf(const fs::path& out, const std::vector<PageSpec>& pages, std::wstring* err = nullptr);
// N pages, each with a unique marker text "PAGE-0007 ..." (for order checks)
bool BuildMarkerPdf(const fs::path& out, int pages, float w = 595, float h = 842, const std::wstring& prefix = L"PAGE");

// 300 text-heavy pages (44 lines each), built once per scratch dir
fs::path Big300();

// ---- reading back with a FRESH PDFium load -----------------------------------------------------------------------------
struct PdfFacts { int pages = 0; std::vector<float> w, h; std::vector<int> rot; std::vector<std::wstring> text; bool ok = false; std::wstring err; };
PdfFacts ReadPdf(const fs::path& p, const std::wstring& password = {});

std::string Utf8(const std::wstring& w);
bool Contains(const std::wstring& hay, const std::wstring& needle);
std::string ReadFileBytes(const fs::path& p);
bool WriteFileBytes(const fs::path& p, const std::string& data);

// Python (isolated) one-liner runner; returns stdout, exit code in *rc.
std::string RunPython(const std::string& code, const std::vector<std::wstring>& args = {}, int* rc = nullptr);
bool HavePil();

// results file
void RecordResult(const std::string& id, bool pass, const std::string& note);
void FlushResults();
} // namespace ptest
