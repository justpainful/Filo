// PDF -> DOCX / PPTX / XLSX / EPUB on top of filo::pdf::ExtractPages (pdf_text.h).
// A PDF carries glyphs at coordinates, not paragraphs, lists, tables or columns: everything below is a heuristic reconstruction
// and the result is best-effort. Layout analysis happens once per page into a neutral page model; each target format is then
// produced from that model.
#include "engines/doc/convert_common.h"
#include "engines/doc/docx.h"
#include "engines/doc/epub.h"
#include "engines/doc/imgenc.h"
#include "engines/doc/pptx.h"
#include "engines/doc/sheet.h"
#include "engines/pdf/pdf_doc.h"
#include "engines/pdf/pdf_ocr.h"
#include "engines/pdf/pdf_text.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace filo::doc {

namespace {

using pdf::PageContent;
using pdf::TextRun;

struct PRun { std::string text; bool bold = false, italic = false, mono = false, rtl = false; float size = 0; uint32_t rgb = 0; };
struct PSeg { std::string text; float x0 = 0, x1 = 0; };
struct ListMark { bool is = false, ordered = false; int number = 0; size_t len = 0; };

struct PPara {
  std::vector<PRun> runs;
  std::vector<std::vector<PSeg>> lines;      // for the spreadsheet fallback
  int lineCount = 1;
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  float size = 0, boldFrac = 0;
  bool rtl = false, ocr = false;
  int column = 0;
  float angle = 0;
  ListMark list; int listLevel = 0;
  int heading = 0;
  size_t chars = 0;
  std::string plain;
};
struct PCell { std::string text; int row = 0, col = 0, rowSpan = 1, colSpan = 1; bool covered = false; float x = 0, y = 0, w = 0, h = 0; };
struct PTable { std::vector<std::vector<PCell>> cells; bool headerRow = false; float x = 0, y = 0, w = 0, h = 0; int rows = 0, cols = 0; };
struct PImage { std::shared_ptr<ImageData> img; float x = 0, y = 0, w = 0, h = 0; };
struct PElement { enum class K { Para, Table, Image } k = K::Para; PPara para; PTable table; PImage image; float y0() const { return k == K::Para ? para.y0 : k == K::Table ? table.y : image.y; } };
struct PPage {
  float w = 0, h = 0;
  std::vector<PElement> el;
  bool scanned = false, ocrUsed = false, pageImageOnly = false;
  std::shared_ptr<ImageData> pageImage;       // scanned page picture (pptx background / docx fallback)
};

struct Layout {
  std::vector<PPage> pages;
  std::vector<std::wstring> warnings;
  float bodySize = 11;
  std::vector<float> headingSizes;            // descending
  bool boldHeadings = false;
  size_t totalChars = 0;
  void Warn(const std::wstring& w) { for (auto& x : warnings) if (x == w) return; warnings.push_back(w); }
};

// ---- small helpers ---------------------------------------------------------------------------------------------------------
std::string U8(const std::wstring& w) { return Narrow(w); }

bool IsMonoName(const std::wstring& n) {
  std::wstring l = ToLower(n);
  return l.find(L"courier") != std::wstring::npos || l.find(L"consolas") != std::wstring::npos || l.find(L"mono") != std::wstring::npos || l.find(L"menlo") != std::wstring::npos || l.find(L"lucida console") != std::wstring::npos;
}

bool IsBulletCp(uint32_t c) {
  switch (c) {
    case 0x2022: case 0x25CF: case 0x25CB: case 0x25E6: case 0x25AA: case 0x25A0: case 0x25A1: case 0x25C6: case 0x25C7: case 0x2023: case 0x2043: case 0x2219: case 0xB7:
    case 0x2013: case 0x2014: case '*': case '-': case 0x27A2: case 0x27A4: case 0x25BA: case 0x2794: case 0x2713: case 0x2714: case 0x2756:
    case 0xF0B7: case 0xF0A7: case 0xF0D8: case 0xF076: case 0xF0FC: case 0xF02D: case 0xF0A8:
      return true;
  }
  return false;
}

ListMark ParseMarker(const std::string& s) {
  ListMark m;
  if (s.empty()) return m;
  size_t i = 0; uint32_t c = DecodeUtf8(s, i);
  if (IsBulletCp(c) && i < s.size() && (s[i] == ' ' || s[i] == '\t' || (uint8_t)s[i] == 0xC2)) {
    if ((c == '-' || c == '*' || c == 0x2013 || c == 0x2014) && i >= s.size()) return m;
    size_t j = i; while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    if ((uint8_t)s[j < s.size() ? j : 0] == 0xC2 && j + 1 < s.size() && (uint8_t)s[j + 1] == 0xA0) j += 2;
    if (j >= s.size()) { m.is = true; m.len = s.size(); return m; }
    m.is = true; m.len = j; return m;
  }
  if (IsBulletCp(c) && i == s.size() && c != '-' && c != '*' && c != 0x2013 && c != 0x2014) { m.is = true; m.len = s.size(); return m; }   // marker alone in its run
  // ordered: 12. 12) (12) and Arabic-Indic digits
  size_t p = 0; bool paren = false;
  if (s[0] == '(') { paren = true; p = 1; }
  size_t ds = p; int num = 0; int digits = 0;
  while (p < s.size() && digits < 4) {
    if (s[p] >= '0' && s[p] <= '9') { num = num * 10 + (s[p] - '0'); ++p; ++digits; }
    else if (p + 1 < s.size() && (uint8_t)s[p] == 0xD9 && (uint8_t)s[p + 1] >= 0xA0 && (uint8_t)s[p + 1] <= 0xA9) { num = num * 10 + ((uint8_t)s[p + 1] - 0xA0); p += 2; ++digits; }
    else break;
  }
  if (digits > 0 && p < s.size() && (s[p] == '.' || s[p] == ')') && (!paren || s[p] == ')')) {
    size_t q = p + 1;
    if (q >= s.size()) { if (s[p] == ')' || digits <= 2) { m.is = true; m.ordered = true; m.number = num; m.len = s.size(); } return m; }
    if (s[q] == ' ' || s[q] == '\t') { while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q; m.is = true; m.ordered = true; m.number = num; m.len = q; return m; }
  }
  (void)ds;
  return m;
}

bool EndsSentence(const std::string& s) {
  size_t n = s.size();
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"' || s[n - 1] == '\'' || s[n - 1] == ')')) --n;
  if (n == 0) return false;
  char c = s[n - 1];
  if (c == '.' || c == '!' || c == '?' || c == ':' || c == ';') return true;
  if (n >= 2 && (uint8_t)s[n - 2] == 0xD8 && (uint8_t)s[n - 1] == 0x9F) return true;             // Arabic question mark
  if (n >= 3 && (uint8_t)s[n - 3] == 0xE3 && (uint8_t)s[n - 2] == 0x80 && (uint8_t)s[n - 1] == 0x82) return true;     // ideographic full stop
  return false;
}
bool StartsLower(const std::string& s) {
  size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '(' || s[i] == '"')) ++i;
  if (i >= s.size()) return false;
  uint32_t c = DecodeUtf8(s, i);
  if (c < 128) return c >= 'a' && c <= 'z';
  if (IsRtlCodepoint(c)) return true;                  // uncased scripts: continuation is decided by the missing sentence end
  if (c > 0xFFFF) return true;
  return IsCharLowerW((wchar_t)c) != 0;
}

