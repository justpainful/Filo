// ValidateEntryPath / ValidateLinkTarget / DecodeLegacyName / NameResolver: malicious and awkward names.
#include "test.h"
#include "engines/archive/archive.h"
#include "engines/archive/internal.h"

using namespace filo;
using namespace filo::archive;

static PathVerdict V(const wchar_t* n) { return ValidateEntryPath(n).verdict; }

TEST(path_plain_names_are_ok) {
  CHECK(V(L"a.txt") == PathVerdict::Ok);
  CHECK(V(L"dir/sub/file.tar.gz") == PathVerdict::Ok);
  CHECK(V(L"./dir/file") == PathVerdict::Ok);
  CHECK(V(L"dir/") == PathVerdict::Ok);
  CHECK(V(L"a//b///c") == PathVerdict::Ok);
  CHECK(ValidateEntryPath(L"a//b///c").path == L"a\\b\\c");
  CHECK(ValidateEntryPath(L"dir\\sub/x.txt").path == L"dir\\sub\\x.txt");
  CHECK(ValidateEntryPath(L"./").path.empty());
  CHECK(ValidateEntryPath(L".").verdict == PathVerdict::Ok);
}

TEST(path_unicode_kept) {
  PathCheck c = ValidateEntryPath(L"\u0645\u0631\u062d\u0628\u0627/\U0001F600 \u65e5\u672c.txt");
  CHECK(c.verdict == PathVerdict::Ok);
  CHECK(c.path == L"\u0645\u0631\u062d\u0628\u0627\\\U0001F600 \u65e5\u672c.txt");
}

TEST(path_traversal_rejected) {
  const wchar_t* bad[] = {L"..", L"../evil.txt", L"..\\..\\evil.txt", L"a/../../evil", L"a/b/../../../x", L"a/..", L"..\\", L"./../x",
                          L"a\\..\\..\\x", L"x/./../../y", L"dir/..\\..\\evil"};
  for (auto* b : bad) { if (V(b) != PathVerdict::Reject) { std::wprintf(L"  not rejected: %ls\n", b); CHECK(false); } }
}

TEST(path_absolute_and_drive_rejected) {
  const wchar_t* bad[] = {L"/etc/passwd", L"\\Windows\\System32\\x.dll", L"C:\\evil.txt", L"c:/evil.txt", L"C:evil.txt", L"Z:\\", L"\\\\server\\share\\x",
                          L"//server/share/x", L"\\\\?\\C:\\x", L"\\\\.\\PhysicalDrive0", L"//?/C:/x", L"//./pipe/x"};
  for (auto* b : bad) { if (V(b) != PathVerdict::Reject) { std::wprintf(L"  not rejected: %ls\n", b); CHECK(false); } }
}

TEST(path_alternate_data_streams_rejected) {
  CHECK(V(L"file.txt:evil.exe") == PathVerdict::Reject);
  CHECK(V(L"file.txt::$DATA") == PathVerdict::Reject);
  CHECK(V(L"dir/file:stream") == PathVerdict::Reject);
  CHECK(V(L"a:b") == PathVerdict::Reject);
}

TEST(path_dots_and_spaces_components) {
  CHECK(V(L"...") == PathVerdict::Reject);
  CHECK(V(L"a/.../b") == PathVerdict::Reject);
  CHECK(V(L".. ") == PathVerdict::Reject);
  CHECK(V(L"a/ /b") == PathVerdict::Reject);
  CHECK(V(L". .") == PathVerdict::Reject);
  PathCheck c = ValidateEntryPath(L"name. ");
  CHECK(c.verdict == PathVerdict::Fixed);
  CHECK(c.path == L"name");
  CHECK(ValidateEntryPath(L"dir.\\file.txt").path == L"dir\\file.txt");
}

TEST(path_reserved_device_names_renamed) {
  const wchar_t* names[] = {L"CON", L"con", L"PRN", L"AUX", L"NUL", L"COM1", L"com9", L"LPT1", L"LPT9", L"CON.txt", L"nul.tar.gz", L"aux.c", L"CONIN$", L"CONOUT$", L"COM\u00b9"};
  for (auto* n : names) {
    PathCheck c = ValidateEntryPath(n);
    CHECK(c.verdict == PathVerdict::Fixed);
    CHECK(!c.path.empty() && c.path[0] == L'_');
  }
  CHECK(ValidateEntryPath(L"dir/NUL/file").path == L"dir\\_NUL\\file");
  CHECK(V(L"CONSOLE") == PathVerdict::Ok);
  CHECK(V(L"COM10") == PathVerdict::Ok);
  CHECK(V(L"com.txt") == PathVerdict::Ok);
  PathPolicy strict; strict.renameReserved = false;
  CHECK(ValidateEntryPath(L"NUL", strict).verdict == PathVerdict::Reject);
}

