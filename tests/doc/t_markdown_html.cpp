#include "t_util.h"
#include "engines/doc/html.h"
#include "engines/doc/markdown.h"
#include "engines/doc/textfmt.h"
#include "engines/doc/xml.h"

using namespace tu;

static std::string Sg(const std::string& md, bool fmt = true) { return SigStr(ParseMarkdown(md), fmt); }

TEST(md_basic_blocks) {
  Document d = ParseMarkdown("# Title\n\nPara one\nwraps here.\n\n## Sub *emph*\n\n- a\n- b\n  - nested\n- c\n\n1. one\n2. two\n\n> quote line\n\n---\n\n```cpp\nint x = 1;\n  return x;\n```\n");
  auto s = Sig(d, true);
  size_t i = 0;
  CHECK_EQ(s[i++], std::string("H1:Title"));
  CHECK_EQ(s[i++], std::string("P:Para one wraps here."));
  CHECK_EQ(s[i++], std::string("H2:Sub _emph_"));
  CHECK_EQ(s[i++], std::string("L0u:a"));
  CHECK_EQ(s[i++], std::string("L0u:b"));
  CHECK_EQ(s[i++], std::string("L1u:nested"));
  CHECK_EQ(s[i++], std::string("L0u:c"));
  CHECK_EQ(s[i++], std::string("L0o:one"));
  CHECK_EQ(s[i++], std::string("L0o:two"));
  CHECK_EQ(s[i++], std::string("Q:quote line"));
  CHECK_EQ(s[i++], std::string("R"));
  CHECK_EQ(s[i++], std::string("C:int x = 1;\n  return x;"));
  CHECK_EQ(s.size(), i);
  CHECK_EQ(d.blocks[11].lang, std::string("cpp"));
  CHECK_EQ(d.meta.title, std::string("Title"));
}

TEST(md_inline_emphasis_matrix) {
  CHECK_EQ(Sg("**bold** and *it* and ***both***"), std::string("P:**bold** and _it_ and _**both**_\n"));
  CHECK_EQ(Sg("__bold__ and _it_"), std::string("P:**bold** and _it_\n"));
  CHECK_EQ(Sg("**bold *nested italic* bold**"), std::string("P:**bold **_**nested italic**_** bold**\n"));
  CHECK_EQ(Sg("snake_case_word stays and 2 * 3 * 4 too"), std::string("P:snake_case_word stays and 2 * 3 * 4 too\n"));
  CHECK_EQ(Sg("~~gone~~ and `code *not em*`"), std::string("P:~~gone~~ and `code *not em*`\n"));
  CHECK_EQ(Sg("\\*literal\\* and \\# and \\[x\\]"), std::string("P:*literal* and # and [x]\n"));
  CHECK_EQ(Sg("a **b** c", false), std::string("P:a b c\n"));
  CHECK_EQ(Sg("*unclosed and **also"), std::string("P:*unclosed and **also\n"));
  CHECK_EQ(Sg("<u>under</u> and H<sub>2</sub>O x<sup>2</sup>", true), std::string("P:<u>under</u> and H2O x2\n"));
  CHECK_EQ(Sg("line one  \nline two\\\nline three", false), std::string("P:line one\\nline two\\nline three\n"));
  CHECK_EQ(Sg("&copy; &amp; &lt;b&gt; &#65;", false), std::string("P:\xC2\xA9 & <b> A\n"));
  // Arabic and emoji are plain text to the emphasis rules
  std::string ar = "**\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7** \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 \xF0\x9F\x98\x80";
  CHECK_EQ(Sg(ar, true), std::string("P:**\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7** \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 \xF0\x9F\x98\x80\n"));
}

TEST(md_links_images_footnotes) {
  Document d = ParseMarkdown("[text **b**](https://a.b/c_d?x=1&y=2 \"title\") and <https://auto.link> and https://bare.example/x. and [ref][r1] and [short]\n\n[r1]: https://ref.example/1\n[short]: https://short.example\n");
  CHECK_EQ(d.blocks.size(), (size_t)1);
  auto& runs = d.blocks[0].runs;
  bool a = false, b = false, c = false, e = false, f = false;
  for (auto& r : runs) {
    if (r.link == "https://a.b/c_d?x=1&y=2") a = true;
    if (r.link == "https://auto.link") b = true;
    if (r.link == "https://bare.example/x") c = true;
    if (r.link == "https://ref.example/1" && r.text == "ref") e = true;
    if (r.link == "https://short.example" && r.text == "short") f = true;
  }
  CHECK(a); CHECK(b); CHECK(c); CHECK(e); CHECK(f);
  Document fn = ParseMarkdown("Text with note[^1] here.\n\n[^1]: The note text.\n");
  CHECK_EQ(fn.blocks.size(), (size_t)2);
  CHECK(fn.blocks[1].kind == Block::Kind::Footnote);
  CHECK_EQ(fn.blocks[1].label, std::string("1"));
  CHECK_EQ(PlainText(fn.blocks[1].runs), std::string("The note text."));
  // embedded image (data URI) becomes an Image block; remote image becomes text + warning
  Bytes png = MakePng(8, 4);
  std::string uri = "data:image/png;base64," + Base64Encode(png.data(), png.size());
  Document im = ParseMarkdown("before\n\n![alt text](" + uri + ")\n\n![remote](https://example.com/x.png)\n");
  CHECK(im.blocks.size() >= 3);
  CHECK(im.blocks[1].kind == Block::Kind::Image);
  CHECK_EQ(im.blocks[1].image->w, 8); CHECK_EQ(im.blocks[1].image->h, 4); CHECK_EQ(im.blocks[1].image->alt, std::string("alt text"));
  CHECK(!im.warnings.empty());
}

