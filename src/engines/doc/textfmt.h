// Plain text <-> Document, macOS .textClipping (XML / binary property list) reader.
#pragma once
#include "engines/doc/docmodel.h"
#include "engines/doc/text_io.h"

namespace filo::doc {

struct TextReadOptions {
  bool reflow = false;        // join the lines of a paragraph with spaces instead of keeping the line breaks
  bool detectLists = true;    // "- item", "* item", "1. item" lines become list items
  bool detectHeadings = true; // "Title" underlined with === / --- becomes a heading
};
// `utf8` must already be decoded (see DecodeText). Control characters are sanitised.
Document ParseText(std::string_view utf8, const TextReadOptions& opt = {});

struct TextWriteOptions {
  bool numberLists = true;
  int wrapColumns = 0;        // 0 = no wrapping
};
// UTF-8 with '\n' line ends; callers convert to CRLF / the requested encoding.
std::string WriteText(const Document& d, const TextWriteOptions& opt = {});

// macOS .textClipping: XML or binary plist holding UTI-Data (public.utf8-plain-text, public.utf16-external-plain-text...).
// Resource-fork-only clippings cannot exist on NTFS (no forks): such files are reported through *err.
bool ParseTextClipping(const Bytes& data, std::string& utf8, std::wstring* err);

} // namespace filo::doc