std::string StripSoftHyphen(const std::string& s) {
  std::string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) { if ((uint8_t)s[i] == 0xC2 && i + 1 < s.size() && (uint8_t)s[i + 1] == 0xAD) { ++i; continue; } o.push_back(s[i]); }
  return o;
}

std::string PlainOf(const std::vector<PRun>& runs) { std::string s; for (auto& r : runs) s += r.text; return s; }

void MergeAdjacent(std::vector<PRun>& runs) {
  std::vector<PRun> out;
  for (auto& r : runs) {
    if (r.text.empty()) continue;
    if (!out.empty() && out.back().bold == r.bold && out.back().italic == r.italic && out.back().mono == r.mono && out.back().rtl == r.rtl && std::fabs(out.back().size - r.size) < 0.4f && out.back().rgb == r.rgb) out.back().text += r.text;
    else out.push_back(r);
  }
  runs = std::move(out);
}

// ---- lines and paragraphs -----------------------------------------------------------------------------------------------------
struct LineRec {
  std::vector<const TextRun*> runs;
  float x0 = 1e9f, x1 = -1e9f, top = 1e9f, bottom = -1e9f, baseline = 0, size = 0;
  bool rtl = false;
  size_t chars = 0;
};

LineRec MakeLine(const std::vector<const TextRun*>& runs) {
  LineRec l; l.runs = runs;
  size_t rtlChars = 0; double sz = 0;
  for (auto* r : runs) {
    l.x0 = std::min(l.x0, r->x); l.x1 = std::max(l.x1, r->x + r->w); l.top = std::min(l.top, r->y); l.bottom = std::max(l.bottom, r->y + r->h);
    size_t n = r->text.size(); l.chars += n; sz += (double)r->fontSize * n; if (r->rtl) rtlChars += n; l.baseline = r->baseline ? r->baseline : r->y + r->h * 0.8f;
  }
  l.size = l.chars ? (float)(sz / l.chars) : (runs.empty() ? 10.f : runs[0]->fontSize);
  if (l.size <= 0) l.size = 10;
  l.rtl = rtlChars * 2 > l.chars;
  return l;
}

std::vector<PRun> RunsOfLine(const LineRec& l, std::vector<PSeg>* segs) {
  std::vector<PRun> out;
  const TextRun* prev = nullptr;
  for (auto* r : l.runs) {
    std::string t = StripSoftHyphen(U8(r->text));
    // control characters never belong to the text
    t = SanitizeText(t);
    for (auto& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (t.empty()) continue;
    bool sp = false;
    if (prev) {
      float gap = std::max(r->x - (prev->x + prev->w), prev->x - (r->x + r->w));
      float sz = std::max(r->fontSize, prev->fontSize);
      std::string pt = out.empty() ? std::string() : out.back().text;
      if (gap > 0.12f * sz && !pt.empty() && pt.back() != ' ' && t.front() != ' ') sp = true;
    }
    if (sp && !out.empty()) out.back().text.push_back(' ');
    PRun pr; pr.text = t; pr.bold = r->bold; pr.italic = r->italic; pr.mono = IsMonoName(r->fontName); pr.rtl = r->rtl; pr.size = r->fontSize; pr.rgb = r->rgb;
    out.push_back(pr);
    if (segs) {
      float gap = prev ? std::max(r->x - (prev->x + prev->w), prev->x - (r->x + r->w)) : 0;
      if (!segs->empty() && gap < 1.2f * std::max(r->fontSize, 1.f)) { segs->back().text += (sp ? " " : "") + t; segs->back().x0 = std::min(segs->back().x0, r->x); segs->back().x1 = std::max(segs->back().x1, r->x + r->w); }
      else segs->push_back({t, r->x, r->x + r->w});
    }
    prev = r;
  }
  return out;
}

bool LineStartsList(const LineRec& l) {
  if (l.runs.empty()) return false;
  std::string s;
  for (auto* r : l.runs) { s += U8(r->text); if (s.size() > 12) break; }
  return ParseMarker(s).is;
}

PPara MakePara(const std::vector<LineRec>& lines, int column) {
  PPara p; p.column = column; p.lineCount = (int)lines.size();
  p.x0 = 1e9f; p.x1 = -1e9f; p.y0 = 1e9f; p.y1 = -1e9f;
  double szSum = 0; size_t chars = 0, boldChars = 0, rtlChars = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const LineRec& l = lines[i];
    std::vector<PSeg> segs;
    std::vector<PRun> lr = RunsOfLine(l, &segs);
    if (lr.empty()) continue;
    p.lines.push_back(segs);
    if (!p.runs.empty()) {
      std::string& prevText = p.runs.back().text;
      // hyphenated line break: "inter-" + "national" -> "international"
      if (!prevText.empty() && prevText.back() == '-' && prevText.size() >= 2 && isalpha((unsigned char)prevText[prevText.size() - 2]) && StartsLower(lr[0].text)) prevText.pop_back();
      else if (!prevText.empty() && prevText.back() != ' ' && lr[0].text.front() != ' ') {
        bool cjk = false; { size_t j = prevText.size(); while (j > 0 && ((uint8_t)prevText[j - 1] & 0xC0) == 0x80) --j; if (j > 0) { size_t k = j - 1; uint32_t c = DecodeUtf8(prevText, k); cjk = c >= 0x3000 && c <= 0xFAFF; } }
        if (!cjk) prevText.push_back(' ');
      }
    }
    for (auto& r : lr) p.runs.push_back(r);
    p.x0 = std::min(p.x0, l.x0); p.x1 = std::max(p.x1, l.x1); p.y0 = std::min(p.y0, l.top); p.y1 = std::max(p.y1, l.bottom);
    for (auto* r : l.runs) { size_t n = r->text.size(); szSum += (double)r->fontSize * n; chars += n; if (r->bold) boldChars += n; if (r->rtl) rtlChars += n; }
  }
  MergeAdjacent(p.runs);
  p.chars = chars; p.size = chars ? (float)(szSum / chars) : 10.f; p.boldFrac = chars ? (float)boldChars / chars : 0; p.rtl = rtlChars * 2 > chars;
  p.plain = PlainOf(p.runs);
  return p;
}