TEST(md_tables_and_lists_edge_cases) {
  Document t = ParseMarkdown("| Name | Qty | Note |\n|:-----|----:|:----:|\n| a\\|b | 2 | **x** |\n| c | 3 |\n\nafter");
  CHECK_EQ(t.blocks.size(), (size_t)2);
  auto& tb = t.blocks[0].table;
  CHECK(t.blocks[0].kind == Block::Kind::Table);
  CHECK_EQ(tb.rows.size(), (size_t)3);
  CHECK_EQ(tb.rows[0].size(), (size_t)3);
  CHECK_EQ(PlainText(tb.rows[1][0]), std::string("a|b"));
  CHECK(tb.rows[1][2].runs[0].bold);
  CHECK_EQ(PlainText(tb.rows[2][2]), std::string(""));            // short row padded
  CHECK(tb.align[0] == 'l' && tb.align[1] == 'r' && tb.align[2] == 'c');
  CHECK(tb.headerRow);
  // ordered list numbering and task items
  Document l = ParseMarkdown("3. three\n4. four\n\n- [ ] todo\n- [x] done\n");
  CHECK_EQ(l.blocks[0].number, 3); CHECK_EQ(l.blocks[1].number, 4);
  CHECK_EQ(l.blocks[2].checked, 0); CHECK_EQ(l.blocks[3].checked, 1);
  CHECK_EQ(PlainText(l.blocks[2].runs), std::string("todo"));
  // setext headings, tabs, CRLF
  Document s = ParseMarkdown("Title\r\n=====\r\n\r\nSub\r\n---\r\n\r\ntext\r\n");
  CHECK_EQ(SigStr(s), std::string("H1:Title\nH2:Sub\nP:text\n"));
  // nested ordered inside bullet with 2-space and 4-space indents
  Document n = ParseMarkdown("- top\n  1. in a\n  2. in b\n- next\n    - deep4\n");
  CHECK_EQ(SigStr(n), std::string("L0u:top\nL1o:in a\nL1o:in b\nL0u:next\nL1u:deep4\n"));
  // front matter
  Document f = ParseMarkdown("---\ntitle: \"My Doc\"\nauthor: Me\n---\n\n# Heading\n");
  CHECK_EQ(f.meta.title, std::string("My Doc")); CHECK_EQ(f.meta.author, std::string("Me"));
  CHECK_EQ(f.blocks.size(), (size_t)1);
  // html block
  Document h = ParseMarkdown("<div align=\"center\">\n<h2>From HTML</h2>\n<p>para <b>bold</b></p>\n</div>\n\nmd after\n");
  CHECK(SigStr(h, true).find("H2:From HTML") != std::string::npos);
  CHECK(SigStr(h, true).find("**bold**") != std::string::npos);
  // empty / garbage input
  CHECK(ParseMarkdown("").blocks.empty());
  CHECK(ParseMarkdown("\n\n   \n").blocks.empty());
  ParseMarkdown(std::string(10000, '*')); ParseMarkdown(std::string(5000, '[') + std::string(5000, ']')); ParseMarkdown("![](" + std::string(5000, '(') + ")");
  std::string nest; for (int i = 0; i < 400; ++i) nest += std::string(i % 20 * 2, ' ') + "- item " + std::to_string(i) + "\n";
  CHECK(ParseMarkdown(nest).blocks.size() >= 300);
}

