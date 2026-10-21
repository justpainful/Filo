#include "engines/image/qr.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>

#ifdef FILO_HAVE_ZXING
#include "ReadBarcode.h"
#endif

namespace filo::img {

bool QrReaderAvailable() {
#ifdef FILO_HAVE_ZXING
  return true;
#else
  return false;
#endif
}

void ClassifyQrText(const std::wstring& text0, bool& isUrl, std::wstring& url, std::wstring& kind) {
  isUrl = false; url.clear(); kind = L"text";
  std::wstring t = Trim(text0);
  std::wstring lower = ToLower(t);
  auto hasSpace = [&] { return t.find_first_of(L" \t\r\n") != std::wstring::npos; };
  auto hostOk = [&](size_t from) {          // at least one character of host before any '/', and the whole thing has no whitespace
    size_t end = t.find_first_of(L"/?#", from);
    return !hasSpace() && (end == std::wstring::npos ? t.size() : end) > from + 1;
  };
  if (StartsWithI(lower, L"http://") && hostOk(7)) { isUrl = true; url = t; kind = L"url"; return; }
  if (StartsWithI(lower, L"https://") && hostOk(8)) { isUrl = true; url = t; kind = L"url"; return; }
  if (StartsWithI(lower, L"ftp://") && hostOk(6)) { isUrl = true; url = t; kind = L"url"; return; }
  if (StartsWithI(lower, L"www.") && !hasSpace() && t.find(L'.', 4) != std::wstring::npos && t.size() > 6) { isUrl = true; url = L"https://" + t; kind = L"url"; return; }
  if (StartsWithI(lower, L"mailto:")) kind = L"email";
  else if (StartsWithI(lower, L"tel:")) kind = L"phone";
  else if (StartsWithI(lower, L"sms:") || StartsWithI(lower, L"smsto:")) kind = L"sms";
  else if (StartsWithI(lower, L"geo:")) kind = L"geo";
  else if (StartsWithI(lower, L"wifi:")) kind = L"wifi";
  else if (StartsWithI(lower, L"begin:vcard")) kind = L"vcard";
}

std::vector<QrResult> ReadQrCodesFromBitmap(const Bitmap& bm0, const QrReadOptions& opt) {
  std::vector<QrResult> out;
#ifdef FILO_HAVE_ZXING
  if (bm0.Empty()) return out;
  Bitmap flat;
  const Bitmap* bm = &bm0;
  if (UsesAlpha(bm0)) { flat = bm0; FlattenOnWhite(flat); bm = &flat; }
  ZXing::ImageView iv(bm->px.data(), bm->width, bm->height, ZXing::ImageFormat::BGRA);
  ZXing::ReaderOptions ro;
  ro.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode | ZXing::BarcodeFormat::RMQRCode);
  ro.setTryHarder(opt.tryHarder).setTryRotate(opt.tryRotate).setTryInvert(opt.tryInvert).setTryDownscale(opt.tryDownscale);
  ro.setMaxNumberOfSymbols((uint8_t)std::clamp(opt.maxCodes, 1, 255));
  ZXing::Barcodes found = ZXing::ReadBarcodes(iv, ro);
  for (auto& b : found) {
    if (!b.isValid()) continue;
    QrResult r;
    r.textUtf8 = b.text();
    r.text = Widen(r.textUtf8);
    r.format = Widen(ZXing::ToString(b.format()));
    const auto& pos = b.position();
    for (int i = 0; i < 4; ++i) { r.corners[i].x = pos[i].x; r.corners[i].y = pos[i].y; }
    const auto& by = b.bytes();
    r.bytes.assign(by.begin(), by.end());
    ClassifyQrText(r.text, r.isUrl, r.url, r.kind);
    // drop exact duplicates (the same symbol found by two scan passes)
    bool dup = false;
    for (auto& e : out) {
      if (e.textUtf8 != r.textUtf8) continue;
      double cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
      for (int i = 0; i < 4; ++i) { cx1 += e.corners[i].x; cy1 += e.corners[i].y; cx2 += r.corners[i].x; cy2 += r.corners[i].y; }
      double side = std::hypot(e.corners[1].x - e.corners[0].x, e.corners[1].y - e.corners[0].y);
      if (std::hypot((cx1 - cx2) / 4, (cy1 - cy2) / 4) < std::max(8.0, side * 0.5)) { dup = true; break; }
    }
    if (!dup) out.push_back(std::move(r));
  }
  // reading order: top-to-bottom, then left-to-right
  std::stable_sort(out.begin(), out.end(), [](const QrResult& a, const QrResult& b) {
    int ay = (a.corners[0].y + a.corners[2].y) / 2, by_ = (b.corners[0].y + b.corners[2].y) / 2;
    int ax = (a.corners[0].x + a.corners[2].x) / 2, bx = (b.corners[0].x + b.corners[2].x) / 2;
    int tol = std::max(1, std::abs(a.corners[2].y - a.corners[0].y) / 2);
    if (std::abs(ay - by_) > tol) return ay < by_;
    return ax < bx;
  });
#else
  (void)opt;
#endif
  return out;
}

std::vector<QrResult> ReadQrCodes(const fs::path& path, std::wstring* err, const QrReadOptions& opt) {
  if (err) err->clear();
  if (!QrReaderAvailable()) { if (err) *err = L"QR code reading is not available in this build."; return {}; }
  Bitmap bm; LoadOptions lo; lo.convertToSRGB = false; lo.applyExifOrientation = true;
  std::wstring e;
  if (!LoadBitmap(path, bm, lo, &e)) { if (err) *err = e; return {}; }
  return ReadQrCodesFromBitmap(bm, opt);
}

} // namespace filo::img
