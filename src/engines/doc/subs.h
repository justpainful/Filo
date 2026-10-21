// Subtitles: SRT <-> WebVTT <-> plain text. Timing is kept to the millisecond as integers.
#pragma once
#include "engines/doc/text_io.h"

namespace filo::doc {

struct Cue {
  int64_t startMs = 0, endMs = 0;
  std::string id;            // WebVTT cue identifier (kept for vtt -> vtt style output), may be empty
  std::string text;          // UTF-8, '\n' between lines; may contain <i> <b> <u>
};
struct SubFile {
  std::vector<Cue> cues;
  std::vector<std::wstring> warnings;
  void Warn(const std::wstring& w) { for (auto& x : warnings) if (x == w) return; warnings.push_back(w); }
};

// `utf8` is already decoded (BOM removed). Tolerant: missing indexes, '.' or ',' decimals, 1-3 digit fractions, CRLF/CR line ends,
// blank lines inside a cue, NOTE/STYLE/REGION blocks (skipped), cue settings (dropped with a warning).
bool ParseSrt(std::string_view utf8, SubFile& out, std::wstring* err);
bool ParseVtt(std::string_view utf8, SubFile& out, std::wstring* err);

struct SubWriteOptions { bool crlf = true; };
// Returns UTF-8 text without BOM. Styling other than <i>/<b>/<u> was already stripped by the parsers; the writers escape what the
// target format requires.
std::string WriteSrt(const SubFile& s, const SubWriteOptions& opt = {});
std::string WriteVtt(const SubFile& s, const SubWriteOptions& opt = {});
std::string WriteSubText(const SubFile& s, bool timestamps);

// "HH:MM:SS,mmm" / "HH:MM:SS.mmm" helpers
std::string FormatSubTime(int64_t ms, char decimalSep);
bool ParseSubTime(std::string_view s, int64_t& ms);

} // namespace filo::doc
