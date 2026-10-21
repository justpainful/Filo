// DOCX (WordprocessingML) reader and writer.
#pragma once
#include "engines/doc/docmodel.h"
#include "engines/doc/zip.h"

namespace filo::doc {

struct DocxWriteOptions {
  PageSize page = PageSize::A4;
  std::string creator = "Filo";
};

// Writes a complete, standards-conformant .docx (content types, rels, styles, numbering, settings, core/app properties).
bool WriteDocx(const Document& d, const fs::path& out, const DocxWriteOptions& opt, std::wstring* err, std::vector<std::wstring>* warnings = nullptr);

// Reads paragraphs, headings (style names / outline levels), lists (numPr), tables, hyperlinks, inline images, RTL (w:bidi / w:rtl),
// footnotes and core properties. Never throws; returns false with a user-facing error for unreadable files.
bool ReadDocx(const fs::path& file, Document& out, std::wstring* err);
bool ReadDocx(ZipReader& zip, Document& out, std::wstring* err);

// Independent check of a produced file: zip integrity, [Content_Types].xml, relationships resolve, every XML part is well-formed
// (XmlLite) and the document reads back.
bool VerifyDocx(const fs::path& file, std::wstring* why);

// Converts an image that Word cannot embed (webp, tiff, ...) to PNG with WIC. Returns false when WIC cannot decode it.
bool ConvertImageToPng(const Bytes& in, Bytes& outPng, int* w = nullptr, int* h = nullptr);

} // namespace filo::doc
