// LibreOffice (optional, system-wide install) driven headlessly as a separate process for the formats we cannot write ourselves
// (DOCX/ODT -> PDF, legacy .doc/.xls/.ppt reading, PPTX -> PDF, spreadsheets -> PDF).
#pragma once
#include "core/job.h"
#include "core/util.h"

namespace filo::doc {

// soffice.exe location: FILO_SOFFICE env override, <exe dir>\LibreOffice\program, registry (HKLM/HKCU, 64/32-bit views),
// Program Files\LibreOffice*, PATH. Empty when not installed. Cached (re-validated every few seconds).
fs::path FindLibreOffice();
inline bool LibreOfficeAvailable() { return !FindLibreOffice().empty(); }
void SetLibreOfficeDisabledForTests(bool disabled);     // simulates a machine without LibreOffice (tests only)
std::wstring LibreOfficeVersion();            // "26.8.0.3" style string from the exe's version resource, "" if unknown

struct LoConvertSpec {
  fs::path input;
  std::wstring inExt;                        // extension given to the private copy of the input ("docx", "xls", "html"...); empty => input's
  std::wstring convertTo;                    // --convert-to value: "pdf", "docx", "xlsx:Calc MS Excel 2007 XML", "txt:Text (encoded):76"
  std::wstring outExt;                       // extension of the file LibreOffice writes ("pdf", "docx", "xlsx", "txt")
  fs::path output;                           // where the produced file is placed (any existing file is replaced)
  DWORD timeoutMs = 0;                       // 0 = derived from the input size
};
struct LoResult { bool ok = false; std::wstring error; };

// Serialised (one LibreOffice at a time per machine), private profile (never the user's), copy of the input in a private temp
// folder, cancellation and timeout through core/process.h, stray soffice.bin cleanup. `ctx` may be null.
LoResult LibreOfficeConvert(const LoConvertSpec& spec, JobContext* ctx);

} // namespace filo::doc
