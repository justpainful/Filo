// Headless PDF operations (merge / split / organize / compress / images -> PDF). Every operation writes through
// OutputTransaction / OutputDirTransaction (never to the final path directly), reports real progress through the JobContext
// and honours cancellation. The source files are only ever read.
//
// Errors are user-facing sentences in OpResult::error. Source PDFs that need a password fail with a sentence naming the file.
#pragma once
#include "core/job.h"
#include "core/util.h"
#include "engines/pdf/pdf_doc.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace filo::pdf {

struct OpResult {
  bool ok = false;
  bool cancelled = false;
  std::wstring error;
  std::vector<std::wstring> warnings;
  std::vector<fs::path> outputs;       // committed output file(s) / folder
  int pages = 0;                       // pages written (merge/organize/create) or files written (split)
  // compress only
  bool notSmaller = false;             // no gain: nothing was written, the original stays the best version
  uint64_t inBytes = 0, outBytes = 0;
  int imagesRecompressed = 0;
};

struct PdfInput { fs::path path; std::wstring password; };

// ---- page ranges -------------------------------------------------------------------------------------------------------
// "1-3,5,8-" (1-based; "8-" = to the end, "-3" = first three, whitespace ignored, "last" accepted). Returns inclusive 0-based
// ranges in the order written. On error returns false with a user-facing sentence.
bool ParsePageRanges(const std::wstring& spec, int pageCount, std::vector<std::pair<int, int>>& out, std::wstring* err);

// ---- merge ------------------------------------------------------------------------------------------------------------
struct MergeOptions {
  bool keepOutlines = true;    // bookmarks of every source are kept, grouped under one entry per source file
};
OpResult MergePdfs(const std::vector<fs::path>& ordered, const fs::path& desiredOutput, JobContext& ctx,
                   const std::map<fs::path, std::wstring>& passwords = {}, const MergeOptions& opt = {});

// ---- split ------------------------------------------------------------------------------------------------------------
enum class SplitMode { Ranges, EveryN, SinglePages };
struct SplitOptions {
  SplitMode mode = SplitMode::SinglePages;
  std::wstring ranges;         // Ranges: one output file per comma separated range
  int everyN = 1;              // EveryN
  std::wstring password;
};
// Writes "<stem>_001.pdf", "<stem>_002.pdf"... into a new folder (desiredDir is the folder the user expects, e.g. "name (split)").
OpResult SplitPdf(const fs::path& input, const fs::path& desiredDir, const SplitOptions& opt, JobContext& ctx);

// ---- organize ---------------------------------------------------------------------------------------------------------
struct PageRef {
  int sourceDoc = 0;           // index into the sources list
  int pageIndex = 0;           // 0-based page in that source
  int rotationDelta = 0;       // degrees, multiple of 90 (added to the page's own /Rotate)
  bool duplicate = false;      // emit this page twice in a row (the same page repeated as separate entries works too)
};
// The output is exactly the listed pages in the listed order: omit a page to delete it, reorder entries to move it.
OpResult OrganizePdf(const std::vector<PdfInput>& sources, const std::vector<PageRef>& pages, const fs::path& desiredOutput,
                     JobContext& ctx);

// ---- compress ---------------------------------------------------------------------------------------------------------
enum class CompressPreset { Low, Medium, High };    // Low = best quality / least saving
struct CompressOptions {
  CompressPreset preset = CompressPreset::Medium;
  int targetPercent = 0;       // >0: aim for this share of the original size (steps through stronger settings until reached)
  uint64_t targetBytes = 0;    // >0: aim for this absolute size
  std::wstring password;
  bool recompressImages = true;
};
// Re-saves the document (dropping superseded revisions) and downsamples / re-encodes large embedded images.
// If the result is not smaller than the input nothing is written and OpResult::notSmaller is set (ok stays true).
OpResult CompressPdf(const fs::path& input, const fs::path& desiredOutput, const CompressOptions& opt, JobContext& ctx);

// ---- images -> PDF ----------------------------------------------------------------------------------------------------
enum class PageSizeKind { A4, A3, A5, Letter, Legal, FitImage, Custom };
enum class PageOrientation { Auto, Portrait, Landscape };
enum class FitMode { Fit, Fill, Stretch };
struct ImagesToPdfOptions {
  PageSizeKind size = PageSizeKind::A4;
  double customWidthPt = 595.276, customHeightPt = 841.89;
  PageOrientation orientation = PageOrientation::Auto;
  double marginPt = 0;
  FitMode fit = FitMode::Fit;
  double dpi = 150;            // FitImage: page = pixels * 72 / dpi
};
// JPEG inputs are embedded byte-for-byte (EXIF orientation is applied through the placement matrix); other formats are decoded
// with WIC and stored losslessly (Flate), EXIF orientation honoured. One page per image, in the given order.
OpResult CreatePdfFromImages(const std::vector<fs::path>& images, const fs::path& desiredOutput, const ImagesToPdfOptions& opt,
                             JobContext& ctx);

} // namespace filo::pdf
