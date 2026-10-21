#include "engines/doc/pptx.h"
#include "engines/doc/docx.h"
#include "engines/doc/opc.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace filo::doc {

namespace {

const char* kNs =
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";

int64_t Emu(double pt) { return (int64_t)std::llround(pt * 12700.0); }

std::string Xfrm(double x, double y, double w, double h, double rot = 0) {
  std::string s = "<a:xfrm";
  if (rot != 0) s += " rot=\"" + std::to_string((int64_t)std::llround(rot * 60000.0)) + "\"";
  s += "><a:off x=\"" + std::to_string(Emu(x)) + "\" y=\"" + std::to_string(Emu(y)) + "\"/><a:ext cx=\"" + std::to_string(std::max<int64_t>(1, Emu(w))) + "\" cy=\"" + std::to_string(std::max<int64_t>(1, Emu(h))) + "\"/></a:xfrm>";
  return s;
}

std::string Theme() {
  std::string ln = "<a:ln w=\"%d\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:prstDash val=\"solid\"/><a:miter lim=\"800000\"/></a:ln>";
  auto lnW = [&](int w) { return Printf(ln.c_str(), w); };
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Filo\"><a:themeElements>"
    "<a:clrScheme name=\"Filo\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>"
    "<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1><a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2><a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
    "<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6><a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme>"
    "<a:fontScheme name=\"Filo\"><a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"Arial\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"Arial\"/></a:minorFont></a:fontScheme>"
    "<a:fmtScheme name=\"Filo\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst>"
    "<a:lnStyleLst>") + lnW(6350) + lnW(12700) + lnW(19050) +
    "</a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>"
    "<a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme>"
    "</a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>";
}

std::string GroupHeader() {
  return "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
}

std::string TitlePlaceholder(const Deck& d, int id) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"Title Placeholder 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
         "<p:spPr>" + Xfrm(d.widthPt * 0.05, d.heightPt * 0.05, d.widthPt * 0.9, d.heightPt * 0.15) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
         "<p:txBody><a:bodyPr vert=\"horz\" lIns=\"91440\" tIns=\"45720\" rIns=\"91440\" bIns=\"45720\" rtlCol=\"0\" anchor=\"ctr\"><a:normAutofit/></a:bodyPr><a:lstStyle/>"
         "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Click to edit Master title style</a:t></a:r><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>";
}

std::string MasterXml(const Deck& d) {
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sldMaster ") + kNs + "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg><p:spTree>" + GroupHeader() +
         TitlePlaceholder(d, 2) + "</p:spTree></p:cSld>"
         "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
         "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
         "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"l\" defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\"><a:lnSpc><a:spcPct val=\"90000\"/></a:lnSpc><a:spcBef><a:spcPct val=\"0\"/></a:spcBef><a:buNone/>"
         "<a:defRPr sz=\"4400\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mj-lt\"/><a:ea typeface=\"+mj-ea\"/><a:cs typeface=\"+mj-cs\"/></a:defRPr></a:lvl1pPr></p:titleStyle>"
         "<p:bodyStyle><a:lvl1pPr marL=\"228600\" indent=\"-228600\" algn=\"l\" defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\"><a:lnSpc><a:spcPct val=\"90000\"/></a:lnSpc><a:spcBef><a:spcPts val=\"1000\"/></a:spcBef>"
         "<a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/><a:defRPr sz=\"2800\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/><a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/></a:defRPr></a:lvl1pPr></p:bodyStyle>"
         "<p:otherStyle><a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr><a:lvl1pPr marL=\"0\" algn=\"l\" defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\"><a:defRPr sz=\"1800\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill>"
         "<a:latin typeface=\"+mn-lt\"/><a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/></a:defRPr></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>";
}

