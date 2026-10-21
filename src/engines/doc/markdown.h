// Markdown (CommonMark subset + GFM tables, strikethrough, task lists, autolinks, footnotes): tolerant reader and writer.
#pragma once
#include "engines/doc/docmodel.h"

namespace filo::doc {

struct MdReadOptions {
  fs::path baseDir;              // local images (![alt](pics/a.png)) are loaded relative to this folder
  bool loadImages = true;
};
Document ParseMarkdown(std::string_view md, const MdReadOptions& opt = {});

struct MdWriteOptions {
  bool embedImages = true;       // images become data: URIs; otherwise they are replaced by their alt text
};
std::string WriteMarkdown(const Document& d, const MdWriteOptions& opt = {});

} // namespace filo::doc
