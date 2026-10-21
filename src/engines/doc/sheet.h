// Spreadsheet model and format codecs: CSV/TSV, XLSX and ODS (own implementations, no spreadsheet library).
#pragma once
#include "engines/doc/text_io.h"

namespace filo::doc {

struct SCell {
  enum class T : uint8_t { Empty, Text, Number, Bool, Date, DateTime, Time } t = T::Empty;
  double n = 0;          // Number; Bool (0/1); Date/DateTime: days since 1899-12-30 (fraction = time of day); Time: fraction of a day
  std::string s;         // Text
};
struct Sheet {
  std::string name = "Sheet1";
  std::vector<std::vector<SCell>> rows;
  size_t MaxCols() const { size_t m = 0; for (auto& r : rows) m = std::max(m, r.size()); return m; }
};
struct Workbook {
  std::vector<Sheet> sheets;
  std::vector<std::wstring> warnings;
  bool date1904 = false;
  void Warn(const std::wstring& w) { for (auto& x : warnings) if (x == w) return; warnings.push_back(w); }
};

// ---- date / number helpers ------------------------------------------------------------------------------------------------
bool ValidDate(int y, int m, int d);
double SerialFromDate(int y, int m, int d);                          // 1900 date system, Excel compatible (incl. the 1900 leap-year quirk)
bool DateFromSerial(double serial, bool date1904, int& y, int& m, int& d, int& hh, int& mm, int& ss, int& ms);
std::string FormatNumber(double v);                                  // shortest round-trip, never scientific for ordinary magnitudes
std::string IsoFromCell(const SCell& c);                             // text form used by CSV: numbers, ISO dates, TRUE/FALSE
std::string SafeSheetName(std::string_view name, const std::vector<std::string>& used);   // <=31 chars, no []:*?/\, unique (case-insensitive)

// ---- CSV / TSV ---------------------------------------------------------------------------------------------------------------
struct CsvReadOptions {
  char delimiter = 0;            // 0 = auto-detect (, ; tab |), honours an Excel "sep=" line
  char quote = '"';
  TextEnc encoding = TextEnc::Auto;
  bool inferTypes = false;       // convert guarded values too (leading zeros, long digit strings, ambiguous dates...)
  bool dayFirst = true;          // dd/mm vs mm/dd when inferTypes and ambiguous
  bool keepText = false;         // no type inference at all: every non-empty field stays text (csv <-> tsv)
};
struct CsvInfo { char delimiter = ','; TextEnc encoding = TextEnc::Utf8; size_t rows = 0, cols = 0; };
// Parses into one sheet of typed cells. Type inference is conservative unless options.inferTypes is set (see ENGINE docs).
bool ParseCsv(const Bytes& data, const CsvReadOptions& opt, Sheet& out, CsvInfo* info, std::wstring* err);
// Pure record splitter (no type inference): used by tests and the delimiter detector.
std::vector<std::vector<std::string>> SplitCsvRecords(std::string_view utf8, char delimiter, char quote);
char DetectDelimiter(std::string_view utf8, char quote);
SCell InferCell(const std::string& s, bool inferAll, bool dayFirst);

struct CsvWriteOptions {
  char delimiter = ',';
  TextEnc encoding = TextEnc::Utf8Bom;
  char quote = '"';
  bool crlf = true;
};
Bytes WriteCsv(const Sheet& s, const CsvWriteOptions& opt);

// ---- XLSX ----------------------------------------------------------------------------------------------------------------------
bool WriteXlsx(const Workbook& wb, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings = nullptr);
bool ReadXlsx(const fs::path& file, Workbook& wb, std::wstring* err);
bool VerifyXlsx(const fs::path& file, std::wstring* why);

// ---- ODS -----------------------------------------------------------------------------------------------------------------------
bool WriteOds(const Workbook& wb, const fs::path& out, std::wstring* err, std::vector<std::wstring>* warnings = nullptr);
bool ReadOds(const fs::path& file, Workbook& wb, std::wstring* err);
bool VerifyOds(const fs::path& file, std::wstring* why);

} // namespace filo::doc