std::string LayoutXml(const Deck& d) {
  (void)d;
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sldLayout ") + kNs + " type=\"titleOnly\" preserve=\"1\"><p:cSld name=\"Title Only\"><p:spTree>" + GroupHeader() +
         "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr><p:spPr/>"
         "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Click to edit Master title style</a:t></a:r><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
         "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
}

std::string RunXml(const SlideRun& r, bool title) {
  std::string rpr = "<a:rPr lang=\"" + std::string(r.rtl || ContainsRtl(r.text) ? "ar-SA" : "en-US") + "\" sz=\"" + std::to_string(std::clamp<int>((int)std::lround(r.sizePt * 100), 100, 400000)) + "\"";
  if (r.bold) rpr += " b=\"1\"";
  if (r.italic) rpr += " i=\"1\"";
  if (r.underline) rpr += " u=\"sng\"";
  if (r.hidden) rpr += " dirty=\"0\"><a:solidFill><a:srgbClr val=\"000000\"><a:alpha val=\"0\"/></a:srgbClr></a:solidFill>";
  else rpr += " dirty=\"0\"><a:solidFill><a:srgbClr val=\"" + Printf("%06X", r.rgb & 0xFFFFFF) + "\"/></a:solidFill>";
  if (!r.font.empty()) { std::string f = XmlEscape(r.font, true); rpr += "<a:latin typeface=\"" + f + "\"/><a:cs typeface=\"" + f + "\"/>"; }
  rpr += "</a:rPr>";
  (void)title;
  std::string o;
  auto parts = SplitStr(r.text, '\n');
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) o += "<a:br>" + rpr + "</a:br>";
    if (!parts[i].empty()) o += "<a:r>" + rpr + "<a:t>" + XmlEscape(parts[i]) + "</a:t></a:r>";
  }
  return o;
}

std::string ParaXml(const SlidePara& p, bool title, double defaultSize) {
  std::string algn = p.align == 'c' ? "ctr" : p.align == 'r' ? "r" : p.align == 'j' ? "just" : "l";
  std::string o = "<a:p><a:pPr algn=\"" + algn + "\" rtl=\"" + (p.rtl ? "1" : "0") + "\"";
  if (p.bullet) o += " marL=\"228600\" indent=\"-228600\"><a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/></a:pPr>";
  else o += "><a:buNone/></a:pPr>";
  for (auto& r : p.runs) o += RunXml(r, title);
  double sz = p.runs.empty() ? defaultSize : p.runs.back().sizePt;
  o += "<a:endParaRPr lang=\"en-US\" sz=\"" + std::to_string((int)std::lround(sz * 100)) + "\" dirty=\"0\"/></a:p>";
  return o;
}

} // namespace

