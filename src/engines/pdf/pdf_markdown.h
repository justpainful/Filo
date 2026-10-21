// PageContent -> Markdown (headings from font size / bold, paragraphs, lists, GFM tables, images). Engine private.
#pragma once
#include "engines/pdf/pdf_text.h"
#include <functional>
#include <map>
#include <string>

namespace filo::pdf {

class MarkdownWriter {
 public:
  struct Options { bool pageBreaks = true; };
  explicit MarkdownWriter(Options o = {}) : opt_(o) {}
  // Pass 1 over all pages: collects the body font size and heading sizes.
  void Analyze(const PageContent& pc);
  void Prepare();
  // Pass 2: Markdown for one page. `saveImage(index, image)` stores an image and returns the relative path to link, or L"" to skip.
  std::wstring RenderPage(const PageContent& pc, const std::function<std::wstring(int, const ImageBlock&)>& saveImage);
  bool firstPageWritten() const { return wrote_; }
  float bodyFontSize() const { return bodyFs_; }
 private:
  Options opt_;
  std::map<int, long> sizeChars_;
  float bodyFs_ = 11;
  std::vector<int> headingKeys_;     // font size keys (size*2) of heading levels, largest first
  bool wrote_ = false;
};

std::wstring EscapeMarkdown(const std::wstring& s, bool atLineStart);

} // namespace filo::pdf
