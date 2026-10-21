// Neutral document model shared by every reader and writer of the document engine.
// Readers (markdown, txt, html, rtf, rtfd, docx, epub, textclipping, pdf) fill a Document; writers (docx, markdown, txt, html,
// epub, pptx...) render it. Strings are UTF-8, already sanitised (no XML-illegal control characters).
#pragma once
#include "engines/doc/docutil.h"
#include <memory>

namespace filo::doc {

struct Run {
  std::string text;
  bool bold = false, italic = false, underline = false, strike = false, code = false, sup = false, sub = false;
  bool br = false;               // a hard line break (text ignored)
  bool rtl = false;              // explicitly right-to-left (writers also detect it from the characters)
  std::string link;              // URL ("https://...", "mailto:...") or "#anchor"; empty = no link
  std::string color;             // "RRGGBB" or ""
  double sizePt = 0;             // 0 = inherit
  std::string font;              // font family hint
  bool SameFormat(const Run& o) const {
    return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike && code == o.code && sup == o.sup && sub == o.sub &&
           br == o.br && rtl == o.rtl && link == o.link && color == o.color && sizePt == o.sizePt && font == o.font;
  }
};
using Runs = std::vector<Run>;

struct ImageData {
  Bytes bytes;
  std::string mime;              // image/png, image/jpeg, image/gif, ...
  int w = 0, h = 0;              // pixels (0 = unknown; writers probe the bytes)
  double widthPt = 0, heightPt = 0;   // intended display size in points (0 = derive from pixels at 96 dpi)
  std::string alt;
  std::string name;              // original file name if known
};

struct Cell {
  Runs runs;
  bool header = false;
  int colspan = 1;
  bool rtl = false;
};
struct Table {
  std::vector<std::vector<Cell>> rows;
  std::vector<char> align;       // per column: 'l', 'c', 'r' or 0
  bool headerRow = false;        // first row is a header row
  bool rtl = false;              // table direction (visual column order)
  size_t Cols() const { size_t m = 0; for (auto& r : rows) { size_t c = 0; for (auto& x : r) c += (size_t)std::max(1, x.colspan); m = std::max(m, c); } return m; }
};

struct Block {
  enum class Kind { Heading, Paragraph, ListItem, Table, Image, PageBreak, Code, Quote, Rule, Footnote };
  Kind kind = Kind::Paragraph;
  int level = 0;                 // Heading: 1..6. ListItem/Quote: nesting depth, 0-based
  bool ordered = false;          // ListItem
  int number = 0;                // ListItem (ordered): the number shown for this item (informational; 0 = unknown)
  int checked = -1;              // ListItem task state: -1 none, 0 unchecked, 1 checked
  bool rtl = false;              // paragraph direction
  char align = 0;                // 'l','c','r','j' or 0
  Runs runs;                     // Heading, Paragraph, ListItem, Quote, Footnote; Code: one run holding the whole text
  std::string lang;              // Code: info string
  std::string label;             // Footnote: label
  Table table;                   // Table
  std::shared_ptr<ImageData> image;   // Image
  std::string anchor;            // optional id (heading anchors)
};

struct Meta {
  std::string title, author, language, subject, description;
  std::string created;           // ISO 8601
};

enum class PageSize { A4, Letter };

struct Document {
  Meta meta;
  std::vector<Block> blocks;
  std::vector<std::wstring> warnings;      // lossy steps found while reading; surfaced to the user
  bool rtl = false;                        // dominant document direction
  void Warn(const std::wstring& w) { for (auto& x : warnings) if (x == w) return; warnings.push_back(w); }
};

// ---- helpers ---------------------------------------------------------------------------------------------------------
std::string PlainText(const Runs& r);                       // concatenated text, br => '\n'
std::string PlainText(const Cell& c);
Runs MergeRuns(const Runs& r);                              // joins adjacent runs with identical formatting, drops empty ones
Run MakeRun(std::string text);
Block MakeParagraph(std::string text);
Block MakeHeading(int level, std::string text);
void FinalizeDocument(Document& d);                         // paragraph/doc direction detection, run merging, title guess
bool DocumentHasContent(const Document& d);

// First heading's text (or first paragraph) for titles.
std::string GuessTitle(const Document& d);

} // namespace filo::doc
