#include "engines/doc/subs.h"
#include "engines/doc/xml.h"
#include <algorithm>
#include <array>

namespace filo::doc {

std::string FormatSubTime(int64_t ms, char sep) {
  if (ms < 0) ms = 0;
  int64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
  return Printf("%02lld:%02lld:%02lld%c%03lld", (long long)h, (long long)m, (long long)s, sep, (long long)f);
}

bool ParseSubTime(std::string_view in, int64_t& ms) {
  std::string s = TrimAscii(in);
  if (s.empty()) return false;
  std::vector<std::string> parts = SplitStr(s, ':');
  if (parts.size() < 2 || parts.size() > 3) return false;
  std::string last = parts.back();
  size_t sepPos = last.find_first_of(",.");
  std::string sec = sepPos == std::string::npos ? last : last.substr(0, sepPos);
  std::string frac = sepPos == std::string::npos ? "" : last.substr(sepPos + 1);
  auto digits = [](const std::string& x) { if (x.empty()) return false; for (char c : x) if (c < '0' || c > '9') return false; return true; };
  if (!digits(sec) || sec.size() > 2) return false;
  for (size_t i = 0; i + 1 < parts.size(); ++i) if (!digits(parts[i]) || parts[i].size() > 6) return false;
  if (!frac.empty() && !digits(frac)) return false;
  int64_t h = 0, m = 0;
  if (parts.size() == 3) { h = atoll(parts[0].c_str()); m = atoll(parts[1].c_str()); } else m = atoll(parts[0].c_str());
  int64_t sc = atoll(sec.c_str());
  int64_t f = 0;
  if (!frac.empty()) { std::string f3 = (frac + "000").substr(0, 3); f = atoll(f3.c_str()); }
  if (m > 59 && parts.size() == 3) return false;
  if (sc > 59) return false;
  ms = ((h * 60 + m) * 60 + sc) * 1000 + f;
  return true;
}

namespace {

// Tag handling: keeps <i> <b> <u>; strips everything else and tells whether anything was removed.
struct Cleaned { std::string text; bool strippedStyle = false; };

Cleaned CleanCueText(const std::string& in, bool decodeEntities) {
  Cleaned c;
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    char ch = in[i];
    if (ch == '<') {
      size_t e = in.find('>', i);
      if (e != std::string::npos && e - i < 200) {
        std::string inner = in.substr(i + 1, e - i - 1);
        std::string low = LowerAscii(inner);
        bool closing = !low.empty() && low[0] == '/';
        std::string tag = closing ? low.substr(1) : low;
        size_t sp = tag.find_first_of(" \t.");
        if (sp != std::string::npos) tag = tag.substr(0, sp);
        // timestamps inside cue text <00:00:01.000>
        bool isTimestamp = !tag.empty() && isdigit((unsigned char)tag[0]);
        if (tag == "i" || tag == "b" || tag == "u") {
          if (closing || low.size() == tag.size()) { out += closing ? "</" + tag + ">" : "<" + tag + ">"; i = e; continue; }
        }
        if (isTimestamp || (!tag.empty() && isalpha((unsigned char)tag[0]))) {
          if (tag == "v" && !closing) { /* speaker voice: kept as plain text prefix */ std::string who = TrimAscii(inner.substr(1)); if (!who.empty()) out += who + ": "; }
          c.strippedStyle = true; i = e; continue;
        }
      }
    }
    if (ch == '{' && i + 1 < in.size() && in[i + 1] == '\\') {      // ASS/SSA override block {\an8}
      size_t e = in.find('}', i);
      if (e != std::string::npos) { c.strippedStyle = true; i = e; continue; }
    }
    out.push_back(ch);
  }
  if (decodeEntities) out = xml::DecodeEntities(out);
  c.text = std::move(out);
  return c;
}

// Escapes text for WebVTT: & and < unless they belong to the kept tags.
std::string EscapeVttText(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '<') {
      if (s.compare(i, 3, "<i>") == 0 || s.compare(i, 3, "<b>") == 0 || s.compare(i, 3, "<u>") == 0) { o.append(s, i, 3); i += 2; continue; }
      if (s.compare(i, 4, "</i>") == 0 || s.compare(i, 4, "</b>") == 0 || s.compare(i, 4, "</u>") == 0) { o.append(s, i, 4); i += 3; continue; }
      o += "&lt;"; continue;
    }
    if (c == '&') { o += "&amp;"; continue; }
    if (c == '>' ) { o += "&gt;"; continue; }
    o.push_back(c);
  }
  return o;
}