TEST(path_invalid_characters_replaced) {
  PathCheck c = ValidateEntryPath(L"a<b>c\"d|e?f*g.txt");
  CHECK(c.verdict == PathVerdict::Fixed);
  CHECK(c.path == L"a_b_c_d_e_f_g.txt");
  CHECK(ValidateEntryPath(L"tab\there").path == L"tab_here");
  PathPolicy strict; strict.replaceInvalidChars = false;
  CHECK(ValidateEntryPath(L"a?b", strict).verdict == PathVerdict::Reject);
}

TEST(path_nul_and_empty_rejected) {
  std::wstring withNul = L"abc";
  withNul.push_back(L'\0');
  withNul += L"def";
  CHECK(ValidateEntryPath(withNul).verdict == PathVerdict::Reject);
  CHECK(V(L"") == PathVerdict::Reject);
}

TEST(path_bidi_override_replaced) {
  PathCheck c = ValidateEntryPath(L"evil\u202Egpj.exe");
  CHECK(c.verdict == PathVerdict::Fixed);
  CHECK(c.path.find(L'\u202E') == std::wstring::npos);
  CHECK(V(L"\u0645\u0631\u062d\u0628\u0627\u200f.txt") == PathVerdict::Ok);   // plain RTL mark is fine
}

TEST(path_long_component_shortened_stably) {
  std::wstring longName(400, L'x');
  longName += L".txt";
  PathCheck a = ValidateEntryPath(longName), b = ValidateEntryPath(longName);
  CHECK(a.verdict == PathVerdict::Fixed);
  CHECK(a.path.size() <= 255);
  CHECK(a.path == b.path);
  CHECK(a.path.size() > 4 && a.path.substr(a.path.size() - 4) == L".txt");
  std::wstring other = std::wstring(400, L'x') + L"y.txt";
  CHECK(ValidateEntryPath(other).path != a.path);     // different long names stay distinct
}

TEST(path_total_length_budget) {
  std::wstring p;
  for (int i = 0; i < 60; ++i) p += std::wstring(200, L'd') + std::to_wstring(i) + L"/";
  p += L"file.txt";
  PathPolicy pol; pol.maxRelative = 4000;
  PathCheck c = ValidateEntryPath(p, pol);
  CHECK(c.verdict == PathVerdict::Fixed);
  CHECK(c.path.size() <= 4000);
  PathPolicy tiny; tiny.maxRelative = 10;
  CHECK(ValidateEntryPath(std::wstring(50, L'a') + L"/" + std::wstring(50, L'b'), tiny).verdict == PathVerdict::Reject);
}

TEST(path_surrogate_not_split) {
  std::wstring n;
  for (int i = 0; i < 300; ++i) n += L"\U0001F600";
  PathCheck c = ValidateEntryPath(n);
  CHECK(c.verdict == PathVerdict::Fixed);
  CHECK(c.path.size() <= 255);
  for (size_t i = 0; i < c.path.size(); ++i) {
    if (c.path[i] >= 0xD800 && c.path[i] <= 0xDBFF) { CHECK(i + 1 < c.path.size() && c.path[i + 1] >= 0xDC00 && c.path[i + 1] <= 0xDFFF); ++i; }
    else CHECK(!(c.path[i] >= 0xDC00 && c.path[i] <= 0xDFFF));
  }
}

TEST(path_result_never_escapes) {
  // property: whatever the input, an accepted path has no '..' component, no leading separator, no ':'
  const wchar_t* inputs[] = {L"a/b", L"..a/b..", L"a/..b/c", L"~/x", L"$MFT", L"x/$Recycle.Bin/y", L"a b/c d.txt", L"CON/../x", L".hidden/.x", L"a\\b\\..c", L"%00", L"..%2f..%2fx"};
  for (auto* in : inputs) {
    PathCheck c = ValidateEntryPath(in);
    if (!c.Usable()) continue;
    CHECK(c.path.empty() || (c.path[0] != L'\\' && c.path[0] != L'/'));
    CHECK(c.path.find(L':') == std::wstring::npos);
    size_t pos = 0;
    while (pos <= c.path.size()) {
      size_t nx = c.path.find(L'\\', pos);
      std::wstring comp = c.path.substr(pos, nx == std::wstring::npos ? std::wstring::npos : nx - pos);
      CHECK(comp != L"..");
      CHECK(comp != L".");
      if (nx == std::wstring::npos) break;
      pos = nx + 1;
    }
  }
}

