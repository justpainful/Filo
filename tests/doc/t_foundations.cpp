#include "t_util.h"
#include "engines/doc/text_io.h"
#include "engines/doc/xml.h"
#include "engines/doc/zip.h"
#include "engines/doc/sheet.h"
#include "core/process.h"

using namespace tu;

// ---------------------------------------------------------------------------------------------------------------- ZIP
TEST(zip_roundtrip_unicode_names) {
  fs::path dir = Work("zip_roundtrip");
  fs::path z = dir / L"a.zip";
  std::string big(200000, 'x'); for (size_t i = 0; i < big.size(); i += 7) big[i] = (char)('a' + i % 26);
  {
    ZipWriter w;
    CHECK(w.Open(z));
    CHECK(w.Add("mimetype", std::string("application/epub+zip"), false));
    CHECK(w.Add("dir/\xD9\x85\xD9\x84\xD9\x81.txt", std::string("arabic name"), true));        // "ملف.txt"
    CHECK(w.Add("big.bin", big, true));
    CHECK(w.Add("empty.txt", std::string(), true));
    CHECK(w.Close());
  }
  ZipReader r;
  std::wstring err;
  CHECK(r.Open(z, &err));
  CHECK_EQ(r.Entries().size(), (size_t)4);
  CHECK_EQ(r.Entries()[0].name, std::string("mimetype"));
  CHECK_EQ(r.Entries()[0].csize, r.Entries()[0].size);           // stored
  Bytes b;
  CHECK(r.Read("big.bin", b)); CHECK(std::string(b.begin(), b.end()) == big);
  CHECK(r.Entries()[2].csize < big.size() / 3);                  // deflated
  auto s = r.ReadString("dir/\xD9\x85\xD9\x84\xD9\x81.txt");
  CHECK(s && *s == "arabic name");
  CHECK(r.Has("EMPTY.TXT"));                                     // case-insensitive fallback
  CHECK(!r.Has("missing"));
  // first local header: no extra field, UTF-8 flag only for non-ASCII names
  Bytes raw; ReadFileBytes(z, raw, 1 << 20);
  CHECK_EQ((int)(raw[28] | (raw[29] << 8)), 0);
  CHECK_EQ((int)(raw[6] | (raw[7] << 8)), 0);
  // independent reader: Windows tar (bsdtar) lists the entries
  int code = -1;
  std::string listing = RunCapture(fs::path(_wgetenv(L"SystemRoot")) / L"System32" / L"tar.exe", {L"-tf", z.wstring()}, 30000, &code);
  CHECK_EQ(code, 0);
  CHECK(Contains(listing, "mimetype"));
  CHECK(Contains(listing, "big.bin"));
}

TEST(zip_slip_is_blocked) {
  CHECK(!IsSafeZipEntryName("../evil.txt"));
  CHECK(!IsSafeZipEntryName("a/../../evil.txt"));
  CHECK(!IsSafeZipEntryName("/abs/evil.txt"));
  CHECK(!IsSafeZipEntryName("C:\\evil.txt"));
  CHECK(!IsSafeZipEntryName("C:/evil.txt"));
  CHECK(!IsSafeZipEntryName("..\\evil.txt"));
  CHECK(!IsSafeZipEntryName("file.txt:stream"));
  CHECK(!IsSafeZipEntryName("nul.txt"));
  CHECK(!IsSafeZipEntryName("dir/COM1"));
  CHECK(!IsSafeZipEntryName("trailing.dot."));
  CHECK(IsSafeZipEntryName("word/document.xml"));
  CHECK(IsSafeZipEntryName("dir/sub/\xD9\x85.txt"));

  fs::path dir = Work("zip_slip");
  fs::path z = dir / L"evil.zip";
  {
    ZipWriter w; w.Open(z);
    w.Add("good/a.txt", std::string("ok"));
    w.Add("../outside.txt", std::string("evil"));
    w.Add("sub/../../outside2.txt", std::string("evil"));
    w.Add("C:/Windows/Temp/filo-evil.txt", std::string("evil"));
    w.Add("/rooted.txt", std::string("evil"));
    w.Add("..\\outside3.txt", std::string("evil"));
    w.Close();
  }
  ZipReader r; CHECK(r.Open(z));
  int skipped = 0; std::wstring err;
  fs::path dest = dir / L"out" / L"x";
  CHECK(r.ExtractAll(dest, &err, &skipped));
  CHECK_EQ(skipped, 5);
  CHECK(fs::exists(dest / L"good" / L"a.txt"));
  CHECK(!fs::exists(dir / L"out" / L"outside.txt"));
  CHECK(!fs::exists(dir / L"outside2.txt"));
  CHECK(!fs::exists(dir / L"outside3.txt"));
  CHECK(!fs::exists(L"C:\\Windows\\Temp\\filo-evil.txt"));
  CHECK(!fs::exists(dest / L"rooted.txt"));
}

