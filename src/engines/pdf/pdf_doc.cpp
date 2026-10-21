#include "engines/pdf/pdf_doc.h"
#include "core/log.h"
#include "fpdfview.h"
#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_formfill.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdf_text.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace filo::pdf {

// ---- library ------------------------------------------------------------------------------------------------------

void PdfLibrary::Ensure() {
  static std::once_flag once;
  std::call_once(once, [] {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    cfg.m_pUserFontPaths = nullptr;
    cfg.m_pIsolate = nullptr;
    cfg.m_v8EmbedderSlot = 0;
    FPDF_InitLibraryWithConfig(&cfg);
  });
}
std::recursive_mutex& PdfLibrary::Mutex() {
  static std::recursive_mutex* m = new std::recursive_mutex();   // intentionally leaked: usable during static destruction
  return *m;
}

std::wstring OpenStatusMessage(OpenStatus s) {
  switch (s) {
    case OpenStatus::Ok: return L"";
    case OpenStatus::NeedsPassword: return L"This PDF is password-protected. Enter the password to open it.";
    case OpenStatus::WrongPassword: return L"The password is incorrect.";
    case OpenStatus::Corrupt: return L"This PDF is damaged and could not be repaired.";
    case OpenStatus::NotPdf: return L"This file is not a PDF document.";
    case OpenStatus::NotFound: return L"The file is no longer available.";
    case OpenStatus::IoError: return L"The PDF could not be read.";
  }
  return L"";
}

static std::wstring Sentence(std::wstring s) {
  while (!s.empty() && (iswspace(s.back()))) s.pop_back();
  if (!s.empty() && s.back() != L'.' && s.back() != L'!' && s.back() != L'?') s += L'.';
  return s;
}

static std::wstring FromUtf16Bytes(const std::vector<uint8_t>& b) {
  size_t n = b.size() / 2;
  std::wstring s((const wchar_t*)b.data(), n);
  while (!s.empty() && s.back() == L'\0') s.pop_back();
  return s;
}

// ---- document impl ------------------------------------------------------------------------------------------------

struct PdfDocument::Impl {
  HANDLE file = INVALID_HANDLE_VALUE;
  FPDF_FILEACCESS access{};
  FPDF_FORMFILLINFO ffi{};
  FPDF_FORMHANDLE form = nullptr;
  ~Impl() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};

static int FileBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
  HANDLE h = (HANDLE)param;
  OVERLAPPED ov{};
  ov.Offset = pos;
  DWORD got = 0;
  if (!ReadFile(h, buf, size, &got, &ov)) return 0;
  return got == size ? 1 : 0;
}

static std::string PasswordBytes(const std::wstring& pw, bool utf8) {
  if (pw.empty()) return {};
  if (utf8) return Narrow(pw);
  int n = WideCharToMultiByte(CP_ACP, 0, pw.data(), (int)pw.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_ACP, 0, pw.data(), (int)pw.size(), s.data(), n, nullptr, nullptr);
  return s;
}

