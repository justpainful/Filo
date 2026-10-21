#include "engines/doc/rtf.h"
#include "engines/doc/text_io.h"
#include "engines/doc/docx.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace filo::doc {

namespace {

enum class Dest { Normal, Info, FontTbl, ColorTbl, StyleSheet, Skip, Pict, FldInst, Title, Author, ListTable, ListOverrideTable, NextGraphic };

struct FontInfo { std::string name; int charset = -1; };

struct GState {
  Run fmt;
  int uc = 1;
  int skipCount = 0;
  Dest dest = Dest::Normal;
  bool star = false;
  int cp = 0;            // code page for \'hh (0 = default)
};

struct ListLevel { int nfc = 23; };
struct ListDef { std::vector<ListLevel> levels; };

struct ParaState {
  int ls = -1, ilvl = 0, style = -1, outline = -1;
  bool rtl = false, intbl = false, pnBullet = false, pnNum = false;
  char align = 0;
};

int CodePageForCharset(int cs, int dflt) {
  switch (cs) {
    case 77: return 10000; case 128: return 932; case 129: return 949; case 130: return 1361; case 134: return 936; case 136: return 950;
    case 161: return 1253; case 162: return 1254; case 163: return 1258; case 177: return 1255; case 178: return 1256; case 186: return 1257;
    case 204: return 1251; case 222: return 874; case 238: return 1250; case 255: return 437;
    default: return dflt;
  }
}

bool IsMonoName(const std::string& f) {
  std::string l = LowerAscii(f);
  return l.find("courier") != std::string::npos || l.find("consolas") != std::string::npos || l.find("mono") != std::string::npos || l.find("menlo") != std::string::npos;
}

class RtfParser {
 public:
  RtfParser(const uint8_t* d, size_t n, Document& out, const RtfOptions& o) : d_(d), n_(n), doc_(out), opt_(o) {}

  bool Parse() {
    size_t i = 0;
    while (i < n_ && (d_[i] == ' ' || d_[i] == '\r' || d_[i] == '\n' || d_[i] == 0xEF || d_[i] == 0xBB || d_[i] == 0xBF)) ++i;
    if (i + 5 > n_ || memcmp(d_ + i, "{\\rtf", 5) != 0) return false;
    stack_.push_back(GState{});
    while (i < n_) {
      uint8_t c = d_[i];
      if (c == '{') { PushGroup(); ++i; }
      else if (c == '}') { PopGroup(); ++i; if (stack_.empty()) break; }
      else if (c == '\\') i = Control(i + 1);
      else if (c == '\r' || c == '\n') ++i;
      else { PlainByte(c); ++i; }
    }
    FlushPending();
    EndParagraph();
    FlushTable();
    if (skippedImages_) doc_.Warn(L"Some embedded images (EMF/WMF or other metafile formats) could not be converted and were left out.");
    FinalizeDocument(doc_);
    return true;
  }

 private:
  const uint8_t* d_; size_t n_;
  Document& doc_;
  const RtfOptions& opt_;
  std::vector<GState> stack_;
  int ansicp_ = 1252, defFont_ = 0;
  std::map<int, FontInfo> fonts_;
  std::vector<int> colors_;           // 0xRRGGBB, -1 = auto
  std::map<int, std::string> styles_;
  std::map<int, ListDef> listDefs_;
  std::map<int, int> overrides_;      // ls -> list id
  int curListId_ = -1; ListDef curList_; bool inList_ = false;
  int ovListId_ = -1, ovLs_ = -1;
  std::vector<uint8_t> pending_;
  uint32_t pendingHigh_ = 0;
  Runs cur_;
  ParaState para_;
  std::map<std::pair<int, int>, int> counters_;
  // tables
  std::vector<Cell> rowCells_;
  Table table_;
  bool rowActive_ = false;
  // transient buffers
  std::string nameBuf_, instrBuf_, titleBuf_, authorBuf_, nextName_;
  std::string pictHex_; int pictKind_ = 0; bool pictSupported_ = false;
  int curFontDef_ = -1, curStyleDef_ = -1;
  int red_ = 0, green_ = 0, blue_ = 0; bool colorSet_ = false;
  std::string fieldUrl_;
  bool skippedImages_ = false;
  bool dropNextAc_ = false;

