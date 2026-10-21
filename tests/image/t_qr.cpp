// QR reading with ZXing-cpp. Fixtures come from OpenCV's QR encoder (independent of ZXing).
#include "t_util.h"
#include "engines/image/qr.h"

using namespace tu;
using namespace filo;
using namespace filo::img;

namespace {
const std::string kUrl = "https://example.com/filo?x=1&y=two";
const std::string kTxt = "Hello Filo QR";

double Dist(QrPoint a, QrPoint b) { return std::hypot((double)a.x - b.x, (double)a.y - b.y); }
bool Upright(const QrResult& r) { return r.corners[0].x < r.corners[1].x && r.corners[0].y < r.corners[3].y && r.corners[1].y < r.corners[2].y; }
bool Square(const QrResult& r, double tol) {
  double d[4] = {Dist(r.corners[0], r.corners[1]), Dist(r.corners[1], r.corners[2]), Dist(r.corners[2], r.corners[3]), Dist(r.corners[3], r.corners[0])};
  double m = (d[0] + d[1] + d[2] + d[3]) / 4;
  for (double x : d) if (std::fabs(x - m) > m * tol) return false;
  return m > 20;
}
}

TEST(qr_reader_is_built_in) { CHECK(QrReaderAvailable()); }

TEST(qr_single_url_and_text) {
  CHECK(EnsureFixtures());
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_url.png", &err);
  CHECK(err.empty()); CHECK_EQ(r.size(), (size_t)1);
  if (r.size() == 1) {
    CHECK_EQ(r[0].textUtf8, kUrl);
    CHECK(r[0].isUrl); CHECK_EQ(r[0].kind, std::wstring(L"url")); CHECK_EQ(r[0].url, Widen(kUrl));
    CHECK_EQ(r[0].format, std::wstring(L"QR Code"));
    CHECK(Upright(r[0])); CHECK(Square(r[0], 0.06));
    Px px; CHECK(WicRead(Src() / "qr_url.png", px));
    for (auto& c : r[0].corners) { CHECK(c.x >= 0 && c.x < px.w && c.y >= 0 && c.y < px.h); }
    // the symbol covers most of the picture (quiet zone is 4 modules of ~33)
    double side = Dist(r[0].corners[0], r[0].corners[1]);
    CHECK(side > px.w * 0.7 && side < px.w);
  }
  auto t = ReadQrCodes(Src() / "qr_text.png", &err);
  CHECK_EQ(t.size(), (size_t)1);
  if (t.size() == 1) { CHECK_EQ(t[0].textUtf8, kTxt); CHECK(!t[0].isUrl); CHECK_EQ(t[0].kind, std::wstring(L"text")); CHECK(t[0].url.empty()); }
}

TEST(qr_multiple_codes_in_one_image) {
  CHECK(EnsureFixtures());
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_two.png", &err);
  CHECK_EQ(r.size(), (size_t)2);
  if (r.size() == 2) {
    CHECK_EQ(r[0].textUtf8, kUrl);           // left one first
    CHECK_EQ(r[1].textUtf8, kTxt);
    CHECK(r[0].corners[1].x < r[1].corners[0].x);
    CHECK(r[0].isUrl); CHECK(!r[1].isUrl);
  }
}

TEST(qr_rotated_inverted_transparent_mirrored_and_jpeg) {
  CHECK(EnsureFixtures());
  std::wstring err;
  {
    auto r = ReadQrCodes(Src() / "qr_rotated.png", &err);
    CHECK_EQ(r.size(), (size_t)1);
    if (r.size() == 1) {
      CHECK_EQ(r[0].textUtf8, kUrl);
      double ang = std::atan2((double)r[0].corners[1].y - r[0].corners[0].y, (double)r[0].corners[1].x - r[0].corners[0].x) * 180.0 / 3.14159265358979;
      std::printf("    rotated code: top edge angle %.1f deg (fixture rotated by 23 deg counter-clockwise)\n", ang);
      CHECK(std::fabs(std::fabs(ang) - 23.0) < 4.0);
      CHECK(Square(r[0], 0.08));
    }
  }
  { auto r = ReadQrCodes(Src() / "qr_url.jpg", &err); CHECK_EQ(r.size(), (size_t)1); if (r.size() == 1) CHECK_EQ(r[0].textUtf8, kUrl); }
  { auto r = ReadQrCodes(Src() / "qr_inverted.png", &err); CHECK_EQ(r.size(), (size_t)1); if (r.size() == 1) CHECK_EQ(r[0].textUtf8, kTxt); }
  { auto r = ReadQrCodes(Src() / "qr_transparent.png", &err); CHECK_EQ(r.size(), (size_t)1); if (r.size() == 1) CHECK_EQ(r[0].textUtf8, kTxt); }
  { auto r = ReadQrCodes(Src() / "qr_mirrored.png", &err); std::printf("    mirrored QR: %zu code(s) found\n", r.size()); if (r.size() == 1) CHECK_EQ(r[0].textUtf8, kUrl); }
  { auto r = ReadQrCodes(Src() / "noise.png", &err); CHECK(r.empty()); CHECK(err.empty()); }
}