PdfDocument::OpenResult PdfDocument::Open(const fs::path& file, const std::wstring& password) {
  OpenResult r;
  auto fail = [&](OpenStatus s, std::wstring msg = {}) {
    r.status = s; r.message = msg.empty() ? OpenStatusMessage(s) : msg; r.doc.reset(); return std::move(r);
  };
  PdfLock lock;
  HANDLE h = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return fail(OpenStatus::NotFound);
    return fail(OpenStatus::IoError, Sentence(L"The PDF could not be opened: " + LastErrorMessage(e)));
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return fail(OpenStatus::IoError); }
  if (sz.QuadPart == 0) { CloseHandle(h); return fail(OpenStatus::NotPdf, L"The file is empty."); }
  if (sz.QuadPart > 0xFFFFFFF0ll) { CloseHandle(h); return fail(OpenStatus::IoError, L"This PDF is larger than 4 GB, which is not supported."); }

  // Cheap "is this a PDF at all" probe: the header must appear within the first KiB (spec) - we accept up to 4 KiB.
  {
    char head[4096]; DWORD got = 0; OVERLAPPED ov{};
    if (!ReadFile(h, head, sizeof(head), &got, &ov) && GetLastError() != ERROR_HANDLE_EOF) { CloseHandle(h); return fail(OpenStatus::IoError); }
    bool found = false;
    for (DWORD i = 0; i + 5 <= got && !found; ++i) if (std::memcmp(head + i, "%PDF-", 5) == 0) found = true;
    if (!found) { CloseHandle(h); return fail(OpenStatus::NotPdf); }
  }

  auto doc = std::unique_ptr<PdfDocument>(new PdfDocument());
  doc->impl_ = std::make_unique<Impl>();
  doc->impl_->file = h;
  doc->impl_->access.m_FileLen = (unsigned long)sz.QuadPart;
  doc->impl_->access.m_GetBlock = &FileBlock;
  doc->impl_->access.m_Param = h;
  doc->path_ = file;

  FPDF_DOCUMENT d = nullptr;
  unsigned long err = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string pw = PasswordBytes(password, attempt == 0);
    d = FPDF_LoadCustomDocument(&doc->impl_->access, pw.empty() ? nullptr : pw.c_str());
    if (d) break;
    err = FPDF_GetLastError();
    bool nonAscii = false; for (wchar_t c : password) if (c > 127) nonAscii = true;
    if (err != FPDF_ERR_PASSWORD || !nonAscii) break;      // retry with the ANSI code page only for non-ASCII passwords
  }
  if (!d) {
    switch (err) {
      case FPDF_ERR_PASSWORD: return fail(password.empty() ? OpenStatus::NeedsPassword : OpenStatus::WrongPassword);
      case FPDF_ERR_SECURITY: return fail(OpenStatus::Corrupt, L"This PDF uses a security method that is not supported.");
      case FPDF_ERR_FILE: return fail(OpenStatus::IoError);
      default: return fail(OpenStatus::Corrupt);
    }
  }
  doc->doc_ = (fpdf_document_t__*)d;
  doc->encrypted_ = FPDF_GetSecurityHandlerRevision(d) != -1;
  // Reject documents that opened but have no usable page tree (severe damage).
  if (FPDF_GetPageCount(d) <= 0) return fail(OpenStatus::Corrupt);
  r.status = OpenStatus::Ok;
  r.doc = std::move(doc);
  return r;
}

std::unique_ptr<PdfDocument> PdfDocument::CreateNew() {
  PdfLock lock;
  FPDF_DOCUMENT d = FPDF_CreateNewDocument();
  if (!d) return nullptr;
  auto doc = std::unique_ptr<PdfDocument>(new PdfDocument());
  doc->impl_ = std::make_unique<Impl>();
  doc->doc_ = (fpdf_document_t__*)d;
  return doc;
}

PdfDocument::~PdfDocument() {
  PdfLock lock;
  if (impl_ && impl_->form) FPDFDOC_ExitFormFillEnvironment(impl_->form);
  if (doc_) FPDF_CloseDocument((FPDF_DOCUMENT)doc_);
}

int PdfDocument::PageCount() const { PdfLock l; return doc_ ? FPDF_GetPageCount((FPDF_DOCUMENT)doc_) : 0; }

PageHandle::PageHandle(const PdfDocument& d, int index) {
  PdfLock l;
  if (d.Handle() && index >= 0 && index < FPDF_GetPageCount((FPDF_DOCUMENT)d.Handle()))
    page_ = (fpdf_page_t__*)FPDF_LoadPage((FPDF_DOCUMENT)d.Handle(), index);
}
PageHandle::~PageHandle() { if (page_) { PdfLock l; FPDF_ClosePage((FPDF_PAGE)page_); } }

bool PdfDocument::PageSize(int index, float* w, float* h) const {
  PdfLock l;
  if (!doc_ || index < 0 || index >= PageCount()) return false;
  FS_SIZEF s{};
  if (!FPDF_GetPageSizeByIndexF((FPDF_DOCUMENT)doc_, index, &s)) return false;
  if (w) *w = s.width;
  if (h) *h = s.height;
  return true;
}

int PdfDocument::PageRotation(int index) const {
  PdfLock l;
  PageHandle p(*this, index);
  if (!p) return -1;
  int r = FPDFPage_GetRotation((FPDF_PAGE)p.get());
  return r < 0 ? -1 : (r & 3) * 90;
}

