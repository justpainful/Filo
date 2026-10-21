#include "engines/doc/sheet.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace filo::doc {

std::vector<std::vector<std::string>> SplitCsvRecords(std::string_view s, char delim, char quote) {
  std::vector<std::vector<std::string>> recs;
  std::vector<std::string> row;
  std::string field;
  bool inQ = false, wasQuoted = false, any = false;
  size_t n = s.size();
  auto endRecord = [&] {
    row.push_back(std::move(field)); field.clear();
    recs.push_back(std::move(row)); row.clear();
    wasQuoted = false; any = false;
  };
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (inQ) {
      if (c == quote) {
        if (i + 1 < n && s[i + 1] == quote) { field.push_back(quote); ++i; }
        else inQ = false;
      } else field.push_back(c);
      continue;
    }
    if (c == quote && field.empty() && !wasQuoted) { inQ = true; wasQuoted = true; any = true; continue; }
    if (c == delim) { row.push_back(std::move(field)); field.clear(); wasQuoted = false; any = true; continue; }
    if (c == '\r' || c == '\n') {
      if (c == '\r' && i + 1 < n && s[i + 1] == '\n') ++i;
      endRecord();
      continue;
    }
    field.push_back(c); any = true;
  }
  if (any || !field.empty() || !row.empty()) endRecord();
  return recs;
}

char DetectDelimiter(std::string_view s, char quote) {
  // Excel hint
  if (StartsWith(s, "sep=") && s.size() >= 5) { char c = s[4]; if (c != '\r' && c != '\n') return c; }
  std::string_view sample = s.substr(0, 96 * 1024);
  // do not cut a record in the middle: trim to the last line break
  size_t nl = sample.find_last_of("\n");
  if (nl != std::string_view::npos && sample.size() == 96 * 1024) sample = sample.substr(0, nl);
  const char cands[] = {',', ';', '\t', '|'};
  double bestScore = -1; char best = ',';
  for (char d : cands) {
    auto recs = SplitCsvRecords(sample, d, quote);
    if (recs.size() > 200) recs.resize(200);
    if (recs.empty()) continue;
    std::map<size_t, size_t> hist;
    size_t nonBlank = 0;
    for (auto& r : recs) { if (r.size() == 1 && r[0].empty()) continue; ++hist[r.size()]; ++nonBlank; }
    if (!nonBlank) continue;
    size_t modal = 0, modalCount = 0;
    for (auto& [cols, cnt] : hist) if (cnt > modalCount || (cnt == modalCount && cols > modal)) { modal = cols; modalCount = cnt; }
    double score = modal > 1 ? (double)modalCount / (double)nonBlank * 100.0 + std::min<double>((double)modal, 20.0) * 0.01 : 0.0;
    if (score > bestScore + 1e-9) { bestScore = score; best = d; }
  }
  return best;
}

// ---- type inference ---------------------------------------------------------------------------------------------------------
static bool AllDigits(std::string_view s) { if (s.empty()) return false; for (char c : s) if (c < '0' || c > '9') return false; return true; }

static bool ParseTimePart(std::string_view t, int& hh, int& mm, int& ss, int& ms) {
  // hh:mm[:ss[.fff]]
  auto parts = SplitStr(t, ':');
  if (parts.size() < 2 || parts.size() > 3) return false;
  if (!AllDigits(parts[0]) || parts[0].size() > 2 || !AllDigits(parts[1]) || parts[1].size() != 2) return false;
  hh = atoi(parts[0].c_str()); mm = atoi(parts[1].c_str()); ss = 0; ms = 0;
  if (parts.size() == 3) {
    std::string sec = parts[2]; std::string frac;
    size_t dot = sec.find('.'); if (dot != std::string::npos) { frac = sec.substr(dot + 1); sec.resize(dot); }
    if (!AllDigits(sec) || sec.size() != 2) return false;
    ss = atoi(sec.c_str());
    if (!frac.empty()) { if (!AllDigits(frac)) return false; std::string f3 = (frac + "000").substr(0, 3); ms = atoi(f3.c_str()); }
  }
  return hh < 24 && mm < 60 && ss < 60;
}

