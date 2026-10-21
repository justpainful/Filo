# PDF engine (`src/engines/pdf`)

Engine id `pdfium`. Registered by `filo::RegisterPdfEngine()` (`pdf_engine.cpp`). Tests: `tests/pdf` -> `filo_tests_pdf`
(41 tests, all pass in Debug and Release). Machine-readable results: `testdata/pdf/matrix-results.json`.

## Capabilities

| id | in > out | output | options | lossy | status |
|----|----------|--------|---------|-------|--------|
| `pdf>png` | pdf > png | folder `name/page-001.png ...` | `dpi` (72-600, def 300), `transparent`, `annotations`, `grayscale`, `pages`, `password` | rasterises | pass |
| `pdf>jpg` | pdf > jpg | folder `name/page-001.jpg ...` (white background) | `dpi`, `quality`, `annotations`, `grayscale`, `pages`, `password` | yes | pass |
| `pdf>txt` | pdf > txt | one file, UTF-8 **with BOM**, CRLF | `ocr` auto/off, `ocrLanguage`, `pageBreak` formfeed/blank, `pages`, `password` | text only | pass |
| `pdf>md` | pdf > md | folder `name/<input stem>.md` + `name/images/*` | `ocr`, `ocrLanguage`, `images`, `pageBreaks`, `pages`, `password` | structure inferred | pass |

Not registered on purpose: `pdf>docx/pptx/xlsx/epub` (document engine, built on `pdf_text.h`), `pdf>pages`.

Page separator in `.txt` (default): a line holding a single form feed (U+000C), like `pdftotext`; `pageBreak=blank` uses two blank lines.
Markdown: pages are separated by `---`; images keep their JPEG bytes when the PDF stored them as DCT, otherwise PNG.

## Headless APIs (headers in `src/engines/pdf`)

* `pdf_doc.h` - `PdfLibrary`/`PdfLock` (process wide PDFium mutex), `PdfDocument` (open read-only, render, thumbnails, text, page ops, save),
  `PageThumbnailer`, `GetPdfFileInfo`.
* `pdf_text.h` - **stable contract with the document engine**: `ExtractPages`, `ExtractPagesStreaming`, `ExtractPage`, `DetectTables`, `PageToPlainText`.
* `pdf_ops.h` - `MergePdfs`, `SplitPdf`, `OrganizePdf`, `CompressPdf`, `CreatePdfFromImages`, `ParsePageRanges`.
  All write through `OutputTransaction` / `OutputDirTransaction`, report progress and honour cancellation.
* Private: `pdf_wic.*` (WIC encode/decode), `pdf_ocr.*` (Windows.Media.Ocr), `pdf_outline.*`/`pdf_rewrite.h` (bookmarks + byte level rewrite),
  `pdf_markdown.*`, `pdf_tables.cpp`, `pdf_internal.h`.

## How things work (and where they are weak)

* **Text extraction.** PDFium characters -> runs (same style/baseline, contiguous) -> visual lines -> fragments -> recursive XY-cut with gutter
  detection (full-width spanners split a page into bands) -> reading order, `line`/`block`/`column` numbers. RTL text keeps PDFium's logical
  order; right-to-left ligature glyphs (lam-alef) are restored (PDFium reverses their characters). Weak spots: text drawn as vector outlines
  or without a ToUnicode map (counted in `unmappedGlyphs`, page flagged `textUnreliable`); footnotes/sidebars are ordered by geometry only;
  paragraph breaks are inferred from vertical gaps and style changes (no indentation heuristics).
* **Tables.** Ruled tables from path objects (thin stroked/filled lines and rectangle edges, merged cells detected from missing separators);
  borderless tables from aligned cells (>= 3 rows, >= 2 columns, short cells, consistent alignment; list-marker columns and prose are rejected).
  Tables with only horizontal rules, nested tables and tables spanning pages are not detected.
* **Markdown.** Headings from font size (levels from the distinct sizes above 1.15 x body size) and stand-alone bold lines; `-`/`1.` lists with
  nesting by indent; `**bold**`/`*italic*`; GFM tables (first row is the header, merged cells become empty cells); monospace blocks become fences.
  Headers/footers and page numbers are not removed. Hyphenated line breaks are not repaired.
* **OCR.** `Windows.Media.Ocr` through C++/WinRT on a private MTA thread; page rendered at 300 dpi. `ocrLanguage=auto` tries the user-profile
  engine and every installed recognizer and keeps the best result for the first scanned page, then reuses that language. Right-to-left
  lines are re-ordered from word rectangles (Windows returns visual order). With OCR off/unavailable the conversion fails with a sentence
  (never an empty file); partly scanned documents get a warning listing the pages.