std::wstring PdfDocument::PageLabel(int index) const {
  PdfLock l;
  if (!doc_ || index < 0 || index >= PageCount()) return {};
  unsigned long n = FPDF_GetPageLabel((FPDF_DOCUMENT)doc_, index, nullptr, 0);
  if (n <= 2) return {};
  std::vector<uint8_t> b(n);
  FPDF_GetPageLabel((FPDF_DOCUMENT)doc_, index, b.data(), n);
  return FromUtf16Bytes(b);
}

DocMeta PdfDocument::Meta() const {
  PdfLock l;
  DocMeta m;
  auto get = [&](const char* tag) {
    unsigned long n = FPDF_GetMetaText((FPDF_DOCUMENT)doc_, tag, nullptr, 0);
    if (n <= 2) return std::wstring();
    std::vector<uint8_t> b(n);
    FPDF_GetMetaText((FPDF_DOCUMENT)doc_, tag, b.data(), n);
    return FromUtf16Bytes(b);
  };
  if (!doc_) return m;
  m.title = get("Title"); m.author = get("Author"); m.subject = get("Subject"); m.keywords = get("Keywords");
  m.creator = get("Creator"); m.producer = get("Producer"); m.creationDate = get("CreationDate"); m.modDate = get("ModDate");
  return m;
}

int PdfDocument::FileVersion() const {
  PdfLock l;
  int v = 0;
  if (doc_ && FPDF_GetFileVersion((FPDF_DOCUMENT)doc_, &v)) return v;
  return 0;
}

// ---- forms --------------------------------------------------------------------------------------------------------

void* PdfDocument::EnsureForm() const {
  if (formTried_) return impl_ ? impl_->form : nullptr;
  formTried_ = true;
  if (!doc_ || !impl_) return nullptr;
  if (FPDF_GetFormType((FPDF_DOCUMENT)doc_) == FORMTYPE_NONE) return nullptr;
  std::memset(&impl_->ffi, 0, sizeof(impl_->ffi));
  impl_->ffi.version = 1;
  impl_->form = FPDFDOC_InitFormFillEnvironment((FPDF_DOCUMENT)doc_, &impl_->ffi);
  if (impl_->form) FPDF_SetFormFieldHighlightAlpha(impl_->form, 0);
  return impl_->form;
}

// ---- rendering ----------------------------------------------------------------------------------------------------

bool PdfDocument::RenderPage(int index, const RenderOptions& opt, Bitmap& out, std::wstring* err, bool* clamped) const {
  if (clamped) *clamped = false;
  auto fail = [&](const wchar_t* m) { if (err) *err = m; return false; };
  PdfLock lock;
  if (!doc_) return fail(L"No document is open.");
  PageHandle page(*this, index);
  if (!page) return fail(L"The page could not be loaded.");
  FPDF_PAGE pg = (FPDF_PAGE)page.get();
  double wpt = FPDF_GetPageWidthF(pg), hpt = FPDF_GetPageHeightF(pg);
  if (!(wpt > 0.01) || !(hpt > 0.01) || wpt > 1e6 || hpt > 1e6) return fail(L"The page has an invalid size.");
  double scale = opt.maxSidePx > 0 ? opt.maxSidePx / std::max(wpt, hpt) : opt.dpi / 72.0;
  if (!(scale > 0)) return fail(L"Invalid resolution.");
  double pw = wpt * scale, ph = hpt * scale;
  double limit = std::min(opt.maxPixels, 32000.0 * 32000.0);
  if (pw * ph > limit || pw > 32000 || ph > 32000) {
    double f = std::min({std::sqrt(limit / (pw * ph)), 32000.0 / pw, 32000.0 / ph});
    if (f < 1) { pw *= f; ph *= f; if (clamped) *clamped = true; }
  }
  int W = std::max(1, (int)std::lround(pw)), H = std::max(1, (int)std::lround(ph));
  try { out.bgra.assign((size_t)W * H * 4, 0); } catch (...) { return fail(L"Not enough memory to render this page."); }
  out.width = W; out.height = H;

  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(W, H, opt.transparent ? FPDFBitmap_BGRA : FPDFBitmap_BGRx, out.bgra.data(), W * 4);
  if (!bmp) return fail(L"Not enough memory to render this page.");
  FPDFBitmap_FillRect(bmp, 0, 0, W, H, opt.transparent ? 0x00000000u : 0xFFFFFFFFu);
  int flags = 0;
  if (opt.annotations) flags |= FPDF_ANNOT;
  if (opt.lcdText) flags |= FPDF_LCD_TEXT;
  if (opt.grayscale) flags |= FPDF_GRAYSCALE;
  FPDF_RenderPageBitmap(bmp, pg, 0, 0, W, H, 0, flags);
  if (opt.forms && opt.annotations) {
    if (auto* form = (FPDF_FORMHANDLE)EnsureForm()) {
      FORM_OnAfterLoadPage(pg, form);
      FPDF_FFLDraw(form, bmp, pg, 0, 0, W, H, 0, flags);
      FORM_OnBeforeClosePage(pg, form);
    }
  }
  FPDFBitmap_Destroy(bmp);
  if (!opt.transparent) {
    uint8_t* p = out.bgra.data();
    for (size_t i = 3, n = out.bgra.size(); i < n; i += 4) p[i] = 255;
  }
  return true;
}

