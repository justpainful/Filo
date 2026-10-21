#include "engines/doc/docx.h"
#include "engines/doc/opc.h"
#include <algorithm>
#include <functional>
#include <map>

namespace filo::doc {

namespace {

bool OnOff(const xml::Node* n, bool defVal = true) {
  if (!n) return false;
  auto* v = n->Attr("val");
  if (!v) return defVal;
  std::string l = LowerAscii(*v);
  return !(l == "0" || l == "false" || l == "off" || l == "none");
}

bool IsMono(const std::string& f) {
  std::string l = LowerAscii(f);
  return l.find("courier") != std::string::npos || l.find("consolas") != std::string::npos || l.find("mono") != std::string::npos || l.find("menlo") != std::string::npos || l.find("lucida console") != std::string::npos;
}

struct StyleInfo {
  std::string name, basedOn, type;
  int outline = -1;
  Run fmt;                        // run properties contributed by the style (b/i/u/color/size/mono)
  bool fmtBold = false, fmtItalic = false;
  int numId = 0, ilvl = 0;
  bool bidi = false;
  char align = 0;
};
struct NumLvl { std::string fmt = "decimal"; int start = 1; };
struct AbstractNum { std::map<int, NumLvl> lvls; };
struct NumInst { int abstractId = -1; std::map<int, int> startOverride; };

struct Seg { Runs runs; std::shared_ptr<ImageData> img; bool pageBreak = false; };

class Reader {
 public:
  Reader(ZipReader& z, Document& d) : z_(z), doc_(d) {}

  bool Parse(std::wstring* err) {
    std::string mainPart = "word/document.xml";
    {
      RelMap rr = ReadRels(z_, "_rels/.rels");
      std::string t = FindRelTarget(rr, "/officeDocument");
      if (!t.empty()) mainPart = ResolvePartPath("", t);
    }
    if (!z_.Has(mainPart)) { if (err) *err = L"This is not a Word document (the main document part is missing)."; return false; }
    mainPart_ = mainPart;
    rels_ = ReadRels(z_, RelsPathFor(mainPart));
    std::string stylesPath = ResolvePartPath(mainPart, FindRelTarget(rels_, "/styles"));
    std::string numPath = ResolvePartPath(mainPart, FindRelTarget(rels_, "/numbering"));
    std::string fnPath = ResolvePartPath(mainPart, FindRelTarget(rels_, "/footnotes"));
    if (!stylesPath.empty() && z_.Has(stylesPath)) LoadStyles(stylesPath);
    if (!numPath.empty() && z_.Has(numPath)) LoadNumbering(numPath);
    if (!fnPath.empty() && z_.Has(fnPath)) LoadFootnotes(fnPath);
    LoadCore();

    Bytes data; std::wstring e2;
    if (!z_.Read(mainPart, data, &e2)) { if (err) *err = e2; return false; }
    std::string perr;
    auto dom = xml::ParseDom(std::string_view((const char*)data.data(), data.size()), &perr);
    if (!dom || !dom->Is("document")) { if (err) *err = L"The Word document is damaged (document.xml cannot be read)."; return false; }
    data.clear(); data.shrink_to_fit();
    const xml::Node* body = dom->Child("body");
    if (!body) { if (err) *err = L"The Word document has no body."; return false; }
    Body(*body, doc_.blocks);
    // footnotes at the end
    for (size_t i = 0; i < usedFootnotes_.size(); ++i) {
      auto it = footnotes_.find(usedFootnotes_[i]);
      if (it == footnotes_.end()) continue;
      Block b; b.kind = Block::Kind::Footnote; b.label = std::to_string(i + 1); b.runs = it->second;
      doc_.blocks.push_back(std::move(b));
    }
    MergeCode();
    if (sawTextBox_) doc_.Warn(L"Text boxes in the document are not converted and were left out.");
    if (sawEquation_) doc_.Warn(L"Equations were converted to plain text.");
    if (droppedCellImages_) doc_.Warn(L"Images inside table cells were left out.");
    FinalizeDocument(doc_);
    return true;
  }