TEST(qr_small_code_in_a_large_busy_photo) {
  CHECK(EnsureFixtures());
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_in_scene.jpg", &err);
  CHECK_EQ(r.size(), (size_t)1);
  if (r.size() == 1) {
    CHECK_EQ(r[0].textUtf8, kUrl);
    CHECK(r[0].corners[0].x >= 1280 && r[0].corners[0].x <= 1330);      // pasted at (1300,700) + quiet zone
    CHECK(r[0].corners[0].y >= 690 && r[0].corners[0].y <= 780);
  }
}

TEST(qr_respects_exif_orientation) {
  CHECK(EnsureFixtures());
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_exif6.jpg", &err);
  CHECK_EQ(r.size(), (size_t)1);
  if (r.size() == 1) { CHECK_EQ(r[0].textUtf8, kUrl); CHECK(Upright(r[0])); }
}

TEST(qr_arabic_utf8_text) {
  CHECK(EnsureFixtures());
  if (!fs::exists(Src() / "qr_arabic.png")) return;
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_arabic.png", &err);
  CHECK_EQ(r.size(), (size_t)1);
  if (r.size() == 1) {
    std::wstring want = L"مرحبا فيلو";
    std::printf("    arabic QR text length %zu (want %zu)\n", r[0].text.size(), want.size());
    CHECK_EQ(r[0].text, want);
  }
}

TEST(qr_from_svg_and_converted_formats) {
  CHECK(EnsureFixtures());
  std::wstring err;
  auto r = ReadQrCodes(Src() / "qr_text.svg", &err);
  CHECK_EQ(r.size(), (size_t)1);
  if (r.size() == 1) CHECK_EQ(r[0].textUtf8, kTxt);
  // through the converter: PNG -> WebP and JPEG -> PNG keep the code readable
  RunOut w = RunCap(L"png", L"webp", Src() / "qr_url.png", OutDir() / L"qr_via.webp");
  CHECK(w.ok); if (w.ok) { auto q = ReadQrCodes(w.out, &err); CHECK_EQ(q.size(), (size_t)1); if (q.size() == 1) CHECK_EQ(q[0].textUtf8, kUrl); }
}

TEST(qr_text_classification_and_no_link_following) {
  struct C { const wchar_t* text; bool url; const wchar_t* urlOut; const wchar_t* kind; };
  const C cases[] = {
    {L"https://example.com/a?b=c", true, L"https://example.com/a?b=c", L"url"},
    {L"HTTP://EXAMPLE.COM", true, L"HTTP://EXAMPLE.COM", L"url"},
    {L"ftp://files.example.org/x", true, L"ftp://files.example.org/x", L"url"},
    {L"www.example.com/path", true, L"https://www.example.com/path", L"url"},
    {L"http://", false, L"", L"text"},
    {L"javascript:alert(1)", false, L"", L"text"},
    {L"hello http://example.com world", false, L"", L"text"},
    {L"mailto:me@example.com", false, L"", L"email"},
    {L"tel:+15551234", false, L"", L"phone"},
    {L"WIFI:S:net;T:WPA;P:secret;;", false, L"", L"wifi"},
    {L"geo:37.78,-122.41", false, L"", L"geo"},
    {L"BEGIN:VCARD\nVERSION:3.0", false, L"", L"vcard"},
    {L"just text", false, L"", L"text"},
  };
  for (auto& c : cases) {
    bool isUrl; std::wstring url, kind;
    ClassifyQrText(c.text, isUrl, url, kind);
    CHECK_EQ(isUrl, c.url); CHECK_EQ(url, std::wstring(c.urlOut)); CHECK_EQ(kind, std::wstring(c.kind));
  }
  // the engine never launches anything: no shell / process APIs in the QR module
  std::ifstream in(Root() / "src" / "engines" / "image" / "qr.cpp", std::ios::binary); std::stringstream ss; ss << in.rdbuf();
  std::string src = ss.str();
  CHECK(src.find("ShellExecute") == std::string::npos); CHECK(src.find("CreateProcess") == std::string::npos); CHECK(src.find("system(") == std::string::npos);
}
