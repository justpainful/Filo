#include "engines/doc/docmodel.h"
#include <algorithm>

namespace filo::doc {

std::string PlainText(const Runs& r) {
  std::string s;
  for (auto& x : r) { if (x.br) s.push_back('\n'); else s += x.text; }
  return s;
}
std::string PlainText(const Cell& c) { return PlainText(c.runs); }

Runs MergeRuns(const Runs& in) {
  Runs out;
  for (auto& r : in) {
    if (!r.br && r.text.empty()) continue;
    if (!out.empty() && !r.br && !out.back().br && out.back().SameFormat(r)) out.back().text += r.text;
    else out.push_back(r);
  }
  return out;
}

Run MakeRun(std::string text) { Run r; r.text = std::move(text); return r; }
Block MakeParagraph(std::string text) { Block b; b.kind = Block::Kind::Paragraph; b.runs.push_back(MakeRun(std::move(text))); return b; }
Block MakeHeading(int level, std::string text) { Block b; b.kind = Block::Kind::Heading; b.level = std::clamp(level, 1, 6); b.runs.push_back(MakeRun(std::move(text))); return b; }

std::string GuessTitle(const Document& d) {
  for (auto& b : d.blocks) if (b.kind == Block::Kind::Heading) { std::string t = TrimAscii(PlainText(b.runs)); if (!t.empty()) return t; }
  for (auto& b : d.blocks) if (b.kind == Block::Kind::Paragraph) {
    std::string t = TrimAscii(PlainText(b.runs));
    if (t.empty()) continue;
    if (t.size() > 80) { size_t cut = 80; while (cut > 0 && ((uint8_t)t[cut] & 0xC0) == 0x80) --cut; t = t.substr(0, cut); }
    return t;
  }
  return {};
}

void FinalizeDocument(Document& d) {
  size_t rtlBlocks = 0, ltrBlocks = 0;
  for (auto& b : d.blocks) {
    b.runs = MergeRuns(b.runs);
    for (auto& r : b.runs) if (!r.br) { r.text = SanitizeText(r.text); if (!r.link.empty()) r.underline = false; }      // links are underlined by convention, not by content
    if (b.kind == Block::Kind::Table) {
      for (auto& row : b.table.rows) for (auto& c : row) { c.runs = MergeRuns(c.runs); if (DetectRtl(PlainText(c.runs))) c.rtl = true; }
    }
    if (!b.rtl && (b.kind == Block::Kind::Paragraph || b.kind == Block::Kind::Heading || b.kind == Block::Kind::ListItem || b.kind == Block::Kind::Quote ||
                   b.kind == Block::Kind::Footnote))
      b.rtl = DetectRtl(PlainText(b.runs));
    if (b.kind == Block::Kind::Paragraph || b.kind == Block::Kind::Heading || b.kind == Block::Kind::ListItem) (b.rtl ? rtlBlocks : ltrBlocks)++;
  }
  d.rtl = rtlBlocks > ltrBlocks;
  if (d.meta.title.empty()) d.meta.title = GuessTitle(d);
  d.meta.title = SanitizeText(d.meta.title);
}

bool DocumentHasContent(const Document& d) {
  for (auto& b : d.blocks) {
    switch (b.kind) {
      case Block::Kind::Image: if (b.image && !b.image->bytes.empty()) return true; break;
      case Block::Kind::Table: for (auto& r : b.table.rows) for (auto& c : r) if (!TrimAscii(PlainText(c.runs)).empty()) return true; break;
      case Block::Kind::PageBreak: case Block::Kind::Rule: break;
      default: if (!TrimAscii(PlainText(b.runs)).empty()) return true;
    }
  }
  return false;
}

} // namespace filo::doc
