// PDF content extraction API (PDFium based). STABLE: the document engine (PDF -> DOCX/PPTX/XLSX/EPUB) codes against
// this header. Fields may be ADDED, never renamed/removed.
//
// Coordinates: PDF points (1/72 in), origin TOP-LEFT of the page *as displayed* (page /Rotate already applied), y grows down.
// Threading: every call serialises on the process-wide PDFium mutex (PDFium is not thread-safe); safe to call from any job
// thread. Nothing here touches COM, the UI or the network.
#pragma once
#include "core/job.h"
#include "core/util.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace filo::pdf {

struct TextRun {
  std::wstring text;                 // logical (reading) order exactly as PDFium reports it; RTL/Arabic is NOT reversed
  float x = 0, y = 0, w = 0, h = 0;  // bounding box in points, origin top-left, y down
  float fontSize = 0;                // points
  bool bold = false, italic = false;
  std::wstring fontName;             // PostScript/base name as PDFium reports it (subset prefix "ABCDEF+" removed)
  uint32_t rgb = 0;                  // fill colour 0xRRGGBB
  // ---- additions (stable) ----
  int line = 0;        // visual line index on the page, reading order; runs sharing a value sit on one visual line
  int block = 0;       // paragraph/column block index on the page, reading order (a column of text = one or more blocks)
  int column = 0;      // 0-based column the block belongs to (0 for single-column pages)
  int table = -1;      // index into DetectTables(page) when the run lies inside a detected table, else -1
  bool rtl = false;    // run is predominantly right-to-left script
  float baseline = 0;  // baseline y (top-left origin)
  float angle = 0;     // text rotation in degrees relative to the displayed page (0 = horizontal, 90 = reads bottom-to-top)
};

struct ImageBlock {
  float x = 0, y = 0, w = 0, h = 0;  // placement on the page in points (top-left origin)
  int pixelW = 0, pixelH = 0;
  std::vector<uint8_t> bgra;         // decoded, top-down, 4 bytes/pixel, straight alpha; EMPTY if unreadable or withImages=false
  // ---- additions ----
  std::vector<uint8_t> rawJpeg;      // original JFIF bytes when the image is a plain DCT stream drawn upright (else empty)
  bool hasAlpha = false;
};

// Axis-aligned ruling line (thin stroked/filled path). Same coordinate system as TextRun.
struct RuleLine { float x1 = 0, y1 = 0, x2 = 0, y2 = 0; float width = 0; };

struct PageContent {
  int index = 0;                     // 0-based
  float width = 0, height = 0;       // points, as displayed
  std::vector<TextRun> runs;         // reading order: lines top->bottom, columns left->right (right->left for RTL pages) within a
                                     // column cluster; consecutive glyphs of the same style on the same line are merged into runs
  std::vector<ImageBlock> images;    // in content-stream order
  bool hasText = false;              // at least one non-whitespace character
  bool looksScanned = false;         // no text and a large image covers (most of) the page
  int columns = 1;                   // detected text columns 1..n
  // ---- additions ----
  int rotation = 0;                  // page /Rotate in degrees (0/90/180/270)
  std::vector<RuleLine> rules;       // ruling lines (used by DetectTables)
  std::wstring label;                // page label ("iv", "A-3") or empty
  int unmappedGlyphs = 0;            // glyphs PDFium could not map to Unicode (dropped from runs)
  bool textUnreliable = false;       // most glyphs are unmapped: the text layer is garbage -> treat like a scanned page
};

// Extracts every page. `ctx` may be null; when set, real progress (0..1) is reported and cancellation is honoured (returns
// false with *err == L"Cancelled"). Password-protected files: pass `password`; a missing/wrong one yields a user-facing error.
// withImages=false skips pixel decoding (ImageBlock geometry is still filled in, bgra stays empty).
bool ExtractPages(const fs::path& file, std::vector<PageContent>& out, JobContext* ctx, std::wstring* err,
                  const std::wstring& password = {}, bool withImages = true);

// Streaming variant for huge documents: calls `sink` once per page (in order). Return false from `sink` to stop (not an error).
bool ExtractPagesStreaming(const fs::path& file, const std::function<bool(PageContent&&)>& sink, JobContext* ctx,
                           std::wstring* err, const std::wstring& password = {}, bool withImages = true);

class PdfDocument;   // pdf_doc.h
// Extracts one page of an already open document (cheap to call page by page for huge files).
bool ExtractPage(const PdfDocument& doc, int index, PageContent& out, bool withImages = true);

// ---- tables -------------------------------------------------------------------------------------------------------

struct TableCell {
  int row = 0, col = 0;              // top-left grid position
  int rowSpan = 1, colSpan = 1;
  bool covered = false;              // true for grid slots swallowed by another cell's span (text empty, not to be emitted)
  float x = 0, y = 0, w = 0, h = 0;  // cell bbox in points (for covered slots: the slot's own box)
  std::wstring text;                 // cell text, lines joined with '\n'
};

struct TableCandidate {
  float x = 0, y = 0, w = 0, h = 0;  // table bbox
  int rows = 0, cols = 0;
  std::vector<std::vector<TableCell>> cells;  // [rows][cols]
  bool ruled = false;                // found from ruling lines (true) or from aligned-text heuristics (false)
  bool headerRow = false;            // first row looks like a header (bold or shaded) - heuristic
};

// Detects tables on one page from ruling lines and/or aligned runs. Deterministic: run.table indexes this vector.
std::vector<TableCandidate> DetectTables(const PageContent& page);

// Plain text of a page in reading order: lines joined by '\n', runs of a line joined with a space when they do not touch,
// tables rendered as tab-separated rows. Used by txt export; convenient for the document engine too.
std::wstring PageToPlainText(const PageContent& page);

} // namespace filo::pdf
