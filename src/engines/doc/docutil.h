// Small shared helpers of the document engine: UTF-8 handling, XML escaping, base64, file IO, image probing.
#pragma once
#include "core/util.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace filo::doc {

using Bytes = std::vector<uint8_t>;

// ---- UTF-8 -----------------------------------------------------------------------------------------------------------
// Decodes one code point at s[i] (advances i). Invalid bytes yield U+FFFD and advance by one byte.
uint32_t DecodeUtf8(std::string_view s, size_t& i);
void AppendUtf8(std::string& out, uint32_t cp);
bool IsValidUtf8(std::string_view s);
// Replaces invalid sequences with U+FFFD and removes characters that no XML 1.0 document may contain (C0 controls except
// TAB/LF/CR, surrogates, U+FFFE/U+FFFF). Keeps TAB, LF, CR.
std::string SanitizeText(std::string_view s);
std::u32string ToCodepoints(std::string_view s);
std::string FromCodepoints(const std::u32string& s);
size_t Utf8Length(std::string_view s);                         // in code points
std::wstring W(std::string_view utf8);                         // = filo::Widen
std::string U(std::wstring_view w);                            // = filo::Narrow

// ---- scripts / direction ---------------------------------------------------------------------------------------------
bool IsRtlCodepoint(uint32_t cp);          // Hebrew, Arabic, Syriac, Thaana, NKo, Arabic presentation forms...
bool IsStrongLtrCodepoint(uint32_t cp);    // letters of other scripts
bool ContainsRtl(std::string_view s);
bool DetectRtl(std::string_view s);        // first strong character is right-to-left
// Splits text into segments by direction: (text, isRtl). Neutrals (spaces, digits, punctuation) join the preceding segment.
std::vector<std::pair<std::string, bool>> SplitByDirection(std::string_view s);

// ---- XML / text escaping ---------------------------------------------------------------------------------------------
std::string XmlEscape(std::string_view s, bool attr = false);   // sanitises illegal XML characters too
std::string XmlUnescapeBasic(std::string_view s);
std::string HtmlEscape(std::string_view s);

// ---- misc strings ----------------------------------------------------------------------------------------------------
std::string LowerAscii(std::string_view s);
std::string UpperAscii(std::string_view s);
std::string TrimAscii(std::string_view s);
bool StartsWith(std::string_view s, std::string_view p);
bool EndsWith(std::string_view s, std::string_view p);
bool EqualsNoCase(std::string_view a, std::string_view b);
std::vector<std::string> SplitLines(std::string_view s);        // \n, \r\n and lone \r
std::vector<std::string> SplitStr(std::string_view s, char sep);
std::string Printf(const char* fmt, ...);
std::string Base64Encode(const uint8_t* d, size_t n);
Bytes Base64Decode(std::string_view s);
std::string Hex2(unsigned v);

// ---- files -----------------------------------------------------------------------------------------------------------
bool ReadFileBytes(const fs::path& p, Bytes& out, uint64_t maxBytes, std::wstring* err = nullptr);
bool WriteFileBytes(const fs::path& p, const void* data, size_t n, std::wstring* err = nullptr);
inline bool WriteFileString(const fs::path& p, const std::string& s, std::wstring* err = nullptr) { return WriteFileBytes(p, s.data(), s.size(), err); }
// Last path component made safe for use as a name inside a ZIP/folder.
std::string SafeName(std::string_view s, size_t maxLen = 80);

// ---- images ----------------------------------------------------------------------------------------------------------
struct ImageInfo { std::string mime; int w = 0, h = 0; };
// PNG / JPEG / GIF / BMP / WEBP header probe (no decoding). Returns false for anything else.
bool ProbeImage(const uint8_t* d, size_t n, ImageInfo& info);
std::string ImageExtForMime(std::string_view mime);

// Calls CoInitializeEx on construction (tolerates an apartment that is already initialised on this thread).
struct ComScope { HRESULT hr; ComScope(); ~ComScope(); };

} // namespace filo::doc