bool PdfDocument::RenderThumbnail(int index, int maxSide, Bitmap& out, std::wstring* err) const {
  RenderOptions o; o.maxSidePx = std::max(1, maxSide); o.annotations = true; o.forms = false;
  return RenderPage(index, o, out, err);
}

std::wstring PdfDocument::PageText(int index) const {
  PdfLock lock;
  PageHandle page(*this, index);
  if (!page) return {};
  FPDF_TEXTPAGE tp = FPDFText_LoadPage((FPDF_PAGE)page.get());
  if (!tp) return {};
  int n = FPDFText_CountChars(tp);
  std::wstring s;
  if (n > 0) {
    std::vector<unsigned short> buf((size_t)n + 1);
    int got = FPDFText_GetText(tp, 0, n, buf.data());
    if (got > 0) s.assign((const wchar_t*)buf.data(), (size_t)got - 1);
  }
  FPDFText_ClosePage(tp);
  return s;
}

// ---- editing ------------------------------------------------------------------------------------------------------

bool PdfDocument::ImportPages(const PdfDocument& src, const std::vector<int>& srcPages, int insertAt, std::wstring* err) {
  PdfLock lock;
  auto fail = [&](const wchar_t* m) { if (err) *err = m; return false; };
  if (!doc_ || !src.doc_) return fail(L"No document is open.");
  int sc = src.PageCount();
  for (int p : srcPages) if (p < 0 || p >= sc) return fail(L"A page number is outside the document.");
  if (srcPages.empty()) return true;
  int at = insertAt < 0 ? PageCount() : std::min(insertAt, PageCount());
  if (!FPDF_ImportPagesByIndex((FPDF_DOCUMENT)doc_, (FPDF_DOCUMENT)src.doc_, srcPages.data(), (unsigned long)srcPages.size(), at))
    return fail(L"Some pages could not be copied (the source may be damaged).");
  return true;
}

bool PdfDocument::SetPageRotation(int index, int degrees) {
  PdfLock lock;
  PageHandle p(*this, index);
  if (!p) return false;
  int q = (((degrees / 90) % 4) + 4) % 4;
  FPDFPage_SetRotation((FPDF_PAGE)p.get(), q);
  return true;
}
bool PdfDocument::RotatePage(int index, int delta) {
  if (delta % 90 != 0) return false;
  PdfLock lock;
  int cur = PageRotation(index);
  if (cur < 0) return false;
  return SetPageRotation(index, cur + delta);
}
bool PdfDocument::DeletePage(int index) {
  PdfLock lock;
  if (!doc_ || index < 0 || index >= PageCount() || PageCount() <= 1) return false;
  FPDFPage_Delete((FPDF_DOCUMENT)doc_, index);
  return true;
}
bool PdfDocument::DuplicatePage(int index) {
  PdfLock lock;
  if (!doc_ || index < 0 || index >= PageCount()) return false;
  int idx = index;
  return FPDF_ImportPagesByIndex((FPDF_DOCUMENT)doc_, (FPDF_DOCUMENT)doc_, &idx, 1, index + 1) != 0;
}
bool PdfDocument::MovePage(int from, int to) {
  PdfLock lock;
  int n = PageCount();
  if (!doc_ || from < 0 || from >= n || to < 0 || to >= n) return false;
  if (from == to) return true;
  return FPDF_MovePages((FPDF_DOCUMENT)doc_, &from, 1, to) != 0;
}