void StripMarker(PPara& p) {
  if (p.runs.empty()) return;
  std::string head;
  for (auto& r : p.runs) { head += r.text; if (head.size() > 12) break; }
  ListMark m = ParseMarker(head);
  if (!m.is) return;
  p.list = m;
  size_t remove = m.len;
  while (remove > 0 && !p.runs.empty()) {
    PRun& r = p.runs.front();
    if (r.text.size() <= remove) { remove -= r.text.size(); p.runs.erase(p.runs.begin()); }
    else { r.text.erase(0, remove); remove = 0; }
  }
  while (!p.runs.empty() && !p.runs.front().text.empty() && p.runs.front().text.front() == ' ') { p.runs.front().text.erase(0, 1); if (p.runs.front().text.empty()) p.runs.erase(p.runs.begin()); }
  p.plain = PlainOf(p.runs);
}

std::vector<std::vector<LineRec>> SplitParagraphs(const std::vector<LineRec>& lines) {
  std::vector<std::vector<LineRec>> out;
  float blockW = 0; for (auto& l : lines) blockW = std::max(blockW, l.x1 - l.x0);
  for (size_t i = 0; i < lines.size(); ++i) {
    bool np = i == 0;
    if (!np) {
      const LineRec& a = lines[i - 1]; const LineRec& b = lines[i];
      float sz = std::max(a.size, b.size);
      float gap = b.baseline - a.baseline;
      bool rtl = b.rtl;
      float indent = rtl ? (a.x1 - b.x1) : (b.x0 - a.x0);
      std::string aText; for (auto* r : a.runs) aText += U8(r->text);
      if (gap > 1.9f * sz) np = true;
      else if (LineStartsList(b)) np = true;
      else if (std::fabs(a.size - b.size) > 0.22f * sz) np = true;
      else if (indent > 1.3f * sz && (a.x1 - a.x0) > 0.7f * blockW) np = true;
      else if ((a.x1 - a.x0) < 0.72f * blockW && EndsSentence(aText) && blockW > 8 * sz) np = true;
    }
    if (np) out.emplace_back();
    out.back().push_back(lines[i]);
  }
  return out;
}

// ---- OCR ---------------------------------------------------------------------------------------------------------------------
bool OcrPage(const pdf::PdfDocument& doc, int index, float pageW, float pageH, std::vector<PPara>& out, std::wstring* err) {
  pdf::RenderOptions ro; ro.dpi = 200; ro.annotations = false; ro.forms = false;
  pdf::Bitmap bm;
  if (!doc.RenderPage(index, ro, bm, err) || bm.width <= 0) return false;
  pdf::OcrResult res;
  if (!pdf::OcrRecognize(bm.bgra.data(), bm.width, bm.height, L"auto", res, err)) return false;
  float sx = pageW / (float)bm.width, sy = pageH / (float)bm.height;
  struct L { std::string text; float x, y, w, h; };
  std::vector<L> lines;
  for (auto& l : res.lines) { std::string t = SanitizeText(U8(l.text)); if (TrimAscii(t).empty()) continue; lines.push_back({t, l.x * sx, l.y * sy, l.w * sx, l.h * sy}); }
  std::sort(lines.begin(), lines.end(), [](const L& a, const L& b) { return a.y < b.y; });
  std::vector<std::vector<L>> groups;
  for (size_t i = 0; i < lines.size(); ++i) {
    bool np = i == 0;
    if (!np) { const L& a = lines[i - 1]; if (lines[i].y - (a.y + a.h) > 0.8f * std::max(a.h, 6.f)) np = true; }
    if (np) groups.emplace_back();
    groups.back().push_back(lines[i]);
  }
  for (auto& g : groups) {
    PPara p; p.ocr = true; p.x0 = 1e9f; p.x1 = -1e9f; p.y0 = 1e9f; p.y1 = -1e9f; p.lineCount = (int)g.size();
    std::string text; double hs = 0; size_t rtlChars = 0, chars = 0;
    for (size_t i = 0; i < g.size(); ++i) {
      if (i) text.push_back(' ');
      text += g[i].text; p.x0 = std::min(p.x0, g[i].x); p.x1 = std::max(p.x1, g[i].x + g[i].w); p.y0 = std::min(p.y0, g[i].y); p.y1 = std::max(p.y1, g[i].y + g[i].h);
      hs += g[i].h; chars += g[i].text.size(); if (DetectRtl(g[i].text)) rtlChars += g[i].text.size();
      p.lines.push_back({{g[i].text, g[i].x, g[i].x + g[i].w}});
    }
    p.size = std::clamp((float)(hs / g.size()) * 0.72f, 6.f, 40.f);
    p.rtl = rtlChars * 2 > chars; p.chars = text.size();
    PRun r; r.text = text; r.size = p.size; r.rtl = p.rtl; p.runs.push_back(r); p.plain = text;
    out.push_back(std::move(p));
  }
  return true;
}

std::shared_ptr<ImageData> PageImage(const pdf::PdfDocument& doc, int index, float widthPt, float heightPt, int dpi) {
  pdf::RenderOptions ro; ro.dpi = dpi; ro.annotations = true; ro.forms = false;
  pdf::Bitmap bm; std::wstring e;
  if (!doc.RenderPage(index, ro, bm, &e) || bm.width <= 0) return nullptr;
  Bytes jpg;
  if (!EncodeBgra(bm.bgra.data(), bm.width, bm.height, bm.width * 4, ImgFormat::Jpeg, 82, jpg)) return nullptr;
  auto img = std::make_shared<ImageData>();
  img->bytes = std::move(jpg); img->mime = "image/jpeg"; img->w = bm.width; img->h = bm.height; img->widthPt = widthPt; img->heightPt = heightPt; img->alt = "Scanned page " + std::to_string(index + 1);
  return img;
}

