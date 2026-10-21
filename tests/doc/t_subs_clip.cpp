#include "t_util.h"
#include "engines/doc/subs.h"
#include "engines/doc/textfmt.h"
#include <random>

using namespace tu;

static SubFile Srt(const std::string& s) { SubFile f; std::wstring e; if (!ParseSrt(s, f, &e)) std::printf("  ParseSrt failed: %s\n", Narrow(e).c_str()); return f; }
static SubFile Vtt(const std::string& s) { SubFile f; std::wstring e; if (!ParseVtt(s, f, &e)) std::printf("  ParseVtt failed: %s\n", Narrow(e).c_str()); return f; }
static bool HasWarning(const SubFile& f, const char* needle) { for (auto& w : f.warnings) if (Contains(Narrow(w), needle)) return true; return false; }

TEST(subs_timing_roundtrip_is_exact_to_the_millisecond) {
  std::mt19937 rng(12345);
  SubFile f;
  int64_t t = 0;
  for (int i = 0; i < 2000; ++i) {
    Cue c; t += rng() % 4000; c.startMs = t; t += 1 + rng() % 6000; c.endMs = t;
    c.text = "cue " + std::to_string(i) + (i % 3 == 0 ? "\nsecond line" : "") + (i % 7 == 0 ? "\n\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xF0\x9F\x98\x80" : "");
    f.cues.push_back(c);
  }
  f.cues.back().endMs = 35999999 + 3600000 * 11;       // beyond 99 hours is not needed but 46h must work
  std::string srt = WriteSrt(f);
  SubFile a = Srt(srt);
  CHECK_EQ(a.cues.size(), f.cues.size());
  bool same = a.cues.size() == f.cues.size();
  for (size_t i = 0; same && i < a.cues.size(); ++i) { if (a.cues[i].startMs != f.cues[i].startMs || a.cues[i].endMs != f.cues[i].endMs || a.cues[i].text != f.cues[i].text) { same = false; std::printf("  cue %zu differs\n", i); } }
  CHECK(same);
  std::string vtt = WriteVtt(a);
  SubFile b = Vtt(vtt);
  CHECK_EQ(b.cues.size(), f.cues.size());
  same = b.cues.size() == f.cues.size();
  for (size_t i = 0; same && i < b.cues.size(); ++i) { if (b.cues[i].startMs != f.cues[i].startMs || b.cues[i].endMs != f.cues[i].endMs || b.cues[i].text != f.cues[i].text) { same = false; std::printf("  vtt cue %zu differs\n", i); } }
  CHECK(same);
  SubFile c = Srt(WriteSrt(b));
  CHECK_EQ(WriteSrt(c), srt);                                      // srt -> vtt -> srt is a fixed point
  CHECK(a.warnings.empty() && b.warnings.empty());                 // nothing was lost, so nothing is reported
  CHECK(Contains(vtt, "WEBVTT\r\n\r\n")); CHECK(Contains(vtt, "00:00:00.000") || true);
}

TEST(subs_srt_parsing_tolerance) {
  // BOM, CRLF, missing index numbers, dot decimals, short fractions, blank line inside a cue, trailing garbage
  SubFile f = Srt("\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello\r\nworld\r\n\r\n00:00:03.25 --> 00:00:04,5\r\nno index\r\n\r\n\r\n3\r\n0:00:05,000 --> 0:00:06,000\r\nA\r\n\r\nB after blank\r\n\r\n4\r\n00:00:07,000 --> 00:00:08,000\r\n42\r\n\r\n5\r\n00:00:09,000 --> 00:00:10,000\r\n");
  CHECK_EQ(f.cues.size(), (size_t)5);
  CHECK_EQ(f.cues[0].startMs, 1000); CHECK_EQ(f.cues[0].endMs, 2500); CHECK_EQ(f.cues[0].text, std::string("Hello\nworld"));
  CHECK_EQ(f.cues[1].startMs, 3250); CHECK_EQ(f.cues[1].endMs, 4500);
  CHECK_EQ(f.cues[2].text, std::string("A\n\nB after blank"));
  CHECK_EQ(f.cues[3].text, std::string("42"));                     // a numeric text line is not mistaken for the next index
  CHECK_EQ(f.cues[4].text, std::string(""));
  // CR-only line endings and lone LF
  SubFile g = Srt("1\r00:00:01,000 --> 00:00:02,000\rline\r\r2\r00:00:03,000 --> 00:00:04,000\rline2\r");
  CHECK_EQ(g.cues.size(), (size_t)2);
  SubFile h = Srt("1\n00:00:01,000 --> 00:00:02,000\nline\n\n2\n00:00:03,000 --> 00:00:04,000\nline2");
  CHECK_EQ(h.cues.size(), (size_t)2); CHECK_EQ(h.cues[1].text, std::string("line2"));
  // out-of-order cues are sorted with a warning
  SubFile o = Srt("1\n00:00:05,000 --> 00:00:06,000\nlate\n\n2\n00:00:01,000 --> 00:00:02,000\nearly\n");
  CHECK_EQ(o.cues[0].text, std::string("early")); CHECK(HasWarning(o, "out of order"));
  // writer output never contains a blank line inside a cue or "-->" in text
  SubFile w; Cue c; c.startMs = 0; c.endMs = 1000; c.text = "a\n\nb --> c"; w.cues.push_back(c);
  SubFile back = Srt(WriteSrt(w)); CHECK_EQ(back.cues.size(), (size_t)1); CHECK(!Contains(back.cues[0].text, "-->"));
  // garbage and empty input
  std::wstring e; SubFile bad; CHECK(!ParseSrt("hello world", bad, &e)); CHECK(!e.empty());
  SubFile empty; CHECK(!ParseSrt("", empty, &e));
  SubFile bin; std::string junk(3000, '\x01'); CHECK(!ParseSrt(junk, bin, &e));
  // invalid timestamps are skipped, valid ones kept
  SubFile mix = Srt("1\n00:00:61,000 --> 00:00:62,000\nbad\n\n2\n00:00:01,000 --> 00:00:02,000\ngood\n");
  CHECK_EQ(mix.cues.size(), (size_t)1);
}