namespace {
struct Writer : FPDF_FILEWRITE {
  HANDLE h = INVALID_HANDLE_VALUE;
  const std::atomic<bool>* cancel = nullptr;
  std::vector<uint8_t> buf;
  bool failed = false;
  DWORD lastError = 0;
  bool Flush() {
    size_t off = 0;
    while (off < buf.size()) {
      DWORD wrote = 0;
      DWORD chunk = (DWORD)std::min<size_t>(buf.size() - off, 1u << 24);
      if (!WriteFile(h, buf.data() + off, chunk, &wrote, nullptr) || wrote == 0) { lastError = GetLastError(); failed = true; return false; }
      off += wrote;
    }
    buf.clear();
    return true;
  }
};
int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  Writer* w = static_cast<Writer*>(self);
  if (w->failed) return 0;
  if (w->cancel && w->cancel->load(std::memory_order_relaxed)) { w->failed = true; return 0; }
  const uint8_t* p = (const uint8_t*)data;
  w->buf.insert(w->buf.end(), p, p + size);
  if (w->buf.size() >= (1u << 20)) return w->Flush() ? 1 : 0;
  return 1;
}
}

bool PdfDocument::Save(const fs::path& file, std::wstring* err, const std::atomic<bool>* cancel, bool removeSecurity) const {
  PdfLock lock;
  auto fail = [&](std::wstring m) { if (err) *err = std::move(m); return false; };
  if (!doc_) return fail(L"No document is open.");
  Writer w;
  w.version = 1;
  w.WriteBlock = &WriteBlock;
  w.cancel = cancel;
  w.h = CreateFileW(LongPath(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (w.h == INVALID_HANDLE_VALUE) return fail(Sentence(L"The output file could not be created: " + LastErrorMessage()));
  DWORD flags = FPDF_NO_INCREMENTAL | (removeSecurity ? FPDF_REMOVE_SECURITY : 0);
  bool ok = FPDF_SaveAsCopy((FPDF_DOCUMENT)doc_, &w, flags) != 0 && w.Flush();
  CloseHandle(w.h);
  if (!ok) {
    std::error_code ec; fs::remove(file, ec);
    if (cancel && cancel->load()) return fail(L"Cancelled");
    if (w.failed && w.lastError) return fail(Sentence(L"The PDF could not be written: " + LastErrorMessage(w.lastError)));
    return fail(L"The PDF could not be written.");
  }
  return true;
}

// ---- helpers for UI lists -----------------------------------------------------------------------------------------

PdfFileInfo GetPdfFileInfo(const fs::path& file, const std::wstring& password) {
  PdfFileInfo i;
  std::error_code ec;
  i.sizeBytes = fs::file_size(file, ec);
  auto r = PdfDocument::Open(file, password);
  i.status = r.status; i.message = r.message;
  if (!r.ok()) return i;
  i.pageCount = r.doc->PageCount();
  r.doc->PageSize(0, &i.firstPageWidthPt, &i.firstPageHeightPt);
  i.encrypted = r.doc->IsEncrypted();
  i.version = r.doc->FileVersion();
  i.meta = r.doc->Meta();
  return i;
}

std::unique_ptr<PageThumbnailer> PageThumbnailer::Open(const fs::path& file, const std::wstring& password, OpenStatus* status,
                                                       std::wstring* message) {
  auto r = PdfDocument::Open(file, password);
  if (status) *status = r.status;
  if (message) *message = r.message;
  if (!r.ok()) return nullptr;
  auto t = std::unique_ptr<PageThumbnailer>(new PageThumbnailer());
  t->doc_ = std::move(r.doc);
  return t;
}
bool PageThumbnailer::Render(int index, int maxSide, Bitmap& out, std::wstring* err) const {
  return doc_ && doc_->RenderThumbnail(index, maxSide, out, err);
}

} // namespace filo::pdf