std::shared_ptr<ImageData> ConvertImage(const pdf::ImageBlock& ib) {
  auto img = std::make_shared<ImageData>();
  if (!ib.rawJpeg.empty()) {
    ImageInfo info;
    if (ProbeImage(ib.rawJpeg.data(), ib.rawJpeg.size(), info)) { img->bytes = ib.rawJpeg; img->mime = "image/jpeg"; img->w = info.w; img->h = info.h; }
  }
  if (img->bytes.empty() && !ib.bgra.empty() && ib.pixelW > 0 && ib.pixelH > 0) {
    Bytes png;
    if (!EncodeBgra(ib.bgra.data(), ib.pixelW, ib.pixelH, ib.pixelW * 4, ImgFormat::Png, 90, png)) return nullptr;
    img->bytes = std::move(png); img->mime = "image/png"; img->w = ib.pixelW; img->h = ib.pixelH;
  }
  if (img->bytes.empty()) return nullptr;
  img->widthPt = ib.w; img->heightPt = ib.h;
  return img;
}

// ---- one page ---------------------------------------------------------------------------------------------------------------
PTable ConvertTable(const pdf::TableCandidate& tc) {
  PTable t; t.x = tc.x; t.y = tc.y; t.w = tc.w; t.h = tc.h; t.rows = tc.rows; t.cols = tc.cols; t.headerRow = tc.headerRow;
  for (auto& row : tc.cells) {
    std::vector<PCell> r;
    for (auto& c : row) {
      PCell pc; pc.text = SanitizeText(U8(c.text)); pc.row = c.row; pc.col = c.col; pc.rowSpan = c.rowSpan; pc.colSpan = c.colSpan; pc.covered = c.covered; pc.x = c.x; pc.y = c.y; pc.w = c.w; pc.h = c.h;
      r.push_back(std::move(pc));
    }
    t.cells.push_back(std::move(r));
  }
  return t;
}

void FlushBlocks(const std::vector<const TextRun*>& pending, int column, PPage& page) {
  if (pending.empty()) return;
  // group the pending runs into blocks, then lines
  size_t i = 0;
  while (i < pending.size()) {
    size_t j = i; int blk = pending[i]->block;
    while (j < pending.size() && pending[j]->block == blk) ++j;
    std::vector<LineRec> lines;
    size_t k = i;
    while (k < j) {
      size_t m = k; int ln = pending[k]->line;
      std::vector<const TextRun*> lr;
      while (m < j && pending[m]->line == ln) { lr.push_back(pending[m]); ++m; }
      lines.push_back(MakeLine(lr));
      k = m;
    }
    for (auto& grp : SplitParagraphs(lines)) {
      PPara p = MakePara(grp, pending[i]->column);
      if (TrimAscii(p.plain).empty()) continue;
      StripMarker(p);
      if (p.runs.empty() && !p.list.is) continue;
      p.angle = pending[i]->angle;
      PElement e; e.k = PElement::K::Para; e.para = std::move(p);
      page.el.push_back(std::move(e));
    }
    i = j;
  }
  (void)column;
}

struct BuildOptions { bool ocr = true; bool wantImages = true; };

void BuildPage(const PageContent& pc, const pdf::PdfDocument* doc, const BuildOptions& bo, PPage& page, Layout& L, std::wstring* ocrErr) {
  page.w = pc.width; page.h = pc.height;
  bool scanned = pc.looksScanned || (!pc.hasText && !pc.images.empty());
  if (scanned && !pc.hasText) {
    page.scanned = true;
    bool done = false;
    if (bo.ocr && doc && pdf::QueryOcr().available) {
      std::vector<PPara> paras; std::wstring e;
      if (OcrPage(*doc, pc.index, pc.width, pc.height, paras, &e) && !paras.empty()) {
        for (auto& p : paras) { PElement el; el.k = PElement::K::Para; el.para = std::move(p); page.el.push_back(std::move(el)); }
        page.ocrUsed = true; done = true;
      } else if (ocrErr) *ocrErr = e;
    }
    if (doc && bo.wantImages) page.pageImage = PageImage(*doc, pc.index, pc.width, pc.height, 150);
    if (!done) {
      page.pageImageOnly = true;
      if (page.pageImage) { PElement el; el.k = PElement::K::Image; el.image.img = page.pageImage; el.image.x = 0; el.image.y = 0; el.image.w = pc.width; el.image.h = pc.height; page.el.push_back(std::move(el)); }
    }
    return;
  }
  auto tables = pdf::DetectTables(pc);
  std::set<int> emitted;
  std::vector<const TextRun*> pending;
  for (auto& r : pc.runs) {
    if (r.text.empty()) continue;
    if (r.table >= 0 && (size_t)r.table < tables.size()) {
      FlushBlocks(pending, 0, page); pending.clear();
      if (emitted.insert(r.table).second) { PElement e; e.k = PElement::K::Table; e.table = ConvertTable(tables[r.table]); page.el.push_back(std::move(e)); }
      continue;
    }
    pending.push_back(&r);
  }
  FlushBlocks(pending, 0, page);
  // tables whose runs were all consumed elsewhere still get emitted
  for (size_t ti = 0; ti < tables.size(); ++ti) if (!emitted.count((int)ti)) { PElement e; e.k = PElement::K::Table; e.table = ConvertTable(tables[ti]); size_t at = page.el.size(); for (size_t q = 0; q < page.el.size(); ++q) if (page.el[q].y0() > e.table.y) { at = q; break; } page.el.insert(page.el.begin() + at, std::move(e)); }
  // images at their reading-order position
  if (bo.wantImages) {
    for (auto& ib : pc.images) {
      if (ib.w < 4 || ib.h < 4 || ib.pixelW < 6 || ib.pixelH < 6) continue;
      auto img = ConvertImage(ib);
      if (!img) continue;
      PElement e; e.k = PElement::K::Image; e.image.img = img; e.image.x = ib.x; e.image.y = ib.y; e.image.w = ib.w; e.image.h = ib.h;
      size_t at = page.el.size();
      for (size_t q = 0; q < page.el.size(); ++q) {
        const PElement& o = page.el[q];
        float ox0, ox1;
        if (o.k == PElement::K::Para) { ox0 = o.para.x0; ox1 = o.para.x1; } else if (o.k == PElement::K::Table) { ox0 = o.table.x; ox1 = o.table.x + o.table.w; } else { ox0 = o.image.x; ox1 = o.image.x + o.image.w; }
        float ov = std::min(ox1, ib.x + ib.w) - std::max(ox0, ib.x);
        bool overlaps = ov > 0.3f * std::min(ox1 - ox0, ib.w);
        if (overlaps && o.y0() >= ib.y - 1.0f) { at = q; break; }
      }
      page.el.insert(page.el.begin() + at, std::move(e));
    }
  }
}