static bool ParseNumberStrict(const std::string& s, double& out) {
  // [-]digits[.digits], no leading zeros (except "0" / "0.x"), at most 15 significant digits, integers below 10 digits
  size_t i = 0;
  if (i < s.size() && s[i] == '-') ++i;
  size_t ds = i;
  while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
  size_t intDigits = i - ds;
  if (intDigits == 0) return false;
  if (intDigits > 1 && s[ds] == '0') return false;
  size_t fracDigits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i; size_t fs = i;
    while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
    fracDigits = i - fs;
    if (fracDigits == 0) return false;
  }
  if (i != s.size()) return false;
  if (fracDigits == 0 && intDigits >= 10) return false;      // phone numbers, account numbers, ids
  if (intDigits + fracDigits > 15) return false;
  out = strtod(s.c_str(), nullptr);
  return true;
}

static bool ParseNumberLoose(std::string s, double& out) {
  s = TrimAscii(s);
  if (s.empty()) return false;
  std::string t;
  // US-style thousands separators: 1,234,567.89
  size_t comma = s.find(',');
  if (comma != std::string::npos) {
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    std::string rest = s.substr(i); size_t dot = rest.find('.');
    std::string ip = dot == std::string::npos ? rest : rest.substr(0, dot);
    auto groups = SplitStr(ip, ',');
    if (groups.size() < 2 || groups[0].empty() || groups[0].size() > 3 || !AllDigits(groups[0])) return false;
    for (size_t g = 1; g < groups.size(); ++g) if (groups[g].size() != 3 || !AllDigits(groups[g])) return false;
    for (char c : s) if (c != ',') t.push_back(c);
  } else t = s;
  char* endp = nullptr;
  if (t.empty() || !(isdigit((unsigned char)t[0]) || t[0] == '-' || t[0] == '+' || t[0] == '.')) return false;
  if (t.find_first_of("xXnNiI") != std::string::npos) return false;      // hex, nan, inf
  double v = strtod(t.c_str(), &endp);
  if (!endp || *endp != 0 || std::isnan(v) || std::isinf(v)) return false;
  out = v;
  return true;
}

SCell InferCell(const std::string& in, bool inferAll, bool dayFirst) {
  SCell c;
  if (in.empty()) return c;
  std::string s = in;
  auto text = [&]() { c.t = SCell::T::Text; c.s = in; return c; };
  if (!inferAll) {
    if (s.front() == ' ' || s.back() == ' ' || s.front() == '\t') return text();
  } else s = TrimAscii(s);
  if (s.empty()) return text();
  // ISO date / datetime
  if (s.size() >= 10 && s[4] == '-' && s[7] == '-' && AllDigits(s.substr(0, 4)) && AllDigits(s.substr(5, 2)) && AllDigits(s.substr(8, 2))) {
    int y = atoi(s.substr(0, 4).c_str()), m = atoi(s.substr(5, 2).c_str()), d = atoi(s.substr(8, 2).c_str());
    if (ValidDate(y, m, d)) {
      if (s.size() == 10) { c.t = SCell::T::Date; c.n = SerialFromDate(y, m, d); return c; }
      if ((s[10] == 'T' || s[10] == ' ')) {
        std::string tp = s.substr(11);
        if (!tp.empty() && tp.back() == 'Z') tp.pop_back();
        int hh, mm, ss, ms;
        if (ParseTimePart(tp, hh, mm, ss, ms)) { c.t = SCell::T::DateTime; c.n = SerialFromDate(y, m, d) + (hh * 3600000.0 + mm * 60000.0 + ss * 1000.0 + ms) / 86400000.0; return c; }
      }
    }
    return text();
  }
  if (!inferAll) {
    double v;
    if (ParseNumberStrict(s, v)) { c.t = SCell::T::Number; c.n = v; return c; }
    return text();
  }
  // ---- aggressive mode ----
  {
    std::string l = LowerAscii(s);
    if (l == "true" || l == "false") { c.t = SCell::T::Bool; c.n = l == "true"; return c; }
  }
  // time of day
  {
    int hh, mm, ss, ms;
    if (s.find(':') != std::string::npos && s.size() <= 12 && ParseTimePart(s, hh, mm, ss, ms)) { c.t = SCell::T::Time; c.n = (hh * 3600000.0 + mm * 60000.0 + ss * 1000.0 + ms) / 86400000.0; return c; }
  }
  // d/m/y, m/d/y, y/m/d with / . - separators
  for (char sep : {'/', '.', '-'}) {
    auto p = SplitStr(s, sep);
    if (p.size() == 3 && AllDigits(p[0]) && AllDigits(p[1]) && AllDigits(p[2]) && p[0].size() <= 4 && p[1].size() <= 2 && p[2].size() <= 4) {
      int a = atoi(p[0].c_str()), b = atoi(p[1].c_str()), cc = atoi(p[2].c_str());
      int y, m, d;
      if (p[0].size() == 4) { y = a; m = b; d = cc; }
      else {
        y = cc; if (p[2].size() <= 2) y += y < 70 ? 2000 : 1900;
        if (a > 12) { d = a; m = b; } else if (b > 12) { m = a; d = b; } else if (dayFirst) { d = a; m = b; } else { m = a; d = b; }
      }
      if (p[0].size() != 4 && p[2].size() < 2) continue;
      if (ValidDate(y, m, d)) { c.t = SCell::T::Date; c.n = SerialFromDate(y, m, d); return c; }
    }
  }
  double v;
  if (ParseNumberLoose(s, v)) { c.t = SCell::T::Number; c.n = v; return c; }
  return text();
}