  GState& St() { return stack_.back(); }
  int DefaultCp() { auto it = fonts_.find(defFont_); if (it != fonts_.end() && it->second.charset >= 0) return CodePageForCharset(it->second.charset, ansicp_); return ansicp_; }
  int CurrentCp() { return St().cp ? St().cp : DefaultCp(); }

  void PushGroup() { GState g = St(); g.star = false; stack_.push_back(std::move(g)); }
  void PopGroup() {
    FlushPending();
    if (stack_.empty()) return;
    GState g = std::move(stack_.back());
    stack_.pop_back();
    switch (g.dest) {
      case Dest::FldInst: {
        std::string t = TrimAscii(instrBuf_);
        if (t.empty()) break;                    // the outer \fldinst group closing after its inner group already delivered the text
        fieldUrl_.clear();
        if (StartsWith(UpperAscii(t.substr(0, 9)), "HYPERLINK")) {
          size_t q1 = t.find('"');
          if (q1 != std::string::npos) { size_t q2 = t.find('"', q1 + 1); if (q2 != std::string::npos) fieldUrl_ = t.substr(q1 + 1, q2 - q1 - 1); }
          else { std::string r = TrimAscii(t.substr(9)); size_t sp = r.find(' '); fieldUrl_ = r.substr(0, sp); }
        }
        instrBuf_.clear();
        break;
      }
      case Dest::Pict: FinishPict(); break;
      case Dest::NextGraphic: FinishNextGraphic(); break;
      case Dest::Title: if (!stack_.empty() && stack_.back().dest != Dest::Title) { doc_.meta.title = TrimAscii(titleBuf_); } break;
      case Dest::Author: if (!stack_.empty() && stack_.back().dest != Dest::Author) { doc_.meta.author = TrimAscii(authorBuf_); } break;
      case Dest::FontTbl: case Dest::ColorTbl: case Dest::StyleSheet: break;
      default: break;
    }
    if (g.dest == Dest::ListTable || g.dest == Dest::ListOverrideTable) {}
  }

