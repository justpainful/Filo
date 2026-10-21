// Thin RAII wrapper around PDFium (prebuilt DLL, third_party/pdfium).
//
// THREADING: PDFium is not thread-safe, not even across documents. Every wrapper call takes the process-wide
// PdfLibrary::Mutex() (recursive), so jobs on different worker threads may use different PdfDocument objects safely but
// serialise page by page. Code that calls raw FPDF_* functions must hold a PdfLock for the duration of the call(s).
// A PdfDocument itself must not be used by two threads at the same time (the lock makes it safe, not meaningful).
#pragma once
#include "core/job.h"
#include "core/util.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct fpdf_document_t__;   // PDFium opaque types (forward declared so this header does not drag in fpdfview.h)
struct fpdf_page_t__;

namespace filo::pdf {

class PdfLibrary {
 public:
  static void Ensure();                         // one-time FPDF_InitLibraryWithConfig (thread-safe, idempotent)
  static std::recursive_mutex& Mutex();         // process-wide PDFium lock
};
class PdfLock {                                 // RAII: Ensure() + lock
 public:
  PdfLock() { PdfLibrary::Ensure(); lk_ = std::unique_lock<std::recursive_mutex>(PdfLibrary::Mutex()); }
 private:
  std::unique_lock<std::recursive_mutex> lk_;
};

enum class OpenStatus { Ok, NeedsPassword, WrongPassword, Corrupt, NotPdf, NotFound, IoError };
// User-facing sentence for a non-Ok status.
std::wstring OpenStatusMessage(OpenStatus s);

struct Bitmap {                                 // top-down BGRA, stride = width*4, straight (non-premultiplied) alpha
  int width = 0, height = 0;
  std::vector<uint8_t> bgra;
};

struct RenderOptions {
  double dpi = 72;              // output pixels per inch (page points * dpi / 72); ignored when maxSidePx > 0
  int maxSidePx = 0;            // >0: scale so the longer side has exactly this many pixels
  bool annotations = true;      // draw annotations (FPDF_ANNOT)
  bool forms = true;            // draw interactive form fields (needs a form environment; silently skipped if unavailable)
  bool grayscale = false;
  bool transparent = false;     // transparent background (alpha); otherwise opaque white
  bool lcdText = false;
  double maxPixels = 150e6;     // safety cap; larger requests are scaled down (RenderPage reports it via *clamped)
};

struct DocMeta { std::wstring title, author, subject, keywords, creator, producer, creationDate, modDate; };

class PdfDocument {
 public:
  struct OpenResult {
    OpenStatus status = OpenStatus::Ok;
    std::wstring message;                       // user-facing sentence when status != Ok
    std::unique_ptr<PdfDocument> doc;
    bool ok() const { return status == OpenStatus::Ok && doc; }
  };
  // Opens read-only (the source file is only ever read, never locked against other readers/writers). PDFium's repair path is
  // used for damaged cross-reference tables; if the file still cannot be parsed the status is Corrupt.
  static OpenResult Open(const fs::path& file, const std::wstring& password = {});
  // New empty document (for assembling output).
  static std::unique_ptr<PdfDocument> CreateNew();
  ~PdfDocument();
  PdfDocument(const PdfDocument&) = delete;
  PdfDocument& operator=(const PdfDocument&) = delete;

  int PageCount() const;
  bool PageSize(int index, float* widthPt, float* heightPt) const;   // as displayed (rotation applied)
  int PageRotation(int index) const;                                  // degrees 0/90/180/270, -1 on error
  std::wstring PageLabel(int index) const;                            // "" when the document has no labels
  DocMeta Meta() const;
  bool IsEncrypted() const { return encrypted_; }
  const fs::path& Path() const { return path_; }
  int FileVersion() const;                                            // 14 = PDF 1.4, 0 unknown