// ---- parse ----------------------------------------------------------------------------------------------------------------------
bool ParseCsv(const Bytes& data, const CsvReadOptions& opt, Sheet& out, CsvInfo* info, std::wstring* err) {
  DecodedText dt = DecodeText(data, opt.encoding);
  std::string text = dt.utf8;
  // NUL and other control characters would make a binary file look like data
  size_t bad = 0, lim = std::min<size_t>(text.size(), 65536);
  for (size_t i = 0; i < lim; ++i) { unsigned char c = (unsigned char)text[i]; if (c < 9 || (c > 13 && c < 32)) ++bad; }
  if (lim && bad * 20 > lim) { if (err) *err = L"This file does not look like a text table (it contains binary data)."; return false; }
  text = SanitizeText(text);
  char delim = opt.delimiter ? opt.delimiter : DetectDelimiter(text, opt.quote);
  if (StartsWith(text, "sep=") && text.size() > 5) { size_t nl = text.find_first_of("\r\n"); if (nl != std::string::npos && nl <= 6) { text.erase(0, nl); size_t k = 0; while (k < text.size() && (text[k] == '\r' || text[k] == '\n')) ++k; text.erase(0, k); } }
  auto recs = SplitCsvRecords(text, delim, opt.quote);
  while (!recs.empty() && recs.back().size() == 1 && recs.back()[0].empty()) recs.pop_back();
  out.rows.clear();
  out.rows.reserve(recs.size());
  size_t cols = 0;
  for (auto& r : recs) {
    std::vector<SCell> row;
    if (!(r.size() == 1 && r[0].empty())) {
      row.reserve(r.size());
      for (auto& f : r) { if (opt.keepText) { SCell tc; if (!f.empty()) { tc.t = SCell::T::Text; tc.s = f; } row.push_back(std::move(tc)); } else row.push_back(InferCell(f, opt.inferTypes, opt.dayFirst)); }
    }
    cols = std::max(cols, row.size());
    out.rows.push_back(std::move(row));
  }
  if (info) { info->delimiter = delim; info->encoding = dt.detected; info->rows = out.rows.size(); info->cols = cols; }
  return true;
}

// ---- write ----------------------------------------------------------------------------------------------------------------------
Bytes WriteCsv(const Sheet& s, const CsvWriteOptions& opt) {
  std::string out;
  out.reserve(s.rows.size() * 32);
  const char* nl = opt.crlf ? "\r\n" : "\n";
  for (auto& row : s.rows) {
    size_t last = row.size();
    while (last > 0 && row[last - 1].t == SCell::T::Empty) --last;
    for (size_t i = 0; i < last; ++i) {
      if (i) out.push_back(opt.delimiter);
      std::string f = IsoFromCell(row[i]);
      bool needQuote = f.find(opt.delimiter) != std::string::npos || f.find(opt.quote) != std::string::npos || f.find('\n') != std::string::npos || f.find('\r') != std::string::npos ||
                       (last == 1 && f.empty());
      if (needQuote) {
        out.push_back(opt.quote);
        for (char c : f) { if (c == opt.quote) out.push_back(opt.quote); out.push_back(c); }
        out.push_back(opt.quote);
      } else out += f;
    }
    out += nl;
  }
  return EncodeText(out, opt.encoding);
}

} // namespace filo::doc
