// Bookmarks (outlines) and document info: PDFium can read them but not write them, so after PDFium saved a merged file we append
// a standards-conforming incremental update (new objects + xref section + trailer) with /Outlines and an /Info dictionary.
#pragma once
#include "engines/pdf/pdf_doc.h"
#include <map>
#include <string>
#include <vector>

namespace filo::pdf {

struct OutlineNode {
  std::wstring title;
  int page = -1;                     // 0-based destination page (in the file the tree belongs to); -1 = no destination
  bool hasTop = false;
  float top = 0;                     // destination y in page space when hasTop
  std::vector<OutlineNode> children;
};

// Reads the bookmark tree of an open document. `pageOffset` is added to every destination page (merge).
std::vector<OutlineNode> ReadOutline(const PdfDocument& doc, int pageOffset = 0);
int CountOutlineNodes(const std::vector<OutlineNode>& nodes);

// Appends an incremental update to a PDF written by PDFium (classic xref table). `info` keys: Title, Author, Subject, Keywords,
// Creator, Producer (empty map = leave /Info alone). Returns false (file untouched) when the file layout is not understood.
bool AppendOutlinesAndInfo(const fs::path& pdf, const std::vector<OutlineNode>& roots, const std::map<std::string, std::wstring>& info,
                           std::wstring* err);

// Rewrites a PDF written by PDFium keeping only objects reachable from the trailer (Root/Info). PDFium never drops objects that
// became unreachable (replaced image streams, deleted pages); this does, byte-for-byte for the objects that stay (same object
// numbers, so every reference remains valid). Returns false (file untouched) when the layout is not understood.
bool GarbageCollectPdf(const fs::path& pdf, uint64_t* bytesBefore, uint64_t* bytesAfter, std::wstring* err);

} // namespace filo::pdf