  // Rendering. `clamped` (optional) is set when the requested size was reduced to respect RenderOptions::maxPixels.
  bool RenderPage(int index, const RenderOptions& opt, Bitmap& out, std::wstring* err = nullptr, bool* clamped = nullptr) const;
  bool RenderThumbnail(int index, int maxSide, Bitmap& out, std::wstring* err = nullptr) const;   // white bg, annotations on

  // PDFium's own plain text for the page (no layout analysis; see pdf_text.h for reading-order extraction).
  std::wstring PageText(int index) const;

  // ---- page editing (in memory; Save() writes a new file, the source file is never touched) ----
  // Copies `srcPages` (0-based, in the given order, repeats allowed) from `src` into this document at `insertAt` (-1 = append).
  bool ImportPages(const PdfDocument& src, const std::vector<int>& srcPages, int insertAt = -1, std::wstring* err = nullptr);
  bool RotatePage(int index, int deltaDegrees);        // delta must be a multiple of 90; adds to the current rotation
  bool SetPageRotation(int index, int degrees);
  bool DeletePage(int index);                          // NOTE: PDFium keeps the page's objects in the file; build outputs with ImportPages
  bool DuplicatePage(int index);                       // inserts a copy right after `index`
  bool MovePage(int from, int to);                     // `to` = final index of the page
  // Writes the whole document (full rewrite). `cancel` is polled while writing. Returns false and a user-facing error on failure.
  // `removeSecurity` writes an unencrypted file (used by operations on password-opened sources).
  bool Save(const fs::path& file, std::wstring* err = nullptr, const std::atomic<bool>* cancel = nullptr,
            bool removeSecurity = false) const;

  // Raw handles for code inside the engine (hold a PdfLock while using them).
  fpdf_document_t__* Handle() const { return doc_; }

 private:
  PdfDocument() = default;
  struct Impl;
  fpdf_document_t__* doc_ = nullptr;
  std::unique_ptr<Impl> impl_;
  fs::path path_;
  bool encrypted_ = false;
  mutable void* form_ = nullptr;                       // FPDF form-fill environment (lazy)
  mutable bool formTried_ = false;
  friend struct Impl;
  void* EnsureForm() const;
};

// RAII page handle (hold PdfLock while using).
class PageHandle {
 public:
  PageHandle(const PdfDocument& d, int index);
  ~PageHandle();
  PageHandle(const PageHandle&) = delete;
  PageHandle& operator=(const PageHandle&) = delete;
  explicit operator bool() const { return page_ != nullptr; }
  fpdf_page_t__* get() const { return page_; }
 private:
  fpdf_page_t__* page_ = nullptr;
};

// ---- document list helpers (for UI lists) -----------------------------------------------------------------------------

struct PdfFileInfo {
  OpenStatus status = OpenStatus::Ok;
  std::wstring message;
  int pageCount = 0;
  float firstPageWidthPt = 0, firstPageHeightPt = 0;
  bool encrypted = false;
  int version = 0;
  uint64_t sizeBytes = 0;
  DocMeta meta;
};
PdfFileInfo GetPdfFileInfo(const fs::path& file, const std::wstring& password = {});

// Async-friendly thumbnail lister: open once, then render ONE page per call from any worker thread (the caller decides the
// scheduling, cancellation and where results go).
class PageThumbnailer {
 public:
  static std::unique_ptr<PageThumbnailer> Open(const fs::path& file, const std::wstring& password, OpenStatus* status = nullptr,
                                               std::wstring* message = nullptr);
  int PageCount() const { return doc_ ? doc_->PageCount() : 0; }
  bool Render(int index, int maxSide, Bitmap& out, std::wstring* err = nullptr) const;   // white background
  bool PageSize(int index, float* w, float* h) const { return doc_ && doc_->PageSize(index, w, h); }
  const PdfDocument& Document() const { return *doc_; }
 private:
  std::unique_ptr<PdfDocument> doc_;
};

} // namespace filo::pdf