 private:
  ZipReader& z_;
  Document& doc_;
  std::string mainPart_;
  RelMap rels_;
  std::map<std::string, StyleInfo> styles_;
  std::map<int, AbstractNum> abs_;
  std::map<int, NumInst> nums_;
  std::map<std::pair<int, int>, int> counters_;
  std::map<std::string, Runs> footnotes_;
  std::vector<std::string> usedFootnotes_;
  bool sawTextBox_ = false, sawEquation_ = false, droppedCellImages_ = false;
  struct Fld { std::string instr; bool separated = false; std::string link; };
  std::vector<Fld> fields_;

  // ---------------------------------------------------------------- styles
  void ParseRPr(const xml::Node* rpr, ::filo::doc::Run& r, bool* bold = nullptr, bool* italic = nullptr) {
    if (!rpr) return;
    for (auto& k : rpr->kids) {
      if (k->isText) continue;
      std::string_view n = k->Local();
      if (n == "b") { r.bold = OnOff(k.get()); if (bold) *bold = r.bold; }
      else if (n == "i") { r.italic = OnOff(k.get()); if (italic) *italic = r.italic; }
      else if (n == "u") { auto* v = k->Attr("val"); r.underline = !v || LowerAscii(*v) != "none"; }
      else if (n == "strike" || n == "dstrike") r.strike = OnOff(k.get());
      else if (n == "color") { auto* v = k->Attr("val"); if (v && *v != "auto" && v->size() == 6) r.color = UpperAscii(*v); }
      else if (n == "sz") { auto* v = k->Attr("val"); if (v) r.sizePt = atoi(v->c_str()) / 2.0; }
      else if (n == "vertAlign") { auto* v = k->Attr("val"); if (v) { r.sup = *v == "superscript"; r.sub = *v == "subscript"; } }
      else if (n == "rtl") r.rtl = OnOff(k.get());
      else if (n == "rFonts") {
        for (const char* a : {"ascii", "hAnsi", "cs"}) { if (auto* v = k->Attr(a)) { r.font = *v; break; } }
        if (!r.font.empty() && IsMono(r.font)) r.code = true;
      }
      else if (n == "rStyle") { if (auto* v = k->Attr("val")) ApplyStyleFmt(*v, r, 0); }
    }
  }

  void LoadStyles(const std::string& path) {
    auto s = z_.ReadString(path);
    if (!s) return;
    auto dom = xml::ParseDom(*s);
    if (!dom) return;
    if (auto* dd = dom->Child("docDefaults")) {
      if (auto* rd = dd->Child("rPrDefault")) if (auto* rp = rd->Child("rPr")) if (auto* lang = rp->Child("lang")) { if (auto* v = lang->Attr("val")) doc_.meta.language = *v; }
    }
    for (auto* st : dom->Children("style")) {
      StyleInfo si;
      std::string id = st->AttrOr("styleId");
      si.type = st->AttrOr("type");
      if (auto* n = st->Child("name")) si.name = n->AttrOr("val");
      if (auto* b = st->Child("basedOn")) si.basedOn = b->AttrOr("val");
      if (auto* ppr = st->Child("pPr")) {
        if (auto* ol = ppr->Child("outlineLvl")) si.outline = atoi(ol->AttrOr("val").c_str());
        if (auto* np = ppr->Child("numPr")) {
          if (auto* ni = np->Child("numId")) si.numId = atoi(ni->AttrOr("val").c_str());
          if (auto* il = np->Child("ilvl")) si.ilvl = atoi(il->AttrOr("val").c_str());
        }
        si.bidi = ppr->Child("bidi") != nullptr && OnOff(ppr->Child("bidi"));
        if (auto* jc = ppr->Child("jc")) si.align = JcToAlign(jc->AttrOr("val"), si.bidi);
      }
      if (auto* rpr = st->Child("rPr")) {
        ParseRPr(rpr, si.fmt, &si.fmtBold, &si.fmtItalic);
        // keep only the style's own settings
      }
      if (!id.empty()) styles_[id] = std::move(si);
    }
  }

  void ApplyStyleFmt(const std::string& id, ::filo::doc::Run& r, int depth) {
    if (depth > 8) return;
    auto it = styles_.find(id);
    if (it == styles_.end()) return;
    if (LowerAscii(it->second.name) == "hyperlink" || LowerAscii(it->second.name) == "followed hyperlink") return;     // look of a link, not content formatting
    if (!it->second.basedOn.empty()) ApplyStyleFmt(it->second.basedOn, r, depth + 1);
    const ::filo::doc::Run& f = it->second.fmt;
    if (f.bold) r.bold = true;
    if (f.italic) r.italic = true;
    if (f.underline) r.underline = true;
    if (f.strike) r.strike = true;
    if (f.code) r.code = true;
    if (!f.color.empty()) r.color = f.color;
    if (f.sizePt > 0) r.sizePt = f.sizePt;
    if (f.sup) r.sup = true;
    if (f.sub) r.sub = true;
  }