bool FindTiming(const std::string& line, int64_t& st, int64_t& en, std::string& settings) {
  size_t a = line.find("-->");
  if (a == std::string::npos) return false;
  std::string left = TrimAscii(line.substr(0, a));
  std::string right = TrimAscii(line.substr(a + 3));
  size_t sp = right.find_first_of(" \t");
  std::string endStr = sp == std::string::npos ? right : right.substr(0, sp);
  settings = sp == std::string::npos ? "" : TrimAscii(right.substr(sp));
  return ParseSubTime(left, st) && ParseSubTime(endStr, en);
}

bool IsIndexLine(const std::string& l) {
  std::string t = TrimAscii(l);
  if (t.empty() || t.size() > 9) return false;
  for (char c : t) if (c < '0' || c > '9') return false;
  return true;
}

bool ParseCues(std::string_view utf8, SubFile& out, bool vtt, std::wstring* err) {
  std::vector<std::string> lines = SplitLines(utf8);
  auto blank = [&](size_t k) { return TrimAscii(lines[k]).empty(); };
  size_t i = 0;
  if (vtt) {
    if (lines.empty() || !StartsWith(TrimAscii(lines[0]), "WEBVTT")) { if (err) *err = L"This is not a WebVTT file (the WEBVTT header is missing)."; return false; }
    i = 1;
    while (i < lines.size() && !blank(i)) ++i;        // header block (title / metadata)
  }
  std::vector<size_t> timingIdx;
  std::vector<std::array<int64_t, 2>> times;
  std::vector<std::string> settingsOf;
  for (size_t k = i; k < lines.size(); ++k) {
    int64_t st, en; std::string se;
    if (lines[k].find("-->") != std::string::npos && FindTiming(lines[k], st, en, se)) { timingIdx.push_back(k); times.push_back({st, en}); settingsOf.push_back(se); }
  }
  if (timingIdx.empty()) { if (err) *err = L"No subtitle cues were found in this file."; return false; }
  bool sawBlocks = false, strippedAny = false, settingsDropped = false;
  if (vtt) for (size_t k = i; k < lines.size(); ++k) { std::string t = TrimAscii(lines[k]); if ((t == "STYLE" || StartsWith(t, "REGION")) && (k == 0 || blank(k - 1))) sawBlocks = true; }

  auto isIdentifierLine = [&](size_t j, size_t timing) {     // is lines[j] (right before `timing`) the next cue's identifier / index?
    if (j >= timing || blank(j) || lines[j].find("-->") != std::string::npos) return false;
    if (vtt) return j == 0 || blank(j - 1);
    return IsIndexLine(lines[j]);
  };

  for (size_t c = 0; c < timingIdx.size(); ++c) {
    size_t t = timingIdx[c];
    size_t limit = c + 1 < timingIdx.size() ? timingIdx[c + 1] : lines.size();
    size_t textEnd = limit;
    if (c + 1 < timingIdx.size() && limit > t + 1 && isIdentifierLine(limit - 1, limit)) textEnd = limit - 1;
    Cue cue;
    cue.startMs = times[c][0]; cue.endMs = times[c][1];
    if (vtt && t > i && isIdentifierLine(t - 1, t)) {
      std::string idl = TrimAscii(lines[t - 1]);
      if (!StartsWith(idl, "NOTE")) cue.id = idl;
    }
    if (!settingsOf[c].empty()) settingsDropped = true;
    std::vector<std::string> tl;
    for (size_t k = t + 1; k < textEnd; ++k) {
      if (vtt && StartsWith(TrimAscii(lines[k]), "NOTE") && blank(k - 1)) { while (k < textEnd && !blank(k)) ++k; continue; }
      tl.push_back(lines[k]);
    }
    while (!tl.empty() && TrimAscii(tl.back()).empty()) tl.pop_back();
    while (!tl.empty() && TrimAscii(tl.front()).empty()) tl.erase(tl.begin());
    if (vtt) for (size_t k = 0; k < tl.size(); ++k) if (TrimAscii(tl[k]).empty()) { tl.resize(k); break; }     // a vtt cue ends at a blank line
    std::string text;
    for (size_t k = 0; k < tl.size(); ++k) {
      std::string x = tl[k];
      while (!x.empty() && (x.back() == ' ' || x.back() == '\t' || x.back() == '\r')) x.pop_back();
      if (k) text.push_back('\n');
      text += x;
    }
    Cleaned cl = CleanCueText(text, vtt);
    if (cl.strippedStyle) strippedAny = true;
    cue.text = cl.text;
    out.cues.push_back(std::move(cue));
  }
  if (strippedAny) out.Warn(L"Styling tags (colours, fonts, positions, speaker voices) were removed; <i>, <b> and <u> were kept.");
  if (settingsDropped) out.Warn(L"Cue positioning settings were removed.");
  if (sawBlocks) out.Warn(L"WebVTT STYLE and REGION blocks were not carried over.");
  auto byStart = [](const Cue& a, const Cue& b) { return a.startMs < b.startMs; };
  if (!std::is_sorted(out.cues.begin(), out.cues.end(), byStart)) {
    std::stable_sort(out.cues.begin(), out.cues.end(), byStart);
    out.Warn(L"Some cues were out of order and have been sorted by start time.");
  }
  return true;
}

} // namespace