TEST(zip_bomb_and_corrupt_inputs) {
  fs::path dir = Work("zip_bomb");
  // 48 MB of zeros compresses to ~48 KB: ratio above the bomb threshold for entries over 16 MiB
  fs::path z = dir / L"bomb.zip";
  { ZipWriter w; w.Open(z); std::string zeros(48u << 20, '\0'); w.Add("zeros.bin", zeros); w.Close(); }
  ZipReader r; std::wstring err;
  CHECK(!r.Open(z, &err));
  CHECK(Contains(Narrow(err), "bomb"));
  // size limits
  ZipLimits lim; lim.maxEntryBytes = 1 << 20; lim.maxTotalBytes = 3 << 20; lim.bombRatio = 100000;
  fs::path z2 = dir / L"limits.zip";
  { ZipWriter w; w.Open(z2); std::string a(2 << 20, 'a'); w.Add("a.bin", a); w.Add("b.bin", std::string("small")); w.Close(); }
  ZipReader r2; CHECK(r2.Open(z2, &err, &lim));
  Bytes b; CHECK(!r2.Read("a.bin", b, &err));                    // over the per-entry limit
  CHECK(r2.Read("b.bin", b));
  ZipLimits tot; tot.maxTotalBytes = 1 << 20; tot.bombRatio = 100000;
  ZipReader r3; CHECK(!r3.Open(z2, &err, &tot));
  // corrupt / truncated / garbage
  Bytes raw; ReadFileBytes(z2, raw, 1 << 26);
  fs::path trunc = dir / L"trunc.zip"; WriteFileBytes(trunc, raw.data(), raw.size() / 2);
  ZipReader r4; CHECK(!r4.Open(trunc, &err)); CHECK(!err.empty());
  fs::path garbage = dir / L"garbage.zip"; WriteAll(garbage, std::string(5000, 'Z'));
  ZipReader r5; CHECK(!r5.Open(garbage, &err));
  fs::path empty = dir / L"empty.zip"; WriteAll(empty, "");
  ZipReader r6; CHECK(!r6.Open(empty, &err));
  ZipReader r7; CHECK(!r7.Open(dir / L"does-not-exist.zip", &err));
  // flipped byte inside the compressed stream is caught by the CRC check
  Bytes bad = raw; bad[40] ^= 0xFF;
  fs::path flipped = dir / L"flipped.zip"; WriteFileBytes(flipped, bad.data(), bad.size());
  ZipReader r8; bool opened = r8.Open(flipped, &err);
  if (opened) { Bytes x; bool a = r8.Read("a.bin", x, &err), c = r8.Read("b.bin", x, &err); CHECK(!(a && c)); }
}

