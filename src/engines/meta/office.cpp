// DOCX / XLSX / PPTX: edit docProps/core.xml and docProps/app.xml by rewriting the zip with miniz. All other parts are
// copied byte for byte (compressed data is reused, not recompressed).
#include "engines/meta/internal.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace filo::meta::detail {

namespace {

bool LoadFile(const fs::path& p, std::vector<uint8_t>& out, size_t cap) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  if (sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > cap) { CloseHandle(h); return false; }
  out.resize((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < out.size()) {
    DWORD g = 0;
    if (!ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 28), &g, nullptr) || !g) break;
    done += g;
  }
  CloseHandle(h);
  return done == out.size();
}
bool SaveFile(const fs::path& p, const void* d, size_t n) {
  HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  const uint8_t* b = (const uint8_t*)d;
  while (done < n) {
    DWORD w = 0;
    if (!WriteFile(h, b + done, (DWORD)std::min<size_t>(n - done, 1u << 28), &w, nullptr)) { CloseHandle(h); return false; }
    done += w;
  }
  CloseHandle(h);
  return true;
}


uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }

struct SrcEntry {
  std::string name;
  uint16_t flags = 0, method = 0, dosT = 0, dosD = 0;
  uint32_t crc = 0, csize = 0, usize = 0, intAttr = 0, extAttr = 0, lho = 0;
};

// Plain (non-ZIP64) central directory reader, enough for Office packages.
bool ParseCentralDirectory(const std::vector<uint8_t>& d, std::vector<SrcEntry>& out) {
  if (d.size() < 22) return false;
  size_t eocd = std::string::npos;
  for (size_t i = d.size() - 22 + 1; i-- > 0 && d.size() - i <= 65557 + 22;) {
    if (Rd32(&d[i]) == 0x06054b50) { eocd = i; break; }
  }
  if (eocd == std::string::npos) return false;
  uint32_t total = Rd16(&d[eocd + 10]), cdSize = Rd32(&d[eocd + 12]), cdOff = Rd32(&d[eocd + 16]);
  if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return false;
  if ((uint64_t)cdOff + cdSize > d.size()) return false;
  size_t p = cdOff;
  for (uint32_t i = 0; i < total; ++i) {
    if (p + 46 > d.size() || Rd32(&d[p]) != 0x02014b50) return false;
    SrcEntry e;
    e.flags = Rd16(&d[p + 8]); e.method = Rd16(&d[p + 10]); e.dosT = Rd16(&d[p + 12]); e.dosD = Rd16(&d[p + 14]);
    e.crc = Rd32(&d[p + 16]); e.csize = Rd32(&d[p + 20]); e.usize = Rd32(&d[p + 24]);
    uint16_t nl = Rd16(&d[p + 28]), xl = Rd16(&d[p + 30]), cl = Rd16(&d[p + 32]);
    e.intAttr = Rd16(&d[p + 36]); e.extAttr = Rd32(&d[p + 38]); e.lho = Rd32(&d[p + 42]);
    if (e.csize == 0xFFFFFFFFu || e.usize == 0xFFFFFFFFu || e.lho == 0xFFFFFFFFu) return false;
    if (p + 46 + nl + xl + cl > d.size()) return false;
    e.name.assign((const char*)&d[p + 46], nl);
    out.push_back(std::move(e));
    p += 46 + nl + xl + cl;
  }
  return true;
}

std::string XmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else o.push_back(c);
  }
  return o;
}

// [start,end) of the whole element <qname ...>...</qname> or <qname .../>
bool FindElement(const std::string& xml, const std::string& qname, size_t& start, size_t& end, size_t from = 0) {
  std::string open = "<" + qname;
  size_t p = from;
  for (;;) {
    p = xml.find(open, p);
    if (p == std::string::npos) return false;
    char after = p + open.size() < xml.size() ? xml[p + open.size()] : 0;
    if (after == '>' || after == ' ' || after == '/' || after == '\t' || after == '\r' || after == '\n') break;
    p += open.size();
  }
  size_t gt = xml.find('>', p);
  if (gt == std::string::npos) return false;
  start = p;
  if (xml[gt - 1] == '/') { end = gt + 1; return true; }
  std::string close = "</" + qname + ">";
  size_t c = xml.find(close, gt);
  if (c == std::string::npos) return false;
  end = c + close.size();
  return true;
}

std::string RootClose(const std::string& xml, const std::string& root) {
  (void)xml;
  return "</" + root + ">";
}

