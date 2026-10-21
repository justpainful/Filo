// Engine-private helpers shared by pdf_text.cpp / pdf_tables.cpp / pdf_engine.cpp. Not part of the public API.
#pragma once
#include "engines/pdf/pdf_text.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace filo::pdf::detail {

bool IsRtlChar(wchar_t c);
bool IsRtlText(const std::wstring& s);   // predominantly right-to-left letters

// Groups the given runs (indices into `runs`) into visual lines, top to bottom; each line sorted in reading direction.
std::vector<std::vector<int>> ClusterLines(const std::vector<TextRun>& runs, std::vector<int> ids);

// Text of a set of runs: lines joined by '\n', runs on a line joined with a space where they do not touch.
std::wstring JoinRunsText(const std::vector<TextRun>& runs, const std::vector<int>& ids);

// Text for two neighbouring runs on one line: returns true when a separating space is needed.
bool NeedsSpaceBetween(const TextRun& a, const TextRun& b, bool rtlLine);

struct TableResult {
  std::vector<TableCandidate> tables;
  std::vector<int> runTable;        // per run index: table index or -1
};
TableResult FindTables(const PageContent& page);

// FPDF_BITMAP (any format) -> straight BGRA. `bmp` is an FPDF_BITMAP handle passed as void*.
bool DecodeFpdfBitmap(void* bmp, int& w, int& h, std::vector<uint8_t>& bgra, bool& alpha);
// Reading order + line/block/column numbering for runs that did not come from PDFium (OCR). Tables are not looked for.
void ArrangeForeignRuns(PageContent& pc);

} // namespace filo::pdf::detail