bool WritePptx(const Deck& deckIn, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings) {
  auto warn = [&](const std::wstring& w) { if (warnings) { for (auto& x : *warnings) if (x == w) return; warnings->push_back(w); } };
  Deck d = deckIn;
  if (d.slides.empty()) { if (err) *err = L"There are no slides to write."; return false; }
  // PowerPoint's maximum slide size is 56 inches; larger pages are scaled uniformly
  double scale = 1.0;
  const double maxPt = 4032.0;
  if (d.widthPt > maxPt || d.heightPt > maxPt) { scale = maxPt / std::max(d.widthPt, d.heightPt); d.widthPt *= scale; d.heightPt *= scale; warn(L"The page size exceeds PowerPoint's maximum slide size and was scaled down."); }
  if (d.widthPt < 72) d.widthPt = 72;
  if (d.heightPt < 72) d.heightPt = 72;

  ZipWriter z;
  if (!z.Open(out, err)) return false;
  std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                   "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/><Default Extension=\"gif\" ContentType=\"image/gif\"/>"
                   "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
                   "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
                   "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
                   "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
                   "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
                   "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
  for (size_t i = 0; i < d.slides.size(); ++i) ct += "<Override PartName=\"/ppt/slides/slide" + std::to_string(i + 1) + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
  ct += "</Types>";
  z.Add("[Content_Types].xml", ct);
  z.Add("_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/></Relationships>");
  time_t t = time(nullptr); tm g{}; gmtime_s(&g, &t);
  std::string now = Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  z.Add("docProps/core.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">" + (d.title.empty() ? std::string() : "<dc:title>" + XmlEscape(d.title) + "</dc:title>") +
        "<dc:creator>Filo</dc:creator><dcterms:created xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:modified></cp:coreProperties>");
  z.Add("docProps/app.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Application>Filo</Application><Slides>" +
        std::to_string(d.slides.size()) + "</Slides></Properties>");

  std::string pres = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:presentation ") + kNs + " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>";
  std::string prels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>";
  for (size_t i = 0; i < d.slides.size(); ++i) {
    pres += "<p:sldId id=\"" + std::to_string(256 + i) + "\" r:id=\"rId" + std::to_string(i + 2) + "\"/>";
    prels += "<Relationship Id=\"rId" + std::to_string(i + 2) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide" + std::to_string(i + 1) + ".xml\"/>";
  }
  prels += "<Relationship Id=\"rId" + std::to_string(d.slides.size() + 2) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/></Relationships>";
  pres += "</p:sldIdLst><p:sldSz cx=\"" + std::to_string(Emu(d.widthPt)) + "\" cy=\"" + std::to_string(Emu(d.heightPt)) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>";
  z.Add("ppt/presentation.xml", pres);
  z.Add("ppt/_rels/presentation.xml.rels", prels);
  z.Add("ppt/theme/theme1.xml", Theme());
  z.Add("ppt/slideMasters/slideMaster1.xml", MasterXml(d));
  z.Add("ppt/slideMasters/_rels/slideMaster1.xml.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/></Relationships>");
  z.Add("ppt/slideLayouts/slideLayout1.xml", LayoutXml(d));
  z.Add("ppt/slideLayouts/_rels/slideLayout1.xml.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>");

  int mediaCount = 0;
  for (size_t si = 0; si < d.slides.size(); ++si) {
    const Slide& s = d.slides[si];
    std::string x = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<p:sld ") + kNs + "><p:cSld><p:spTree>" + GroupHeader();
    std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                       "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>";
    int shapeId = 2, rid = 2;
    bool hasTitle = false;
    for (auto& sh : s.shapes) {
      double X = sh.x * scale, Y = sh.y * scale, W = sh.w * scale, H = sh.h * scale;
      if (sh.kind == SlideShape::Kind::Image) {
        if (!sh.image || sh.image->bytes.empty()) continue;
        Bytes data = sh.image->bytes; ImageInfo info; std::string ext;
        bool ok = ProbeImage(data.data(), data.size(), info);
        if (ok && info.mime == "image/png") ext = "png"; else if (ok && info.mime == "image/jpeg") ext = "jpeg"; else if (ok && info.mime == "image/gif") ext = "gif";
        else { Bytes png; if (ConvertImageToPng(data, png)) { data = std::move(png); ext = "png"; } else { warn(L"An image in an unsupported format was left out."); continue; } }
        std::string name = "image" + std::to_string(++mediaCount) + "." + ext;
        z.Add("ppt/media/" + name, data, false);
        rels += "<Relationship Id=\"rId" + std::to_string(rid) + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/" + name + "\"/>";
        x += "<p:pic><p:nvPicPr><p:cNvPr id=\"" + std::to_string(shapeId) + "\" name=\"Picture " + std::to_string(shapeId) + "\" descr=\"" + XmlEscape(sh.image->alt, true) + "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
             "<p:blipFill><a:blip r:embed=\"rId" + std::to_string(rid) + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + Xfrm(X, Y, W, H) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
        ++rid; ++shapeId;
        continue;
      }
      bool title = sh.kind == SlideShape::Kind::Title && !hasTitle;
      if (title) hasTitle = true;
      std::string body;
      for (auto& p : sh.paras) body += ParaXml(p, title, 18);
      if (body.empty()) body = "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>";
      std::string name = title ? "Title " + std::to_string(shapeId) : "TextBox " + std::to_string(shapeId);
      x += "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(shapeId) + "\" name=\"" + name + "\"/>";
      x += title ? "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>" : "<p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>";
      x += "<p:spPr>" + Xfrm(X, Y, W, H, sh.rotationDeg) + "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr>";
      x += std::string("<p:txBody><a:bodyPr wrap=\"") + (sh.wrap ? "square" : "none") + "\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" rtlCol=\"0\" anchor=\"t\"><a:noAutofit/></a:bodyPr><a:lstStyle/>" + body + "</p:txBody></p:sp>";
      ++shapeId;
    }
    x += "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    rels += "</Relationships>";
    z.Add("ppt/slides/slide" + std::to_string(si + 1) + ".xml", x);
    z.Add("ppt/slides/_rels/slide" + std::to_string(si + 1) + ".xml.rels", rels);
  }
  if (!z.Close()) { if (err) *err = L"The presentation file could not be written."; return false; }
  return true;
}

