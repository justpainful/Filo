// Entry-name validation (zip-slip & friends), legacy-encoding heuristics and destination name bookkeeping.
#include "engines/archive/internal.h"
#include <algorithm>
#include <unordered_map>

namespace filo::archive {

namespace {

bool IsSep(wchar_t c) { return c == L'/' || c == L'\\'; }

std::vector<std::wstring> SplitSep(std::wstring_view s) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = i;
    while (j < s.size() && !IsSep(s[j])) ++j;
    out.emplace_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

bool IsAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

uint32_t Fnv(std::wstring_view s) {
  uint32_t h = 2166136261u;
  for (wchar_t c : s) { h ^= (uint16_t)c; h *= 16777619u; }
  return h;
}

bool IsReservedDeviceName(const std::wstring& comp) {
  size_t dot = comp.find(L'.');
  std::wstring base = comp.substr(0, dot);
  while (!base.empty() && base.back() == L' ') base.pop_back();
  std::wstring u = base;
  for (auto& c : u) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
  if (u == L"CON" || u == L"PRN" || u == L"AUX" || u == L"NUL" || u == L"CONIN$" || u == L"CONOUT$" || u == L"CLOCK$") return true;
  if (u.size() == 4 && (u.compare(0, 3, L"COM") == 0 || u.compare(0, 3, L"LPT") == 0)) {
    wchar_t d = u[3];
    if ((d >= L'0' && d <= L'9') || d == 0xB9 || d == 0xB2 || d == 0xB3) return true;
  }
  return false;
}

// Cuts `comp` down to <= limit UTF-16 units, keeping a short extension and adding a hash so different long names stay distinct.
std::wstring ShortenComponent(const std::wstring& comp, size_t limit) {
  if (comp.size() <= limit) return comp;
  limit = std::max<size_t>(limit, 16);
  size_t dot = comp.rfind(L'.');
  std::wstring ext;
  if (dot != std::wstring::npos && dot > 0 && comp.size() - dot <= 16) ext = comp.substr(dot);
  size_t keep = limit - ext.size() - 9;
  std::wstring stem = comp.substr(0, std::min(keep, comp.size()));
  if (!stem.empty() && stem.back() >= 0xD800 && stem.back() <= 0xDBFF) stem.pop_back();   // do not split a surrogate pair
  wchar_t h[16];
  swprintf_s(h, L"~%08x", Fnv(comp));
  return stem + h + ext;
}

bool BadChar(wchar_t c) { return c < 0x20 || c == 0x7F || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*'; }
bool BidiOverride(wchar_t c) { return c == 0x202D || c == 0x202E; }

} // namespace

PathCheck ValidateEntryPath(std::wstring_view name, const PathPolicy& pol) {
  PathCheck r;
  auto reject = [&](const wchar_t* why) { r.verdict = PathVerdict::Reject; r.path.clear(); r.reason = why; return r; };
  if (name.find(L'\0') != std::wstring_view::npos) return reject(L"the name contains a NUL character");
  if (name.empty()) return reject(L"the name is empty");
  if (name.size() >= 4 && IsSep(name[0]) && IsSep(name[1]) && (name[2] == L'?' || name[2] == L'.') && IsSep(name[3]))
    return reject(L"it uses a device / extended-length path prefix");
  if (IsSep(name[0])) return reject(name.size() > 1 && IsSep(name[1]) ? L"it is a network (UNC) path" : L"it is an absolute path");
  if (name.size() >= 2 && IsAlpha(name[0]) && name[1] == L':') return reject(L"it starts with a drive letter");
  if (name.find(L':') != std::wstring_view::npos) return reject(L"it contains ':' (drive or alternate data stream)");

  std::vector<std::wstring> out;
  auto note = [&](std::wstring n) { r.notes.push_back(std::move(n)); };
  for (auto& c0 : SplitSep(name)) {
    if (c0.empty() || c0 == L".") continue;
    if (c0 == L"..") return reject(L"it contains a '..' component (path traversal)");
    if (c0.find_first_not_of(L". ") == std::wstring::npos) return reject(L"it contains a component made only of dots or spaces");
    std::wstring c = c0;
    bool badChars = false, bidi = false;
    for (auto& ch : c) {
      if (BidiOverride(ch)) { ch = L'_'; bidi = true; }
      else if (BadChar(ch)) { if (!pol.replaceInvalidChars) return reject(L"it contains characters Windows does not allow"); ch = L'_'; badChars = true; }
    }
    if (badChars) note(L"characters Windows does not allow were replaced in '" + c0 + L"'");
    if (bidi) note(L"a text-direction override character was replaced in a file name");
    size_t before = c.size();
    while (!c.empty() && (c.back() == L'.' || c.back() == L' ')) c.pop_back();
    if (c.size() != before) note(L"trailing dots / spaces were removed from '" + c0 + L"'");
    if (IsReservedDeviceName(c)) {
      if (!pol.renameReserved) return reject(L"it uses a reserved Windows device name");
      note(L"reserved Windows name '" + c + L"' was renamed");
      c = L"_" + c;
    }
    if (c.size() > pol.maxComponent) {
      note(L"a very long name was shortened ('" + c.substr(0, 24) + L"...')");
      c = ShortenComponent(c, pol.maxComponent);
    }
    out.push_back(std::move(c));
  }
  auto total = [&] { size_t t = 0; for (auto& c : out) t += c.size() + 1; return t ? t - 1 : 0; };
  if (total() > pol.maxRelative) {
    bool shortened = false;
    for (int guard = 0; guard < 4096 && total() > pol.maxRelative; ++guard) {
      auto it = std::max_element(out.begin(), out.end(), [](auto& a, auto& b) { return a.size() < b.size(); });
      if (it->size() <= 20) return reject(L"its path is too long for Windows");
      *it = ShortenComponent(*it, std::max<size_t>(20, it->size() / 2));
      shortened = true;
    }
    if (total() > pol.maxRelative) return reject(L"its path is too long for Windows");
    if (shortened) note(L"a very long path was shortened");
  }
  std::wstring joined;
  for (size_t i = 0; i < out.size(); ++i) { if (i) joined.push_back(L'\\'); joined += out[i]; }
  r.path = std::move(joined);
  r.verdict = r.notes.empty() ? PathVerdict::Ok : PathVerdict::Fixed;
  if (!r.notes.empty()) r.reason = r.notes.front();
  return r;
}

PathCheck ValidateLinkTarget(std::wstring_view entryName, std::wstring_view target, bool hardLink) {
  PathCheck r;
  auto reject = [&](const wchar_t* why) { r.verdict = PathVerdict::Reject; r.path.clear(); r.reason = why; return r; };
  if (target.empty()) return reject(L"the link has no target");
  if (target.find(L'\0') != std::wstring_view::npos) return reject(L"the link target contains a NUL character");
  if (IsSep(target[0])) return reject(L"the link points to an absolute path");
  if (target.size() >= 2 && IsAlpha(target[0]) && target[1] == L':') return reject(L"the link points to another drive");
  if (target.find(L':') != std::wstring_view::npos) return reject(L"the link target contains ':'");
  std::vector<std::wstring> stack;
  if (!hardLink) {
    auto base = SplitSep(entryName);
    if (!base.empty()) base.pop_back();
    for (auto& c : base) {
      if (c.empty() || c == L".") continue;
      if (c == L"..") { if (stack.empty()) return reject(L"the link's own location is outside the archive"); stack.pop_back(); }
      else stack.push_back(c);
    }
  }
  for (auto& c : SplitSep(target)) {
    if (c.empty() || c == L".") continue;
    if (c == L"..") { if (stack.empty()) return reject(L"the link points outside the extraction folder"); stack.pop_back(); }
    else stack.push_back(c);
  }
  for (size_t i = 0; i < stack.size(); ++i) { if (i) r.path.push_back(L'\\'); r.path += stack[i]; }
  r.verdict = PathVerdict::Ok;
  return r;
}

// ---- legacy encodings ---------------------------------------------------------------------------------------------------------

namespace {
bool StrictDecode(const std::string& b, UINT cp, std::wstring* out) {
  if (b.empty()) { out->clear(); return true; }
  int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), nullptr, 0);
  if (n <= 0) return false;
  out->assign(n, L'\0');
  MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), out->data(), n);
  return true;
}
std::wstring LooseDecode(const std::string& b, UINT cp) {
  if (b.empty()) return {};
  int n = MultiByteToWideChar(cp, 0, b.data(), (int)b.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(cp, 0, b.data(), (int)b.size(), w.data(), n);
  return w;
}
bool CjkText(wchar_t c) {
  return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0xAC00 && c <= 0xD7AF) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x2000 && c <= 0x206F) || (c >= 0xF900 && c <= 0xFAFF);
}
}

