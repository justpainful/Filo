// Developer aid: FILO_DUMP=<pdf> filo_tests_pdf.exe dump  -> writes a layout dump next to the scratch dir.
#include "pdf_testutil.h"
#include "engines/pdf/pdf_text.h"
#include <cstdio>
using namespace ptest;
using namespace filo;

TEST(dump_layout) {
  const char* e = std::getenv("FILO_DUMP");
  if (!e) return;
  std::vector<pdf::PageContent> pages; std::wstring err;
  bool ok = pdf::ExtractPages(Widen(e), pages, nullptr, &err);
  std::string out;
  out += ok ? "ok\n" : ("FAILED " + Narrow(err) + "\n");
  for (auto& p : pages) {
    char b[256]; snprintf(b, sizeof b, "== page %d %.1fx%.1f rot=%d runs=%zu images=%zu rules=%zu cols=%d scanned=%d text=%d\n", p.index, p.width, p.height, p.rotation, p.runs.size(), p.images.size(), p.rules.size(), p.columns, p.looksScanned, p.hasText);
    out += b;
    for (auto& r : p.runs) {
      snprintf(b, sizeof b, "L%d B%d C%d T%d %s%s%s sz=%.1f [%.1f,%.1f %.1fx%.1f] ", r.line, r.block, r.column, r.table, r.bold ? "b" : "-", r.italic ? "i" : "-", r.rtl ? "r" : "-", r.fontSize, r.x, r.y, r.w, r.h);
      out += b; out += Narrow(r.fontName) + " | " + Narrow(r.text) + "\n";
    }
    for (auto& im : p.images) { snprintf(b, sizeof b, "IMG [%.1f,%.1f %.1fx%.1f] px=%dx%d bgra=%zu jpeg=%zu\n", im.x, im.y, im.w, im.h, im.pixelW, im.pixelH, im.bgra.size(), im.rawJpeg.size()); out += b; }
    auto tabs = pdf::DetectTables(p);
    for (size_t t = 0; t < tabs.size(); ++t) {
      snprintf(b, sizeof b, "TABLE %zu ruled=%d %dx%d header=%d [%.1f,%.1f %.1fx%.1f]\n", t, tabs[t].ruled, tabs[t].rows, tabs[t].cols, tabs[t].headerRow, tabs[t].x, tabs[t].y, tabs[t].w, tabs[t].h);
      out += b;
      for (auto& row : tabs[t].cells) { for (auto& c : row) { out += c.covered ? "<cov>" : ("[" + Narrow(c.text) + "]"); snprintf(b, sizeof b, "(%dx%d) ", c.rowSpan, c.colSpan); out += b; } out += "\n"; }
    }
    out += "-- plain --\n" + Narrow(pdf::PageToPlainText(p)) + "\n";
  }
  WriteFileBytes(Scratch() / L"dump.txt", out);
}
static int g_unbuf = (setvbuf(stdout, nullptr, _IONBF, 0), 0);