// ---- reader (info / verification) ----------------------------------------------------------------------------------------------
bool ReadPptxInfo(const fs::path& file, PptxInfo& info, std::wstring* err) {
  ZipReader z;
  if (!z.Open(file, err)) return false;
  RelMap root = ReadRels(z, "_rels/.rels");
  std::string pp = ResolvePartPath("", FindRelTarget(root, "/officeDocument"));
  if (pp.empty()) pp = "ppt/presentation.xml";
  auto ps = z.ReadString(pp);
  auto pdom = ps ? xml::ParseDom(*ps) : nullptr;
  if (!pdom) { if (err) *err = L"The presentation definition is missing or damaged."; return false; }
  RelMap rels = ReadRels(z, RelsPathFor(pp));
  if (auto* sz = pdom->Child("sldSz")) { info.widthPt = atof(sz->AttrOr("cx").c_str()) / 12700.0; info.heightPt = atof(sz->AttrOr("cy").c_str()) / 12700.0; }
  if (auto* lst = pdom->Child("sldIdLst")) for (auto* sid : lst->Children("sldId")) {
    // <p:sldId id="256" r:id="rId2">: the relationship id is the NAMESPACED attribute (the plain `id` is the numeric slide id)
    std::string rid;
    for (auto& a : sid->attrs) if (a.first.size() > 3 && a.first.compare(a.first.size() - 3, 3, ":id") == 0) rid = a.second;
    auto it = rels.find(rid);
    if (it == rels.end()) continue;
    std::string sp = ResolvePartPath(pp, it->second.target);
    auto ss = z.ReadString(sp);
    auto sdom = ss ? xml::ParseDom(*ss) : nullptr;
    if (!sdom) { if (err) *err = L"A slide is missing or damaged."; return false; }
    std::string text; int pics = 0;
    std::function<void(const xml::Node&)> walk = [&](const xml::Node& n) {
      for (auto& k : n.kids) {
        if (k->isText) continue;
        if (k->Is("pic")) ++pics;
        if (k->Is("p") && k->name.find("a:") == 0) { text += k->InnerText() + "\n"; continue; }
        walk(*k);
      }
    };
    walk(*sdom);
    info.slideText.push_back(text);
    info.imagesPerSlide.push_back(pics);
    ++info.slides;
  }
  return info.slides > 0;
}

bool VerifyPptx(const fs::path& file, std::wstring* why) {
  ZipReader z;
  if (!z.Open(file, why)) return false;
  if (!VerifyOpc(z, "ppt/presentation.xml", why)) return false;
  PptxInfo info; std::wstring e;
  if (!ReadPptxInfo(file, info, &e)) { if (why) *why = L"the presentation does not read back: " + e; return false; }
  return true;
}

} // namespace filo::doc