// ---- document-wide analysis ---------------------------------------------------------------------------------------------------
std::string HfKey(const std::string& s) {
  std::string k;
  for (char c : s) { if (c >= '0' && c <= '9') { if (k.empty() || k.back() != '#') k.push_back('#'); } else if (c == ' ') { if (!k.empty() && k.back() != ' ') k.push_back(' '); } else k.push_back((char)tolower((unsigned char)c)); }
  while (!k.empty() && k.back() == ' ') k.pop_back();
  return k;
}

size_t RemoveHeadersFooters(Layout& L) {
  size_t pagesWithText = 0; for (auto& p : L.pages) if (!p.scanned) ++pagesWithText;
  if (pagesWithText < 2) return 0;
  std::map<std::string, std::set<size_t>> seen;
  for (size_t pi = 0; pi < L.pages.size(); ++pi) {
    auto& pg = L.pages[pi];
    for (auto& e : pg.el) {
      if (e.k != PElement::K::Para || e.para.lineCount != 1 || e.para.ocr) continue;
      bool top = e.para.y1 < 0.09f * pg.h, bottom = e.para.y0 > 0.91f * pg.h;
      if (!top && !bottom) continue;
      seen[std::string(top ? "T" : "B") + HfKey(e.para.plain)].insert(pi);
    }
  }
  size_t need = std::max<size_t>(2, (size_t)std::ceil(0.4 * pagesWithText));
  size_t removed = 0;
  for (size_t pi = 0; pi < L.pages.size(); ++pi) {
    auto& pg = L.pages[pi];
    for (size_t q = 0; q < pg.el.size();) {
      auto& e = pg.el[q];
      bool drop = false;
      if (e.k == PElement::K::Para && e.para.lineCount == 1 && !e.para.ocr) {
        bool top = e.para.y1 < 0.09f * pg.h, bottom = e.para.y0 > 0.91f * pg.h;
        if (top || bottom) { auto it = seen.find(std::string(top ? "T" : "B") + HfKey(e.para.plain)); if (it != seen.end() && it->second.size() >= need) drop = true; }
      }
      if (drop) { pg.el.erase(pg.el.begin() + q); ++removed; } else ++q;
    }
  }
  return removed;
}

void ComputeStats(Layout& L) {
  std::map<int, size_t> hist;     // size*2 -> chars
  size_t total = 0;
  for (auto& pg : L.pages) for (auto& e : pg.el) if (e.k == PElement::K::Para && !e.para.ocr) { hist[(int)std::lround(e.para.size * 2)] += e.para.chars; total += e.para.chars; }
  L.totalChars = total;
  size_t best = 0; int bestKey = 22;
  for (auto& [k, n] : hist) if (n > best) { best = n; bestKey = k; }
  L.bodySize = bestKey / 2.0f;
  std::set<int, std::greater<int>> sizes;
  size_t boldChars = 0;
  for (auto& pg : L.pages) for (auto& e : pg.el) if (e.k == PElement::K::Para && !e.para.ocr) {
    boldChars += (size_t)(e.para.boldFrac * e.para.chars);
    bool shortPara = e.para.lineCount <= 3 && e.para.chars <= 160 && !e.para.list.is;
    if (shortPara && e.para.size >= L.bodySize * 1.12f + 0.4f) sizes.insert((int)std::lround(e.para.size * 2));
  }
  L.headingSizes.clear();
  for (int s : sizes) {
    bool merged = false;
    for (float h : L.headingSizes) if (std::fabs(h - s / 2.0f) < 0.8f) merged = true;
    if (!merged) L.headingSizes.push_back(s / 2.0f);
  }
  L.boldHeadings = total > 0 && (double)boldChars / (double)total < 0.3;
  for (auto& pg : L.pages) for (size_t q = 0; q < pg.el.size(); ++q) {
    auto& e = pg.el[q];
    if (e.k != PElement::K::Para || e.para.ocr) continue;
    PPara& p = e.para;
    p.heading = 0;
    if (p.list.is || p.lineCount > 3 || p.chars > 160) continue;
    if (p.size >= L.bodySize * 1.12f + 0.4f) {
      for (size_t r = 0; r < L.headingSizes.size(); ++r) if (std::fabs(L.headingSizes[r] - p.size) < 0.8f) { p.heading = (int)std::min<size_t>(r + 1, 6); break; }
    } else if (L.boldHeadings && p.boldFrac >= 0.9f && p.lineCount == 1 && p.chars <= 70 && !EndsSentence(p.plain) && q + 1 < pg.el.size() && pg.el[q + 1].k == PElement::K::Para && p.size >= L.bodySize - 0.6f) {
      p.heading = (int)std::min<size_t>(std::max<size_t>(L.headingSizes.size() + 1, 2), 6);
    }
  }
  // list nesting from indentation, per page
  for (auto& pg : L.pages) {
    float base = 1e9f, baseR = -1e9f;
    for (auto& e : pg.el) if (e.k == PElement::K::Para) { base = std::min(base, e.para.x0); baseR = std::max(baseR, e.para.x1); }
    for (auto& e : pg.el) if (e.k == PElement::K::Para && e.para.list.is) {
      float off = e.para.rtl ? (baseR - e.para.x1) : (e.para.x0 - base);
      e.para.listLevel = std::clamp((int)std::lround(off / std::max(8.0f, e.para.size * 1.6f)), 0, 4);
    }
  }
}

