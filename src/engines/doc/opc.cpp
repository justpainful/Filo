#include "engines/doc/opc.h"
#include <algorithm>
#include <set>

namespace filo::doc {

std::string ResolvePartPath(const std::string& basePart, const std::string& target) {
  std::string t = target;
  std::replace(t.begin(), t.end(), '\\', '/');
  std::vector<std::string> parts;
  if (!t.empty() && t[0] == '/') t.erase(0, 1);
  else {
    size_t slash = basePart.rfind('/');
    if (slash != std::string::npos) parts = SplitStr(basePart.substr(0, slash), '/');
  }
  for (auto& c : SplitStr(t, '/')) {
    if (c.empty() || c == ".") continue;
    if (c == "..") { if (!parts.empty()) parts.pop_back(); continue; }
    parts.push_back(c);
  }
  std::string r;
  for (size_t i = 0; i < parts.size(); ++i) { if (i) r += "/"; r += parts[i]; }
  return r;
}

std::string RelsPathFor(const std::string& part) {
  size_t slash = part.rfind('/');
  if (slash == std::string::npos) return "_rels/" + part + ".rels";
  return part.substr(0, slash) + "/_rels/" + part.substr(slash + 1) + ".rels";
}

RelMap ReadRels(ZipReader& z, const std::string& relsPath) {
  RelMap m;
  auto s = z.ReadString(relsPath);
  if (!s) return m;
  auto dom = xml::ParseDom(*s);
  if (!dom) return m;
  for (auto* r : dom->Children("Relationship")) {
    Relationship rel;
    rel.id = r->AttrOr("Id"); rel.type = r->AttrOr("Type"); rel.target = r->AttrOr("Target");
    rel.external = LowerAscii(r->AttrOr("TargetMode")) == "external";
    if (!rel.id.empty()) m[rel.id] = std::move(rel);
  }
  return m;
}

std::string FindRelTarget(const RelMap& rels, std::string_view suffix) {
  for (auto& [id, r] : rels) if (EndsWith(r.type, suffix)) return r.target;
  return {};
}

bool ContentTypes::Load(ZipReader& z) {
  defaults_.clear(); overrides_.clear();
  auto s = z.ReadString("[Content_Types].xml");
  if (!s) return false;
  auto dom = xml::ParseDom(*s);
  if (!dom || !dom->Is("Types")) return false;
  for (auto& k : dom->kids) {
    if (k->isText) continue;
    if (k->Is("Default")) defaults_[LowerAscii(k->AttrOr("Extension"))] = k->AttrOr("ContentType");
    else if (k->Is("Override")) overrides_[k->AttrOr("PartName")] = k->AttrOr("ContentType");
  }
  return true;
}
std::string ContentTypes::TypeOf(const std::string& part) const {
  auto it = overrides_.find("/" + part);
  if (it != overrides_.end()) return it->second;
  size_t dot = part.rfind('.');
  if (dot != std::string::npos) { auto d = defaults_.find(LowerAscii(part.substr(dot + 1))); if (d != defaults_.end()) return d->second; }
  return {};
}
bool ContentTypes::HasOverride(const std::string& part) const { return overrides_.count("/" + part) > 0; }
std::vector<std::string> ContentTypes::OverrideParts() const {
  std::vector<std::string> v;
  for (auto& [k, t] : overrides_) v.push_back(k.empty() ? k : k.substr(1));
  return v;
}

bool VerifyOpc(ZipReader& z, const std::string& mainPart, std::wstring* why) {
  auto fail = [&](const std::wstring& m) { if (why) *why = m; return false; };
  if (!z.Has("[Content_Types].xml")) return fail(L"[Content_Types].xml is missing");
  if (!z.Has("_rels/.rels")) return fail(L"_rels/.rels is missing");
  if (!z.Has(mainPart)) return fail(L"main document part " + Widen(mainPart) + L" is missing");
  ContentTypes ct;
  if (!ct.Load(z)) return fail(L"[Content_Types].xml is not a valid content type list");
  for (auto& p : ct.OverrideParts()) if (!z.Has(p)) return fail(L"content type override for missing part " + Widen(p));
  for (auto& e : z.Entries()) {
    if (e.isDir) continue;
    std::string lower = LowerAscii(e.name);
    if (lower == "[content_types].xml") continue;
    if (ct.TypeOf(e.name).empty()) return fail(L"part " + Widen(e.name) + L" has no content type");
    bool isXml = EndsWith(lower, ".xml") || EndsWith(lower, ".rels") || EndsWith(lower, ".vml");
    if (!isXml) continue;
    Bytes b; std::wstring err;
    if (!z.Read(e.name, b, &err)) return fail(L"part " + Widen(e.name) + L" cannot be read");
    std::string xmlWhy;
    if (!xml::CheckWellFormed(b.data(), b.size(), &xmlWhy)) return fail(L"part " + Widen(e.name) + L" is not well-formed XML (" + Widen(xmlWhy) + L")");
    if (EndsWith(lower, ".rels")) {
      std::string source;
      size_t rp = e.name.rfind("_rels/");
      if (rp != std::string::npos) source = e.name.substr(0, rp) + e.name.substr(rp + 6, e.name.size() - rp - 6 - 5);
      auto dom = xml::ParseDom(std::string_view((const char*)b.data(), b.size()));
      if (!dom) return fail(L"relationships part " + Widen(e.name) + L" is malformed");
      std::set<std::string> ids;
      for (auto* r : dom->Children("Relationship")) {
        std::string id = r->AttrOr("Id");
        if (id.empty() || !ids.insert(id).second) return fail(L"duplicate or empty relationship id in " + Widen(e.name));
        if (LowerAscii(r->AttrOr("TargetMode")) == "external") continue;
        std::string target = ResolvePartPath(source, r->AttrOr("Target"));
        if (!z.Has(target)) return fail(L"relationship " + Widen(id) + L" in " + Widen(e.name) + L" points to missing part " + Widen(target));
      }
    }
  }
  return true;
}

} // namespace filo::doc
