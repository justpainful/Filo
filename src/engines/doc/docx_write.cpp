#include "engines/doc/docx.h"
#include "engines/doc/imgenc.h"
#include <algorithm>
#include <ctime>
#include <map>

namespace filo::doc {

namespace {

const char* kNsDecl =
    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";

std::string IsoNow() {
  time_t t = time(nullptr); tm g{}; gmtime_s(&g, &t);
  return Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
}

struct Rel { std::string id, type, target; bool external; };
struct Media { std::string name; Bytes bytes; };

struct ListInstance { bool topOrdered = false; int orderedNumId = 0; int altNumId = 0; };

class DocxBuilder {
 public:
  DocxBuilder(const Document& d, const DocxWriteOptions& o, std::vector<std::wstring>* w) : doc_(d), opt_(o), warnings_(w) {
    rels_.push_back({"rId1", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles", "styles.xml", false});
    rels_.push_back({"rId2", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering", "numbering.xml", false});
    rels_.push_back({"rId3", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings", "settings.xml", false});
    nextRid_ = 4;
    pageW_ = opt_.page == PageSize::A4 ? 11906 : 12240;
    pageH_ = opt_.page == PageSize::A4 ? 16838 : 15840;
    textWidthTwips_ = pageW_ - 2880;
  }

  bool Build(const fs::path& out, std::wstring* err) {
    BuildBody();
    ZipWriter z;
    if (!z.Open(out, err)) return false;
    z.Add("[Content_Types].xml", ContentTypes());
    z.Add("_rels/.rels", RootRels());
    z.Add("word/document.xml", DocumentXml());
    z.Add("word/_rels/document.xml.rels", DocRels());
    z.Add("word/styles.xml", StylesXml());
    z.Add("word/numbering.xml", NumberingXml());
    z.Add("word/settings.xml", SettingsXml());
    z.Add("docProps/core.xml", CoreXml());
    z.Add("docProps/app.xml", AppXml());
    for (auto& m : media_) z.Add("word/media/" + m.name, m.bytes, false);
    if (!z.Close()) { if (err) *err = L"The output file could not be written."; return false; }
    return true;
  }

 private:
  const Document& doc_;
  DocxWriteOptions opt_;
  std::vector<std::wstring>* warnings_;
  std::string body_;
  std::vector<Rel> rels_;
  std::vector<Media> media_;
  std::map<std::string, std::string> linkRid_;
  int nextRid_ = 4;
  int docPr_ = 1;
  int pageW_, pageH_, textWidthTwips_;
  std::vector<int> orderedStarts_;          // start numbers for numIds 2.. (index = numId - 2)
  bool usedPng_ = false, usedJpeg_ = false, usedGif_ = false, usedBmp_ = false;

  void Warn(const std::wstring& w) { if (warnings_) { for (auto& x : *warnings_) if (x == w) return; warnings_->push_back(w); } }

  // ---- runs ----
  std::string RunProps(const Run& r, bool segRtl, bool extraBold, bool inCell = false) {
    (void)inCell;
    std::string p;
    if (!r.link.empty()) p += "<w:rStyle w:val=\"Hyperlink\"/>";
    if (r.code) p += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/>";
    else if (!r.font.empty()) { std::string f = XmlEscape(r.font, true); p += "<w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\" w:cs=\"" + f + "\"/>"; }
    if (r.bold || extraBold) p += "<w:b/><w:bCs/>";
    if (r.italic) p += "<w:i/><w:iCs/>";
    if (r.strike) p += "<w:strike/>";
    if (!r.color.empty() && r.color.size() == 6) p += "<w:color w:val=\"" + UpperAscii(r.color) + "\"/>";
    if (r.sizePt > 0) { int hp = (int)(r.sizePt * 2 + 0.5); p += Printf("<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", hp, hp); }
    else if (r.code) p += "<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>";
    if (r.underline) p += "<w:u w:val=\"single\"/>";
    if (r.sup) p += "<w:vertAlign w:val=\"superscript\"/>";
    else if (r.sub) p += "<w:vertAlign w:val=\"subscript\"/>";
    if (segRtl) p += "<w:rtl/>";
    return p.empty() ? "" : "<w:rPr>" + p + "</w:rPr>";
  }

  void TextRuns(std::string& o, const std::string& text, const std::string& rpr) {
    // splits on tab / newline
    size_t i = 0;
    std::string cur;
    auto flushText = [&] { if (!cur.empty()) { o += "<w:r>" + rpr + "<w:t xml:space=\"preserve\">" + XmlEscape(cur) + "</w:t></w:r>"; cur.clear(); } };
    while (i < text.size()) {
      char c = text[i];
      if (c == '\t') { flushText(); o += "<w:r>" + rpr + "<w:tab/></w:r>"; }
      else if (c == '\n') { flushText(); o += "<w:r>" + rpr + "<w:br/></w:r>"; }
      else if (c == '\r') {}
      else cur.push_back(c);
      ++i;
    }
    flushText();
  }

  std::string LinkRid(const std::string& url) {
    auto it = linkRid_.find(url);
    if (it != linkRid_.end()) return it->second;
    std::string id = "rId" + std::to_string(nextRid_++);
    rels_.push_back({id, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink", url, true});
    linkRid_[url] = id;
    return id;
  }

  std::string RunsXml(const Runs& runs, bool extraBold = false) {
    std::string o;
    size_t i = 0;
    while (i < runs.size()) {
      const Run& first = runs[i];
      size_t j = i;
      while (j < runs.size() && runs[j].link == first.link) ++j;
      bool linked = !first.link.empty();
      if (linked) {
        if (first.link[0] == '#') o += "<w:hyperlink w:anchor=\"" + XmlEscape(first.link.substr(1), true) + "\" w:history=\"1\">";
        else o += "<w:hyperlink r:id=\"" + LinkRid(first.link) + "\" w:history=\"1\">";
      }
      for (size_t k = i; k < j; ++k) {
        const Run& r = runs[k];
        if (r.br) { o += "<w:r><w:br/></w:r>"; continue; }
        if (r.text.empty()) continue;
        if (r.rtl || ContainsRtl(r.text)) {
          for (auto& [seg, rtl] : SplitByDirection(r.text)) TextRuns(o, seg, RunProps(r, rtl || r.rtl, extraBold));
        } else TextRuns(o, r.text, RunProps(r, false, extraBold));
      }
      if (linked) o += "</w:hyperlink>";
      i = j;
    }
    return o;
  }

  std::string Jc(char align, bool rtl) {
    // In a bidi paragraph "left"/"right" are start/end relative: swap so that the model's visual alignment is kept.
    switch (align) {
      case 'c': return "<w:jc w:val=\"center\"/>";
      case 'j': return "<w:jc w:val=\"both\"/>";
      case 'l': return rtl ? "<w:jc w:val=\"right\"/>" : "<w:jc w:val=\"left\"/>";
      case 'r': return rtl ? "<w:jc w:val=\"left\"/>" : "<w:jc w:val=\"right\"/>";
    }
    return "";
  }

  std::string Para(const Runs& runs, const std::string& style, bool rtl, char align, const std::string& numPr = {}, const std::string& extraPPr = {}, bool extraBold = false) {
    std::string ppr;
    if (!style.empty()) ppr += "<w:pStyle w:val=\"" + style + "\"/>";
    ppr += numPr;
    if (rtl) ppr += "<w:bidi/>";
    ppr += extraPPr;
    ppr += Jc(align, rtl);
    std::string p = "<w:p>";
    if (!ppr.empty()) p += "<w:pPr>" + ppr + "</w:pPr>";
    p += RunsXml(runs, extraBold);
    p += "</w:p>";
    return p;
  }

  // ---- images ----
  std::string ImageXml(const Block& b) {
    if (!b.image || b.image->bytes.empty()) return {};
    Bytes data = b.image->bytes;
    ImageInfo info;
    std::string mime = b.image->mime;
    bool ok = ProbeImage(data.data(), data.size(), info);
    if (ok) mime = info.mime;
    int w = b.image->w, h = b.image->h;
    if (ok && (w <= 0 || h <= 0)) { w = info.w; h = info.h; }
    std::string ext;
    if (ok && (mime == "image/png")) ext = "png";
    else if (ok && mime == "image/jpeg") ext = "jpeg";
    else if (ok && mime == "image/gif") ext = "gif";
    else if (ok && mime == "image/bmp") ext = "bmp";
    else {
      Bytes png; int cw = 0, ch = 0;
      if (ConvertImageToPng(data, png, &cw, &ch)) { data = std::move(png); ext = "png"; w = cw; h = ch; }
      else { Warn(L"An image in a format Word cannot embed was left out."); return {}; }
    }
    if (ext == "png") usedPng_ = true; else if (ext == "jpeg") usedJpeg_ = true; else if (ext == "gif") usedGif_ = true; else usedBmp_ = true;
    if (w <= 0 || h <= 0) { w = 400; h = 300; }
    std::string name = "image" + std::to_string(media_.size() + 1) + "." + ext;
    media_.push_back({name, std::move(data)});
    std::string rid = "rId" + std::to_string(nextRid_++);
    rels_.push_back({rid, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image", "media/" + name, false});
    // 96 dpi => 9525 EMU per pixel; fit the text width, keep the aspect ratio
    int64_t cx = (int64_t)w * 9525, cy = (int64_t)h * 9525;
    if (b.image->widthPt > 0 && b.image->heightPt > 0) { cx = (int64_t)(b.image->widthPt * 12700.0); cy = (int64_t)(b.image->heightPt * 12700.0); }
    int64_t maxW = (int64_t)textWidthTwips_ * 635;
    if (cx > maxW) { cy = cy * maxW / cx; cx = maxW; }
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    int id = docPr_++;
    std::string alt = XmlEscape(b.image->alt, true);
    std::string run =
        "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
        "<wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/>"
        "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>"
        "<wp:docPr id=\"" + std::to_string(id) + "\" name=\"Picture " + std::to_string(id) + "\" descr=\"" + alt + "\"/>"
        "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
        "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
        "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name + "\" descr=\"" + alt + "\"/><pic:cNvPicPr/></pic:nvPicPr>"
        "<pic:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
        "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>";
    return "<w:p>" + run + "</w:p>";
  }

  // ---- tables ----
  std::string TableXml(const Block& b) {
    const Table& t = b.table;
    size_t cols = t.Cols();
    if (t.rows.empty() || cols == 0) return {};
    int colW = std::max(300, textWidthTwips_ / (int)cols);
    std::string o = "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/>";
    if (t.rtl) o += "<w:bidiVisual/>";
    o += "<w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"1\" w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>";
    for (size_t c = 0; c < cols; ++c) o += "<w:gridCol w:w=\"" + std::to_string(colW) + "\"/>";
    o += "</w:tblGrid>";
    for (size_t r = 0; r < t.rows.size(); ++r) {
      o += "<w:tr>";
      if (r == 0 && t.headerRow) o += "<w:trPr><w:tblHeader/></w:trPr>";
      size_t col = 0;
      for (auto& c : t.rows[r]) {
        int span = std::max(1, c.colspan);
        o += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(colW * span) + "\" w:type=\"dxa\"/>";
        if (span > 1) o += "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>";
        o += "</w:tcPr>";
        bool hdr = (r == 0 && t.headerRow) || c.header;
        char al = col < t.align.size() ? t.align[col] : 0;
        bool crtl = c.rtl || t.rtl;
        o += Para(c.runs, "", crtl, al, {}, "<w:spacing w:before=\"40\" w:after=\"40\"/>", hdr);
        o += "</w:tc>";
        col += (size_t)span;
      }
      for (; col < cols; ++col) o += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(colW) + "\" w:type=\"dxa\"/></w:tcPr><w:p/></w:tc>";
      o += "</w:tr>";
    }
    o += "</w:tbl><w:p><w:pPr><w:spacing w:after=\"0\"/></w:pPr></w:p>";
    return o;
  }

  // ---- body ----
  void BuildBody() {
    ListInstance inst;
    bool inList = false;
    for (auto& b : doc_.blocks) {
      bool isList = b.kind == Block::Kind::ListItem;
      if (!isList) inList = false;
      switch (b.kind) {
        case Block::Kind::Heading:
          body_ += Para(b.runs, "Heading" + std::to_string(std::clamp(b.level, 1, 6)), b.rtl, b.align);
          break;
        case Block::Kind::Paragraph:
          body_ += Para(b.runs, "", b.rtl, b.align);
          break;
        case Block::Kind::Quote: {
          std::string ind = Printf("<w:ind w:left=\"%d\"/>", 720 * (b.level + 1));
          body_ += Para(b.runs, "Quote", b.rtl, b.align, {}, b.level > 0 ? ind : "");
          break;
        }
        case Block::Kind::ListItem: {
          int lvl = std::clamp(b.level, 0, 8);
          if (!inList || (lvl == 0 && b.ordered != inst.topOrdered)) {
            inst = ListInstance{};
            inst.topOrdered = lvl == 0 ? b.ordered : false;
            if (lvl == 0 && b.ordered) { orderedStarts_.push_back(std::max(1, b.number)); inst.orderedNumId = (int)orderedStarts_.size() + 1; }
            inList = true;
          }
          int numId = 1;
          if (b.ordered) {
            if (lvl == 0 || inst.topOrdered) { if (!inst.orderedNumId) { orderedStarts_.push_back(std::max(1, b.number)); inst.orderedNumId = (int)orderedStarts_.size() + 1; } numId = inst.orderedNumId; }
            else { if (!inst.altNumId) { orderedStarts_.push_back(std::max(1, b.number)); inst.altNumId = (int)orderedStarts_.size() + 1; } numId = inst.altNumId; }
          }
          std::string numPr = Printf("<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", lvl, numId);
          Runs runs = b.runs;
          if (b.checked >= 0) { Run box; box.text = b.checked ? "\xE2\x98\x91 " : "\xE2\x98\x90 "; runs.insert(runs.begin(), box); }
          body_ += Para(runs, "ListParagraph", b.rtl, b.align, numPr);
          break;
        }
        case Block::Kind::Table: body_ += TableXml(b); break;
        case Block::Kind::Image: body_ += ImageXml(b); break;
        case Block::Kind::PageBreak: body_ += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"; break;
        case Block::Kind::Rule: body_ += "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"999999\"/></w:pBdr><w:spacing w:after=\"120\"/></w:pPr></w:p>"; break;
        case Block::Kind::Code: {
          std::string code = PlainText(b.runs);
          for (auto& line : SplitLines(code.empty() ? std::string(" ") : code)) {
            Run r; r.text = line.empty() ? " " : line; r.code = true;
            body_ += Para({r}, "CodeBlock", false, 0);
          }
          break;
        }
        case Block::Kind::Footnote: {
          Runs runs = b.runs;
          Run lab; lab.text = b.label; lab.sup = true;
          Run sp; sp.text = " ";
          runs.insert(runs.begin(), sp); runs.insert(runs.begin(), lab);
          body_ += Para(runs, "FootnoteText", b.rtl, b.align);
          break;
        }
      }
    }
    if (body_.empty()) body_ = "<w:p/>";
  }

  // ---- parts ----
  std::string ContentTypes() {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
    if (usedPng_) o += "<Default Extension=\"png\" ContentType=\"image/png\"/>";
    if (usedJpeg_) o += "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>";
    if (usedGif_) o += "<Default Extension=\"gif\" ContentType=\"image/gif\"/>";
    if (usedBmp_) o += "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>";
    o += "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
         "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
         "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
         "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
         "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
         "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/></Types>";
    return o;
  }
  std::string RootRels() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
           "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
           "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/></Relationships>";
  }
  std::string DocRels() {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (auto& r : rels_) {
      o += "<Relationship Id=\"" + r.id + "\" Type=\"" + r.type + "\" Target=\"" + XmlEscape(r.target, true) + "\"";
      if (r.external) o += " TargetMode=\"External\"";
      o += "/>";
    }
    return o + "</Relationships>";
  }
  std::string DocumentXml() {
    std::string sect = Printf("<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/><w:cols w:space=\"708\"/><w:docGrid w:linePitch=\"360\"/></w:sectPr>", pageW_, pageH_);
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document ") + kNsDecl + "><w:body>" + body_ + sect + "</w:body></w:document>";
  }
  std::string CoreXml() {
    std::string now = IsoNow();
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">";
    if (!doc_.meta.title.empty()) o += "<dc:title>" + XmlEscape(doc_.meta.title) + "</dc:title>";
    if (!doc_.meta.subject.empty()) o += "<dc:subject>" + XmlEscape(doc_.meta.subject) + "</dc:subject>";
    o += "<dc:creator>" + XmlEscape(doc_.meta.author.empty() ? opt_.creator : doc_.meta.author) + "</dc:creator>";
    if (!doc_.meta.language.empty()) o += "<dc:language>" + XmlEscape(doc_.meta.language) + "</dc:language>";
    o += "<dcterms:created xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:modified></cp:coreProperties>";
    return o;
  }
  std::string AppXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
           "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"><Application>Filo</Application><AppVersion>1.0000</AppVersion></Properties>";
  }
  std::string SettingsXml() {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings ") + kNsDecl +
           "><w:zoom w:percent=\"100\"/><w:defaultTabStop w:val=\"720\"/><w:characterSpacingControl w:val=\"doNotCompress\"/>"
           "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>";
  }

  static std::string HeadingStyle(int lvl) {
    static const int sizes[] = {36, 30, 26, 24, 22, 22};
    static const char* colors[] = {"1F3864", "2F5496", "2F5496", "1F3864", "1F3864", "404040"};
    std::string id = "Heading" + std::to_string(lvl);
    return "<w:style w:type=\"paragraph\" w:styleId=\"" + id + "\"><w:name w:val=\"heading " + std::to_string(lvl) + "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:qFormat/>"
           "<w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"" + std::to_string(lvl == 1 ? 360 : 240) + "\" w:after=\"120\"/><w:outlineLvl w:val=\"" + std::to_string(lvl - 1) + "\"/></w:pPr>"
           "<w:rPr><w:rFonts w:ascii=\"Calibri Light\" w:hAnsi=\"Calibri Light\" w:cs=\"Arial\"/><w:b/><w:bCs/><w:color w:val=\"" + colors[lvl - 1] + "\"/><w:sz w:val=\"" + std::to_string(sizes[lvl - 1]) + "\"/><w:szCs w:val=\"" + std::to_string(sizes[lvl - 1]) + "\"/></w:rPr></w:style>";
  }

  std::string StylesXml() {
    std::string o = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles ") + kNsDecl + ">"
      "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Calibri\" w:cs=\"Arial\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"en-US\" w:eastAsia=\"en-US\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault>"
      "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
      "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
      "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/><w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>"
      "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/><w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/><w:tblPr><w:tblInd w:w=\"0\" w:type=\"dxa\"/><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
      "<w:style w:type=\"numbering\" w:default=\"1\" w:styleId=\"NoList\"><w:name w:val=\"No List\"/><w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>";
    for (int i = 1; i <= 6; ++i) o += HeadingStyle(i);
    o += "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"10\"/><w:qFormat/><w:pPr><w:spacing w:after=\"120\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"48\"/><w:szCs w:val=\"48\"/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"34\"/><w:qFormat/><w:pPr><w:spacing w:after=\"60\"/><w:contextualSpacing/></w:pPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"29\"/><w:qFormat/><w:pPr><w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"BFBFBF\"/></w:pBdr><w:ind w:left=\"720\"/></w:pPr><w:rPr><w:i/><w:iCs/><w:color w:val=\"404040\"/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"CodeBlock\"><w:name w:val=\"Code Block\"/><w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"35\"/><w:qFormat/><w:pPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F2F2\"/><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/><w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/><w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"99\"/><w:pPr><w:spacing w:after=\"40\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:rPr><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/></w:rPr></w:style>"
         "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:basedOn w:val=\"DefaultParagraphFont\"/><w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/><w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
         "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:basedOn w:val=\"TableNormal\"/><w:uiPriority w:val=\"39\"/><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:tblPr><w:tblBorders>"
         "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
         "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/></w:tblBorders></w:tblPr></w:style>"
         "</w:styles>";
    return o;
  }

  std::string NumberingXml() {
    auto level = [](int i, bool bullet) {
      static const char* fmts[] = {"decimal", "lowerLetter", "lowerRoman"};
      std::string o = "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" + (bullet ? "bullet" : fmts[i % 3]) + "\"/>";
      if (bullet) o += std::string("<w:lvlText w:val=\"") + (i % 2 == 0 ? "\xE2\x80\xA2" : "\xE2\x80\x93") + "\"/>";
      else o += "<w:lvlText w:val=\"%" + std::to_string(i + 1) + ".\"/>";
      o += "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (i + 1)) + "\" w:hanging=\"360\"/></w:pPr>";
      if (bullet) o += "<w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:cs=\"Arial\" w:hint=\"default\"/></w:rPr>";
      o += "</w:lvl>";
      return o;
    };
    std::string o = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering ") + kNsDecl + ">";
    o += "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
    for (int i = 0; i < 9; ++i) o += level(i, true);
    o += "</w:abstractNum><w:abstractNum w:abstractNumId=\"1\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
    for (int i = 0; i < 9; ++i) o += level(i, false);
    o += "</w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
    for (size_t i = 0; i < orderedStarts_.size(); ++i)
      o += "<w:num w:numId=\"" + std::to_string(i + 2) + "\"><w:abstractNumId w:val=\"1\"/><w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"" + std::to_string(orderedStarts_[i]) + "\"/></w:lvlOverride></w:num>";
    return o + "</w:numbering>";
  }
};

} // namespace

bool WriteDocx(const Document& d, const fs::path& out, const DocxWriteOptions& opt, std::wstring* err, std::vector<std::wstring>* warnings) {
  DocxBuilder b(d, opt, warnings);
  return b.Build(out, err);
}

} // namespace filo::doc