void MergeAcrossPages(Layout& L) {
  for (size_t i = 0; i + 1 < L.pages.size(); ++i) {
    auto& a = L.pages[i]; auto& b = L.pages[i + 1];
    if (a.el.empty() || b.el.empty()) continue;
    PElement& last = a.el.back(); PElement& first = b.el.front();
    if (last.k != PElement::K::Para || first.k != PElement::K::Para) continue;
    PPara& p = last.para; PPara& n = first.para;
    if (p.heading || n.heading || n.list.is || p.ocr || n.ocr) continue;
    if (EndsSentence(p.plain) || !StartsLower(n.plain)) continue;
    if (std::fabs(p.size - n.size) > 0.6f || p.rtl != n.rtl) continue;
    if (p.runs.empty() || n.runs.empty()) continue;
    std::string& pt = p.runs.back().text;
    if (!pt.empty() && pt.back() == '-' && pt.size() >= 2 && isalpha((unsigned char)pt[pt.size() - 2])) pt.pop_back(); else if (!pt.empty() && pt.back() != ' ') pt.push_back(' ');
    for (auto& r : n.runs) p.runs.push_back(r);
    MergeAdjacent(p.runs);
    p.plain = PlainOf(p.runs); p.chars += n.chars; p.lineCount += n.lineCount;
    b.el.erase(b.el.begin());
  }
}

bool Analyze(const ConvertTask& t, JobContext& ctx, bool wantImages, bool ocrAllowed, bool removeHf, Layout& L, std::wstring* err) {
  auto open = pdf::PdfDocument::Open(t.input);
  if (!open.ok()) { if (err) *err = open.message.empty() ? L"The PDF could not be opened." : open.message; return false; }
  const pdf::PdfDocument* doc = open.doc.get();
  BuildOptions bo; bo.ocr = ocrAllowed; bo.wantImages = wantImages;
  bool anyOcrFailure = false; std::wstring ocrErr;
  size_t scannedNoOcr = 0, scannedOcr = 0;
  bool ok = pdf::ExtractPagesStreaming(t.input, [&](PageContent&& pc) {
    PPage page;
    BuildPage(pc, doc, bo, page, L, &ocrErr);
    if (page.scanned) { if (page.ocrUsed) ++scannedOcr; else ++scannedNoOcr; }
    if (page.scanned && !page.ocrUsed && ocrAllowed) anyOcrFailure = true;
    L.pages.push_back(std::move(page));
    return !ctx.Cancelled();
  }, &ctx, err, {}, wantImages);
  if (ctx.Cancelled()) { if (err) *err = L"Cancelled"; return false; }
  if (!ok) { if (err && err->empty()) *err = L"The PDF could not be read."; return false; }
  if (L.pages.empty()) { if (err) *err = L"The PDF has no pages."; return false; }
  (void)anyOcrFailure;
  if (scannedOcr) L.Warn(L"Scanned pages were read with Windows OCR (" + std::to_wstring(scannedOcr) + L" page(s)); recognition errors are possible, please proofread.");
  if (scannedNoOcr) {
    auto info = pdf::QueryOcr();
    if (!ocrAllowed) L.Warn(L"Scanned pages (no text layer) were kept as pictures because OCR is turned off.");
    else if (!info.available) L.Warn(L"Scanned pages (no text layer) were kept as pictures because no Windows OCR language pack is installed (" + info.message + L").");
    else L.Warn(L"Some scanned pages could not be read with OCR and were kept as pictures.");
  }
  if (removeHf) {
    size_t n = RemoveHeadersFooters(L);
    if (n) L.Warn(L"Repeating page headers, footers and page numbers (" + std::to_wstring(n) + L" lines) were removed.");
  }
  ComputeStats(L);
  MergeAcrossPages(L);
  return true;
}

// ---- builders ---------------------------------------------------------------------------------------------------------------
Runs ToRuns(const PPara& p, bool withSizes, float bodySize, bool headingPara) {
  Runs out;
  for (auto& r : p.runs) {
    Run x; x.text = r.text; x.bold = r.bold && !headingPara; x.italic = r.italic; x.code = r.mono; x.rtl = r.rtl;
    if ((r.rgb & 0xFFFFFF) != 0 && (r.rgb & 0xFFFFFF) != 0x010101 && !headingPara) x.color = Printf("%06X", r.rgb & 0xFFFFFF);
    if (withSizes && !headingPara && std::fabs(r.size - 11.0f) > 0.75f && r.size > 4) x.sizePt = std::round(r.size * 2) / 2.0;
    out.push_back(std::move(x));
  }
  (void)bodySize;
  return out;
}

Document BuildDocument(const Layout& L, bool withSizes) {
  Document d;
  for (auto& w : L.warnings) d.Warn(w);
  int pageNo = 0;
  for (auto& pg : L.pages) {
    ++pageNo;
    for (auto& e : pg.el) {
      switch (e.k) {
        case PElement::K::Para: {
          const PPara& p = e.para;
          Block b;
          b.runs = ToRuns(p, withSizes, L.bodySize, p.heading > 0);
          if (b.runs.empty()) break;
          b.rtl = p.rtl;
          if (p.heading) { b.kind = Block::Kind::Heading; b.level = p.heading; }
          else if (p.list.is) { b.kind = Block::Kind::ListItem; b.level = p.listLevel; b.ordered = p.list.ordered; b.number = p.list.number; }
          else b.kind = Block::Kind::Paragraph;
          d.blocks.push_back(std::move(b));
          break;
        }
        case PElement::K::Table: {
          const PTable& t = e.table;
          Block b; b.kind = Block::Kind::Table; b.table.headerRow = t.headerRow;
          for (auto& row : t.cells) {
            std::vector<Cell> r;
            for (auto& c : row) {
              if (c.covered) continue;
              Cell cell; cell.colspan = std::max(1, c.colSpan); cell.header = t.headerRow && &row == &t.cells[0];
              auto parts = SplitStr(c.text, '\n');
              for (size_t i = 0; i < parts.size(); ++i) { if (i) { Run br; br.br = true; cell.runs.push_back(br); } if (!parts[i].empty()) { Run x; x.text = parts[i]; cell.runs.push_back(x); } }
              r.push_back(std::move(cell));
            }
            if (!r.empty()) b.table.rows.push_back(std::move(r));
          }
          if (!b.table.rows.empty()) d.blocks.push_back(std::move(b));
          break;
        }
        case PElement::K::Image: {
          Block b; b.kind = Block::Kind::Image; b.image = e.image.img;
          d.blocks.push_back(std::move(b));
          break;
        }
      }
    }
  }
  FinalizeDocument(d);
  return d;
}

