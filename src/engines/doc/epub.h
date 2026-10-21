// EPUB 3 writer (with an EPUB 2 NCX for older readers) and reader (spine order -> Document).
#pragma once
#include "engines/doc/docmodel.h"
#include "engines/doc/zip.h"

namespace filo::doc {

struct EpubWriteOptions {
  std::string identifier;            // empty => urn:uuid generated
  bool splitOnH1 = true;             // one XHTML chapter per level-1 heading
};
bool WriteEpub(const Document& d, const fs::path& out, const EpubWriteOptions& opt, std::wstring* err, std::vector<std::wstring>* warnings = nullptr);

bool ReadEpub(const fs::path& file, Document& out, std::wstring* err);
bool ReadEpub(ZipReader& zip, Document& out, std::wstring* err);

// mimetype first + stored + exact, container.xml, OPF metadata/manifest/spine consistency, nav document, XmlLite well-formedness of
// every XML/XHTML part, and the book reads back.
bool VerifyEpub(const fs::path& file, std::wstring* why);

} // namespace filo::doc