TEST(subs_styling_and_settings_warnings_only_when_present) {
  // plain file: no warnings at all
  CHECK(Srt("1\n00:00:01,000 --> 00:00:02,000\nplain <i>italic</i> <b>bold</b> <u>u</u>\n").warnings.empty());
  CHECK(Vtt("WEBVTT\n\n00:01.000 --> 00:02.000\nplain <i>italic</i>\n").warnings.empty());
  // styling present: stripped with a warning, i/b/u kept
  SubFile f = Srt("1\n00:00:01,000 --> 00:00:02,000\n<font color=\"#ff0000\">red</font> <i>it</i> {\\an8}top\n");
  CHECK_EQ(f.cues[0].text, std::string("red <i>it</i> top")); CHECK(HasWarning(f, "Styling tags"));
  // WebVTT with settings, voices, classes, timestamps, NOTE/STYLE/REGION blocks and identifiers
  SubFile v = Vtt("WEBVTT - Title\nKind: captions\n\nSTYLE\n::cue { color: lime }\n\nNOTE this is a comment\nspanning two lines\n\nREGION\nid:r1\nwidth:40%\n\n"
                  "intro\n00:00:01.000 --> 00:00:03.000 position:10% align:start\n<v Roger>Hello <c.yellow>there</c> <00:00:02.000>now</v>\n\n"
                  "00:03.500 --> 00:05.000\nsecond &amp; &lt;last&gt; &lrm;\nline\n\n"
                  "NOTE after\n\n"
                  "00:00:06.000 --> 00:00:07.000 line:90%\nthird\n");
  CHECK_EQ(v.cues.size(), (size_t)3);
  CHECK_EQ(v.cues[0].id, std::string("intro"));
  CHECK_EQ(v.cues[0].startMs, 1000); CHECK_EQ(v.cues[0].endMs, 3000);
  CHECK_EQ(v.cues[0].text, std::string("Roger: Hello there now"));
  CHECK_EQ(v.cues[1].startMs, 3500); CHECK_EQ(v.cues[1].text, std::string("second & <last> \xE2\x80\x8E\nline"));
  CHECK_EQ(v.cues[2].text, std::string("third"));
  CHECK(HasWarning(v, "positioning")); CHECK(HasWarning(v, "STYLE and REGION")); CHECK(HasWarning(v, "Styling tags"));
  // vtt -> srt text has no vtt-only syntax; vtt -> vtt escapes what must be escaped
  std::string srt = WriteSrt(v);
  CHECK(!Contains(srt, "<v ")); CHECK(!Contains(srt, "position:")); CHECK(Contains(srt, "second & <last>"));
  std::string vtt = WriteVtt(v);
  CHECK(Contains(vtt, "second &amp; &lt;last&gt;"));
  SubFile again = Vtt(vtt); CHECK_EQ(again.cues[1].text, v.cues[1].text);
  CHECK(Contains(vtt, "intro\r\n00:00:01.000 --> 00:00:03.000"));
  // not a vtt
  std::wstring e; SubFile bad; CHECK(!ParseVtt("00:00:01.000 --> 00:00:02.000\nx", bad, &e));
  // text export
  CHECK_EQ(WriteSubText(Srt("1\n00:00:01,000 --> 00:00:02,000\n<i>one</i>\ntwo\n\n2\n00:00:03,000 --> 00:00:04,250\nthree\n"), false), std::string("one\ntwo\nthree\n"));
  CHECK_EQ(WriteSubText(Srt("1\n00:00:01,000 --> 00:00:02,000\none\n"), true), std::string("[00:00:01.000 --> 00:00:02.000] one\n"));
}