  static char JcToAlign(const std::string& v, bool bidi) {
    std::string l = LowerAscii(v);
    if (l == "center") return 'c';
    if (l == "both" || l == "distribute" || l == "justify") return 'j';
    if (l == "left" || l == "start") return bidi ? 'r' : 'l';
    if (l == "right" || l == "end") return bidi ? 'l' : 'r';
    return 0;
  }

  // style helpers -----------------------------------------------------------
  std::string StyleName(const std::string& id) const { auto it = styles_.find(id); return it == styles_.end() ? std::string() : it->second.name; }

  int HeadingLevel(const std::string& styleId, int directOutline) const {
    std::string id = styleId; int depth = 0;
    while (!id.empty() && depth++ < 8) {
      auto it = styles_.find(id);
      std::string nm = it == styles_.end() ? id : it->second.name;
      std::string ln = LowerAscii(nm);
      if (StartsWith(ln, "heading ") && ln.size() > 8 && isdigit((unsigned char)ln[8])) return std::clamp(ln[8] - '0', 1, 6);
      if (StartsWith(ln, "heading") && ln.size() == 8 && isdigit((unsigned char)ln[7])) return std::clamp(ln[7] - '0', 1, 6);
      if (ln == "title") return 1;
      if (it == styles_.end()) break;
      if (it->second.outline >= 0 && it->second.outline < 9) return std::min(it->second.outline + 1, 6);
      id = it->second.basedOn;
    }
    if (directOutline >= 0 && directOutline < 6) return directOutline + 1;
    return 0;
  }
  bool StyleIs(const std::string& styleId, std::initializer_list<const char*> names) const {
    std::string id = styleId; int depth = 0;
    while (!id.empty() && depth++ < 8) {
      auto it = styles_.find(id);
      std::string ln = LowerAscii(it == styles_.end() ? id : it->second.name);
      for (auto* n : names) if (ln == n) return true;
      if (it == styles_.end()) break;
      id = it->second.basedOn;
    }
    return false;
  }
  bool StyleBidi(const std::string& styleId) const {
    std::string id = styleId; int depth = 0;
    while (!id.empty() && depth++ < 8) { auto it = styles_.find(id); if (it == styles_.end()) break; if (it->second.bidi) return true; id = it->second.basedOn; }
    return false;
  }
  void StyleNum(const std::string& styleId, int& numId, int& ilvl) const {
    std::string id = styleId; int depth = 0;
    while (!id.empty() && depth++ < 8) { auto it = styles_.find(id); if (it == styles_.end()) break; if (it->second.numId) { numId = it->second.numId; ilvl = it->second.ilvl; return; } id = it->second.basedOn; }
  }

  // ---------------------------------------------------------------- numbering
  void LoadNumbering(const std::string& path) {
    auto s = z_.ReadString(path);
    if (!s) return;
    auto dom = xml::ParseDom(*s);
    if (!dom) return;
    for (auto* an : dom->Children("abstractNum")) {
      int id = atoi(an->AttrOr("abstractNumId").c_str());
      AbstractNum a;
      for (auto* l : an->Children("lvl")) {
        NumLvl nl; int il = atoi(l->AttrOr("ilvl").c_str());
        if (auto* f = l->Child("numFmt")) nl.fmt = f->AttrOr("val", "decimal");
        if (auto* st = l->Child("start")) nl.start = atoi(st->AttrOr("val", "1").c_str());
        a.lvls[il] = nl;
      }
      abs_[id] = std::move(a);
    }
    for (auto* n : dom->Children("num")) {
      int id = atoi(n->AttrOr("numId").c_str());
      NumInst ni;
      if (auto* a = n->Child("abstractNumId")) ni.abstractId = atoi(a->AttrOr("val").c_str());
      for (auto* lo : n->Children("lvlOverride")) {
        int il = atoi(lo->AttrOr("ilvl").c_str());
        if (auto* so = lo->Child("startOverride")) ni.startOverride[il] = atoi(so->AttrOr("val").c_str());
      }
      nums_[id] = std::move(ni);
    }
  }