// ---------------------------------------------------------------------------------------------------------------- text encodings
TEST(text_encoding_detection) {
  auto dec = [](const std::string& s) { return DecodeText((const uint8_t*)s.data(), s.size()); };
  const std::string ar = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";   // مرحبا
  { auto d = dec("\xEF\xBB\xBF" + ar); CHECK_EQ(d.utf8, ar); CHECK(d.hadBom); CHECK(d.detected == TextEnc::Utf8Bom); }
  { auto d = dec(ar + " plain"); CHECK_EQ(d.utf8, ar + " plain"); CHECK(!d.hadBom); CHECK(d.detected == TextEnc::Utf8); }
  { std::string w = "\xFF\xFE"; for (char c : std::string("Hi\xD9\x85")) (void)c; Bytes b = EncodeText("Hi " + ar, TextEnc::Utf16LEBom); auto d = DecodeText(b); CHECK_EQ(d.utf8, "Hi " + ar); CHECK(d.detected == TextEnc::Utf16LEBom); }
  { Bytes b = EncodeText("Hi " + ar, TextEnc::Utf16BEBom); auto d = DecodeText(b); CHECK_EQ(d.utf8, "Hi " + ar); }
  { Bytes b = EncodeText("Hello world, no BOM here", TextEnc::Utf16LE); auto d = DecodeText(b); CHECK_EQ(d.utf8, "Hello world, no BOM here"); CHECK(d.detected == TextEnc::Utf16LE); }
  { Bytes b = EncodeText("Hello world, no BOM here", TextEnc::Utf16BE); auto d = DecodeText(b); CHECK_EQ(d.utf8, "Hello world, no BOM here"); CHECK(d.detected == TextEnc::Utf16BE); }
  { Bytes b = EncodeText("caf\xC3\xA9 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xF0\x9F\x98\x80", TextEnc::Utf32LE); auto d = DecodeText(b); CHECK(Contains(d.utf8, "\xF0\x9F\x98\x80")); }
  // windows-1256 Arabic text without BOM must not be read as Latin-1 mojibake
  { Bytes b = EncodeText(ar + " " + ar + " " + ar + " " + ar, TextEnc::Cp1256); auto d = DecodeText(b); CHECK(d.detected == TextEnc::Cp1256); CHECK(Contains(d.utf8, ar)); }
  // windows-1252 accents
  { Bytes b = EncodeText("caf\xC3\xA9 na\xC3\xAFve", TextEnc::Cp1252); auto d = DecodeText(b); CHECK_EQ(d.utf8, std::string("caf\xC3\xA9 na\xC3\xAFve")); }
  // forced encoding
  { Bytes b = EncodeText(ar, TextEnc::Cp1256); auto d = DecodeText(b, TextEnc::Cp1256); CHECK_EQ(d.utf8, ar); }
  // invalid UTF-8 is repaired, never crashes
  { std::string bad = "ok \xFF\xFE broken \xC3"; auto d = DecodeText((const uint8_t*)bad.data(), bad.size(), TextEnc::Utf8); CHECK(IsValidUtf8(d.utf8)); CHECK(d.hadErrors); }
  CHECK(ParseEncodingName("UTF-8") == TextEnc::Utf8);
  CHECK(ParseEncodingName("utf-8-bom") == TextEnc::Utf8Bom);
  CHECK(ParseEncodingName("Windows-1256") == TextEnc::Cp1256);
  CHECK(ParseEncodingName("whatever") == TextEnc::Auto);
}

TEST(text_sanitise_control_characters) {
  std::string dirty = std::string("a\x01" "b\x08" "c\t" "d\x0B" "e\x1F" "f\xEF\xBF\xBE" "g\xED\xA0\x80" "h\n");     // C0 controls, U+FFFE, surrogate
  std::string clean = SanitizeText(dirty);
  CHECK(Contains(clean, "abc\td"));
  CHECK(!Contains(clean, "\x01")); CHECK(!Contains(clean, "\x0B"));
  CHECK(IsValidUtf8(clean));
  std::string esc = XmlEscape("<a & \"b\" > \x01 end");
  CHECK(Contains(esc, "&lt;a &amp; ")); CHECK(!Contains(esc, "\x01"));
  CHECK(XmlEscape("a\"b", true) == "a&quot;b");
  std::u32string cp = ToCodepoints("a\xF0\x9F\x98\x80z");
  CHECK_EQ(cp.size(), (size_t)3);
  CHECK_EQ(FromCodepoints(cp), std::string("a\xF0\x9F\x98\x80z"));
}

TEST(text_direction_helpers) {
  CHECK(DetectRtl("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 hello"));
  CHECK(!DetectRtl("hello \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"));
  CHECK(!DetectRtl("123 456"));
  CHECK(DetectRtl("123 \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"));       // Hebrew after digits
  auto segs = SplitByDirection("abc \xD9\x85\xD8\xB1 def 12");
  CHECK(segs.size() >= 3);
  CHECK(!segs[0].second); CHECK(segs[1].second);
  std::string joined; for (auto& s : segs) joined += s.first;
  CHECK_EQ(joined, std::string("abc \xD9\x85\xD8\xB1 def 12"));
}

TEST(base64_roundtrip) {
  for (size_t n : {0, 1, 2, 3, 4, 5, 100, 1001}) {
    Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)(i * 37 + 11);
    std::string e = Base64Encode(b.data(), b.size());
    CHECK(Base64Decode(e) == b);
  }
  CHECK(Base64Decode("SGVs\nbG8g\r\nV29ybGQ=") == Bytes({'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'}));
}