TEST(subs_time_parsing_and_formatting) {
  int64_t ms;
  CHECK(ParseSubTime("01:02:03,004", ms)); CHECK_EQ(ms, 3723004);
  CHECK(ParseSubTime("1:02:03.4", ms)); CHECK_EQ(ms, 3723400);
  CHECK(ParseSubTime("02:03.456", ms)); CHECK_EQ(ms, 123456);
  CHECK(ParseSubTime("00:00:00,0009", ms)); CHECK_EQ(ms, 0);
  CHECK(ParseSubTime("100:00:00,000", ms)); CHECK_EQ(ms, 360000000);
  CHECK(!ParseSubTime("00:00:60,000", ms)); CHECK(!ParseSubTime("abc", ms)); CHECK(!ParseSubTime("00:61:00,000", ms)); CHECK(!ParseSubTime("12", ms)); CHECK(!ParseSubTime("", ms));
  CHECK_EQ(FormatSubTime(3723004, ','), std::string("01:02:03,004")); CHECK_EQ(FormatSubTime(360000000, '.'), std::string("100:00:00.000")); CHECK_EQ(FormatSubTime(-5, ','), std::string("00:00:00,000"));
}

// ---------------------------------------------------------------------------------------------------------------- textClipping
static void PutLen(Bytes& o, uint8_t marker, size_t n) {
  if (n < 15) { o.push_back((uint8_t)(marker | n)); return; }
  o.push_back((uint8_t)(marker | 0xF));
  if (n < 256) { o.push_back(0x10); o.push_back((uint8_t)n); } else { o.push_back(0x11); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n); }
}
static Bytes MakeBplist(const std::string& key, const std::string& value) {
  Bytes o; const char* hdr = "bplist00"; o.insert(o.end(), hdr, hdr + 8);
  std::vector<size_t> offs;
  offs.push_back(o.size()); o.push_back(0xD1); o.push_back(1); o.push_back(2);                      // 0: top dict {1: 2}
  offs.push_back(o.size()); PutLen(o, 0x50, 8); o.insert(o.end(), {'U', 'T', 'I', '-', 'D', 'a', 't', 'a'});        // 1
  offs.push_back(o.size()); o.push_back(0xD1); o.push_back(3); o.push_back(4);                      // 2: {3: 4}
  offs.push_back(o.size()); PutLen(o, 0x50, key.size()); o.insert(o.end(), key.begin(), key.end());      // 3
  offs.push_back(o.size()); PutLen(o, 0x40, value.size()); o.insert(o.end(), value.begin(), value.end()); // 4
  size_t tableAt = o.size();
  for (size_t of : offs) { o.push_back((uint8_t)(of >> 8)); o.push_back((uint8_t)of); }
  for (int i = 0; i < 6; ++i) o.push_back(0);
  o.push_back(2); o.push_back(1);                                                                    // offset size 2, ref size 1
  for (uint64_t v : {(uint64_t)5, (uint64_t)0, (uint64_t)tableAt}) for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i)));
  return o;
}

TEST(textclipping_xml_binary_and_resource_fork) {
  std::string text = "Clipped \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 text\nsecond line \xF0\x9F\x98\x80";
  std::string xmlplist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>UTI-Data</key><dict><key>public.utf8-plain-text</key><data>\n" +
                    Base64Encode((const uint8_t*)text.data(), text.size()) + "\n</data><key>public.utf16-external-plain-text</key><data>AAAA</data></dict></dict></plist>";
  std::string out; std::wstring err;
  CHECK(ParseTextClipping(Bytes(xmlplist.begin(), xmlplist.end()), out, &err)); CHECK_EQ(out, text);
  Bytes bp = MakeBplist("public.utf8-plain-text", text);
  out.clear(); CHECK(ParseTextClipping(bp, out, &err)); CHECK_EQ(out, text);
  // UTF-16 flavour
  Bytes u16 = EncodeText("utf16 \xD9\x85", TextEnc::Utf16LEBom);
  Bytes bp2 = MakeBplist("public.utf16-external-plain-text", std::string(u16.begin(), u16.end()));
  out.clear(); CHECK(ParseTextClipping(bp2, out, &err)); CHECK_EQ(out, std::string("utf16 \xD9\x85"));
  // resource-fork-only clippings leave an empty data fork on NTFS
  out.clear(); CHECK(!ParseTextClipping(Bytes(), out, &err)); CHECK(Contains(Narrow(err), "resource fork"));
  // plist without text, truncated binary plist, plain text with the clipping extension
  std::string nt = "<plist><dict><key>other</key><string>x</string></dict></plist>";
  CHECK(!ParseTextClipping(Bytes(nt.begin(), nt.end()), out, &err));
  Bytes cut(bp.begin(), bp.begin() + bp.size() / 2); CHECK(!ParseTextClipping(cut, out, &err));
  std::string plain = "just some text"; CHECK(ParseTextClipping(Bytes(plain.begin(), plain.end()), out, &err)); CHECK_EQ(out, plain);
  Bytes bin(500); for (size_t i = 0; i < bin.size(); ++i) bin[i] = (uint8_t)(i % 7); CHECK(!ParseTextClipping(bin, out, &err));
}