  bool ListInfo(int numId, int ilvl, bool& ordered, int& number) {
    auto it = nums_.find(numId);
    ordered = false; number = 0;
    if (it == nums_.end()) return true;           // unknown definition: treat as a bullet list
    auto ab = abs_.find(it->second.abstractId);
    NumLvl lvl;
    if (ab != abs_.end()) { auto l = ab->second.lvls.find(ilvl); if (l != ab->second.lvls.end()) lvl = l->second; }
    ordered = !(lvl.fmt == "bullet" || lvl.fmt == "none");
    if (!ordered) return true;
    int start = lvl.start;
    auto so = it->second.startOverride.find(ilvl);
    if (so != it->second.startOverride.end()) start = so->second;
    auto key = std::make_pair(numId, ilvl);
    auto c = counters_.find(key);
    if (c == counters_.end()) number = start; else number = c->second + 1;
    counters_[key] = number;
    for (auto cit = counters_.begin(); cit != counters_.end();) { if (cit->first.first == numId && cit->first.second > ilvl) cit = counters_.erase(cit); else ++cit; }
    return true;
  }

  // ---------------------------------------------------------------- footnotes / core props
  void LoadFootnotes(const std::string& path) {
    auto s = z_.ReadString(path);
    if (!s) return;
    auto dom = xml::ParseDom(*s);
    if (!dom) return;
    for (auto* fn : dom->Children("footnote")) {
      std::string type = fn->AttrOr("type");
      if (type == "separator" || type == "continuationSeparator" || type == "continuationNotice") continue;
      Runs all;
      for (auto* p : fn->Children("p")) {
        std::vector<Seg> segs; ParagraphInline(*p, segs);
        Runs r;
        for (auto& sg : segs) for (auto& x : sg.runs) r.push_back(x);
        if (!r.empty() && !all.empty()) { ::filo::doc::Run br; br.br = true; all.push_back(br); }
        for (auto& x : r) all.push_back(x);
      }
      footnotes_[fn->AttrOr("id")] = MergeRuns(all);
    }
  }
  void LoadCore() {
    RelMap rr = ReadRels(z_, "_rels/.rels");
    std::string t = FindRelTarget(rr, "/core-properties");
    std::string path = t.empty() ? "docProps/core.xml" : ResolvePartPath("", t);
    auto s = z_.ReadString(path);
    if (!s) return;
    auto dom = xml::ParseDom(*s);
    if (!dom) return;
    for (auto& k : dom->kids) {
      if (k->isText) continue;
      if (k->Is("title")) doc_.meta.title = TrimAscii(k->InnerText());
      else if (k->Is("creator")) doc_.meta.author = TrimAscii(k->InnerText());
      else if (k->Is("subject")) doc_.meta.subject = TrimAscii(k->InnerText());
    }
  }

  // ---------------------------------------------------------------- images
  std::shared_ptr<ImageData> ImageFromRid(const std::string& rid, const std::string& alt) {
    auto it = rels_.find(rid);
    if (it == rels_.end() || it->second.external) return nullptr;
    std::string path = ResolvePartPath(mainPart_, it->second.target);
    Bytes b;
    if (!z_.Has(path) || !z_.Read(path, b) || b.empty()) return nullptr;
    ImageInfo info;
    auto img = std::make_shared<ImageData>();
    if (ProbeImage(b.data(), b.size(), info)) { img->mime = info.mime; img->w = info.w; img->h = info.h; }
    else { img->mime = "application/octet-stream"; }
    img->bytes = std::move(b); img->alt = alt;
    size_t sl = path.rfind('/'); img->name = sl == std::string::npos ? path : path.substr(sl + 1);
    if (img->mime == "application/octet-stream") {
      // EMF/WMF and other formats: try WIC; otherwise drop
      Bytes png; int w = 0, h = 0;
      if (ConvertImageToPng(img->bytes, png, &w, &h)) { img->bytes = std::move(png); img->mime = "image/png"; img->w = w; img->h = h; }
      else { doc_.Warn(L"An image in an unsupported format (such as EMF) was left out."); return nullptr; }
    }
    return img;
  }