std::wstring DecodeLegacyName(const std::string& b, int forced, int* chosen, bool* legacy) {
  if (legacy) *legacy = false;
  bool ascii = std::all_of(b.begin(), b.end(), [](char c) { return (unsigned char)c < 0x80; });
  std::wstring w;
  if (ascii) { if (chosen) *chosen = 65001; return LooseDecode(b, CP_UTF8); }
  if (forced > 0) {
    if (StrictDecode(b, (UINT)forced, &w) || forced == 437) { if (chosen) *chosen = forced; if (legacy) *legacy = forced != 65001; return w.empty() ? LooseDecode(b, (UINT)forced) : w; }
  }
  if (StrictDecode(b, CP_UTF8, &w)) { if (chosen) *chosen = 65001; return w; }
  if (legacy) *legacy = true;
  const UINT cands[] = {936, 932, 949, 950};
  std::vector<std::pair<UINT, std::wstring>> good;
  for (UINT cp : cands) {
    std::wstring d;
    if (!StrictDecode(b, cp, &d)) continue;
    bool allCjk = true;
    for (wchar_t c : d) if (c >= 0x80 && !CjkText(c) && !(c >= 0xFF61 && c <= 0xFF9F)) { allCjk = false; break; }
    if (allCjk && d.size() < b.size()) good.emplace_back(cp, std::move(d));
  }
  if (!good.empty()) {
    UINT acp = detail::SystemAnsiCodePage();
    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < good.size(); ++i) if (good[i].first == acp) { pick = i; found = true; break; }
    if (!found) {
      for (size_t i = 0; i < good.size(); ++i) {   // kana in the Shift-JIS reading is a strong sign of Japanese text
        if (good[i].first == 932 && std::any_of(good[i].second.begin(), good[i].second.end(), [](wchar_t c) { return c >= 0x3040 && c <= 0x30FF; })) { pick = i; found = true; break; }
      }
    }
    if (chosen) *chosen = (int)good[pick].first;
    return good[pick].second;
  }
  if (chosen) *chosen = 437;
  return LooseDecode(b, 437);
}

} // namespace filo::archive

