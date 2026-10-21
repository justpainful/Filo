# Third-party software and licences

Filo is built from its own code plus the components below. Nothing is downloaded at run time, and no component sends data anywhere.
Texts of the licences ship in the installer under `licenses\`. This file is the summary; the text files are authoritative.

## Bundled with the installer

| Component | Used for | How it is used | Licence |
|---|---|---|---|
| **FFmpeg** (Gyan "full" build 9.0) incl. x264, x265, libvpx, SVT-AV1, libaom, opus, vorbis, lame, xvid and others | video and audio conversion and editing, WebP/AVIF encoding, thumbnails | separate process (`tools\ffmpeg.exe`, `tools\ffprobe.exe`), **never linked** into Filo | **GPL v3** (the build is configured `--enable-gpl --enable-version3`); x264/x265/xvid are GPL, libvpx/libaom/SVT-AV1/opus/vorbis BSD-style, lame LGPL |
| **PDFium** (build 158.0.8111) | PDF rendering, page operations, text extraction | `pdfium.dll` loaded by Filo | BSD-3-Clause / Apache-2.0 (bundled third-party licences in `licenses\pdfium`) |
| **ExifTool** 13.59 | writing and reading metadata in images, media and documents | separate process (`tools\exiftool\exiftool.exe`) | Perl "Artistic or GPL"; the Windows executable embeds Strawberry Perl (its licence list is in `licenses\exiftool`) |

### FFmpeg GPL obligations (read this before redistributing)

The FFmpeg binaries are GPL v3. Filo runs them as separate programs and does not link to them, so Filo's own source is not
a derivative work, but **anyone who redistributes the installer must also provide the FFmpeg licence text and the corresponding
source offer**. The licence text is shipped in `licenses\ffmpeg\LICENSE`. The corresponding source for the exact build is available from
the build's publisher (https://www.gyan.dev/ffmpeg/builds/ - source links on that page) and from https://ffmpeg.org/download.html.
If you ship the installer yourself, keep a copy of that source (or a written offer valid for three years) with it.

## Linked into Filo (static)

| Component | Used for | Licence |
|---|---|---|
| **miniz** | ZIP / DEFLATE (archives, DOCX/EPUB/PPTX packages, PDF stream recompression) | MIT |
| **ZXing-cpp** 3.1.1 (readers only, QR family) | Read QR Code | Apache-2.0 |

## Operating-system components (not redistributed)

Direct3D 11, Direct2D, DirectWrite, DirectComposition, Windows Imaging Component (JPEG/PNG/TIFF/BMP/GIF/HEIF), Windows.Media.Ocr and
Windows.Media.FaceAnalysis (Redact detection), Shell APIs (thumbnails, share, drag and drop). HEIC/HEIF and AV1 handling depends on the
Microsoft HEVC / AV1 extensions being installed on the PC; Filo reports the capability as unavailable when they are not.

## Optional, not bundled

- **LibreOffice** (MPL-2.0): used as a separate headless process only for conversions Filo cannot write itself (for example DOC/PPT to
  other formats). It is detected on the PC; if it is not installed those conversions are not offered. Filo does not install it.

## Used only for development and tests (never shipped)

Python 3 with Pillow (HPND), NumPy (BSD), OpenCV (Apache-2.0; QR fixtures), PyMuPDF (AGPL - used only on the developer machine to author
fixtures and cross-check results; nothing from it is distributed), 7-Zip and bsdtar (to author archive fixtures).

## Fonts and artwork

The interface uses the system fonts (Segoe UI Variable Text/Display, falling back to Segoe UI; Cascadia Mono for timecodes).
No fonts are bundled. All icons are original vector paths drawn for Filo; the reference screenshots in `reference\` are used only
to measure layout and colour and are not shipped.