// Inserts `element` before the closing tag of `root` (converting a self-closed root first).
bool InsertInRoot(std::string& xml, const std::string& root, const std::string& element) {
  size_t s, e;
  if (!FindElement(xml, root, s, e)) return false;
  size_t gt = xml.find('>', s);
  if (xml[gt - 1] == '/') {          // <root .../>
    xml.replace(gt - 1, 2, ">" + element + RootClose(xml, root));
    return true;
  }
  size_t close = xml.rfind(RootClose(xml, root));
  if (close == std::string::npos) return false;
  xml.insert(close, element);
  return true;
}

struct Mapping { const char* part; const char* qname; const char* attrs; bool date; };
bool MapName(std::string tag, Mapping& m) {
  size_t c = tag.rfind(':');
  if (c != std::string::npos) tag = tag.substr(c + 1);
  static const struct { const char* name; Mapping m; } table[] = {
      {"Title", {"core", "dc:title", "", false}}, {"Subject", {"core", "dc:subject", "", false}}, {"Creator", {"core", "dc:creator", "", false}},
      {"Author", {"core", "dc:creator", "", false}}, {"Keywords", {"core", "cp:keywords", "", false}}, {"Description", {"core", "dc:description", "", false}},
      {"Comments", {"core", "dc:description", "", false}}, {"LastModifiedBy", {"core", "cp:lastModifiedBy", "", false}}, {"Category", {"core", "cp:category", "", false}},
      {"ContentStatus", {"core", "cp:contentStatus", "", false}}, {"Language", {"core", "dc:language", "", false}}, {"Revision", {"core", "cp:revision", "", false}},
      {"Created", {"core", "dcterms:created", " xsi:type=\"dcterms:W3CDTF\"", true}}, {"CreateDate", {"core", "dcterms:created", " xsi:type=\"dcterms:W3CDTF\"", true}},
      {"Modified", {"core", "dcterms:modified", " xsi:type=\"dcterms:W3CDTF\"", true}}, {"ModifyDate", {"core", "dcterms:modified", " xsi:type=\"dcterms:W3CDTF\"", true}},
      {"LastPrinted", {"core", "cp:lastPrinted", "", true}},
      {"Application", {"app", "Application", "", false}}, {"Company", {"app", "Company", "", false}}, {"Manager", {"app", "Manager", "", false}},
      {"Template", {"app", "Template", "", false}}, {"AppVersion", {"app", "AppVersion", "", false}}};
  for (auto& t : table) if (tag == t.name) { m = t.m; return true; }
  return false;
}

bool NormalizeDate(const std::wstring& in, std::string& out) {
  std::string s = Narrow(Trim(in));
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  int n = sscanf_s(s.c_str(), "%d%*[:-]%d%*[:-]%d", &y, &mo, &d);
  if (n != 3) return false;
  size_t t = s.find_first_of(" T");
  if (t != std::string::npos) sscanf_s(s.c_str() + t + 1, "%d:%d:%d", &h, &mi, &sec);
  if (y < 1900 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return false;
  char b[40];
  snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, sec);
  out = b;
  return true;
}

void RemoveElement(std::string& xml, const std::string& qname) {
  size_t s, e;
  while (FindElement(xml, qname, s, e)) xml.erase(s, e - s);
}

void SetElement(std::string& xml, const std::string& root, const Mapping& m, const std::string& text) {
  size_t s, e;
  std::string el = std::string("<") + m.qname + m.attrs + ">" + XmlEscape(text) + "</" + m.qname + ">";
  if (FindElement(xml, m.qname, s, e)) {
    // keep existing attributes (e.g. xsi:type) when we have none of our own
    size_t gt = xml.find('>', s);
    std::string attrs = xml.substr(s + 1 + strlen(m.qname), gt - s - 1 - strlen(m.qname));
    if (!attrs.empty() && attrs.back() == '/') attrs.pop_back();
    if (!attrs.empty() && !*m.attrs) el = std::string("<") + m.qname + attrs + ">" + XmlEscape(text) + "</" + m.qname + ">";
    xml.replace(s, e - s, el);
  } else {
    InsertInRoot(xml, root, el);
  }
}

// Removes every child of the root element, keeping the root start tag with its namespaces.
void ClearRoot(std::string& xml, const std::string& root) {
  size_t s, e;
  if (!FindElement(xml, root, s, e)) return;
  size_t gt = xml.find('>', s);
  if (xml[gt - 1] == '/') return;
  xml.erase(gt + 1, e - (gt + 1) - RootClose(xml, root).size());
}

