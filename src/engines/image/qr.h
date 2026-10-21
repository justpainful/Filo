// QR code reading (ZXing-cpp, Apache-2.0, built from third_party/zxing as a readers-only static library: QR / Micro QR / rMQR).
// Works offline on any image the bitmap layer can decode (JPG, PNG, WebP, HEIC, SVG...). Links found in a code are only REPORTED:
// Filo never opens them.
#pragma once
#include "engines/image/bitmap.h"

namespace filo::img {

struct QrPoint { int x = 0, y = 0; };

struct QrResult {
  std::wstring text;                 // decoded content (UTF-16)
  std::string textUtf8;              // decoded content as UTF-8 (same text)
  std::wstring format;               // "QR Code", "Micro QR Code", "rMQR Code"
  QrPoint corners[4];                // top-left, top-right, bottom-right, bottom-left of the symbol, in pixels of the (upright) image
  bool isUrl = false;                // http(s):// or ftp:// URL, or a bare "www.host.tld" address. NEVER opened by Filo.
  std::wstring url;                  // normalised URL when isUrl ("www.x.com" -> "https://www.x.com"); empty otherwise
  std::wstring kind;                 // "url", "email", "phone", "sms", "geo", "wifi", "vcard", "text"
  std::vector<uint8_t> bytes;        // raw payload bytes
};

struct QrReadOptions {
  bool tryHarder = true;
  bool tryRotate = true;
  bool tryInvert = true;             // light-on-dark codes
  bool tryDownscale = true;          // photos where the code is small relative to the frame
  int maxCodes = 16;
};

// Reads every QR code in the image (several per image are fine). Returns an empty vector (and err empty) when none is found.
std::vector<QrResult> ReadQrCodes(const fs::path& path, std::wstring* err = nullptr, const QrReadOptions& opt = {});
std::vector<QrResult> ReadQrCodesFromBitmap(const Bitmap& bm, const QrReadOptions& opt = {});

// Classifies decoded text (exposed for tests and the UI).
void ClassifyQrText(const std::wstring& text, bool& isUrl, std::wstring& url, std::wstring& kind);

bool QrReaderAvailable();

} // namespace filo::img