namespace filo::archive::detail {

std::wstring NormalizeSeparators(std::wstring_view s) {
  std::wstring r(s);
  std::replace(r.begin(), r.end(), L'\\', L'/');
  return r;
}
std::string ToUtf8(std::wstring_view w) { return Narrow(w); }

struct NameResolver::Map {
  struct Node { std::wstring actual; bool isDir; };
  std::unordered_map<std::wstring, Node> nodes;          // lower(actual path) -> node
  std::unordered_map<std::wstring, std::wstring> byOriginal;
};
NameResolver::NameResolver() : m_(std::make_shared<Map>()) {}

std::wstring NameResolver::Lookup(const std::wstring& rel) const {
  auto it = m_->byOriginal.find(ToLower(rel));
  return it == m_->byOriginal.end() ? std::wstring() : it->second;
}

std::wstring NameResolver::Resolve(const std::wstring& rel, bool isDir, std::wstring* note) {
  std::vector<std::wstring> comps = SplitSep(rel);
  std::wstring prefix;
  for (size_t i = 0; i < comps.size(); ++i) {
    bool last = i + 1 == comps.size();
    bool wantDir = !last || isDir;
    std::wstring base = comps[i];
    auto keyFor = [&](const std::wstring& c) { return ToLower(prefix.empty() ? c : prefix + L"\\" + c); };
    auto it = m_->nodes.find(keyFor(base));
    if (it != m_->nodes.end()) {
      if (it->second.isDir && wantDir) { prefix = it->second.actual; continue; }
      std::wstring stem = base, ext;
      if (!wantDir) { size_t dot = base.rfind(L'.'); if (dot != std::wstring::npos && dot > 0) { stem = base.substr(0, dot); ext = base.substr(dot); } }
      std::wstring cand;
      for (int k = 1; k < 1000000; ++k) {
        cand = stem + L" (" + std::to_wstring(k) + L")" + ext;
        if (m_->nodes.find(keyFor(cand)) == m_->nodes.end()) break;
      }
      if (note) *note = L"'" + base + L"' already exists in the same folder (names are case-insensitive on Windows); saved as '" + cand + L"'";
      base = cand;
    }
    std::wstring actual = prefix.empty() ? base : prefix + L"\\" + base;
    m_->nodes[ToLower(actual)] = {actual, wantDir};
    prefix = actual;
  }
  m_->byOriginal[ToLower(rel)] = prefix;
  return prefix;
}

} // namespace filo::archive::detail
