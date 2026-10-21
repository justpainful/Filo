#include "engines/doc/sheet.h"
#include "engines/doc/opc.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

namespace filo::doc {

namespace {

constexpr const char* kMime = "application/vnd.oasis.opendocument.spreadsheet";

std::string OdfParas(const std::string& text) {
  std::string out;
  for (auto& line : SplitStr(text, '\n')) {
    out += "<text:p>";
    size_t i = 0;
    std::string run;
    auto flushRun = [&] { out += XmlEscape(run); run.clear(); };
    while (i < line.size()) {
      char c = line[i];
      if (c == '\r') { ++i; continue; }
      if (c == '\t') { flushRun(); out += "<text:tab/>"; ++i; continue; }
      if (c == ' ') {
        size_t j = i; while (j < line.size() && line[j] == ' ') ++j;
        size_t cnt = j - i;
        bool lead = run.empty() && out.size() >= 8 && out.compare(out.size() - 8, 8, "<text:p>") == 0;
        if (cnt == 1 && !lead) { run.push_back(' '); }
        else {
          size_t enc = lead ? cnt : cnt - 1;
          if (!lead) run.push_back(' ');
          flushRun();
          out += "<text:s text:c=\"" + std::to_string(enc) + "\"/>";
        }
        i = j; continue;
      }
      run.push_back(c); ++i;
    }
    flushRun();
    out += "</text:p>";
  }
  return out;
}

bool LooksRtl(const Sheet& s) {
  size_t rtl = 0, ltr = 0;
  for (size_t r = 0; r < s.rows.size() && r < 50; ++r)
    for (auto& c : s.rows[r]) if (c.t == SCell::T::Text) { if (DetectRtl(c.s)) ++rtl; else if (!c.s.empty()) ++ltr; }
  return rtl > 0 && rtl > ltr;
}

std::string IsoNow() {
  time_t t = time(nullptr); tm g{}; gmtime_s(&g, &t);
  return Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
}

} // namespace

bool WriteOds(const Workbook& wb, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings) {
  (void)warnings;
  ZipWriter z;
  if (!z.Open(out, err)) return false;
  z.Add("mimetype", std::string(kMime), false);
  z.Add("META-INF/manifest.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/></manifest:manifest>");
  z.Add("meta.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" office:version=\"1.2\"><office:meta><meta:generator>Filo</meta:generator><meta:creation-date>" + IsoNow() + "</meta:creation-date></office:meta></office:document-meta>");
  z.Add("styles.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
        "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\"><office:styles>"
        "<style:style style:name=\"Default\" style:family=\"table-cell\"/></office:styles></office:document-styles>");

  std::string c = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
                  "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
                  "xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" office:version=\"1.2\">"
                  "<office:automatic-styles>"
                  "<number:date-style style:name=\"NDate\"><number:year number:style=\"long\"/><number:text>-</number:text><number:month number:style=\"long\"/><number:text>-</number:text><number:day number:style=\"long\"/></number:date-style>"
                  "<number:date-style style:name=\"NDateTime\"><number:year number:style=\"long\"/><number:text>-</number:text><number:month number:style=\"long\"/><number:text>-</number:text><number:day number:style=\"long\"/><number:text> </number:text>"
                  "<number:hours number:style=\"long\"/><number:text>:</number:text><number:minutes number:style=\"long\"/><number:text>:</number:text><number:seconds number:style=\"long\"/></number:date-style>"
                  "<number:time-style style:name=\"NTime\"><number:hours number:style=\"long\"/><number:text>:</number:text><number:minutes number:style=\"long\"/><number:text>:</number:text><number:seconds number:style=\"long\"/></number:time-style>"
                  "<style:style style:name=\"ceDate\" style:family=\"table-cell\" style:parent-style-name=\"Default\" style:data-style-name=\"NDate\"/>"
                  "<style:style style:name=\"ceDateTime\" style:family=\"table-cell\" style:parent-style-name=\"Default\" style:data-style-name=\"NDateTime\"/>"
                  "<style:style style:name=\"ceTime\" style:family=\"table-cell\" style:parent-style-name=\"Default\" style:data-style-name=\"NTime\"/>"
                  "<style:style style:name=\"taLtr\" style:family=\"table\"><style:table-properties table:display=\"true\" style:writing-mode=\"lr-tb\"/></style:style>"
                  "<style:style style:name=\"taRtl\" style:family=\"table\"><style:table-properties table:display=\"true\" style:writing-mode=\"rl-tb\"/></style:style>"
                  "</office:automatic-styles><office:body><office:spreadsheet>";
  std::vector<std::string> used;
  Workbook empty; if (wb.sheets.empty()) empty.sheets.push_back(Sheet{});
  const Workbook& src = wb.sheets.empty() ? empty : wb;
  for (auto& sh : src.sheets) {
    std::string name = SafeSheetName(sh.name, used);
    used.push_back(name);
    size_t cols = std::max<size_t>(1, sh.MaxCols());
    c += "<table:table table:name=\"" + XmlEscape(name, true) + "\" table:style-name=\"" + (LooksRtl(sh) ? "taRtl" : "taLtr") + "\">";
    c += "<table:table-column table:number-columns-repeated=\"" + std::to_string(cols) + "\"/>";
    for (auto& row : sh.rows) {
      size_t last = row.size();
      while (last > 0 && row[last - 1].t == SCell::T::Empty) --last;
      c += "<table:table-row>";
      if (last == 0) c += "<table:table-cell/>";
      for (size_t i = 0; i < last;) {
        const SCell& cell = row[i];
        if (cell.t == SCell::T::Empty) {
          size_t j = i; while (j < last && row[j].t == SCell::T::Empty) ++j;
          c += j - i > 1 ? "<table:table-cell table:number-columns-repeated=\"" + std::to_string(j - i) + "\"/>" : "<table:table-cell/>";
          i = j; continue;
        }
        switch (cell.t) {
          case SCell::T::Text: c += "<table:table-cell office:value-type=\"string\" calcext:value-type=\"string\">" + OdfParas(SanitizeText(cell.s)) + "</table:table-cell>"; break;
          case SCell::T::Number: { std::string v = FormatNumber(cell.n); c += "<table:table-cell office:value-type=\"float\" office:value=\"" + v + "\"><text:p>" + v + "</text:p></table:table-cell>"; break; }
          case SCell::T::Bool: c += std::string("<table:table-cell office:value-type=\"boolean\" office:boolean-value=\"") + (cell.n != 0 ? "true" : "false") + "\"><text:p>" + (cell.n != 0 ? "TRUE" : "FALSE") + "</text:p></table:table-cell>"; break;
          case SCell::T::Date: { std::string v = IsoFromCell(cell); c += "<table:table-cell table:style-name=\"ceDate\" office:value-type=\"date\" office:date-value=\"" + v + "\"><text:p>" + v + "</text:p></table:table-cell>"; break; }
          case SCell::T::DateTime: { std::string v = IsoFromCell(cell); std::string iso = v; size_t sp = iso.find(' '); if (sp != std::string::npos) iso[sp] = 'T'; c += "<table:table-cell table:style-name=\"ceDateTime\" office:value-type=\"date\" office:date-value=\"" + iso + "\"><text:p>" + v + "</text:p></table:table-cell>"; break; }
          case SCell::T::Time: {
            int64_t tot = (int64_t)std::llround(cell.n * 86400000.0);
            long long hh = tot / 3600000; int mm = (int)(tot / 60000 % 60), ss = (int)(tot / 1000 % 60);
            c += Printf("<table:table-cell table:style-name=\"ceTime\" office:value-type=\"time\" office:time-value=\"PT%02lldH%02dM%02dS\"><text:p>%02lld:%02d:%02d</text:p></table:table-cell>", hh, mm, ss, hh, mm, ss);
            break;
          }
          default: break;
        }
        ++i;
      }
      c += "</table:table-row>";
    }
    c += "</table:table>";
  }
  c += "</office:spreadsheet></office:body></office:document-content>";
  // the string cells above use the calcext attribute; declare its namespace
  const std::string needle = "xmlns:of=";
  size_t p = c.find(needle);
  if (p != std::string::npos) c.insert(p, "xmlns:calcext=\"urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0\" ");
  z.Add("content.xml", c);
  if (!z.Close()) { if (err) *err = L"The spreadsheet file could not be written."; return false; }
  return true;
}

// ============================================================================================================================
namespace {

bool ParseDuration(const std::string& v, double& days) {     // PT12H30M45.5S, P1DT2H
  if (v.size() < 3 || v[0] != 'P') return false;
  double total = 0; size_t i = 1; bool time = false; std::string num;
  for (; i < v.size(); ++i) {
    char c = v[i];
    if (c == 'T') { time = true; continue; }
    if (isdigit((unsigned char)c) || c == '.') { num.push_back(c); continue; }
    double x = atof(num.c_str()); num.clear();
    if (c == 'D') total += x * 86400; else if (c == 'H') total += x * 3600; else if (c == 'M' && time) total += x * 60; else if (c == 'S') total += x; else return false;
  }
  days = total / 86400.0;
  return true;
}

class OdsReader {
 public:
  OdsReader(ZipReader& z, Workbook& wb) : z_(z), wb_(wb) {}
  bool Run(std::wstring* err) {
    Bytes b;
    if (!z_.Has("content.xml")) { if (err) *err = L"This is not an OpenDocument spreadsheet (content.xml is missing)."; return false; }
    if (!z_.Read("content.xml", b, err)) return false;
    xml::Sax sax;
    Sheet* sh = nullptr;
    int tableDepth = 0, cellDepth = 0, annoDepth = 0;
    std::vector<SCell> row; size_t rowRep = 1, pendingEmpty = 0;
    bool inRow = false, inCell = false, rowHasData = false;
    SCell cell; size_t colRep = 1; std::string valType, strValue; std::string ptext; bool firstPara = true, inP = false;
    bool tooBig = false;
    size_t cellBudget = 24000000, rowBudget = 1048576;

    sax.start = [&](std::string_view n, const xml::AttrList& at) {
      std::string_view ln = xml::LocalName(n);
      if (annoDepth) { if (ln == "annotation") ++annoDepth; return; }
      if (ln == "annotation") { ++annoDepth; return; }
      if (ln == "table" && !inCell) {
        if (tableDepth++ == 0) {
          wb_.sheets.emplace_back();
          sh = &wb_.sheets.back();
          sh->name.clear();
          for (auto& a : at) if (xml::LocalName(a.first) == "name") sh->name = a.second;
          if (sh->name.empty()) sh->name = "Sheet" + std::to_string(wb_.sheets.size());
          pendingEmpty = 0;
        }
      } else if (ln == "table-row" && tableDepth == 1 && !inCell) {
        inRow = true; row.clear(); rowRep = 1; rowHasData = false;
        for (auto& a : at) if (xml::LocalName(a.first) == "number-rows-repeated") rowRep = std::max<size_t>(1, (size_t)atoll(a.second.c_str()));
      } else if ((ln == "table-cell" || ln == "covered-table-cell") && inRow && !inCell) {
        inCell = true; cell = SCell(); colRep = 1; valType.clear(); strValue.clear(); ptext.clear(); firstPara = true;
        std::string val, dateVal, timeVal, boolVal; bool hasFormula = false;
        for (auto& a : at) {
          std::string_view an = xml::LocalName(a.first);
          if (an == "number-columns-repeated") colRep = std::max<size_t>(1, (size_t)atoll(a.second.c_str()));
          else if (an == "value-type") valType = a.second;
          else if (an == "value") val = a.second;
          else if (an == "date-value") dateVal = a.second;
          else if (an == "time-value") timeVal = a.second;
          else if (an == "boolean-value") boolVal = a.second;
          else if (an == "string-value") strValue = a.second;
          else if (an == "formula") hasFormula = true;
        }
        if (hasFormula) formulas_ = true;
        if (valType == "float" || valType == "percentage" || valType == "currency") { if (!val.empty()) { cell.t = SCell::T::Number; cell.n = atof(val.c_str()); } }
        else if (valType == "date") {
          SCell d = InferCell(dateVal, false, false);
          if (d.t == SCell::T::Date || d.t == SCell::T::DateTime) cell = d;
          else { cell.t = SCell::T::Text; cell.s = dateVal; }
        }
        else if (valType == "time") { double dd; if (ParseDuration(timeVal, dd)) { cell.t = SCell::T::Time; cell.n = dd; } }
        else if (valType == "boolean") { cell.t = SCell::T::Bool; cell.n = boolVal == "true"; }
      } else if (ln == "p" && inCell) { inP = true; if (!firstPara) ptext.push_back('\n'); firstPara = false; }
      else if (ln == "s" && inP) { size_t c = 1; for (auto& a : at) if (xml::LocalName(a.first) == "c") c = (size_t)std::min<long long>(atoll(a.second.c_str()), 10000); ptext.append(c, ' '); }
      else if (ln == "tab" && inP) ptext.push_back('\t');
      else if (ln == "line-break" && inP) ptext.push_back('\n');
      if (inCell && ln == "table") ++cellDepth;
    };
    sax.end = [&](std::string_view n) {
      std::string_view ln = xml::LocalName(n);
      if (annoDepth) { if (ln == "annotation") --annoDepth; return; }
      if (ln == "p") inP = false;
      else if ((ln == "table-cell" || ln == "covered-table-cell") && inCell) {
        inCell = false;
        if (cell.t == SCell::T::Empty && (valType == "string" || valType.empty())) {
          std::string s = !strValue.empty() ? strValue : ptext;
          if (!s.empty()) { cell.t = SCell::T::Text; cell.s = s; }
        }
        bool empty = cell.t == SCell::T::Empty;
        if (!empty) rowHasData = true;
        size_t rep = colRep;
        if (empty && rep > 16384) rep = 1;
        if (rep > 16384) rep = 16384;
        if (empty && row.size() + rep > 16384) { row.push_back(SCell()); return; }
        if (row.size() + rep > cellBudget) { tooBig = true; return; }
        cellBudget -= rep;
        for (size_t k = 0; k < rep; ++k) row.push_back(cell);
      } else if (ln == "table-row" && inRow && tableDepth == 1) {
        inRow = false;
        while (!row.empty() && row.back().t == SCell::T::Empty) row.pop_back();
        if (!sh) return;
        if (!rowHasData && row.empty()) { pendingEmpty += rowRep; return; }
        if (pendingEmpty) { size_t add = std::min(pendingEmpty, rowBudget); for (size_t k = 0; k < add && sh->rows.size() < 1048576; ++k) sh->rows.emplace_back(); pendingEmpty = 0; }
        size_t reps = std::min<size_t>(rowRep, 1048576 > sh->rows.size() ? 1048576 - sh->rows.size() : 0);
        if (rowRep > 1000 && row.size() * rowRep > cellBudget) { tooBig = true; return; }
        for (size_t k = 0; k < reps; ++k) { sh->rows.push_back(row); cellBudget -= std::min(cellBudget, row.size()); }
      } else if (ln == "table") {
        if (cellDepth > 0) --cellDepth;
        else if (tableDepth > 0) --tableDepth;
      }
    };
    sax.text = [&](const std::string& t) { if (inP && !annoDepth) ptext += t; };
    xml::Parse(std::string_view((const char*)b.data(), b.size()), sax);
    if (tooBig) { if (err) *err = L"The spreadsheet is too large to process safely."; return false; }
    if (wb_.sheets.empty()) { if (err) *err = L"The spreadsheet contains no sheets."; return false; }
    if (formulas_) wb_.Warn(L"Formulas were replaced by their last calculated values.");
    return true;
  }
 private:
  ZipReader& z_;
  Workbook& wb_;
  bool formulas_ = false;
};

} // namespace

bool ReadOds(const fs::path& file, Workbook& wb, std::wstring* err) {
  ZipReader z;
  if (!z.Open(file, err)) return false;
  OdsReader r(z, wb);
  return r.Run(err);
}

bool VerifyOds(const fs::path& file, std::wstring* why) {
  auto fail = [&](const std::wstring& m) { if (why) *why = m; return false; };
  ZipReader z;
  if (!z.Open(file, why)) return false;
  const auto& es = z.Entries();
  if (es.empty() || es[0].name != "mimetype") return fail(L"the first entry is not 'mimetype'");
  Bytes mt;
  if (!z.Read(0, mt) || std::string(mt.begin(), mt.end()) != kMime) return fail(L"'mimetype' has the wrong content");
  if (es[0].csize != es[0].size) return fail(L"'mimetype' is compressed");
  for (const char* part : {"META-INF/manifest.xml", "content.xml", "styles.xml"}) {
    auto s = z.ReadString(part);
    if (!s) return fail(Widen(part) + L" is missing");
    std::string wf;
    if (!xml::CheckWellFormed(*s, &wf)) return fail(Widen(part) + L" is not well-formed (" + Widen(wf) + L")");
  }
  Workbook wb; std::wstring e;
  OdsReader r(z, wb);
  if (!r.Run(&e)) return fail(L"the spreadsheet does not read back: " + e);
  return true;
}

} // namespace filo::doc