  static const xml::Node* FindDesc(const xml::Node& n, std::string_view local) {
    for (auto& k : n.kids) {
      if (k->isText) continue;
      if (k->Is(local)) return k.get();
      if (auto* r = FindDesc(*k, local)) return r;
    }
    return nullptr;
  }

  // ---------------------------------------------------------------- paragraphs
  void Fmt(const xml::Node& run, ::filo::doc::Run& r) { ParseRPr(run.Child("rPr"), r); }

  void RunNode(const xml::Node& rn, ::filo::doc::Run base, Runs& cur, std::vector<Seg>& segs) {
    ::filo::doc::Run fmt = base;
    Fmt(rn, fmt);
    auto addText = [&](const std::string& t) {
      if (t.empty()) return;
      // text inside the instruction part of a field is not content
      if (!fields_.empty() && !fields_.back().separated) return;
      ::filo::doc::Run x = fmt; x.text = t;
      if (!fields_.empty() && fields_.back().separated && !fields_.back().link.empty() && x.link.empty()) x.link = fields_.back().link;
      cur.push_back(std::move(x));
    };
    for (auto& k : rn.kids) {
      if (k->isText) continue;
      std::string_view n = k->Local();
      if (n == "t") addText(k->InnerText());
      else if (n == "tab" || n == "ptab") addText("\t");
      else if (n == "br" || n == "cr") {
        std::string ty = k->AttrOr("type");
        if (ty == "page") { if (!cur.empty()) { Seg s; s.runs = std::move(cur); cur.clear(); segs.push_back(std::move(s)); } Seg pb; pb.pageBreak = true; segs.push_back(std::move(pb)); }
        else if (ty == "column") {}
        else if (fields_.empty() || fields_.back().separated) { ::filo::doc::Run b = fmt; b.br = true; b.text.clear(); cur.push_back(std::move(b)); }
      }
      else if (n == "noBreakHyphen") addText("\xE2\x80\x91");
      else if (n == "fldChar") {
        std::string ty = k->AttrOr("fldCharType");
        if (ty == "begin") fields_.push_back({});
        else if (ty == "separate") {
          if (!fields_.empty()) {
            fields_.back().separated = true;
            fields_.back().link = HyperlinkFromInstr(fields_.back().instr);
          }
        }
        else if (ty == "end") { if (!fields_.empty()) fields_.pop_back(); }
      }
      else if (n == "instrText") { if (!fields_.empty()) fields_.back().instr += k->InnerText(); }
      else if (n == "footnoteReference") {
        std::string id = k->AttrOr("id");
        if (footnotes_.count(id)) {
          usedFootnotes_.push_back(id);
          ::filo::doc::Run x = fmt; x.sup = true; x.text = std::to_string(usedFootnotes_.size());
          cur.push_back(std::move(x));
        }
      }
      else if (n == "drawing" || n == "pict" || n == "object") {
        if (FindDesc(*k, "txbxContent")) sawTextBox_ = true;
        const xml::Node* blip = FindDesc(*k, "blip");
        std::string rid; std::string alt;
        if (blip) rid = blip->AttrOr("embed");
        else if (auto* im = FindDesc(*k, "imagedata")) rid = im->AttrOr("id");
        if (auto* dp = FindDesc(*k, "docPr")) { alt = dp->AttrOr("descr"); if (alt.empty()) alt = dp->AttrOr("title"); }
        if (!rid.empty()) {
          if (auto img = ImageFromRid(rid, alt)) {
            if (!cur.empty()) { Seg s; s.runs = std::move(cur); cur.clear(); segs.push_back(std::move(s)); }
            Seg is; is.img = img; segs.push_back(std::move(is));
          }
        }
      }
      else if (n == "AlternateContent") {
        const xml::Node* choice = k->Child("Choice");
        const xml::Node* use = choice ? choice : k->Child("Fallback");
        if (use) {
          xml::Node wrapper; // process children of the chosen branch as run children
          for (auto& c : use->kids) {
            if (c->isText) continue;
            if (c->Is("drawing") || c->Is("pict")) {
              if (FindDesc(*c, "txbxContent")) sawTextBox_ = true;
              const xml::Node* blip = FindDesc(*c, "blip");
              if (blip) {
                std::string alt; if (auto* dp = FindDesc(*c, "docPr")) alt = dp->AttrOr("descr");
                if (auto img = ImageFromRid(blip->AttrOr("embed"), alt)) {
                  if (!cur.empty()) { Seg s; s.runs = std::move(cur); cur.clear(); segs.push_back(std::move(s)); }
                  Seg is; is.img = img; segs.push_back(std::move(is));
                }
              }
            }
          }
        }
      }
    }
  }

