// Text encodings: detection (BOM + heuristics), decoding to UTF-8, encoding from UTF-8, plain-text document reader/writer helpers.
#pragma once
#include "engines/doc/docutil.h"

namespace filo::doc {

enum class TextEnc { Auto, Utf8, Utf8Bom, Utf16LE, Utf16LEBom, Utf16BE, Utf16BEBom, Utf32LE, Utf32BE, Cp1252, Cp1256, Cp1251, Cp1250, Cp1255, Latin1 };

struct DecodedText {
  std::string utf8;            // BOM removed, invalid sequences replaced
  TextEnc detected = TextEnc::Utf8;
  bool hadBom = false;
  bool hadErrors = false;      // replacement characters had to be inserted
};

// Maps names such as "utf-8", "utf8bom", "utf-16le", "windows-1256", "cp1252", "latin1", "auto" (case-insensitive). Unknown => Auto.
TextEnc ParseEncodingName(std::string_view name);
const char* EncodingName(TextEnc e);

// forced == Auto => detect: BOM, then UTF-16 by NUL pattern, then strict UTF-8, then the ANSI code page (windows-1252 unless the
// system code page is a legacy Arabic/Hebrew/Cyrillic/CJK page).
DecodedText DecodeText(const uint8_t* data, size_t n, TextEnc forced = TextEnc::Auto);
inline DecodedText DecodeText(const Bytes& b, TextEnc forced = TextEnc::Auto) { return DecodeText(b.data(), b.size(), forced); }

// UTF-8 -> bytes in the requested encoding (adds the BOM for *Bom variants). Characters not representable in a legacy page become '?'.
Bytes EncodeText(std::string_view utf8, TextEnc enc);

// Normalises line endings: "\r\n", lone "\r" -> "\n" (for parsing) / "\n" -> "\r\n" (for writing).
std::string NormalizeNewlines(std::string_view s);
std::string ToCrLf(std::string_view s);

} // namespace filo::doc
