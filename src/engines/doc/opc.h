// Open Packaging Conventions helpers shared by DOCX / XLSX / PPTX: relationships, content types, strict package verification.
#pragma once
#include "engines/doc/xml.h"
#include "engines/doc/zip.h"
#include <map>

namespace filo::doc {

struct Relationship { std::string id, type, target; bool external = false; };
using RelMap = std::map<std::string, Relationship>;      // by Id

// Resolves `target` relative to the directory of `basePart` ("word/document.xml" + "media/a.png" => "word/media/a.png").
std::string ResolvePartPath(const std::string& basePart, const std::string& target);
// Relationship part path of a part: "word/document.xml" => "word/_rels/document.xml.rels".
std::string RelsPathFor(const std::string& part);
RelMap ReadRels(ZipReader& z, const std::string& relsPath);
std::string FindRelTarget(const RelMap& rels, std::string_view typeSuffix);     // first relationship whose Type ends with the suffix

// Content type of a part per [Content_Types].xml ("" when none).
class ContentTypes {
 public:
  bool Load(ZipReader& z);
  std::string TypeOf(const std::string& partName) const;
  bool HasOverride(const std::string& partName) const;
  std::vector<std::string> OverrideParts() const;
 private:
  std::map<std::string, std::string> defaults_;     // lower-case extension -> type
  std::map<std::string, std::string> overrides_;    // "/word/document.xml" -> type
};

// Strict structural verification of an OPC package. `mainPart` (e.g. "word/document.xml") must exist.
bool VerifyOpc(ZipReader& z, const std::string& mainPart, std::wstring* why);

} // namespace filo::doc
