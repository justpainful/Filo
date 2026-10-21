// HTML / XHTML: tolerant reader (tag soup -> Document) and writer (Document -> HTML5 or XHTML).
#pragma once
#include "engines/doc/docmodel.h"
#include <functional>

namespace filo::doc {

struct HtmlReadOptions {
  fs::path baseDir;                                                   // relative image paths are resolved here (empty = no local files)
  std::function<bool(const std::string& href, Bytes& out)> resolve;   // alternative resource loader (e.g. inside an EPUB); takes precedence
  bool loadImages = true;
  size_t maxImageBytes = 32u << 20;
};
Document ParseHtml(std::string_view html, const HtmlReadOptions& opt = {});
// Decodes raw HTML bytes (BOM, <meta charset>, UTF-8, else windows-1252) to UTF-8.
std::string DecodeHtmlBytes(const uint8_t* d, size_t n);

struct HtmlWriteOptions {
  bool xhtml = false;               // XML-well-formed output (EPUB): self-closing void tags, no named entities
  bool fullDocument = true;         // <html><head>...; false => only the body fragment
  bool embedImages = true;          // data: URIs; otherwise imageHref() decides the src
  std::function<std::string(const Block& imageBlock, int index)> imageHref;
  std::string css;                  // inline <style> content (default stylesheet when empty and fullDocument)
  std::string title;                // <title>; defaults to document title
  std::string lang;                 // document language
  bool headingAnchors = false;      // add id="h-N" to headings (EPUB navigation)
  int firstHeadingId = 0;
  bool epubTypes = false;           // xmlns:epub declaration
};
std::string WriteHtml(const Document& d, const HtmlWriteOptions& opt = {});
// Renders only blocks[from, to) as body markup (no <html>). Image indexes passed to imageHref are the block indexes.
std::string RenderBlocksHtml(const std::vector<Block>& blocks, size_t from, size_t to, const HtmlWriteOptions& opt);
std::string DefaultCss();

// Renders runs as inline HTML (bold/italic/links...). Shared with the markdown writer for underline etc.
std::string RunsToHtml(const Runs& runs, bool xhtml);

} // namespace filo::doc
