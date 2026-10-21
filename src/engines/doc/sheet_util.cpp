#include "engines/doc/sheet.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace filo::doc {

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
static void CivilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yy + (m <= 2));
}

bool ValidDate(int y, int m, int d) {
  if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max = dim[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
  return d <= max;
}

double SerialFromDate(int y, int m, int d) {
  int64_t days = DaysFromCivil(y, (unsigned)m, (unsigned)d);
  if (y < 1900 || (y == 1900 && m < 3)) return (double)(days - DaysFromCivil(1899, 12, 31));
  return (double)(days - DaysFromCivil(1899, 12, 30));
}

bool DateFromSerial(double serial, bool date1904, int& y, int& m, int& d, int& hh, int& mm, int& ss, int& ms) {
  if (!(serial >= 0) || serial > 2958465.99999) return false;
  double dayf = std::floor(serial);
  double frac = serial - dayf;
  int64_t totalMs = (int64_t)std::llround(frac * 86400000.0);
  int64_t days = (int64_t)dayf;
  if (totalMs >= 86400000) { totalMs -= 86400000; ++days; }
  if (date1904) CivilFromDays(DaysFromCivil(1904, 1, 1) + days, y, m, d);
  else if (days >= 61) CivilFromDays(DaysFromCivil(1899, 12, 30) + days, y, m, d);
  else if (days == 60) { y = 1900; m = 2; d = 29; }
  else if (days >= 1) CivilFromDays(DaysFromCivil(1899, 12, 31) + days, y, m, d);
  else { y = 1900; m = 1; d = 0; }
  hh = (int)(totalMs / 3600000); mm = (int)(totalMs / 60000 % 60); ss = (int)(totalMs / 1000 % 60); ms = (int)(totalMs % 1000);
  return true;
}

std::string FormatNumber(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    char buf[32]; auto r = std::to_chars(buf, buf + sizeof buf, (int64_t)v);
    return std::string(buf, r.ptr);
  }
  char buf[400];
  double a = std::fabs(v);
  std::to_chars_result r;
  if (a >= 1e21 || (a != 0 && a < 1e-7)) r = std::to_chars(buf, buf + sizeof buf, v);
  else r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
  return std::string(buf, r.ptr);
}

std::string IsoFromCell(const SCell& c) {
  switch (c.t) {
    case SCell::T::Empty: return {};
    case SCell::T::Text: return c.s;
    case SCell::T::Number: return FormatNumber(c.n);
    case SCell::T::Bool: return c.n != 0 ? "TRUE" : "FALSE";
    case SCell::T::Date: case SCell::T::DateTime: case SCell::T::Time: {
      if (c.t == SCell::T::Time) {
        int64_t tot = (int64_t)std::llround(c.n * 86400000.0);       // elapsed time may exceed 24 hours
        int64_t th = tot / 3600000; int tm2 = (int)(tot / 60000 % 60), ts = (int)(tot / 1000 % 60), tms = (int)(tot % 1000);
        return tms ? Printf("%02lld:%02d:%02d.%03d", (long long)th, tm2, ts, tms) : Printf("%02lld:%02d:%02d", (long long)th, tm2, ts);
      }
      int y, m, d, hh, mm, ss, ms;
      if (!DateFromSerial(c.n, false, y, m, d, hh, mm, ss, ms)) return FormatNumber(c.n);
      if (c.t == SCell::T::Date) return Printf("%04d-%02d-%02d", y, m, d);
      return ms ? Printf("%04d-%02d-%02d %02d:%02d:%02d.%03d", y, m, d, hh, mm, ss, ms) : Printf("%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mm, ss);
    }
  }
  return {};
}

std::string SafeSheetName(std::string_view name, const std::vector<std::string>& used) {
  std::string base;
  size_t i = 0;
  while (i < name.size() && Utf8Length(base) < 31) {
    size_t st = i; uint32_t c = DecodeUtf8(name, i);
    if (c < 0x20 || c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\') { base.push_back('_'); continue; }
    base.append(name.substr(st, i - st));
  }
  base = TrimAscii(base);
  while (!base.empty() && base.front() == '\'') base.erase(0, 1);
  while (!base.empty() && base.back() == '\'') base.pop_back();
  if (base.empty()) base = "Sheet";
  auto taken = [&](const std::string& n) { for (auto& u : used) if (LowerAscii(u) == LowerAscii(n)) return true; return false; };
  std::string cand = base;
  for (int k = 2; taken(cand) && k < 10000; ++k) {
    std::string suffix = " (" + std::to_string(k) + ")";
    std::string b = base;
    while (Utf8Length(b) + suffix.size() > 31 && !b.empty()) { while (!b.empty() && ((uint8_t)b.back() & 0xC0) == 0x80) b.pop_back(); if (!b.empty()) b.pop_back(); }
    cand = b + suffix;
  }
  return cand;
}

} // namespace filo::doc