// numbers stay numbers: plain numbers (also with US thousands separators or accounting parentheses), ISO dates; everything else is text
SCell TableCell(const std::string& raw) {
  std::string s = TrimAscii(raw);
  SCell c;
  if (s.empty()) return c;
  SCell inf = InferCell(s, false, true);
  if (inf.t == SCell::T::Number || inf.t == SCell::T::Date || inf.t == SCell::T::DateTime) return inf;
  bool neg = false; std::string t = s;
  if (t.size() > 2 && t.front() == '(' && t.back() == ')') { neg = true; t = t.substr(1, t.size() - 2); }
  if (!t.empty() && t[0] == '-') { neg = !neg; t = t.substr(1); }
  // 1,234,567.89
  size_t dot = t.find('.');
  std::string ip = dot == std::string::npos ? t : t.substr(0, dot);
  std::string fp = dot == std::string::npos ? "" : t.substr(dot + 1);
  auto digits = [](const std::string& x) { if (x.empty()) return false; for (char ch : x) if (ch < '0' || ch > '9') return false; return true; };
  auto groups = SplitStr(ip, ',');
  bool ok = groups.size() >= 2 && !groups[0].empty() && groups[0].size() <= 3 && digits(groups[0]) && (dot == std::string::npos || digits(fp));
  for (size_t g = 1; ok && g < groups.size(); ++g) ok = groups[g].size() == 3 && digits(groups[g]);
  if (ok && ip.find_first_not_of("0123456789,") == std::string::npos && !(groups[0].size() > 1 && groups[0][0] == '0')) {
    std::string clean; for (char ch : t) if (ch != ',') clean.push_back(ch);
    c.t = SCell::T::Number; c.n = atof(clean.c_str()) * (neg ? -1 : 1);
    return c;
  }
  if (neg && digits(ip) && (dot == std::string::npos || digits(fp)) && !(ip.size() > 1 && ip[0] == '0')) { c.t = SCell::T::Number; c.n = -atof(t.c_str()); return c; }
  c.t = SCell::T::Text; c.s = SanitizeText(raw);
  return c;
}

Workbook BuildWorkbook(const Layout& L, bool* anyTable) {
  Workbook wb;
  struct Pending { Sheet sheet; size_t cols = 0; size_t lastPage = 0; float x = 0; std::string headerKey; };
  std::vector<Pending> sheets;
  size_t tableNo = 0;
  for (size_t pi = 0; pi < L.pages.size(); ++pi) {
    bool firstOnPage = true;
    for (auto& e : L.pages[pi].el) {
      if (e.k != PElement::K::Table) continue;
      const PTable& t = e.table;
      std::vector<std::vector<SCell>> rows;
      for (auto& row : t.cells) {
        std::vector<SCell> r;
        for (auto& c : row) {
          if (c.covered) { r.push_back(SCell()); continue; }
          std::string txt = c.text; for (auto& ch : txt) if (ch == '\n') ch = ' ';
          r.push_back(TableCell(txt));
        }
        rows.push_back(std::move(r));
      }
      if (rows.empty()) continue;
      std::string key; for (auto& c : rows[0]) key += IsoFromCell(c) + "|";
      bool cont = !sheets.empty() && firstOnPage && sheets.back().lastPage + 1 == pi && sheets.back().cols == (size_t)t.cols && std::fabs(sheets.back().x - t.x) < 0.06f * L.pages[pi].w;
      if (cont) {
        auto& sh = sheets.back();
        size_t from = (key == sh.headerKey) ? 1 : 0;          // a repeated header row is not data
        for (size_t r = from; r < rows.size(); ++r) sh.sheet.rows.push_back(rows[r]);
        sh.lastPage = pi;
      } else {
        Pending p; p.sheet.name = "Table " + std::to_string(++tableNo); p.sheet.rows = rows; p.cols = (size_t)t.cols; p.lastPage = pi; p.x = t.x; p.headerKey = key;
        sheets.push_back(std::move(p));
      }
      firstOnPage = false;
    }
  }
  if (anyTable) *anyTable = !sheets.empty();
  for (auto& p : sheets) wb.sheets.push_back(std::move(p.sheet));
  if (sheets.empty()) {
    // no table: one sheet with the text lines; wide gaps between text segments of a line become separate cells
    Sheet sh; sh.name = "Text";
    for (size_t pi = 0; pi < L.pages.size(); ++pi) {
      for (auto& e : L.pages[pi].el) {
        if (e.k != PElement::K::Para) continue;
        for (auto& line : e.para.lines) {
          std::vector<SCell> row;
          for (auto& seg : line) row.push_back(TableCell(seg.text));
          if (!row.empty()) sh.rows.push_back(std::move(row));
        }
      }
    }
    wb.sheets.push_back(std::move(sh));
  }
  return wb;
}