void RemoveRelationships(std::string& rels, const char* needle) {
  size_t p = 0;
  while ((p = rels.find("<Relationship ", p)) != std::string::npos) {
    size_t end = rels.find("/>", p);
    size_t close = rels.find("</Relationship>", p);
    size_t gt = rels.find('>', p);
    size_t e = (gt != std::string::npos && rels[gt - 1] == '/') ? gt + 1 : (close != std::string::npos ? close + 15 : end + 2);
    std::string rel = rels.substr(p, e - p);
    if (rel.find(needle) != std::string::npos) rels.erase(p, e - p); else p = e;
  }
}
void RemoveOverrides(std::string& ct, const char* needle) {
  size_t p = 0;
  while ((p = ct.find("<Override ", p)) != std::string::npos) {
    size_t gt = ct.find('>', p);
    std::string el = ct.substr(p, gt - p + 1);
    if (el.find(needle) != std::string::npos) ct.erase(p, gt - p + 1); else p = gt + 1;
  }
}

} // namespace

WriteResult WriteOffice(const fs::path& input, const Edits& e, const fs::path& tempOut, bool stripAll, bool) {
  WriteResult r;
  if (e.coverArt || e.removeCoverArt) { r.error = L"Cover art does not apply to documents."; return r; }
  std::vector<uint8_t> data;
  if (!LoadFile(input, data, 1u << 30)) { r.error = L"The document could not be read (or it is larger than 1 GB)."; return r; }
  mz_zip_archive in{};
  if (!mz_zip_reader_init_mem(&in, data.data(), data.size(), 0)) { r.error = L"The document is not a valid Office file (zip container damaged)."; return r; }
  auto endIn = OnExit([&] { mz_zip_reader_end(&in); });

  auto readPart = [&](const char* name, std::string& out) {
    int idx = mz_zip_reader_locate_file(&in, name, nullptr, 0);
    if (idx < 0) return false;
    size_t len = 0;
    void* p = mz_zip_reader_extract_to_heap(&in, (mz_uint)idx, &len, 0);
    if (!p && len) return false;
    out.assign((const char*)p, len);
    mz_free(p);
    return true;
  };
  std::string core, app, rels, ct;
  bool hasCore = readPart("docProps/core.xml", core), hasApp = readPart("docProps/app.xml", app);
  readPart("_rels/.rels", rels);
  readPart("[Content_Types].xml", ct);
  if (!hasCore) { r.error = L"This document has no core properties part, so there is nothing to edit."; return r; }

  std::set<std::string> drop;
  const bool all = stripAll || e.removeAll;
  if (all) {
    ClearRoot(core, "cp:coreProperties");
    if (hasApp) for (const char* q : {"Company", "Manager", "Application", "AppVersion", "Template", "TotalTime", "HyperlinkBase", "DocSecurity", "ScaleCrop", "SharedDoc"}) RemoveElement(app, q);
    for (const char* name : {"docProps/custom.xml"}) { drop.insert(name); }
    // thumbnails (any extension)
    mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n; ++i) {
      char nm[512];
      mz_zip_reader_get_filename(&in, i, nm, sizeof nm);
      if (strncmp(nm, "docProps/thumbnail", 18) == 0) drop.insert(nm);
    }
    RemoveRelationships(rels, "docProps/custom.xml");
    RemoveRelationships(rels, "docProps/thumbnail");
    RemoveOverrides(ct, "/docProps/custom.xml");
  }
  // explicit removals
  for (auto& k : e.remove) {
    Mapping m;
    if (!MapName(k, m)) { r.error = L"The field '" + Widen(k).substr(0, 60) + L"' cannot be edited in this kind of document."; return r; }
    if (std::string(m.part) == "core") RemoveElement(core, m.qname);
    else if (hasApp) RemoveElement(app, m.qname);
  }
  for (Group g : e.removeGroups) {
    if (g == Group::Gps) continue;
    for (const char* q : {"dc:title", "dc:subject", "dc:creator", "cp:keywords", "dc:description", "cp:lastModifiedBy", "cp:category", "cp:contentStatus"}) RemoveElement(core, q);
    if (hasApp) for (const char* q : {"Company", "Manager"}) RemoveElement(app, q);
  }
  for (auto& [k, v] : e.set) {
    Mapping m;
    if (!MapName(k, m)) { r.error = L"The field '" + Widen(k).substr(0, 60) + L"' cannot be edited in this kind of document."; return r; }
    std::string text = Narrow(v);
    if (m.date) {
      if (!NormalizeDate(v, text)) { r.error = L"The date for '" + Widen(k).substr(0, 40) + L"' is not valid. Use YYYY-MM-DD HH:MM:SS."; return r; }
    }
    if (std::string(m.part) == "core") SetElement(core, "cp:coreProperties", m, text);
    else {
      if (!hasApp) { r.error = L"This document has no extended properties part (docProps/app.xml)."; return r; }
      SetElement(app, "Properties", m, text);
    }
  }

  // Rewrite the package ourselves: unchanged parts are copied as raw compressed bytes (no recompression), the edited
  // parts are stored; every local header carries its sizes (no data descriptors), so signature sniffers keep working.
  std::vector<SrcEntry> src;
  if (!ParseCentralDirectory(data, src)) { r.error = L"The document's zip directory could not be read (or it uses ZIP64, which is not supported here)."; return r; }
  std::vector<uint8_t> out;
  out.reserve(data.size() + 8192);
  std::vector<SrcEntry> written;
  for (auto& se : src) {
    if (drop.count(se.name)) continue;
    if (se.flags & 1) { r.error = L"This document contains encrypted parts, which is not supported."; return r; }
    const std::string* repl = nullptr;
    if (se.name == "docProps/core.xml") repl = &core;
    else if (se.name == "docProps/app.xml" && hasApp) repl = &app;
    else if (se.name == "_rels/.rels" && all) repl = &rels;
    else if (se.name == "[Content_Types].xml" && all) repl = &ct;
    SrcEntry ne = se;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (repl) {
      ne.method = 0;
      ne.crc = (uint32_t)mz_crc32(mz_crc32(0, nullptr, 0), (const unsigned char*)repl->data(), repl->size());
      ne.csize = ne.usize = (uint32_t)repl->size();
      payload = (const uint8_t*)repl->data(); payloadSize = repl->size();
    } else {
      if ((uint64_t)se.lho + 30 > data.size()) { r.error = L"The document is damaged (a part points outside the file)."; return r; }
      const uint8_t* lh = &data[se.lho];
      if (Rd32(lh) != 0x04034b50) { r.error = L"The document is damaged (bad local header)."; return r; }
      uint64_t off = (uint64_t)se.lho + 30 + Rd16(lh + 26) + Rd16(lh + 28);
      if (off + se.csize > data.size()) { r.error = L"The document is damaged (a part is cut off)."; return r; }
      payload = &data[(size_t)off]; payloadSize = se.csize;
    }
    ne.flags = (uint16_t)(se.flags & 0x0800);
    ne.lho = (uint32_t)out.size();
    Put32(out, 0x04034b50); Put16(out, 20); Put16(out, ne.flags); Put16(out, ne.method); Put16(out, ne.dosT); Put16(out, ne.dosD);
    Put32(out, ne.crc); Put32(out, ne.csize); Put32(out, ne.usize); Put16(out, (uint16_t)ne.name.size()); Put16(out, 0);
    out.insert(out.end(), ne.name.begin(), ne.name.end());
    out.insert(out.end(), payload, payload + payloadSize);
    written.push_back(std::move(ne));
    if (out.size() > 0xF0000000ull) { r.error = L"The document is too large to rewrite."; return r; }
  }
  uint32_t cdStart = (uint32_t)out.size();
  for (auto& ne : written) {
    Put32(out, 0x02014b50); Put16(out, 20); Put16(out, 20); Put16(out, ne.flags); Put16(out, ne.method); Put16(out, ne.dosT); Put16(out, ne.dosD);
    Put32(out, ne.crc); Put32(out, ne.csize); Put32(out, ne.usize); Put16(out, (uint16_t)ne.name.size()); Put16(out, 0); Put16(out, 0); Put16(out, 0);
    Put16(out, (uint16_t)ne.intAttr); Put32(out, ne.extAttr); Put32(out, ne.lho);
    out.insert(out.end(), ne.name.begin(), ne.name.end());
  }
  uint32_t cdSize = (uint32_t)out.size() - cdStart;
  if (written.size() >= 0xFFFF) { r.error = L"The document has too many parts."; return r; }
  Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, (uint16_t)written.size()); Put16(out, (uint16_t)written.size()); Put32(out, cdSize); Put32(out, cdStart); Put16(out, 0);
  if (!SaveFile(tempOut, out.data(), out.size())) { r.error = L"The edited document could not be saved."; return r; }
  r.ok = true;
  return r;
}

} // namespace filo::meta::detail