  static std::string HyperlinkFromInstr(const std::string& instr) {
    std::string t = TrimAscii(instr);
    if (!StartsWith(UpperAscii(t.substr(0, 9)), "HYPERLINK")) return {};
    size_t q1 = t.find('"');
    std::string url;
    if (q1 != std::string::npos) { size_t q2 = t.find('"', q1 + 1); if (q2 != std::string::npos) url = t.substr(q1 + 1, q2 - q1 - 1); }
    size_t l = t.find("\\l");
    if (l != std::string::npos) { size_t a1 = t.find('"', l); if (a1 != std::string::npos) { size_t a2 = t.find('"', a1 + 1); if (a2 != std::string::npos) return "#" + t.substr(a1 + 1, a2 - a1 - 1); } }
    return url;
  }

  void InlineChildren(const xml::Node& parent, ::filo::doc::Run base, Runs& cur, std::vector<Seg>& segs) {
    for (auto& k : parent.kids) {
      if (k->isText) continue;
      std::string_view n = k->Local();
      if (n == "r") RunNode(*k, base, cur, segs);
      else if (n == "hyperlink") {
        ::filo::doc::Run b = base;
        std::string rid = k->AttrOr("id");
        if (!rid.empty()) { auto it = rels_.find(rid); if (it != rels_.end() && it->second.external) b.link = it->second.target; }
        else if (auto* a = k->Attr("anchor")) b.link = "#" + *a;
        InlineChildren(*k, b, cur, segs);
      }
      else if (n == "fldSimple") {
        ::filo::doc::Run b = base;
        std::string l = HyperlinkFromInstr(k->AttrOr("instr"));
        if (!l.empty()) b.link = l;
        InlineChildren(*k, b, cur, segs);
      }
      else if (n == "ins" || n == "smartTag" || n == "sdt" || n == "sdtContent" || n == "customXml" || n == "moveTo" || n == "bdo" || n == "dir") InlineChildren(*k, base, cur, segs);
      else if (n == "oMath" || n == "oMathPara") {
        sawEquation_ = true;
        std::string t; std::function<void(const xml::Node&)> grab = [&](const xml::Node& x) { for (auto& c : x.kids) { if (c->isText) continue; if (c->Is("t")) t += c->InnerText(); else grab(*c); } };
        grab(*k);
        if (!t.empty()) { ::filo::doc::Run x = base; x.text = t; x.italic = true; cur.push_back(std::move(x)); }
      }
      // del, moveFrom, bookmarks, proofErr, commentRange*: ignored
    }
  }

  void ParagraphInline(const xml::Node& p, std::vector<Seg>& segs) {
    Runs cur;
    ::filo::doc::Run base;
    InlineChildren(p, base, cur, segs);
    if (!cur.empty()) { Seg s; s.runs = std::move(cur); segs.push_back(std::move(s)); }
  }