  // ---- text output ----
  void FlushPending() {
    if (pending_.empty()) return;
    int cp = CurrentCp();
    if (cp == 42 || cp == 0) cp = 1252;
    int len = MultiByteToWideChar((UINT)cp, 0, (const char*)pending_.data(), (int)pending_.size(), nullptr, 0);
    if (len <= 0) { cp = 1252; len = MultiByteToWideChar(1252, 0, (const char*)pending_.data(), (int)pending_.size(), nullptr, 0); }
    std::wstring w(len > 0 ? len : 0, L'\0');
    if (len > 0) MultiByteToWideChar((UINT)cp, 0, (const char*)pending_.data(), (int)pending_.size(), w.data(), len);
    pending_.clear();
    for (size_t k = 0; k < w.size(); ++k) {
      uint32_t c = w[k];
      if (c >= 0xD800 && c <= 0xDBFF && k + 1 < w.size() && w[k + 1] >= 0xDC00 && w[k + 1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (w[k + 1] - 0xDC00); ++k; }
      Emit(c);
    }
  }

  void Emit(uint32_t cp) {
    if (dropNextAc_) { dropNextAc_ = false; if (cp == 0xFFFC || cp == 0xAC || cp == 0xA8) return; }
    if (cp == 0 ) return;
    std::string u; AppendUtf8(u, cp);
    switch (St().dest) {
      case Dest::Normal: {
        if (cp < 0x20 && cp != '\t') return;
        Run r = St().fmt; r.text = u;
        cur_.push_back(std::move(r));
        break;
      }
      case Dest::FontTbl: case Dest::StyleSheet:
        if (cp == ';') CommitName(); else nameBuf_ += u;
        break;
      case Dest::ColorTbl:
        if (cp == ';') CommitColor();
        break;
      case Dest::FldInst: instrBuf_ += u; break;
      case Dest::Title: titleBuf_ += u; break;
      case Dest::Author: authorBuf_ += u; break;
      case Dest::NextGraphic: nextName_ += u; break;
      default: break;
    }
  }

  void CommitName() {
    std::string nm = TrimAscii(nameBuf_); nameBuf_.clear();
    if (St().dest == Dest::FontTbl) { if (curFontDef_ >= 0) fonts_[curFontDef_].name = nm; }
    else if (St().dest == Dest::StyleSheet) { if (curStyleDef_ >= 0) styles_[curStyleDef_] = nm; }
  }
  void CommitColor() {
    if (colorSet_) colors_.push_back((red_ << 16) | (green_ << 8) | blue_); else colors_.push_back(-1);
    red_ = green_ = blue_ = 0; colorSet_ = false;
  }

  void PlainByte(uint8_t c) {
    if (St().dest == Dest::Pict) { if (isxdigit(c)) pictHex_.push_back((char)c); return; }
    if (St().dest == Dest::Skip) return;
    if (St().skipCount > 0) { --St().skipCount; return; }
    if (c >= 0x80) { pending_.push_back(c); return; }
    FlushPending();
    Emit(c);
  }

  // ---- control words ----
  size_t Control(size_t i) {
    if (i >= n_) return i;
    uint8_t c = d_[i];
    if (!isalpha(c)) {
      ++i;
      switch (c) {
        case '\'': {
          if (i + 1 < n_ && isxdigit(d_[i]) && isxdigit(d_[i + 1])) {
            char hx[3] = {(char)d_[i], (char)d_[i + 1], 0};
            uint8_t b = (uint8_t)strtol(hx, nullptr, 16);
            i += 2;
            if (St().dest == Dest::Skip || St().dest == Dest::Pict) return i;
            if (St().skipCount > 0) { --St().skipCount; return i; }
            if (dropNextAc_ && (b == 0xAC)) { dropNextAc_ = false; return i; }
            pending_.push_back(b);
          }
          return i;
        }
        case '*': St().star = true; return i;
        case '\\': case '{': case '}': FlushPending(); if (St().skipCount > 0) { --St().skipCount; return i; } Emit(c); return i;
        case '~': FlushPending(); Emit(0xA0); return i;
        case '_': FlushPending(); Emit(0x2011); return i;
        case '-': case '|': case ':': return i;
        case '\r': case '\n': FlushPending(); EndParagraph(); return i;
        default: return i;
      }
    }
    size_t st = i;
    while (i < n_ && isalpha(d_[i])) ++i;
    std::string word((const char*)d_ + st, i - st);
    bool hasParam = false; int param = 0;
    if (i < n_ && (d_[i] == '-' || isdigit(d_[i]))) {
      size_t p = i; bool neg = false;
      if (d_[p] == '-') { neg = true; ++p; }
      if (p < n_ && isdigit(d_[p])) {
        long v = 0; while (p < n_ && isdigit(d_[p])) { v = v * 10 + (d_[p] - '0'); if (v > 100000000) v = 100000000; ++p; }
        param = (int)(neg ? -v : v); hasParam = true; i = p;
      }
    }
    if (i < n_ && d_[i] == ' ') ++i;
    Word(word, hasParam, param);
    return i;
  }

  void StartDest(Dest d) { St().dest = d; }

  void Word(const std::string& w, bool hasParam, int p) {
    GState& s = St();
    bool star = s.star; s.star = false;
    if (w != "u") FlushPending();
    // ---- destinations (valid anywhere) ----
    static const std::set<std::string> skipDests = {"info_", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf", "footnote", "pgdsctbl", "operator", "private",
      "xmlnstbl", "rsidtbl", "themedata", "colorschememapping", "latentstyles", "datastore", "generator", "mmath", "mmathPr", "wgrffmtfilter", "fchars", "lchars", "listtext", "pntext", "pntxta", "pntxtb",
      "bkmkstart", "bkmkend", "nonshppict", "shpinst", "shp", "object", "objdata", "revtbl", "template", "stylesheet_", "fldtype", "docvar", "userprops", "defchp", "defpap", "listtables_", "pn", "pnseclvl",
      "shptxt", "sp", "sn", "sv", "falt", "panose", "fontemb", "fontfile", "ud", "upr", "background", "blipuid", "annotation", "atnauthor", "atnid", "atntime", "atnref", "ftnsep", "ftnsepc", "aftnsep", "aftnsepc",
      "protusertbl", "pgptbl", "factoidname", "mhtmltag", "txfieldstart", "txfieldend", "mfcsubdir", "xe", "tc", "txe", "rxe", "bkmk", "company", "manager", "category", "keywords", "subject", "comment", "doccomm", "hlinkbase", "buptim", "creatim", "revtim", "printim"};
    if (w == "rtf") return;
    if (w == "fonttbl") { StartDest(Dest::FontTbl); nameBuf_.clear(); return; }
    if (w == "colortbl") { StartDest(Dest::ColorTbl); colors_.clear(); colorSet_ = false; return; }
    if (w == "stylesheet") { StartDest(Dest::StyleSheet); nameBuf_.clear(); return; }
    if (w == "info") { StartDest(Dest::Info); return; }
    if (w == "title" && s.dest == Dest::Info) { StartDest(Dest::Title); titleBuf_.clear(); return; }
    if (w == "author" && s.dest == Dest::Info) { StartDest(Dest::Author); authorBuf_.clear(); return; }
    if (w == "pict") { StartDest(Dest::Pict); pictHex_.clear(); pictKind_ = 0; pictSupported_ = false; return; }
    if (w == "fldinst") { StartDest(Dest::FldInst); instrBuf_.clear(); return; }
    if (w == "field") { fieldUrl_.clear(); return; }
    if (w == "fldrslt") { if (!fieldUrl_.empty()) s.fmt.link = fieldUrl_; return; }
    if (w == "NeXTGraphic") { StartDest(Dest::NextGraphic); nextName_.clear(); return; }
    if (w == "listtable") { StartDest(Dest::ListTable); return; }
    if (w == "listoverridetable") { StartDest(Dest::ListOverrideTable); return; }
    if (w == "shppict") return;
    if (skipDests.count(w)) { if (s.dest != Dest::StyleSheet && s.dest != Dest::FontTbl) StartDest(Dest::Skip); return; }

    switch (s.dest) {
      case Dest::Skip: return;
      case Dest::FontTbl:
        if (w == "f") { curFontDef_ = p; fonts_[p]; }
        else if (w == "fcharset") { if (curFontDef_ >= 0) fonts_[curFontDef_].charset = p; }
        return;
      case Dest::ColorTbl:
        if (w == "red") { red_ = p & 255; colorSet_ = true; } else if (w == "green") { green_ = p & 255; colorSet_ = true; } else if (w == "blue") { blue_ = p & 255; colorSet_ = true; }
        return;
      case Dest::StyleSheet:
        if (w == "s" || w == "cs" || w == "ds") curStyleDef_ = (w == "s") ? p : -1;
        return;
      case Dest::ListTable:
        if (w == "list") { curList_ = ListDef{}; curListId_ = -1; inList_ = true; }
        else if (w == "listlevel") curList_.levels.push_back(ListLevel{});
        else if (w == "levelnfc" || w == "levelnfcn") { if (!curList_.levels.empty()) curList_.levels.back().nfc = p; }
        else if (w == "listid") { curListId_ = p; if (inList_) listDefs_[p] = curList_; }
        // keep the stored copy current as levels are appended
        if (inList_ && curListId_ >= 0) listDefs_[curListId_] = curList_;
        return;
      case Dest::ListOverrideTable:
        if (w == "listoverride") { ovListId_ = -1; ovLs_ = -1; }
        else if (w == "listid") ovListId_ = p;
        else if (w == "ls") ovLs_ = p;
        if (ovListId_ >= 0 && ovLs_ >= 0) { overrides_[ovLs_] = ovListId_; }
        return;
      case Dest::Pict:
        if (w == "pngblip") { pictKind_ = 1; pictSupported_ = true; }
        else if (w == "jpegblip") { pictKind_ = 2; pictSupported_ = true; }
        else if (w == "emfblip" || w == "wmetafile" || w == "dibitmap" || w == "wbitmap" || w == "macpict" || w == "pmmetafile") { pictKind_ = 9; }
        return;
      case Dest::NextGraphic: return;
      case Dest::Info: return;
      case Dest::FldInst: case Dest::Title: case Dest::Author: break;
      case Dest::Normal: break;
    }
    if (s.dest != Dest::Normal) {
      if (w == "u") {}   // fall through to the generic handling below for \u
      else return;
    }

    (void)star;
    // ---- character formatting ----
    if (w == "b") s.fmt.bold = !hasParam || p != 0;
    else if (w == "i") s.fmt.italic = !hasParam || p != 0;
    else if (w == "ul" || w == "uld" || w == "uldb" || w == "ulth" || w == "ulw" || w == "ulwave" || w == "uldash" || w == "uldashd" || w == "uldashdd" || w == "ulhwave" || w == "ulldash" || w == "ulthd") s.fmt.underline = !hasParam || p != 0;
    else if (w == "ulnone") s.fmt.underline = false;
    else if (w == "strike" || w == "striked") s.fmt.strike = !hasParam || p != 0;
    else if (w == "super") s.fmt.sup = true, s.fmt.sub = false;
    else if (w == "sub") s.fmt.sub = true, s.fmt.sup = false;
    else if (w == "nosupersub") s.fmt.sup = s.fmt.sub = false;
    else if (w == "cf") { s.fmt.color.clear(); if (p > 0 && (size_t)p < colors_.size() && colors_[(size_t)p] >= 0) s.fmt.color = Printf("%06X", colors_[(size_t)p]); }
    else if (w == "fs") { s.fmt.sizePt = p > 0 ? p / 2.0 : 0; }
    else if (w == "f") {
      auto it = fonts_.find(p);
      if (it != fonts_.end()) { s.cp = it->second.charset >= 0 ? CodePageForCharset(it->second.charset, ansicp_) : 0; s.fmt.code = IsMonoName(it->second.name); s.fmt.font = it->second.name; }
    }
    else if (w == "plain") { s.fmt = Run{}; s.fmt.link = fieldUrlActive(); }
    else if (w == "rtlch") s.fmt.rtl = true;
    else if (w == "ltrch") s.fmt.rtl = false;
    else if (w == "uc") s.uc = p;
    else if (w == "u") {
      int v = p; if (v < 0) v += 65536;
      uint32_t cu = (uint32_t)v;
      if (s.skipCount > 0) {}
      if (cu >= 0xD800 && cu <= 0xDBFF) { pendingHigh_ = cu; }
      else if (cu >= 0xDC00 && cu <= 0xDFFF && pendingHigh_) { uint32_t full = 0x10000 + ((pendingHigh_ - 0xD800) << 10) + (cu - 0xDC00); pendingHigh_ = 0; Emit(full); }
      else { pendingHigh_ = 0; Emit(cu); }
      s.skipCount = s.uc;
    }
    // ---- paragraph / structure ----
    else if (w == "par" || w == "sect") { EndParagraph(); }
    else if (w == "pard") { bool keepTbl = false; (void)keepTbl; ParaState np; np.pnBullet = false; para_ = np; }
    else if (w == "line") { Run r = s.fmt; r.br = true; r.text.clear(); cur_.push_back(std::move(r)); }
    else if (w == "tab") { Run r = s.fmt; r.text = "\t"; cur_.push_back(std::move(r)); }
    else if (w == "page" || w == "pagebb") { EndParagraph(); FlushTable(); Block b; b.kind = Block::Kind::PageBreak; doc_.blocks.push_back(std::move(b)); }
    else if (w == "emdash") Emit(0x2014); else if (w == "endash") Emit(0x2013); else if (w == "bullet") Emit(0x2022);
    else if (w == "lquote") Emit(0x2018); else if (w == "rquote") Emit(0x2019); else if (w == "ldblquote") Emit(0x201C); else if (w == "rdblquote") Emit(0x201D);
    else if (w == "enspace") Emit(0x2002); else if (w == "emspace") Emit(0x2003); else if (w == "qmspace") Emit(0x2005);
    else if (w == "zwj") Emit(0x200D); else if (w == "zwnj") Emit(0x200C); else if (w == "lrm") Emit(0x200E); else if (w == "rlm") Emit(0x200F);
    else if (w == "ansicpg") { ansicp_ = p; }
    else if (w == "ansi") ansicp_ = 1252; else if (w == "mac") ansicp_ = 10000; else if (w == "pc") ansicp_ = 437; else if (w == "pca") ansicp_ = 850;
    else if (w == "deff") defFont_ = p;
    else if (w == "ql") para_.align = 'l'; else if (w == "qr") para_.align = 'r'; else if (w == "qc") para_.align = 'c'; else if (w == "qj") para_.align = 'j';
    else if (w == "rtlpar") para_.rtl = true; else if (w == "ltrpar") para_.rtl = false;
    else if (w == "ls") para_.ls = p; else if (w == "ilvl") para_.ilvl = p;
    else if (w == "s") para_.style = p;
    else if (w == "outlinelevel") para_.outline = p;
    else if (w == "pnlvlblt") para_.pnBullet = true;
    else if (w == "pndec" || w == "pnlvlbody" || w == "pnlvl") para_.pnNum = w != "pnlvl" || p > 0;
    else if (w == "intbl") para_.intbl = true;
    else if (w == "trowd") { rowActive_ = true; rowCells_.clear(); }
    else if (w == "cell") { CellEnd(); }
    else if (w == "row") { RowEnd(); }
    else if (w == "nestcell" || w == "nestrow") {}
    else if (w == "listtext") {}
  }

  std::string fieldUrlActive() { return stack_.size() >= 2 ? stack_[stack_.size() - 2].fmt.link : std::string(); }

  // ---- structure ----
  static void TrimRuns(Runs& r) {
    while (!r.empty() && (r.back().br || TrimAscii(r.back().text).empty())) r.pop_back();
    size_t lead = 0;
    while (lead < r.size() && !r[lead].br && TrimAscii(r[lead].text).empty()) ++lead;
    if (lead) r.erase(r.begin(), r.begin() + lead);
  }

  void CellEnd() {
    if (!rowActive_) rowActive_ = true;
    Cell c;
    while (!cur_.empty() && cur_.back().br) cur_.pop_back();
    c.runs = MergeRuns(cur_); cur_.clear();
    c.rtl = para_.rtl;
    rowCells_.push_back(std::move(c));
  }
  void RowEnd() {
    if (!rowCells_.empty()) {
      if (table_.rows.empty()) table_.headerRow = false;
      table_.rows.push_back(std::move(rowCells_));
    }
    rowCells_.clear(); rowActive_ = false; cur_.clear(); para_.intbl = false;
  }
  void FlushTable() {
    if (table_.rows.empty()) return;
    Block b; b.kind = Block::Kind::Table; b.table = std::move(table_);
    table_ = Table{};
    // header guess: first row entirely bold
    bool allBold = true, any = false;
    for (auto& c : b.table.rows[0]) { for (auto& r : c.runs) if (!r.br && !TrimAscii(r.text).empty()) { any = true; if (!r.bold) allBold = false; } }
    if (any && allBold && b.table.rows.size() > 1) { b.table.headerRow = true; for (auto& c : b.table.rows[0]) c.header = true; }
    doc_.blocks.push_back(std::move(b));
  }

  void EmitImageBlock(std::shared_ptr<ImageData> img) {
    EndParagraphKeep();
    FlushTable();
    Block b; b.kind = Block::Kind::Image; b.image = std::move(img);
    doc_.blocks.push_back(std::move(b));
  }

  void EndParagraphKeep() {          // emit the text collected so far as a paragraph without touching paragraph properties
    ParaState saved = para_;
    EndParagraph();
    para_ = saved;
  }

  void EndParagraph() {
    FlushPending();
    if (para_.intbl || rowActive_) {
      if (!cur_.empty() && !cur_.back().br) { Run r; r.br = true; cur_.push_back(r); }
      return;
    }
    Runs runs = MergeRuns(cur_); cur_.clear();
    TrimRuns(runs);
    bool any = false; for (auto& r : runs) if (!r.br && !r.text.empty()) any = true;
    if (!any) return;
    FlushTable();
    Block b;
    b.runs = std::move(runs);
    b.rtl = para_.rtl; b.align = para_.align;
    int heading = 0;
    auto sit = styles_.find(para_.style);
    if (sit != styles_.end()) {
      std::string ln = LowerAscii(sit->second);
      if (StartsWith(ln, "heading ") && ln.size() > 8 && isdigit((unsigned char)ln[8])) heading = std::clamp(ln[8] - '0', 1, 6);
      else if (ln == "title") heading = 1;
    }
    if (!heading && para_.outline >= 0 && para_.outline < 6) heading = para_.outline + 1;
    if (heading) { b.kind = Block::Kind::Heading; b.level = heading; }
    else if (para_.ls >= 0 || para_.pnBullet || para_.pnNum) {
      b.kind = Block::Kind::ListItem; b.level = std::clamp(para_.ilvl, 0, 8);
      bool ordered = false;
      if (para_.ls >= 0) {
        auto ov = overrides_.find(para_.ls);
        int listId = ov != overrides_.end() ? ov->second : para_.ls;
        auto ld = listDefs_.find(listId);
        if (ld != listDefs_.end() && (size_t)b.level < ld->second.levels.size()) ordered = ld->second.levels[b.level].nfc != 23 && ld->second.levels[b.level].nfc != 255;
        else if (ld == listDefs_.end()) ordered = para_.pnNum;
      } else ordered = para_.pnNum && !para_.pnBullet;
      b.ordered = ordered;
      if (ordered) {
        auto key = std::make_pair(para_.ls, b.level);
        int num = ++counters_[key];
        for (auto it = counters_.begin(); it != counters_.end();) { if (it->first.first == para_.ls && it->first.second > b.level) it = counters_.erase(it); else ++it; }
        b.number = num;
      }
    }
    else b.kind = Block::Kind::Paragraph;
    doc_.blocks.push_back(std::move(b));
  }

  // ---- images ----
  void FinishPict() {
    if (pictHex_.empty()) return;
    if (!pictSupported_) { skippedImages_ = true; pictHex_.clear(); return; }
    Bytes b; b.reserve(pictHex_.size() / 2);
    for (size_t k = 0; k + 1 < pictHex_.size(); k += 2) { char hx[3] = {pictHex_[k], pictHex_[k + 1], 0}; b.push_back((uint8_t)strtol(hx, nullptr, 16)); }
    pictHex_.clear();
    ImageInfo info;
    if (!ProbeImage(b.data(), b.size(), info)) { skippedImages_ = true; return; }
    auto img = std::make_shared<ImageData>();
    img->bytes = std::move(b); img->mime = info.mime; img->w = info.w; img->h = info.h;
    EmitImageBlock(std::move(img));
  }
  void FinishNextGraphic() {
    std::string name = TrimAscii(nextName_);
    nextName_.clear();
    // the name may be followed by \width/\height words (already consumed as control words); keep the file name part
    if (name.empty() || !opt_.attachment) { skippedImages_ = true; return; }
    Bytes b;
    if (!opt_.attachment(name, b)) { skippedImages_ = true; return; }
    ImageInfo info;
    auto img = std::make_shared<ImageData>();
    if (ProbeImage(b.data(), b.size(), info)) { img->mime = info.mime; img->w = info.w; img->h = info.h; img->bytes = std::move(b); }
    else {
      Bytes png; int w = 0, h = 0;
      if (!ConvertImageToPng(b, png, &w, &h)) { skippedImages_ = true; return; }
      img->bytes = std::move(png); img->mime = "image/png"; img->w = w; img->h = h;
    }
    img->name = name;
    EmitImageBlock(std::move(img));
    dropNextAc_ = true;
  }
};

} // namespace

bool ParseRtf(const uint8_t* data, size_t n, Document& out, const RtfOptions& opt, std::wstring* err) {
  RtfParser p(data, n, out, opt);
  if (!p.Parse()) { if (err) *err = L"This file is not valid RTF."; return false; }
  return true;
}

bool ParseRtfd(const fs::path& dir, Document& out, std::wstring* err) {
  std::error_code ec;
  fs::path rtf = dir / L"TXT.rtf";
  if (!fs::exists(rtf, ec)) {
    rtf.clear();
    for (auto& e : fs::directory_iterator(dir, ec)) if (ToLower(e.path().extension().wstring()) == L".rtf") { rtf = e.path(); break; }
  }
  if (rtf.empty()) { if (err) *err = L"This RTFD bundle does not contain a TXT.rtf document."; return false; }
  Bytes data;
  if (!ReadFileBytes(rtf, data, 256ull << 20, err)) return false;
  RtfOptions opt;
  opt.attachment = [dir](const std::string& name, Bytes& b) {
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) return false;
    return ReadFileBytes(dir / Widen(name), b, 64u << 20);
  };
  return ParseRtf(data.data(), data.size(), out, opt, err);
}

} // namespace filo::doc