Deck BuildDeck(const Layout& L, const std::string& title) {
  Deck d; d.title = title;
  d.widthPt = L.pages.empty() ? 720 : L.pages[0].w; d.heightPt = L.pages.empty() ? 405 : L.pages[0].h;
  for (auto& pg : L.pages) {
    Slide s;
    // the title: the largest text near the top that is clearly bigger than the body text
    int titleIdx = -1; float bestSize = 0;
    for (size_t q = 0; q < pg.el.size(); ++q) {
      auto& e = pg.el[q];
      if (e.k != PElement::K::Para || e.para.ocr || e.para.list.is || e.para.lineCount > 3 || e.para.angle != 0) continue;
      if (e.para.y0 > 0.45f * pg.h) continue;
      if (e.para.size > bestSize && e.para.size >= L.bodySize * 1.25f) { bestSize = e.para.size; titleIdx = (int)q; }
    }
    if (pg.pageImage && (pg.scanned)) {
      SlideShape im; im.kind = SlideShape::Kind::Image; im.x = 0; im.y = 0; im.w = pg.w; im.h = pg.h; im.image = pg.pageImage; s.shapes.push_back(std::move(im));
    }
    for (size_t q = 0; q < pg.el.size(); ++q) {
      auto& e = pg.el[q];
      if (e.k == PElement::K::Image) {
        if (pg.scanned && pg.pageImageOnly) continue;      // already placed as the page picture
        SlideShape im; im.kind = SlideShape::Kind::Image; im.x = e.image.x; im.y = e.image.y; im.w = e.image.w; im.h = e.image.h; im.image = e.image.img; s.shapes.push_back(std::move(im));
        continue;
      }
      auto addPara = [&](const PPara& p, double x, double y, double w, double h, bool isTitle) {
        SlideShape sh; sh.kind = isTitle ? SlideShape::Kind::Title : SlideShape::Kind::Text;
        double slack = p.lineCount > 1 ? 0.04 : 0.0;
        sh.x = x; sh.y = y; sh.w = w * (1.0 + slack) + (p.lineCount > 1 ? 2 : 4); sh.h = std::max(h, (double)p.size * 1.2);
        if (p.rtl) sh.x = x - (sh.w - w);
        sh.wrap = p.lineCount > 1;
        if (p.angle != 0 && (std::fabs(std::fmod(p.angle, 180.0f) - 90.0f) < 1.0f)) {
          // rotated text: PowerPoint rotates an unrotated box about its centre
          double cx = x + w / 2, cy = y + h / 2;
          sh.w = h; sh.h = w; sh.x = cx - sh.w / 2; sh.y = cy - sh.h / 2; sh.rotationDeg = -p.angle; sh.wrap = false;
        }
        SlidePara sp; sp.rtl = p.rtl; sp.align = p.rtl ? 'r' : 'l'; sp.bullet = p.list.is;
        for (auto& r : p.runs) {
          SlideRun sr; sr.text = r.text; sr.bold = r.bold; sr.italic = r.italic; sr.sizePt = std::clamp((double)(r.size > 0 ? r.size : p.size), 4.0, 400.0); sr.rgb = r.rgb & 0xFFFFFF; sr.rtl = r.rtl;
          sr.hidden = p.ocr && pg.pageImage != nullptr;
          if (r.mono) sr.font = "Courier New"; else if (r.rtl || ContainsRtl(r.text)) sr.font = "Arial";
          sp.runs.push_back(sr);
        }
        if (sp.runs.empty()) return;
        sh.paras.push_back(std::move(sp));
        s.shapes.push_back(std::move(sh));
      };
      if (e.k == PElement::K::Para) {
        const PPara& p = e.para;
        addPara(p, p.x0, p.y0, std::max(1.0f, p.x1 - p.x0), std::max(1.0f, p.y1 - p.y0), (int)q == titleIdx);
      } else {
        for (auto& row : e.table.cells) for (auto& c : row) {
          if (c.covered || TrimAscii(c.text).empty()) continue;
          PPara p; p.size = std::clamp(c.h * 0.5f, 6.f, 14.f); p.lineCount = 1 + (int)std::count(c.text.begin(), c.text.end(), '\n');
          if (L.bodySize > 0 && L.bodySize < 24) p.size = std::min(p.size, L.bodySize);
          PRun r; r.text = c.text; for (auto& ch : r.text) if (ch == '\n') ch = ' '; r.size = p.size; r.rtl = DetectRtl(c.text); p.rtl = r.rtl; p.runs.push_back(r);
          addPara(p, c.x + 2, c.y + 2, std::max(1.0f, c.w - 4), std::max(1.0f, c.h - 4), false);
        }
      }
    }
    d.slides.push_back(std::move(s));
  }
  return d;
}

} // namespace

EngineResult RunPdfToOffice(const ConvertTask& t, JobContext& ctx) {
  const std::wstring& out = t.outExt;
  std::wstring err;
  EngineResult er;
  if (t.inKind.size == 0) return Fail(L"The file is empty, so there is nothing to convert.");
  ComScope com;
  bool ocrAllowed = OptStr(t.options, "ocr", "auto") != "off";
  bool removeHf = !OptBool(t.options, "keepHeadersFooters", false) && (out == L"docx" || out == L"epub");
  bool wantImages = out != L"xlsx";
  Layout L;
  ctx.SetProgress(0.02);
  if (!Analyze(t, ctx, wantImages, ocrAllowed, removeHf, L, &err)) return Fail(err);
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  AddWarnings(er, L.warnings);
  er.warnings.push_back(L"Layout fidelity is best-effort: paragraphs, lists, tables and columns are reconstructed from glyph positions and can differ from the PDF.");
  ctx.SetProgress(0.85);
  std::vector<std::wstring> w;

  size_t textChars = 0, imageEls = 0;
  for (auto& pg : L.pages) for (auto& e : pg.el) { if (e.k == PElement::K::Para) textChars += e.para.chars; else if (e.k == PElement::K::Image) ++imageEls; }

  if (out == L"docx") {
    Document d = BuildDocument(L, true);
    if (d.blocks.empty()) return Fail(L"No text or images could be extracted from this PDF.");
    DocxWriteOptions o;
    std::string ps = OptStr(t.options, "pageSize", "auto");
    if (ps == "letter") o.page = PageSize::Letter;
    else if (ps == "a4") o.page = PageSize::A4;
    else {
      float pw = L.pages[0].w, ph = L.pages[0].h;
      double dA4 = std::fabs(pw - 595.3) + std::fabs(ph - 841.9), dLt = std::fabs(pw - 612.0) + std::fabs(ph - 792.0);
      o.page = dLt < dA4 ? PageSize::Letter : PageSize::A4;
    }
    if (!WriteDocx(d, t.output, o, &err, &w)) return Fail(err);
  }
  else if (out == L"epub") {
    Document d = BuildDocument(L, false);
    if (d.blocks.empty()) return Fail(L"No text or images could be extracted from this PDF.");
    if (d.meta.title.empty()) d.meta.title = Narrow(t.input.stem().wstring());
    EpubWriteOptions o; o.splitOnH1 = true;
    if (!WriteEpub(d, t.output, o, &err, &w)) return Fail(err);
  }
  else if (out == L"xlsx") {
    bool anyTable = false;
    Workbook wb = BuildWorkbook(L, &anyTable);
    size_t cells = 0; for (auto& s : wb.sheets) for (auto& r : s.rows) cells += r.size();
    if (cells == 0) return Fail(L"No tables or text could be extracted from this PDF.");
    if (!anyTable) er.warnings.push_back(L"No tables were detected, so the text lines were written to a single sheet (wide gaps between words split a line into cells).");
    else er.warnings.push_back(L"Only detected tables were exported; text outside the tables is not included.");
    if (!WriteXlsx(wb, t.output, &err, &w)) return Fail(err);
  }
  else if (out == L"pptx") {
    Deck d = BuildDeck(L, Narrow(t.input.stem().wstring()));
    if (!WritePptx(d, t.output, &err, &w)) return Fail(err);
    er.warnings.push_back(L"Each page became one slide with text boxes and pictures at the PDF coordinates; table borders, vector graphics and charts are not reproduced.");
  }
  else return Fail(L"This output format is not supported.");
  AddWarnings(er, w);
  if (textChars == 0 && imageEls > 0) er.warnings.push_back(L"The PDF contains no recognisable text; pages were kept as pictures.");
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  ctx.SetProgress(1.0);
  er.ok = true;
  return er;
}

} // namespace filo::doc