bool ParseSrt(std::string_view utf8, SubFile& out, std::wstring* err) {
  std::string s(utf8);
  if (StartsWith(s, "\xEF\xBB\xBF")) s.erase(0, 3);
  return ParseCues(SanitizeText(s), out, false, err);
}
bool ParseVtt(std::string_view utf8, SubFile& out, std::wstring* err) {
  std::string s(utf8);
  if (StartsWith(s, "\xEF\xBB\xBF")) s.erase(0, 3);
  return ParseCues(SanitizeText(s), out, true, err);
}

std::string WriteSrt(const SubFile& s, const SubWriteOptions& opt) {
  std::string nl = opt.crlf ? "\r\n" : "\n";
  std::string o;
  size_t n = 1;
  for (auto& c : s.cues) {
    o += std::to_string(n++) + nl;
    o += FormatSubTime(c.startMs, ',') + " --> " + FormatSubTime(c.endMs, ',') + nl;
    std::string text;
    for (auto& l : SplitStr(c.text, '\n')) {
      if (TrimAscii(l).empty()) continue;                    // a blank line would end the cue
      std::string x = l;
      size_t p;
      while ((p = x.find("-->")) != std::string::npos) x.replace(p, 3, "->");
      text += x + nl;
    }
    if (text.empty()) text = " " + nl;
    o += text + nl;
  }
  return o;
}

std::string WriteVtt(const SubFile& s, const SubWriteOptions& opt) {
  std::string nl = opt.crlf ? "\r\n" : "\n";
  std::string o = "WEBVTT" + nl + nl;
  for (auto& c : s.cues) {
    if (!c.id.empty() && c.id.find("-->") == std::string::npos) o += c.id + nl;
    o += FormatSubTime(c.startMs, '.') + " --> " + FormatSubTime(c.endMs, '.') + nl;
    std::string text;
    for (auto& l : SplitStr(c.text, '\n')) {
      if (TrimAscii(l).empty()) continue;
      text += EscapeVttText(l) + nl;
    }
    if (text.empty()) text = " " + nl;
    o += text + nl;
  }
  return o;
}

std::string WriteSubText(const SubFile& s, bool timestamps) {
  std::string o;
  for (auto& c : s.cues) {
    std::string plain;
    // strip the kept tags
    const std::string& t = c.text;
    for (size_t i = 0; i < t.size(); ++i) {
      if (t[i] == '<') { size_t e = t.find('>', i); if (e != std::string::npos && e - i <= 4) { i = e; continue; } }
      plain.push_back(t[i]);
    }
    if (TrimAscii(plain).empty()) continue;
    if (timestamps) o += "[" + FormatSubTime(c.startMs, '.') + " --> " + FormatSubTime(c.endMs, '.') + "] ";
    o += plain + "\n";
  }
  return o;
}

} // namespace filo::doc