// ---------------------------------------------------------------------------------------------------------------- XML
TEST(xml_parser_and_xmllite) {
  std::string x = "<?xml version=\"1.0\"?><!DOCTYPE r [<!ENTITY e \"x\">]><r xmlns:w=\"urn:w\" a=\"1 &amp; 2\"><w:p w:val=\"v\">t&lt;x&gt; &#65;&#x42; <![CDATA[<raw>]]> &copy;</w:p><!-- c --><e/></r>";
  std::string err;
  auto dom = xml::ParseDom(x, &err);
  CHECK(dom != nullptr);
  CHECK_EQ(dom->AttrOr("a"), std::string("1 & 2"));
  auto* p = dom->Child("p");
  CHECK(p != nullptr);
  CHECK_EQ(p->AttrOr("val"), std::string("v"));
  CHECK_EQ(p->InnerText(), std::string("t<x> AB <raw> \xC2\xA9"));
  CHECK(dom->Child("e") != nullptr);
  CHECK(xml::CheckWellFormed("<a><b>x</b></a>"));
  std::string why;
  CHECK(!xml::CheckWellFormed("<a><b>x</a></b>", &why)); CHECK(!why.empty());
  CHECK(!xml::CheckWellFormed("<a>&nbsp;</a>"));              // undefined entity: strict readers reject
  CHECK(!xml::CheckWellFormed("<a>unterminated"));
  CHECK(!xml::CheckWellFormed(""));
  // tolerant parser never crashes on garbage
  for (const char* g : {"", "<", "<<<>>>", "<a", "<a b=\"", "</a>", "text only", "<a><b></a>", "<![CDATA[", "<?xml", "\xFF\xFE\x00"}) { std::string e2; xml::ParseDom(std::string(g), &e2); }
  // nesting bomb is rejected, not recursed into
  std::string deep; for (int i = 0; i < 100000; ++i) deep += "<a>";
  std::string e3; auto d2 = xml::ParseDom(deep, &e3);
  CHECK(d2 == nullptr);
  // UTF-16 input
  Bytes u16 = EncodeText("<r>\xD9\x85</r>", TextEnc::Utf16LEBom);
  auto d3 = xml::ParseDom(std::string_view((const char*)u16.data(), u16.size()));
  CHECK(d3 && d3->InnerText() == "\xD9\x85");
}

TEST(date_and_number_helpers) {
  int y, m, d, hh, mm, ss, ms;
  CHECK(DateFromSerial(45000.0, false, y, m, d, hh, mm, ss, ms)); CHECK(y == 2023 && m == 3 && d == 15);
  CHECK(DateFromSerial(61.0, false, y, m, d, hh, mm, ss, ms)); CHECK(y == 1900 && m == 3 && d == 1);
  CHECK(DateFromSerial(59.0, false, y, m, d, hh, mm, ss, ms)); CHECK(y == 1900 && m == 2 && d == 28);
  CHECK(DateFromSerial(1.0, false, y, m, d, hh, mm, ss, ms)); CHECK(y == 1900 && m == 1 && d == 1);
  CHECK(DateFromSerial(45000.75, false, y, m, d, hh, mm, ss, ms)); CHECK(hh == 18 && mm == 0 && ss == 0);
  CHECK_NEAR(SerialFromDate(2023, 3, 15), 45000, 0);
  CHECK_NEAR(SerialFromDate(1900, 3, 1), 61, 0);
  CHECK_NEAR(SerialFromDate(1900, 1, 1), 1, 0);
  for (int serial : {1, 59, 61, 100, 25569, 36526, 44927, 45000, 73050, 2958465}) {
    CHECK(DateFromSerial(serial, false, y, m, d, hh, mm, ss, ms));
    CHECK_NEAR(SerialFromDate(y, m, d), serial, 0);
  }
  CHECK_EQ(FormatNumber(1000000), std::string("1000000"));
  CHECK_EQ(FormatNumber(0.1), std::string("0.1"));
  CHECK_EQ(FormatNumber(-12.5), std::string("-12.5"));
  CHECK_EQ(FormatNumber(1e22), std::string("1e+22"));
  CHECK_EQ(FormatNumber(123456789012345.0), std::string("123456789012345"));
  CHECK(ValidDate(2024, 2, 29)); CHECK(!ValidDate(2023, 2, 29)); CHECK(!ValidDate(2023, 13, 1)); CHECK(!ValidDate(1899, 12, 31));
  std::vector<std::string> used = {"Sheet1", "sheet1 (2)"};
  CHECK_EQ(SafeSheetName("Sheet1", used), std::string("Sheet1 (3)"));
  CHECK_EQ(SafeSheetName("a/b:c*d?[e]\\f", {}), std::string("a_b_c_d__e__f"));
  CHECK_EQ(Utf8Length(SafeSheetName(std::string(100, 'x'), {})), (size_t)31);
  CHECK_EQ(SafeSheetName("", {}), std::string("Sheet"));
}