* **Compress.** PDFium re-saves the file (dropping superseded revisions), then a byte level pass rewrites it: every unreachable object is
  dropped and 8-bit Gray/RGB/ICC/Cal images (raw or Flate with PNG/TIFF predictors, or DCT) are downscaled to the preset dpi and stored as
  JPEG in place (same object numbers; effective dpi comes from PDFium's page objects). Images with masks/SMask, indexed/CMYK/Lab colour,
  1-bit, JBIG2/CCITT/JPX are left alone. Presets: low = 300 dpi q88, medium = 150 dpi q75, high = 96 dpi q55; target size/percent steps through
  the ladder until reached (else best effort + warning). If the result is not at least 1 % smaller nothing is written and
  `OpResult::notSmaller` is set. Bookmarks, forms, attachments and properties survive (they are never touched). Signed PDFs lose their
  signature (any re-save does).
* **Merge/Split/Organize** build new documents with `FPDF_ImportPagesByIndex` (one call per consecutive run from one source, so shared fonts/images
  are copied once). Merge keeps every source's bookmarks, grouped under one entry per file, by appending a standards conforming incremental
  update (`pdf_outline.cpp`); split/organize do **not** carry bookmarks, forms or attachments over. Password-protected sources are opened with
  the supplied password and the result is written **unencrypted** (with a warning).
* **Images -> PDF.** JPEG inputs are embedded byte-for-byte (`FPDFImageObj_LoadJpegFileInline`), EXIF orientation 1-8 is applied through the
  placement matrix; other formats are decoded with WIC and stored losslessly (Flate, alpha as SMask). CMYK JPEGs are re-encoded through WIC.
  Only the first frame of multi-frame images is used (warning).
* **Safety.** Source files are opened read-only with share-all. Truncated/mutated inputs: 190 fuzz-style files in the test suite, no crash
  (PDFium's repair path recovers ~80 % of them). Files > 4 GB are rejected (PDFium's `FPDF_FILEACCESS` limit). Rendering is capped at 150 MP
  per page (reported through a warning), extraction at 120 MP per image.

## Dependencies and licences

| component | use | licence |
|-----------|-----|---------|
| PDFium (prebuilt `pdfium.dll`, `third_party/pdfium`) | parse, render, text, page import/save | BSD-3-Clause / Apache-2.0 (+ bundled FreeType, lcms, libjpeg-turbo, zlib, ICU... see `third_party/pdfium/licenses`) |
| miniz (`third_party/miniz`) | Flate decode of image streams in `CompressPdf` | MIT |
| WIC, Windows.Media.Ocr (OS components, C++/WinRT headers from the Windows SDK) | image codecs, OCR | part of Windows; no redistribution |
| `normaliz.lib` (`NormalizeString`) | Arabic presentation forms / Latin ligatures -> base letters | part of Windows |

Test-only tools (never linked or shipped): LibreOffice (authors the Arabic/2-column/table/rich fixtures), Pillow (HPND) and PyMuPDF (AGPL-3.0 -
used **only** as an independent reader/author of fixtures when installed; the tests skip those checks if it is missing). Nothing from them is
part of the product.

## Verified (all run; see `tests/pdf`)

Open statuses (ok / needs password / wrong password / corrupt / not a PDF / empty / missing, AES-256 and RC4 files), page sizes/rotation/labels,
render sizes at 72-300 dpi (= pt x dpi / 72 +-1), straight-alpha transparent rendering, grayscale, pixel cap, form fields, thumbnails, page ops +
fresh reload, 190 fuzz inputs, Unicode and > 260 character paths, extraction geometry/style/colour, rotated pages and rotated text, 2-column
reading order (S001..S040 + full-width footer), ruled + borderless tables with merged cells and no false positives on prose/lists/columns,
Arabic logical order incl. lam-alef and mixed digits/Latin, images in all 8 orientations vs rendering, scanned detection, JPEG pass-through,
300-page extraction (Release 0.75 s / +5 MB; Debug ~2.5 s), cancellation of extraction/merge/image->PDF/PDF->PNG (no output, no temp files),
merge order + bookmarks (checked with PDFium and PyMuPDF), split in 3 modes, organize (reorder/rotate/duplicate/delete, pixel-compared),
compress (image-heavy 17.5 MB -> 0.4 MB with mean pixel difference 1.3; JPEG photo PDF; bookmarks/properties kept; `notSmaller`),
images -> PDF (sizes, fit/fill/stretch/margins, JPEG bytes identical, EXIF 1-8, TIFF, alpha PNG), OCR English and Arabic scans with Windows OCR
(en-US and ar-SA installed on the dev machine).

## Not verified / known gaps

* The "no OCR language installed" path: this machine has languages, so only the message text and the bad-language error path are tested.
* Release/Debug x64 only; ARM64 and the installer layout (pdfium.dll next to the exe) not exercised.
* PDFs using xref streams / object streams are fine for reading and for everything that goes through PDFium, but the byte level steps
  (bookmark append in merge, rewrite in compress) only run on files PDFium wrote itself (classic xref table); if their layout is not
  understood the merge keeps its pages and warns that bookmarks were dropped, compress keeps PDFium's plain re-save.
* `pdf>md` quality on real-world documents (magazines, forms, footnotes) has only been checked on the generated fixtures.
* Windows OCR accuracy depends on the installed language pack; mixed-language scans use one language per document.