TEST(link_targets) {
  CHECK(ValidateLinkTarget(L"dir/link", L"../other/file", false).verdict == PathVerdict::Ok);
  CHECK(ValidateLinkTarget(L"dir/link", L"../../escape", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"../escape", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"/etc/passwd", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"C:\\Windows\\win.ini", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"\\\\server\\share", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"a/b/link", L"sibling.txt", false).path == L"a\\b\\sibling.txt");
  CHECK(ValidateLinkTarget(L"a/b/link", L"../c/x", false).path == L"a\\c\\x");
  CHECK(ValidateLinkTarget(L"x/hard", L"y/file", true).path == L"y\\file");
  CHECK(ValidateLinkTarget(L"x/hard", L"../file", true).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"", false).verdict == PathVerdict::Reject);
  CHECK(ValidateLinkTarget(L"link", L"file:stream", false).verdict == PathVerdict::Reject);
}

TEST(legacy_name_decoding) {
  int cp = 0; bool legacy = false;
  CHECK(DecodeLegacyName("plain.txt", 0, &cp, &legacy) == L"plain.txt");
  CHECK(!legacy);
  std::string utf8 = "caf\xC3\xA9.txt";                 // valid UTF-8 wins
  CHECK(DecodeLegacyName(utf8, 0, &cp, &legacy) == L"caf\u00e9.txt");
  CHECK(cp == 65001);
  std::string sjis = "\x83\x65\x83\x58\x83\x67.txt";    // katakana "tesuto" in Shift-JIS
  CHECK(DecodeLegacyName(sjis, 0, &cp, &legacy) == L"\u30c6\u30b9\u30c8.txt");
  CHECK(legacy);
  CHECK(cp == 932);
  std::string gbk = "\xD6\xD0\xCE\xC4.txt";            // two Chinese characters in GBK
  CHECK(DecodeLegacyName(gbk, 936, &cp, &legacy) == L"\u4e2d\u6587.txt");
  std::wstring g = DecodeLegacyName(gbk, 0, &cp, &legacy);
  CHECK(g.size() == 6);                                // decoded as some double-byte page (2 chars + ".txt")
  std::string dos = "caf\x82.txt";                     // CP437 e-acute; not a clean pair in any DBCS page
  CHECK(DecodeLegacyName(dos, 0, &cp, &legacy) == L"caf\u00e9.txt");
  CHECK(cp == 437);
  CHECK(DecodeLegacyName(sjis, 932, &cp, &legacy) == L"\u30c6\u30b9\u30c8.txt");
  CHECK(DecodeLegacyName(dos, 437, &cp, &legacy) == L"caf\u00e9.txt");
}

TEST(name_resolver_collisions) {
  detail::NameResolver r;
  std::wstring note;
  CHECK(r.Resolve(L"a.txt", false, &note) == L"a.txt");
  CHECK(note.empty());
  CHECK(r.Resolve(L"A.TXT", false, &note) == L"A (1).TXT");        // case collision
  CHECK(!note.empty());
  note.clear();
  CHECK(r.Resolve(L"a.txt", false, &note) == L"a (2).txt");        // exact duplicate ("a (1).txt" is taken by the case collision)
  CHECK(!note.empty());
  note.clear();
  CHECK(r.Resolve(L"a.txt", false, &note) == L"a (3).txt");
  note.clear();
  CHECK(r.Resolve(L"Dir", true, &note) == L"Dir");                 // directories merge case-insensitively
  CHECK(r.Resolve(L"dir", true, &note) == L"Dir");
  CHECK(r.Resolve(L"DIR\\x.txt", false, &note) == L"Dir\\x.txt");
  CHECK(note.empty());
  CHECK(r.Resolve(L"f", false, &note) == L"f");                    // file, then a directory of the same name
  CHECK(r.Resolve(L"f\\inner.txt", false, &note) == L"f (1)\\inner.txt");
  CHECK(!note.empty());
  CHECK(r.Lookup(L"dir\\x.txt") == L"Dir\\x.txt");
}