TEST(md_writer_roundtrip_is_semantically_stable) {
  const char* inputs[] = {
    "# Title\n\nSome *emphasis*, **strong**, `code`, ~~strike~~ and a [link](https://x.y/z).\n\n- one\n- two\n    - deep\n- three\n\n1. a\n2. b\n\n> quoted\n\n---\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n```\nraw  code\n```\n",
    "Special \\* \\_ \\# chars and 1\\. numbers and a\\|b and <not html>\n",
    "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 **\xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85** and English \xF0\x9F\x98\x80\n\n- \xD8\xA8\xD9\x86\xD8\xAF 1\n- \xD8\xA8\xD9\x86\xD8\xAF 2\n",
    "Text with trailing  \nhard break and **bold at end**\n\n## H2\n\n- [ ] task\n- [x] done\n",
  };
  for (const char* in : inputs) {
    Document a = ParseMarkdown(in);
    std::string md = WriteMarkdown(a);
    Document b = ParseMarkdown(md);
    CHECK_EQ(SigStr(a, true), SigStr(b, true));
    std::string md2 = WriteMarkdown(b);
    CHECK_EQ(md, md2);                                            // fixed point
  }
  // formatting transitions that are tricky for naive writers
  Document d;
  Block p; p.kind = Block::Kind::Paragraph;
  Run r1 = MakeRun("bold "); r1.bold = true; Run r2 = MakeRun("both"); r2.bold = r2.italic = true; Run r3 = MakeRun(" italic"); r3.italic = true; Run r4 = MakeRun(" plain "); Run r5 = MakeRun("code"); r5.code = true;
  Run r6 = MakeRun(" under"); r6.underline = true;
  p.runs = {r1, r2, r3, r4, r5, r6};
  d.blocks.push_back(p);
  FinalizeDocument(d);
  Document back = ParseMarkdown(WriteMarkdown(d));
  CHECK_EQ(SigStr(d, false), SigStr(back, false));
  // leading/trailing spaces inside emphasis must be moved outside the markers
  Document sp; Block q; q.kind = Block::Kind::Paragraph; Run a1 = MakeRun("bold with space "); a1.bold = true; q.runs = {MakeRun("x"), a1, MakeRun("y")}; sp.blocks.push_back(q);
  std::string md = WriteMarkdown(sp);
  CHECK(Contains(md, "**bold with space** y"));
  // special characters survive
  Document sc; sc.blocks.push_back(MakeParagraph("1. not a list * star _ under [brackets] # hash > gt `tick` \\ back"));
  sc.blocks.push_back(MakeParagraph("- not a bullet"));
  sc.blocks.push_back(MakeParagraph("# not a heading"));
  Document sb = ParseMarkdown(WriteMarkdown(sc));
  CHECK_EQ(SigStr(sc), SigStr(sb));
}

TEST(md_image_roundtrip_via_data_uri) {
  Document d = SampleDoc();
  std::string md = WriteMarkdown(d);
  CHECK(Contains(md, "![gradient](data:image/png;base64,"));
  Document b = ParseMarkdown(md);
  size_t imgs = 0; for (auto& x : b.blocks) if (x.kind == Block::Kind::Image) { ++imgs; CHECK_EQ(x.image->w, 120); CHECK_EQ(x.image->h, 60); }
  CHECK_EQ(imgs, (size_t)1);
  MdWriteOptions o; o.embedImages = false;
  CHECK(!Contains(WriteMarkdown(d, o), "base64"));
  // table + footnote + quote + code round trip of the sample document
  CHECK_EQ(SigStr(d), SigStr(b));
}

// ---------------------------------------------------------------------------------------------------------------- HTML
TEST(html_reader_basics) {
  const char* h = "<!DOCTYPE html><html lang='ar' dir='rtl'><head><title>T &amp; U</title><style>p{color:red}</style><script>var x='<b>';</script></head>"
                  "<body><h1 id=top>Head</h1><p>one<br>two <a href=\"https://x.y/?a=1&amp;b=2\">link</a> <b>bold</b><i>it</i> <span style=\"color:#f00;font-size:14pt\">red</span></p>"
                  "<ul><li>a<ul><li>a1</li></ul></li><li>b</li></ul><ol start=3><li>x<li>y</ol>"
                  "<table><tr><th>H1<th>H2<tr><td>1<td colspan=1>2</table><pre><code class=\"language-py\">a\n  b</code></pre><blockquote>q</blockquote><hr><p>caf&eacute; &nbsp;&#8212; &mdash;</p></body></html>";
  Document d = ParseHtml(h);
  CHECK_EQ(d.meta.title, std::string("T & U"));
  CHECK_EQ(d.meta.language, std::string("ar"));
  auto s = SigStr(d, true);
  CHECK(Contains(s, "H1:Head\n"));
  CHECK(Contains(s, "P:one\\ntwo [link](https://x.y/?a=1&b=2) **bold**_it_ red\n"));
  CHECK(Contains(s, "L0u:a\nL1u:a1\nL0u:b\n"));
  CHECK(Contains(s, "L0o:x\nL0o:y\n"));
  CHECK(Contains(s, "T:H1|H2|;1|2|;"));
  CHECK(Contains(s, "C:a\n  b"));
  CHECK(Contains(s, "Q:q\nR\n"));
  CHECK(Contains(s, "P:caf\xC3\xA9 \xC2\xA0\xE2\x80\x94 \xE2\x80\x94"));
  CHECK(!Contains(s, "var x"));
  CHECK(!Contains(s, "color:red"));
  CHECK_EQ(d.blocks[0].anchor, std::string("top"));
  bool red = false; for (auto& r : d.blocks[1].runs) if (r.color == "FF0000" && r.sizePt == 14) red = true; CHECK(red);
  CHECK_EQ(d.blocks[2 + 1].ordered, false);
  // ordered list start
  bool seen3 = false; for (auto& b : d.blocks) if (b.kind == Block::Kind::ListItem && b.ordered && b.number == 3) seen3 = true; CHECK(seen3);
  // tag soup never crashes
  for (const char* g : {"", "<", "<<>", "<p><b>unclosed", "</p></div>", "<table><td>x", "<ul><li>a<li>b</ul>", "<a href=", "<p a='>", "<!--", "<![CDATA[x"}) ParseHtml(g);
}