  void Paragraph(const xml::Node& p, std::vector<Block>& out) {
    std::string styleId; int numId = 0, ilvl = 0; bool bidi = false; char align = 0; int outline = -1; bool pageBefore = false; bool hasNumPr = false;
    ::filo::doc::Run styleFmt;
    if (auto* ppr = p.Child("pPr")) {
      if (auto* ps = ppr->Child("pStyle")) styleId = ps->AttrOr("val");
      if (auto* np = ppr->Child("numPr")) {
        hasNumPr = true;
        if (auto* ni = np->Child("numId")) numId = atoi(ni->AttrOr("val").c_str());
        if (auto* il = np->Child("ilvl")) ilvl = atoi(il->AttrOr("val").c_str());
      }
      if (auto* b = ppr->Child("bidi")) bidi = OnOff(b);
      else bidi = StyleBidi(styleId);
      if (auto* jc = ppr->Child("jc")) align = JcToAlign(jc->AttrOr("val"), bidi);
      if (auto* ol = ppr->Child("outlineLvl")) outline = atoi(ol->AttrOr("val").c_str());
      pageBefore = ppr->Child("pageBreakBefore") && OnOff(ppr->Child("pageBreakBefore"));
    } else bidi = StyleBidi(styleId);
    if (!hasNumPr && !styleId.empty()) StyleNum(styleId, numId, ilvl);

    int heading = HeadingLevel(styleId, outline);
    bool quote = StyleIs(styleId, {"quote", "intense quote", "block text"});
    bool codeStyle = StyleIs(styleId, {"code", "code block", "source code", "html preformatted", "preformatted text", "macro text", "plain text"});

    std::vector<Seg> segs;
    ParagraphInline(p, segs);
    if (pageBefore) { Block pb; pb.kind = Block::Kind::PageBreak; out.push_back(std::move(pb)); }

    // paragraph style character formatting applies to runs without a heading (headings are formatted by the writer)
    if (!styleId.empty() && heading == 0 && !quote && !codeStyle) {
      ::filo::doc::Run sf; ApplyStyleFmt(styleId, sf, 0);
      if (sf.bold || sf.italic || sf.underline || sf.strike || sf.code || !sf.color.empty())
        for (auto& sg : segs) for (auto& r : sg.runs) { r.bold |= sf.bold; r.italic |= sf.italic; r.underline |= sf.underline; r.strike |= sf.strike; r.code |= sf.code; if (r.color.empty()) r.color = sf.color; }
    }

    bool firstText = true;
    for (auto& sg : segs) {
      if (sg.pageBreak) { Block pb; pb.kind = Block::Kind::PageBreak; out.push_back(std::move(pb)); continue; }
      if (sg.img) { Block ib; ib.kind = Block::Kind::Image; ib.image = sg.img; out.push_back(std::move(ib)); continue; }
      Runs runs = MergeRuns(sg.runs);
      bool any = false; for (auto& r : runs) if (!r.br && !r.text.empty()) any = true;
      if (!any) continue;
      Block b;
      b.rtl = bidi; b.align = align; b.runs = std::move(runs);
      if (codeStyle && heading == 0) {
        b.kind = Block::Kind::Code; for (auto& r : b.runs) r.code = true; b.rtl = false;
      }
      else if (firstText && heading > 0) { b.kind = Block::Kind::Heading; b.level = heading; }
      else if (firstText && numId > 0 && heading == 0) {
        b.kind = Block::Kind::ListItem; b.level = std::clamp(ilvl, 0, 8);
        ListInfo(numId, ilvl, b.ordered, b.number);
      }
      else if (quote) { b.kind = Block::Kind::Quote; }
      else b.kind = Block::Kind::Paragraph;
      firstText = false;
      out.push_back(std::move(b));
    }
  }

  // ---------------------------------------------------------------- tables
  Cell CellFrom(const xml::Node& tc) {
    Cell c;
    bool firstPara = true;
    std::function<void(const xml::Node&)> walk = [&](const xml::Node& n) {
      for (auto& k : n.kids) {
        if (k->isText) continue;
        if (k->Is("p")) {
          std::vector<Seg> segs; ParagraphInline(*k, segs);
          Runs r;
          for (auto& sg : segs) { if (sg.img) { droppedCellImages_ = true; continue; } for (auto& x : sg.runs) r.push_back(x); }
          bool any = false; for (auto& x : r) if (x.br || !x.text.empty()) any = true;
          if (!any) continue;
          if (!firstPara) { ::filo::doc::Run br; br.br = true; c.runs.push_back(br); }
          for (auto& x : r) c.runs.push_back(std::move(x));
          firstPara = false;
          if (auto* ppr = k->Child("pPr")) if (auto* b = ppr->Child("bidi")) if (OnOff(b)) c.rtl = true;
        }
        else if (k->Is("tbl")) walk(*k);
        else if (k->Is("tr") || k->Is("tc") || k->Is("sdt") || k->Is("sdtContent")) walk(*k);
      }
    };
    walk(tc);
    c.runs = MergeRuns(c.runs);
    return c;
  }

