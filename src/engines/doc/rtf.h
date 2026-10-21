// RTF reader (own parser) and RTFD (directory bundle with TXT.rtf + attachments) reader.
#pragma once
#include "engines/doc/docmodel.h"
#include <functional>

namespace filo::doc {

struct RtfOptions {
  // RTFD: resolves \NeXTGraphic attachments by file name to bytes. Null for plain RTF.
  std::function<bool(const std::string& name, Bytes& out)> attachment;
};

// Groups, \par \line \tab, \b \i \ul \strike \super \sub, \cf/\colortbl, \fs, \fonttbl (per-font code pages), \u<N> with \uc skipping,
// \'hh code-page bytes (multi-byte aware), \~ \- \_ and typographic specials, lists (\ls \ilvl, \listtable), simple tables
// (\trowd \cell \row), HYPERLINK fields, \pict PNG/JPEG images, headings via style names / \outlinelevel, \rtlpar.
// Never throws; returns false only when the data is not RTF.
bool ParseRtf(const uint8_t* data, size_t n, Document& out, const RtfOptions& opt = {}, std::wstring* err = nullptr);

// Reads <bundle>/TXT.rtf (or the only .rtf inside) with attachments resolved from the bundle folder.
bool ParseRtfd(const fs::path& bundleDir, Document& out, std::wstring* err);

} // namespace filo::doc