TEST(html_writer_validity_and_roundtrip) {
  Document d = SampleDoc();
  HtmlWriteOptions xo; xo.xhtml = true;
  std::string xhtml = WriteHtml(d, xo);
  std::string why;
  CHECK(xml::CheckWellFormed(xhtml, &why));                           // XHTML output is well-formed XML
  HtmlWriteOptions ho;
  std::string html = WriteHtml(d, ho);
  CHECK(Contains(html, "<html lang=\"en\">") || Contains(html, "<html "));
  Document b = ParseHtml(html);
  CHECK_EQ(SigStr(d, true), SigStr(b, true));
  // direction is written for RTL blocks
  CHECK(Contains(html, "dir=\"rtl\""));
  // escaping
  Document e; e.blocks.push_back(MakeParagraph("a < b && c > d \"q\" \x01 end"));
  FinalizeDocument(e);
  std::string eh = WriteHtml(e, xo);
  CHECK(xml::CheckWellFormed(eh, &why));
  CHECK(Contains(eh, "a &lt; b &amp;&amp; c &gt; d"));
}

TEST(html_charset_detection) {
  std::string cp1252 = "<html><head><meta charset=\"windows-1252\"></head><body><p>caf\xE9</p></body></html>";
  CHECK_EQ(DecodeHtmlBytes((const uint8_t*)cp1252.data(), cp1252.size()).find("caf\xC3\xA9") != std::string::npos, true);
  std::string u8 = "<html><body><p>\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7</p></body></html>";
  CHECK(Contains(DecodeHtmlBytes((const uint8_t*)u8.data(), u8.size()), "\xD9\x85\xD8\xB1"));
  Bytes u16 = EncodeText("<p>hi \xD9\x85</p>", TextEnc::Utf16LEBom);
  CHECK(Contains(DecodeHtmlBytes(u16.data(), u16.size()), "hi \xD9\x85"));
}

// ---------------------------------------------------------------------------------------------------------------- plain text
TEST(text_reader_and_writer) {
  Document d = ParseText("Title\n=====\n\nFirst para line1\nline2\n\n- bullet a\n- bullet b\n  continued\n\n1. one\n2. two\n\n\n\nLast  para   with   spaces.\n");
  CHECK_EQ(SigStr(d), std::string("H1:Title\nP:First para line1\\nline2\nL0u:bullet a\nL0u:bullet b continued\nL0o:one\nL0o:two\nP:Last  para   with   spaces.\n"));
  TextReadOptions reflow; reflow.reflow = true;
  CHECK_EQ(SigStr(ParseText("a\nb\nc\n", reflow)), std::string("P:a b c\n"));
  // control characters and mixed newlines are sanitised
  Document c = ParseText(std::string("a\x01" "b\r\nc\rd\x0C" "e\n"));
  CHECK(Contains(AllText(c), "ab"));
  CHECK(!Contains(AllText(c), "\x01"));
  // writer
  std::string t = WriteText(d);
  CHECK(Contains(t, "Title\n\nFirst para line1\nline2\n\n\xE2\x80\xA2 bullet a\n"));
  CHECK(Contains(t, "1. one\n2. two\n"));
  CHECK(EndsWith(t, "spaces.\n"));
  Document tab = SampleDoc();
  CHECK(Contains(WriteText(tab), "H0\tH1\tH2\nr1c0\tr1c1\tr1c2\n"));
  TextWriteOptions wrap; wrap.wrapColumns = 20;
  Document w; w.blocks.push_back(MakeParagraph("the quick brown fox jumps over the lazy dog"));
  CHECK_EQ(WriteText(w, wrap), std::string("the quick brown fox\njumps over the lazy\ndog\n"));
  CHECK(ParseText("").blocks.empty());
}