  Block TableFrom(const xml::Node& tbl) {
    Block b; b.kind = Block::Kind::Table;
    if (auto* pr = tbl.Child("tblPr")) if (auto* bv = pr->Child("bidiVisual")) b.table.rtl = OnOff(bv);
    bool firstRowHeaderFlag = false;
    bool firstRowAllBold = true, firstRowAny = false;
    size_t rowIdx = 0;
    for (auto* tr : tbl.Children("tr")) {
      std::vector<Cell> row;
      bool hdr = false;
      if (auto* trp = tr->Child("trPr")) hdr = trp->Child("tblHeader") != nullptr;
      for (auto& k : tr->kids) {
        if (k->isText) continue;
        const xml::Node* tc = nullptr;
        if (k->Is("tc")) tc = k.get();
        else if (k->Is("sdt")) { if (auto* sc = k->Child("sdtContent")) tc = sc->Child("tc"); }
        if (!tc) continue;
        Cell c = CellFrom(*tc);
        if (auto* tcp = tc->Child("tcPr")) {
          if (auto* gs = tcp->Child("gridSpan")) c.colspan = std::clamp(atoi(gs->AttrOr("val", "1").c_str()), 1, 64);
          if (auto* vm = tcp->Child("vMerge")) { if (LowerAscii(vm->AttrOr("val", "continue")) != "restart") { c.runs.clear(); } }
        }
        if (rowIdx == 0) {
          bool allBold = !c.runs.empty(); for (auto& r : c.runs) if (!r.br && !r.bold && !TrimAscii(r.text).empty()) allBold = false;
          if (!c.runs.empty()) firstRowAny = true;
          if (!allBold && !c.runs.empty()) firstRowAllBold = false;
        }
        row.push_back(std::move(c));
      }
      if (row.empty()) continue;
      if (rowIdx == 0) firstRowHeaderFlag = hdr;
      if (rowIdx == 0) {
        for (size_t ci = 0, col = 0; ci < row.size(); ++ci) { for (int s = 0; s < row[ci].colspan; ++s) b.table.align.push_back(0); (void)col; }
      }
      b.table.rows.push_back(std::move(row));
      ++rowIdx;
    }
    b.table.headerRow = firstRowHeaderFlag || (firstRowAny && firstRowAllBold && b.table.rows.size() > 1);
    if (b.table.headerRow) for (auto& c : b.table.rows[0]) c.header = true;
    return b;
  }

  void Body(const xml::Node& body, std::vector<Block>& out) {
    for (auto& k : body.kids) {
      if (k->isText) continue;
      if (k->Is("p")) Paragraph(*k, out);
      else if (k->Is("tbl")) { Block t = TableFrom(*k); if (!t.table.rows.empty()) out.push_back(std::move(t)); }
      else if (k->Is("sdt")) { if (auto* sc = k->Child("sdtContent")) Body(*sc, out); }
      else if (k->Is("customXml") || k->Is("ins") || k->Is("smartTag")) Body(*k, out);
    }
  }

  void MergeCode() {
    std::vector<Block> merged;
    for (auto& b : doc_.blocks) {
      bool allMono = b.kind == Block::Kind::Paragraph && !b.runs.empty();
      if (allMono) for (auto& r : b.runs) if (!r.br && !r.code && !TrimAscii(r.text).empty()) allMono = false;
      if (allMono) { b.kind = Block::Kind::Code; }
      if (b.kind == Block::Kind::Code && !merged.empty() && merged.back().kind == Block::Kind::Code) {
        std::string t = PlainText(merged.back().runs) + "\n" + PlainText(b.runs);
        ::filo::doc::Run r; r.text = t; r.code = true; merged.back().runs = {r};
        continue;
      }
      if (b.kind == Block::Kind::Code) {
        std::string t = PlainText(b.runs);
        ::filo::doc::Run r; r.text = t; r.code = true; b.runs = {r};
      }
      merged.push_back(std::move(b));
    }
    doc_.blocks = std::move(merged);
  }
};

} // namespace

bool ReadDocx(ZipReader& zip, Document& out, std::wstring* err) {
  Reader r(zip, out);
  return r.Parse(err);
}

bool ReadDocx(const fs::path& file, Document& out, std::wstring* err) {
  ZipReader z;
  if (!z.Open(file, err)) return false;
  return ReadDocx(z, out, err);
}

bool VerifyDocx(const fs::path& file, std::wstring* why) {
  ZipReader z;
  if (!z.Open(file, why)) return false;
  if (!VerifyOpc(z, "word/document.xml", why)) return false;
  Document d;
  std::wstring e;
  if (!ReadDocx(z, d, &e)) { if (why) *why = L"the document does not read back: " + e; return false; }
  return true;
}

} // namespace filo::doc
