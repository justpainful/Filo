#include "engines/doc/sheet.h"
#include "engines/doc/opc.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <unordered_map>

namespace filo::doc {

namespace {

constexpr size_t kMaxRows = 1048576;
constexpr size_t kMaxCols = 16384;
constexpr size_t kMaxCellChars = 32767;

std::string ColName(size_t col) {            // 0-based
  std::string s;
  for (size_t c = col + 1; c > 0; c = (c - 1) / 26) s.insert(s.begin(), (char)('A' + (c - 1) % 26));
  return s;
}
bool ParseRef(const std::string& ref, size_t& row, size_t& col) {   // "B3" -> row 2, col 1 (0-based)
  size_t i = 0; size_t c = 0;
  while (i < ref.size() && isalpha((unsigned char)ref[i])) { c = c * 26 + (size_t)(toupper(ref[i]) - 'A' + 1); if (c > 100000) return false; ++i; }
  if (i == 0 || i >= ref.size()) return false;
  size_t r = 0;
  while (i < ref.size() && isdigit((unsigned char)ref[i])) { r = r * 10 + (size_t)(ref[i] - '0'); if (r > 100000000) return false; ++i; }
  if (r == 0 || c == 0) return false;
  row = r - 1; col = c - 1;
  return true;
}
std::string IsoNow() {
  time_t t = time(nullptr); tm g{}; gmtime_s(&g, &t);
  return Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
}

bool LooksRtl(const Sheet& s) {
  size_t rtl = 0, ltr = 0;
  for (size_t r = 0; r < s.rows.size() && r < 50; ++r)
    for (auto& c : s.rows[r]) if (c.t == SCell::T::Text) { if (DetectRtl(c.s)) ++rtl; else if (!c.s.empty()) ++ltr; }
  return rtl > 0 && rtl > ltr;
}

} // namespace

// ============================================================================================================================
bool WriteXlsx(const Workbook& wbIn, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings) {
  auto warn = [&](const std::wstring& w) { if (warnings) { for (auto& x : *warnings) if (x == w) return; warnings->push_back(w); } };
  // prepare: split over-long sheets, unique valid names
  struct Part { std::string name; const Sheet* sheet; size_t from, to; };
  std::vector<Part> parts;
  std::vector<std::string> used;
  for (auto& sh : wbIn.sheets) {
    size_t total = sh.rows.size();
    size_t chunks = std::max<size_t>(1, (total + kMaxRows - 1) / kMaxRows);
    for (size_t k = 0; k < chunks; ++k) {
      std::string nm = SafeSheetName(k == 0 ? sh.name : sh.name + " (" + std::to_string(k + 1) + ")", used);
      used.push_back(nm);
      parts.push_back({nm, &sh, k * kMaxRows, std::min(total, (k + 1) * kMaxRows)});
      if (k == 1) warn(L"A sheet has more rows than Excel allows (1,048,576); the remaining rows were moved to extra sheets.");
    }
  }
  if (parts.empty()) { static const Sheet empty; parts.push_back({"Sheet1", &empty, 0, 0}); }

  std::vector<std::string> sst;
  std::unordered_map<std::string, size_t> sstIndex;
  size_t sstRefs = 0;
  bool truncated = false, colsDropped = false;

  ZipWriter z;
  if (!z.Open(out, err)) return false;

  std::vector<std::string> sheetXml;
  sheetXml.reserve(parts.size());
  for (size_t pi = 0; pi < parts.size(); ++pi) {
    const Part& p = parts[pi];
    const Sheet& sh = *p.sheet;
    size_t maxc = 0;
    for (size_t r = p.from; r < p.to; ++r) maxc = std::max(maxc, sh.rows[r].size());
    if (maxc > kMaxCols) { maxc = kMaxCols; colsDropped = true; }
    std::vector<size_t> widths(maxc, 0);
    std::string x;
    x.reserve((p.to - p.from) * (maxc + 1) * 24 + 1024);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    size_t nrows = p.to - p.from;
    x += "<dimension ref=\"A1" + (nrows && maxc ? ":" + ColName(maxc - 1) + std::to_string(nrows) : std::string()) + "\"/>";
    x += std::string("<sheetViews><sheetView workbookViewId=\"0\"") + (pi == 0 ? " tabSelected=\"1\"" : "") + (LooksRtl(sh) ? " rightToLeft=\"1\"" : "") + "/></sheetViews>";
    x += "<sheetFormatPr defaultRowHeight=\"15\"/>";
    // column widths from the first rows
    size_t sample = std::min<size_t>(nrows, 200);
    for (size_t r = 0; r < sample; ++r) {
      const auto& row = sh.rows[p.from + r];
      for (size_t c = 0; c < row.size() && c < maxc; ++c) {
        size_t len = row[c].t == SCell::T::Text ? Utf8Length(row[c].s) : (row[c].t == SCell::T::Empty ? 0 : (row[c].t == SCell::T::DateTime ? 19 : 11));
        if (row[c].t == SCell::T::Text) { size_t nl = row[c].s.find('\n'); if (nl != std::string::npos) len = Utf8Length(row[c].s.substr(0, nl)); }
        widths[c] = std::max(widths[c], len);
      }
    }
    if (maxc) {
      x += "<cols>";
      for (size_t c = 0; c < maxc; ++c) {
        double w = std::clamp<double>((double)widths[c] + 2.0, 8.43, 60.0);
        x += Printf("<col min=\"%zu\" max=\"%zu\" width=\"%.2f\" customWidth=\"1\"/>", c + 1, c + 1, w);
      }
      x += "</cols>";
    }
    x += "<sheetData>";
    for (size_t r = 0; r < nrows; ++r) {
      const auto& row = sh.rows[p.from + r];
      size_t lastCell = std::min(row.size(), maxc);
      while (lastCell > 0 && row[lastCell - 1].t == SCell::T::Empty) --lastCell;
      if (lastCell == 0) continue;
      x += "<row r=\"" + std::to_string(r + 1) + "\">";
      for (size_t c = 0; c < lastCell; ++c) {
        const SCell& cell = row[c];
        if (cell.t == SCell::T::Empty) continue;
        std::string ref = ColName(c) + std::to_string(r + 1);
        switch (cell.t) {
          case SCell::T::Text: {
            std::string s = cell.s;
            if (Utf8Length(s) > kMaxCellChars) { size_t cut = 0, cnt = 0; while (cut < s.size() && cnt < kMaxCellChars) { DecodeUtf8(s, cut); ++cnt; } s.resize(cut); truncated = true; }
            s = SanitizeText(s);
            auto it = sstIndex.find(s);
            size_t idx;
            if (it == sstIndex.end()) { idx = sst.size(); sstIndex.emplace(s, idx); sst.push_back(s); } else idx = it->second;
            ++sstRefs;
            x += "<c r=\"" + ref + "\" t=\"s\"" + (s.find('\n') != std::string::npos ? " s=\"4\"" : "") + "><v>" + std::to_string(idx) + "</v></c>";
            break;
          }
          case SCell::T::Number: x += "<c r=\"" + ref + "\"><v>" + FormatNumber(cell.n) + "</v></c>"; break;
          case SCell::T::Bool: x += "<c r=\"" + ref + "\" t=\"b\"><v>" + (cell.n != 0 ? "1" : "0") + "</v></c>"; break;
          case SCell::T::Date: x += "<c r=\"" + ref + "\" s=\"1\"><v>" + FormatNumber(cell.n) + "</v></c>"; break;
          case SCell::T::DateTime: x += "<c r=\"" + ref + "\" s=\"2\"><v>" + Printf("%.10f", cell.n) + "</v></c>"; break;
          case SCell::T::Time: x += "<c r=\"" + ref + "\" s=\"3\"><v>" + Printf("%.10f", cell.n) + "</v></c>"; break;
          default: break;
        }
      }
      x += "</row>";
    }
    x += "</sheetData><pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/></worksheet>";
    sheetXml.push_back(std::move(x));
  }
  if (truncated) warn(L"Some cells held more than 32,767 characters (the Excel limit) and were shortened.");
  if (colsDropped) warn(L"Columns beyond XFD (16,384) were dropped.");

  // ---- package parts ----
  std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                   "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";
  for (size_t i = 0; i < parts.size(); ++i) ct += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
  ct += "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/></Types>";
  z.Add("[Content_Types].xml", ct);
  z.Add("_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/></Relationships>");
  std::string wbx = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                    "<workbookPr/><bookViews><workbookView xWindow=\"0\" yWindow=\"0\" windowWidth=\"24000\" windowHeight=\"12000\"/></bookViews><sheets>";
  std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  for (size_t i = 0; i < parts.size(); ++i) {
    wbx += "<sheet name=\"" + XmlEscape(parts[i].name, true) + "\" sheetId=\"" + std::to_string(i + 1) + "\" r:id=\"rId" + std::to_string(i + 1) + "\"/>";
    rels += "<Relationship Id=\"rId" + std::to_string(i + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + std::to_string(i + 1) + ".xml\"/>";
  }
  wbx += "</sheets></workbook>";
  rels += "<Relationship Id=\"rId" + std::to_string(parts.size() + 1) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
          "<Relationship Id=\"rId" + std::to_string(parts.size() + 2) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/></Relationships>";
  z.Add("xl/workbook.xml", wbx);
  z.Add("xl/_rels/workbook.xml.rels", rels);
  z.Add("xl/styles.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<numFmts count=\"3\"><numFmt numFmtId=\"164\" formatCode=\"yyyy\\-mm\\-dd\"/><numFmt numFmtId=\"165\" formatCode=\"yyyy\\-mm\\-dd\\ hh:mm:ss\"/><numFmt numFmtId=\"166\" formatCode=\"hh:mm:ss\"/></numFmts>"
        "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"5\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
        "<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
        "<xf numFmtId=\"166\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
        "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"top\" wrapText=\"1\"/></xf></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
  {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" + std::to_string(sstRefs) + "\" uniqueCount=\"" + std::to_string(sst.size()) + "\">";
    for (auto& t : sst) s += "<si><t xml:space=\"preserve\">" + XmlEscape(t) + "</t></si>";
    s += "</sst>";
    z.Add("xl/sharedStrings.xml", s);
  }
  for (size_t i = 0; i < sheetXml.size(); ++i) z.Add("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", sheetXml[i]);
  std::string now = IsoNow();
  z.Add("docProps/core.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:creator>Filo</dc:creator><dcterms:created xsi:type=\"dcterms:W3CDTF\">" + now +
        "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:modified></cp:coreProperties>");
  z.Add("docProps/app.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Application>Filo</Application></Properties>");
  if (!z.Close()) { if (err) *err = L"The spreadsheet file could not be written."; return false; }
  return true;
}

// ============================================================================================================================
namespace {

enum class FmtKind { Number, Date, DateTime, Time };

FmtKind ClassifyBuiltin(int id) {
  if ((id >= 14 && id <= 17) || id == 27 || id == 28 || id == 29 || id == 30 || id == 31 || id == 32 || id == 33 || id == 34 || id == 35 || id == 36 || (id >= 50 && id <= 58) || id == 22 + 100) return FmtKind::Date;
  if (id == 18 || id == 19 || id == 20 || id == 21 || id == 45 || id == 46 || id == 47) return FmtKind::Time;
  if (id == 22) return FmtKind::DateTime;
  return FmtKind::Number;
}

FmtKind ClassifyCode(const std::string& code) {
  bool y = false, d = false, h = false, s = false, mo = false;
  bool inQuote = false, inBracket = false;
  for (size_t i = 0; i < code.size(); ++i) {
    char c = code[i];
    if (c == '\\') { ++i; continue; }
    if (c == '"') { inQuote = !inQuote; continue; }
    if (inQuote) continue;
    if (c == '[') { inBracket = true; // [h] [mm] [ss] elapsed time
      size_t e = code.find(']', i);
      if (e != std::string::npos) { std::string in = LowerAscii(code.substr(i + 1, e - i - 1)); if (in == "h" || in == "hh") h = true; else if (in == "mm" || in == "m") mo = true; else if (in == "ss" || in == "s") s = true; i = e; }
      inBracket = false; continue;
    }
    (void)inBracket;
    char l = (char)tolower((unsigned char)c);
    if (l == 'y') y = true; else if (l == 'd') d = true; else if (l == 'h') h = true; else if (l == 's') s = true; else if (l == 'm') mo = true;
    else if (c == '_' || c == '*') ++i;
  }
  bool date = y || d || (mo && !h && !s);
  bool time = h || s;
  if (date && time) return FmtKind::DateTime;
  if (date) return FmtKind::Date;
  if (time) return FmtKind::Time;
  return FmtKind::Number;
}

struct SheetRef { std::string name, path; };

class XlsxReader {
 public:
  XlsxReader(ZipReader& z, Workbook& wb) : z_(z), wb_(wb) {}
  bool Run(std::wstring* err) {
    RelMap root = ReadRels(z_, "_rels/.rels");
    std::string wbPath = ResolvePartPath("", FindRelTarget(root, "/officeDocument"));
    if (wbPath.empty()) wbPath = "xl/workbook.xml";
    if (!z_.Has(wbPath)) { if (err) *err = L"This is not an Excel workbook (xl/workbook.xml is missing)."; return false; }
    RelMap rels = ReadRels(z_, RelsPathFor(wbPath));
    auto wbStr = z_.ReadString(wbPath);
    auto wbDom = wbStr ? xml::ParseDom(*wbStr) : nullptr;
    if (!wbDom) { if (err) *err = L"The workbook definition is damaged."; return false; }
    if (auto* pr = wbDom->Child("workbookPr")) { std::string d = pr->AttrOr("date1904"); wb_.date1904 = d == "1" || LowerAscii(d) == "true"; }
    std::vector<SheetRef> sheets;
    if (auto* ss = wbDom->Child("sheets")) for (auto* sh : ss->Children("sheet")) {
      std::string rid = sh->AttrOr("id");
      auto it = rels.find(rid);
      if (it == rels.end()) continue;
      sheets.push_back({sh->AttrOr("name"), ResolvePartPath(wbPath, it->second.target)});
    }
    if (sheets.empty()) { if (err) *err = L"The workbook contains no sheets."; return false; }
    std::string sstPath = FindRelTarget(rels, "/sharedStrings");
    if (!sstPath.empty()) LoadSst(ResolvePartPath(wbPath, sstPath));
    std::string stPath = FindRelTarget(rels, "/styles");
    if (!stPath.empty()) LoadStyles(ResolvePartPath(wbPath, stPath));
    for (auto& s : sheets) {
      Sheet sh; sh.name = s.name;
      if (!LoadSheet(s.path, sh, err)) return false;
      wb_.sheets.push_back(std::move(sh));
    }
    if (formulas_) wb_.Warn(L"Formulas were replaced by their last calculated values.");
    if (uncached_) wb_.Warn(L"Some formulas had no stored result and were left empty.");
    return true;
  }

 private:
  ZipReader& z_;
  Workbook& wb_;
  std::vector<std::string> sst_;
  std::vector<FmtKind> xfKind_;
  bool formulas_ = false, uncached_ = false;
  size_t cellBudget_ = 24000000;

  void LoadSst(const std::string& path) {
    Bytes b;
    if (!z_.Has(path) || !z_.Read(path, b)) return;
    xml::Sax sax;
    std::string cur; bool inSi = false; int phon = 0; bool inT = false;
    sax.start = [&](std::string_view n, const xml::AttrList&) {
      n = xml::LocalName(n);
      if (n == "si") { inSi = true; cur.clear(); }
      else if (n == "rPh") ++phon;
      else if (n == "t" && inSi && !phon) inT = true;
    };
    sax.end = [&](std::string_view n) {
      n = xml::LocalName(n);
      if (n == "si") { sst_.push_back(cur); inSi = false; }
      else if (n == "rPh") --phon;
      else if (n == "t") inT = false;
    };
    sax.text = [&](const std::string& t) { if (inT && inSi) cur += t; };
    xml::Parse(std::string_view((const char*)b.data(), b.size()), sax);
  }

  void LoadStyles(const std::string& path) {
    auto s = z_.ReadString(path);
    if (!s) return;
    auto dom = xml::ParseDom(*s);
    if (!dom) return;
    std::map<int, std::string> codes;
    if (auto* nf = dom->Child("numFmts")) for (auto* f : nf->Children("numFmt")) codes[atoi(f->AttrOr("numFmtId").c_str())] = f->AttrOr("formatCode");
    if (auto* xfs = dom->Child("cellXfs")) for (auto* xf : xfs->Children("xf")) {
      int id = atoi(xf->AttrOr("numFmtId", "0").c_str());
      auto it = codes.find(id);
      xfKind_.push_back(it != codes.end() ? ClassifyCode(it->second) : ClassifyBuiltin(id));
    }
  }

  bool LoadSheet(const std::string& path, Sheet& sh, std::wstring* err) {
    Bytes b;
    if (!z_.Has(path)) { if (err) *err = L"A worksheet listed in the workbook is missing."; return false; }
    if (!z_.Read(path, b, err)) return false;
    xml::Sax sax;
    size_t rowIdx = 0, nextRow = 0, colIdx = 0, nextCol = 0;
    bool inRow = false, inC = false, inV = false, inIs = false, inT = false, hasF = false, tooBig = false;
    int phon = 0;
    std::string t, vtext, istext; int style = 0;
    sax.start = [&](std::string_view n, const xml::AttrList& at) {
      n = xml::LocalName(n);
      if (n == "row") {
        inRow = true;
        rowIdx = nextRow;
        for (auto& a : at) if (a.first == "r") { size_t r = (size_t)atoll(a.second.c_str()); if (r >= 1 && r <= kMaxRows) rowIdx = r - 1; }
        nextRow = rowIdx + 1; nextCol = 0;
      } else if (n == "c" && inRow) {
        inC = true; hasF = false; vtext.clear(); istext.clear(); t.clear(); style = 0; colIdx = nextCol;
        for (auto& a : at) {
          if (a.first == "r") { size_t r, c; if (ParseRef(a.second, r, c) && c < kMaxCols) colIdx = c; }
          else if (a.first == "t") t = a.second;
          else if (a.first == "s") style = atoi(a.second.c_str());
        }
        nextCol = colIdx + 1;
      } else if (n == "v" && inC) { inV = true; vtext.clear(); }
      else if (n == "is" && inC) { inIs = true; istext.clear(); }
      else if (n == "rPh") ++phon;
      else if (n == "t" && inIs && !phon) inT = true;
      else if (n == "f" && inC) hasF = true;
    };
    sax.end = [&](std::string_view n) {
      n = xml::LocalName(n);
      if (n == "v") inV = false;
      else if (n == "is") inIs = false;
      else if (n == "t") inT = false;
      else if (n == "rPh") --phon;
      else if (n == "row") inRow = false;
      else if (n == "c" && inC) {
        inC = false;
        SCell cell;
        if (hasF) { formulas_ = true; }
        FmtKind fk = (size_t)style < xfKind_.size() ? xfKind_[style] : FmtKind::Number;
        if (t == "s") { size_t i = (size_t)atoll(vtext.c_str()); if (i < sst_.size() && !sst_[i].empty()) { cell.t = SCell::T::Text; cell.s = sst_[i]; } }
        else if (t == "inlineStr") { if (!istext.empty()) { cell.t = SCell::T::Text; cell.s = istext; } }
        else if (t == "str") { if (!vtext.empty()) { cell.t = SCell::T::Text; cell.s = vtext; } else if (hasF) uncached_ = true; }
        else if (t == "b") { cell.t = SCell::T::Bool; cell.n = TrimAscii(vtext) == "1"; }
        else if (t == "e") { cell.t = SCell::T::Text; cell.s = vtext; }
        else if (t == "d") { SCell d = InferCell(vtext, false, false); cell = d.t == SCell::T::Empty ? SCell() : d; if (cell.t == SCell::T::Text) cell.s = vtext; }
        else {
          std::string vv = TrimAscii(vtext);
          if (!vv.empty()) {
            char* endp = nullptr; double v = strtod(vv.c_str(), &endp);
            if (endp && *endp == 0) {
              cell.n = v;
              if (fk == FmtKind::Number) cell.t = SCell::T::Number;
              else if (fk == FmtKind::Time) cell.t = SCell::T::Time;
              else if (fk == FmtKind::DateTime) cell.t = SCell::T::DateTime;
              else cell.t = SCell::T::Date;
              if (wb_.date1904 && cell.t != SCell::T::Number && cell.t != SCell::T::Time) cell.n += 1462.0;     // normalise to the 1900 system
              if ((cell.t == SCell::T::DateTime) && cell.n == std::floor(cell.n)) cell.t = SCell::T::Date;
              if (cell.t == SCell::T::Date && cell.n != std::floor(cell.n)) cell.t = SCell::T::DateTime;
            } else { cell.t = SCell::T::Text; cell.s = vv; }
          } else if (hasF) uncached_ = true;
        }
        if (cell.t == SCell::T::Empty) return;
        if (rowIdx >= kMaxRows) return;
        if (sh.rows.size() <= rowIdx) sh.rows.resize(rowIdx + 1);
        auto& row = sh.rows[rowIdx];
        if (row.size() <= colIdx) {
          size_t add = colIdx + 1 - row.size();
          if (add > cellBudget_) { tooBig = true; return; }
          cellBudget_ -= add;
          row.resize(colIdx + 1);
        }
        row[colIdx] = std::move(cell);
      }
    };
    sax.text = [&](const std::string& s) { if (inV) vtext += s; else if (inT) istext += s; };
    xml::Parse(std::string_view((const char*)b.data(), b.size()), sax);
    if (tooBig) { if (err) *err = L"The spreadsheet is too large to process safely."; return false; }
    return true;
  }
};

} // namespace

bool ReadXlsx(const fs::path& file, Workbook& wb, std::wstring* err) {
  ZipReader z;
  if (!z.Open(file, err)) return false;
  XlsxReader r(z, wb);
  return r.Run(err);
}

bool VerifyXlsx(const fs::path& file, std::wstring* why) {
  ZipReader z;
  if (!z.Open(file, why)) return false;
  if (!VerifyOpc(z, "xl/workbook.xml", why)) return false;
  Workbook wb; std::wstring e;
  XlsxReader r(z, wb);
  if (!r.Run(&e)) { if (why) *why = L"the workbook does not read back: " + e; return false; }
  return true;
}

} // namespace filo::doc
