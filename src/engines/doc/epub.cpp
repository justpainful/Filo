#include "engines/doc/epub.h"
#include "engines/doc/docx.h"
#include "engines/doc/html.h"
#include "engines/doc/opc.h"
#include <objbase.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <set>

namespace filo::doc {

namespace {

std::string IsoNowZ() {
  time_t t = time(nullptr); tm g{}; gmtime_s(&g, &t);
  return Printf("%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
}

std::string NewUuid() {
  GUID g{}; CoCreateGuid(&g);
  return Printf("%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

const char* kCss =
    "body{font-family:serif;line-height:1.5;margin:5%}h1,h2,h3,h4,h5,h6{font-family:sans-serif;line-height:1.25;margin:1.2em 0 .5em}"
    "p{margin:.6em 0}table{border-collapse:collapse;margin:1em 0}th,td{border:1px solid #888;padding:.3em .5em}th{background:#eee}"
    "pre{background:#f4f4f4;padding:.6em;white-space:pre-wrap}code{font-family:monospace}blockquote{border-left:3px solid #bbb;margin:1em 0;padding:0 1em;color:#444}"
    "img{max-width:100%;height:auto}p.img{text-align:center}.footnote{font-size:.9em}[dir=rtl]{text-align:right}";

struct Chapter { size_t from, to; std::string title; };

struct EpubImage { std::string name; std::string mime; Bytes bytes; };
struct NavEntry { int level; std::string title, href; };
std::string BuildNavOl(const std::vector<NavEntry>& nav, size_t& i, int level) {
  std::string s = "<ol>\n";
  while (i < nav.size() && nav[i].level >= level) {
    s += "<li><a href=\"" + XmlEscape(nav[i].href, true) + "\">" + XmlEscape(nav[i].title.empty() ? std::string("Untitled") : nav[i].title) + "</a>";
    ++i;
    if (i < nav.size() && nav[i].level > level) s += "\n" + BuildNavOl(nav, i, level + 1);
    s += "</li>\n";
  }
  return s + "</ol>\n";
}


}

bool WriteEpub(const Document& d, const fs::path& out, const EpubWriteOptions& opt, std::wstring* err, std::vector<std::wstring>* warnings) {
  auto warn = [&](const std::wstring& w) { if (warnings) { for (auto& x : *warnings) if (x == w) return; warnings->push_back(w); } };
  std::string lang = !d.meta.language.empty() ? d.meta.language : (d.rtl ? "ar" : "en");
  std::string title = d.meta.title.empty() ? std::string("Untitled") : d.meta.title;
  std::string ident = opt.identifier.empty() ? "urn:uuid:" + NewUuid() : opt.identifier;

  // ---- chapters ----
  std::vector<Chapter> chapters;
  std::vector<size_t> h1s;
  for (size_t i = 0; i < d.blocks.size(); ++i) if (d.blocks[i].kind == Block::Kind::Heading && d.blocks[i].level == 1) h1s.push_back(i);
  if (!opt.splitOnH1 || h1s.empty()) chapters.push_back({0, d.blocks.size(), title});
  else {
    if (h1s[0] > 0) chapters.push_back({0, h1s[0], title});
    for (size_t k = 0; k < h1s.size(); ++k) chapters.push_back({h1s[k], k + 1 < h1s.size() ? h1s[k + 1] : d.blocks.size(), TrimAscii(PlainText(d.blocks[h1s[k]].runs))});
  }
  for (auto& c : chapters) if (c.title.empty()) c.title = "Section";

  // ---- images ----
  std::vector<EpubImage> images;
  std::map<size_t, size_t> imageOfBlock;
  for (size_t i = 0; i < d.blocks.size(); ++i) {
    const Block& b = d.blocks[i];
    if (b.kind != Block::Kind::Image || !b.image || b.image->bytes.empty()) continue;
    EpubImage im;
    ImageInfo info;
    Bytes data = b.image->bytes;
    std::string mime = ProbeImage(data.data(), data.size(), info) ? info.mime : b.image->mime;
    if (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp" && mime != "image/svg+xml") {
      Bytes png;
      if (ConvertImageToPng(data, png)) { data = std::move(png); mime = "image/png"; }
      else { warn(L"An image in an unsupported format was left out of the EPUB."); continue; }
    }
    im.mime = mime; im.bytes = std::move(data);
    im.name = "img" + std::to_string(images.size() + 1) + "." + ImageExtForMime(mime);
    imageOfBlock[i] = images.size();
    images.push_back(std::move(im));
  }

  // ---- render chapters ----
  struct Rendered { std::string file, body; };
  std::vector<Rendered> rendered;
  std::vector<NavEntry> nav;
  int headingsBefore = 0;
  for (size_t ci = 0; ci < chapters.size(); ++ci) {
    const Chapter& ch = chapters[ci];
    HtmlWriteOptions ho;
    ho.xhtml = true; ho.fullDocument = false; ho.embedImages = false; ho.headingAnchors = true; ho.firstHeadingId = headingsBefore;
    ho.imageHref = [&](const Block&, int idx) { auto it = imageOfBlock.find((size_t)idx); return it == imageOfBlock.end() ? std::string() : "../images/" + images[it->second].name; };
    Rendered r;
    char fn[32]; snprintf(fn, sizeof fn, "chapter%03zu.xhtml", ci + 1);
    r.file = fn;
    r.body = RenderBlocksHtml(d.blocks, ch.from, ch.to, ho);
    // nav entries: the chapter itself (its H1 when it starts with one), then H2/H3 inside
    int hid = headingsBefore;
    bool chapterListed = false;
    for (size_t i = ch.from; i < ch.to; ++i) {
      const Block& b = d.blocks[i];
      if (b.kind != Block::Kind::Heading) continue;
      std::string anchor = Printf("h-%d", hid++);
      if (b.level == 1 && !chapterListed) { nav.push_back({1, TrimAscii(PlainText(b.runs)).empty() ? ch.title : TrimAscii(PlainText(b.runs)), "text/" + r.file}); chapterListed = true; }
      else if (b.level <= 3) {
        if (!chapterListed) { nav.push_back({1, ch.title, "text/" + r.file}); chapterListed = true; }
        nav.push_back({b.level, TrimAscii(PlainText(b.runs)), "text/" + r.file + "#" + anchor});
      }
    }
    if (!chapterListed) nav.push_back({1, ch.title, "text/" + r.file});
    headingsBefore = hid;
    rendered.push_back(std::move(r));
  }

  // ---- package ----
  ZipWriter z;
  if (!z.Open(out, err)) return false;
  z.Add("mimetype", std::string("application/epub+zip"), false);
  z.Add("META-INF/container.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
        "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
  z.Add("OEBPS/css/style.css", std::string(kCss));

  std::string dir = d.rtl ? " dir=\"rtl\"" : "";
  for (size_t i = 0; i < rendered.size(); ++i) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"" + XmlEscape(lang, true) +
                    "\" xml:lang=\"" + XmlEscape(lang, true) + "\"" + dir + ">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + XmlEscape(chapters[i].title) +
                    "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"../css/style.css\"/>\n</head>\n<body>\n" + rendered[i].body + "</body>\n</html>\n";
    z.Add("OEBPS/text/" + rendered[i].file, x);
  }
  for (auto& im : images) z.Add("OEBPS/images/" + im.name, im.bytes, false);

  // nav.xhtml
  {
    std::string n = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"" + XmlEscape(lang, true) +
                    "\" xml:lang=\"" + XmlEscape(lang, true) + "\"" + dir + ">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + XmlEscape(title) + "</title>\n</head>\n<body>\n<nav epub:type=\"toc\" id=\"toc\">\n<h1>" +
                    XmlEscape(title) + "</h1>\n";
    // normalise levels so that each entry is at most one level deeper than its predecessor
    int prev = 0;
    for (auto& e : nav) { e.level = std::clamp(e.level, 1, 3); if (e.level > prev + 1) e.level = prev + 1; prev = e.level; }
    size_t pos = 0;
    std::string lst = BuildNavOl(nav, pos, 1);
    n += lst + "</nav>\n</body>\n</html>\n";
    z.Add("OEBPS/nav.xhtml", n);
  }
  // toc.ncx
  {
    std::string n = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" xml:lang=\"" + XmlEscape(lang, true) + "\"><head>"
                    "<meta name=\"dtb:uid\" content=\"" + XmlEscape(ident, true) + "\"/><meta name=\"dtb:depth\" content=\"1\"/><meta name=\"dtb:totalPageCount\" content=\"0\"/><meta name=\"dtb:maxPageNumber\" content=\"0\"/></head>"
                    "<docTitle><text>" + XmlEscape(title) + "</text></docTitle><navMap>";
    int po = 1;
    for (auto& e : nav) {
      if (e.level != 1) continue;
      n += "<navPoint id=\"np" + std::to_string(po) + "\" playOrder=\"" + std::to_string(po) + "\"><navLabel><text>" + XmlEscape(e.title) + "</text></navLabel><content src=\"" + XmlEscape(e.href, true) + "\"/></navPoint>";
      ++po;
    }
    n += "</navMap></ncx>";
    z.Add("OEBPS/toc.ncx", n);
  }
  // content.opf
  {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"" + XmlEscape(lang, true) + "\"" + dir + ">\n"
                    "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
                    "<dc:identifier id=\"bookid\">" + XmlEscape(ident) + "</dc:identifier>\n<dc:title>" + XmlEscape(title) + "</dc:title>\n<dc:language>" + XmlEscape(lang) + "</dc:language>\n";
    if (!d.meta.author.empty()) o += "<dc:creator>" + XmlEscape(d.meta.author) + "</dc:creator>\n";
    if (!d.meta.description.empty()) o += "<dc:description>" + XmlEscape(d.meta.description) + "</dc:description>\n";
    o += "<meta property=\"dcterms:modified\">" + IsoNowZ() + "</meta>\n</metadata>\n<manifest>\n"
         "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
         "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
         "<item id=\"css\" href=\"css/style.css\" media-type=\"text/css\"/>\n";
    for (size_t i = 0; i < rendered.size(); ++i) o += "<item id=\"ch" + std::to_string(i + 1) + "\" href=\"text/" + rendered[i].file + "\" media-type=\"application/xhtml+xml\"/>\n";
    for (size_t i = 0; i < images.size(); ++i) o += "<item id=\"im" + std::to_string(i + 1) + "\" href=\"images/" + images[i].name + "\" media-type=\"" + images[i].mime + "\"/>\n";
    o += "</manifest>\n<spine toc=\"ncx\"" + std::string(d.rtl ? " page-progression-direction=\"rtl\"" : "") + ">\n";
    for (size_t i = 0; i < rendered.size(); ++i) o += "<itemref idref=\"ch" + std::to_string(i + 1) + "\"/>\n";
    o += "</spine>\n</package>\n";
    z.Add("OEBPS/content.opf", o);
  }
  if (!z.Close()) { if (err) *err = L"The EPUB file could not be written."; return false; }
  return true;
}

// ============================================================================================================================
bool ReadEpub(ZipReader& z, Document& out, std::wstring* err) {
  auto fail = [&](const wchar_t* m) { if (err) *err = m; return false; };
  auto cont = z.ReadString("META-INF/container.xml");
  if (!cont) return fail(L"This is not an EPUB book (META-INF/container.xml is missing).");
  auto cdom = xml::ParseDom(*cont);
  std::string opfPath;
  if (cdom) {
    std::function<const xml::Node*(const xml::Node&)> find = [&](const xml::Node& n) -> const xml::Node* {
      for (auto& k : n.kids) { if (k->isText) continue; if (k->Is("rootfile")) return k.get(); if (auto* r = find(*k)) return r; }
      return nullptr;
    };
    if (auto* rf = find(*cdom)) opfPath = rf->AttrOr("full-path");
  }
  if (opfPath.empty() || !z.Has(opfPath)) return fail(L"The EPUB package document could not be found.");
  auto opfStr = z.ReadString(opfPath);
  auto opf = opfStr ? xml::ParseDom(*opfStr) : nullptr;
  if (!opf) return fail(L"The EPUB package document is damaged.");
  struct Item { std::string href, type, props; };
  std::map<std::string, Item> manifest;
  if (auto* m = opf->Child("manifest")) for (auto* it : m->Children("item")) manifest[it->AttrOr("id")] = {it->AttrOr("href"), it->AttrOr("media-type"), it->AttrOr("properties")};
  if (auto* md = opf->Child("metadata")) {
    for (auto& k : md->kids) {
      if (k->isText) continue;
      if (k->Is("title") && out.meta.title.empty()) out.meta.title = TrimAscii(k->InnerText());
      else if (k->Is("creator") && out.meta.author.empty()) out.meta.author = TrimAscii(k->InnerText());
      else if (k->Is("language") && out.meta.language.empty()) out.meta.language = TrimAscii(k->InnerText());
      else if (k->Is("description") && out.meta.description.empty()) out.meta.description = TrimAscii(k->InnerText());
    }
  }
  std::vector<std::string> order;
  if (auto* sp = opf->Child("spine")) for (auto* ir : sp->Children("itemref")) order.push_back(ir->AttrOr("idref"));
  if (order.empty()) for (auto& [id, it] : manifest) if (it.type.find("html") != std::string::npos) order.push_back(id);
  size_t chapters = 0;
  for (auto& id : order) {
    auto it = manifest.find(id);
    if (it == manifest.end()) continue;
    const Item& item = it->second;
    if (item.type.find("html") == std::string::npos && item.type.find("xml") == std::string::npos) continue;
    if (item.props.find("nav") != std::string::npos) continue;
    std::string path = ResolvePartPath(opfPath, item.href);
    size_t hash = path.find('#'); if (hash != std::string::npos) path.resize(hash);
    Bytes data;
    if (!z.Has(path) || !z.Read(path, data)) continue;
    std::string html = DecodeHtmlBytes(data.data(), data.size());
    HtmlReadOptions ho;
    std::string base = path;
    ho.resolve = [&z, base](const std::string& href, Bytes& b) {
      std::string dec; for (size_t i = 0; i < href.size(); ++i) { if (href[i] == '%' && i + 2 < href.size() && isxdigit((unsigned char)href[i + 1]) && isxdigit((unsigned char)href[i + 2])) { dec.push_back((char)strtol(href.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; } else dec.push_back(href[i]); }
      std::string p = ResolvePartPath(base, dec);
      return z.Has(p) && z.Read(p, b);
    };
    Document sub = ParseHtml(html, ho);
    for (auto& w : sub.warnings) out.Warn(w);
    if (sub.blocks.empty()) continue;
    ++chapters;
    for (auto& b : sub.blocks) out.blocks.push_back(std::move(b));
    if (out.meta.language.empty()) out.meta.language = sub.meta.language;
  }
  if (chapters == 0) return fail(L"The EPUB contains no readable chapters.");
  FinalizeDocument(out);
  return true;
}

bool ReadEpub(const fs::path& file, Document& out, std::wstring* err) {
  ZipReader z;
  if (!z.Open(file, err)) return false;
  return ReadEpub(z, out, err);
}

bool VerifyEpub(const fs::path& file, std::wstring* why) {
  auto fail = [&](const std::wstring& m) { if (why) *why = m; return false; };
  ZipReader z;
  if (!z.Open(file, why)) return false;
  const auto& es = z.Entries();
  if (es.empty() || es[0].name != "mimetype") return fail(L"the first entry is not 'mimetype'");
  Bytes mt;
  if (!z.Read(0, mt) || std::string(mt.begin(), mt.end()) != "application/epub+zip") return fail(L"'mimetype' has the wrong content");
  // stored (method 0): csize == size
  if (es[0].csize != es[0].size) return fail(L"'mimetype' is compressed");
  // no extra field in the first local header, per the OCF spec
  {
    Bytes raw(64);
    HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, raw.data(), 64, &got, nullptr); CloseHandle(h); }
    raw.resize(got);
    if (raw.size() < 30) return fail(L"file too small");
    {
      uint16_t nlen = raw[26] | (raw[27] << 8), xlen = raw[28] | (raw[29] << 8);
      if (xlen != 0) return fail(L"'mimetype' has an extra field");
      if (nlen != 8) return fail(L"first local header does not name 'mimetype'");
    }
  }
  auto cont = z.ReadString("META-INF/container.xml");
  if (!cont) return fail(L"container.xml is missing");
  std::string wf;
  if (!xml::CheckWellFormed(*cont, &wf)) return fail(L"container.xml: " + Widen(wf));
  auto cdom = xml::ParseDom(*cont);
  std::string opfPath;
  if (cdom) { std::function<void(const xml::Node&)> f = [&](const xml::Node& n) { for (auto& k : n.kids) { if (k->isText) continue; if (k->Is("rootfile") && opfPath.empty()) opfPath = k->AttrOr("full-path"); f(*k); } }; f(*cdom); }
  if (opfPath.empty() || !z.Has(opfPath)) return fail(L"the package document named by container.xml is missing");
  auto opfStr = z.ReadString(opfPath);
  if (!opfStr || !xml::CheckWellFormed(*opfStr, &wf)) return fail(L"package document: " + Widen(wf));
  auto opf = xml::ParseDom(*opfStr);
  if (!opf || !opf->Is("package")) return fail(L"package document root is not <package>");
  bool v3 = StartsWith(opf->AttrOr("version"), "3");
  auto* md = opf->Child("metadata");
  if (!md) return fail(L"metadata is missing");
  bool hasId = false, hasTitle = false, hasLang = false, hasModified = false;
  for (auto& k : md->kids) {
    if (k->isText) continue;
    if (k->Is("identifier") && !TrimAscii(k->InnerText()).empty()) hasId = true;
    if (k->Is("title") && !TrimAscii(k->InnerText()).empty()) hasTitle = true;
    if (k->Is("language") && !TrimAscii(k->InnerText()).empty()) hasLang = true;
    if (k->Is("meta") && k->AttrOr("property") == "dcterms:modified") hasModified = true;
  }
  if (!hasId || !hasTitle || !hasLang) return fail(L"required Dublin Core metadata (identifier, title, language) is incomplete");
  if (v3 && !hasModified) return fail(L"dcterms:modified is missing");
  auto* man = opf->Child("manifest");
  auto* spine = opf->Child("spine");
  if (!man || !spine) return fail(L"manifest or spine is missing");
  std::set<std::string> ids; bool hasNav = false;
  for (auto* it : man->Children("item")) {
    std::string id = it->AttrOr("id");
    if (id.empty() || !ids.insert(id).second) return fail(L"duplicate or empty manifest id");
    std::string path = ResolvePartPath(opfPath, it->AttrOr("href"));
    if (!z.Has(path)) return fail(L"manifest item " + Widen(id) + L" points to a missing file");
    if (it->AttrOr("properties").find("nav") != std::string::npos) hasNav = true;
    std::string type = it->AttrOr("media-type");
    if (type.find("xml") != std::string::npos || type == "text/html") {
      auto s = z.ReadString(path);
      if (!s || !xml::CheckWellFormed(*s, &wf)) return fail(Widen(path) + L" is not well-formed XML (" + Widen(wf) + L")");
    }
  }
  if (v3 && !hasNav) return fail(L"no navigation document (properties=\"nav\")");
  size_t spineItems = 0;
  for (auto* ir : spine->Children("itemref")) { if (!ids.count(ir->AttrOr("idref"))) return fail(L"spine references an unknown manifest item"); ++spineItems; }
  if (spineItems == 0) return fail(L"the spine is empty");
  Document d; std::wstring e;
  if (!ReadEpub(z, d, &e)) return fail(L"the book does not read back: " + e);
  return true;
}

} // namespace filo::doc
