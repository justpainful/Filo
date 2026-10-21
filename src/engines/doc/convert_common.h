// Run functions behind the capabilities registered in register.cpp.
#pragma once
#include "registry/capability.h"
#include "engines/doc/docmodel.h"

namespace filo::doc {

// ---- option helpers -----------------------------------------------------------------------------------------------------
bool OptBool(const Json& o, const char* key, bool def);
std::string OptStr(const Json& o, const char* key, const std::string& def);
double OptNum(const Json& o, const char* key, double def);

EngineResult Fail(const std::wstring& why);
void AddWarnings(EngineResult& er, const std::vector<std::wstring>& w);

// ---- conversions --------------------------------------------------------------------------------------------------------
// Document group: docx/md/txt/html/rtf/rtfd/epub/textclipping/doc -> docx/md/txt/html/epub/pdf
EngineResult RunDocConversion(const ConvertTask& t, JobContext& ctx);
// csv/tsv/xlsx/ods/xls -> csv/tsv/xlsx/ods/pdf
EngineResult RunSheetConversion(const ConvertTask& t, JobContext& ctx);
EngineResult RunSheetFolderConversion(const ConvertTask& t, JobContext& ctx);        // xlsx/ods/xls -> one CSV/TSV per sheet in a folder
// srt/vtt -> srt/vtt/txt
EngineResult RunSubtitleConversion(const ConvertTask& t, JobContext& ctx);
// pptx/ppt -> pdf, pptx/ppt -> png/jpg (folder), pages -> pdf
EngineResult RunPresentationConversion(const ConvertTask& t, JobContext& ctx);
EngineResult RunPagesToPdf(const ConvertTask& t, JobContext& ctx);
// pdf -> docx/pptx/xlsx/epub (pdf_office.cpp)
EngineResult RunPdfToOffice(const ConvertTask& t, JobContext& ctx);

// image (jpg/png/webp/heic/tiff/bmp/gif/avif) -> docx: the picture on one page
EngineResult RunImageToDocx(const ConvertTask& t, JobContext& ctx);

// Verifier used by every capability: opens the produced file with an independent reader, dispatching on its (real) extension.
bool VerifyProduced(const fs::path& produced, std::wstring* why);

// Shared by pdf_office.cpp: renders a PDF through PDFium to images in `dir` (slide-001.png...). Returns the page count or -1.
int RenderPdfToImages(const fs::path& pdf, const fs::path& dir, bool jpeg, int dpi, int jpegQuality, JobContext& ctx, std::wstring* err, double progressFrom, double progressTo);
// LibreOffice helpers used by several run functions.
bool LoViaTemp(const fs::path& input, const std::wstring& inExt, const std::wstring& convertTo, const std::wstring& outExt, const fs::path& outFile, JobContext& ctx, std::wstring* err);
// Reads a whole input document into the model according to the detected format of `t.inKind`.
bool LoadDocument(const ConvertTask& t, JobContext& ctx, Document& d, std::wstring* err);

} // namespace filo::doc
