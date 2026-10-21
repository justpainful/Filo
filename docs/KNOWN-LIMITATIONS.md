# Known limitations

Written to be read before trusting a claim anywhere else in the docs. Engine-level details are in `docs/fragments/{image,media,pdf}.md`
(written by the engine authors; the document, archive and metadata engines have no fragment because their work was stopped before they were written - their behaviour is described only by their tests and `FORMAT-MATRIX.md`).

## Not verified

- **A drag that starts in the real File Explorer** (or the Desktop) has not been verified by a human or by automation; only the harness drag (`filo_e2e_drag`) and the state-machine unit tests were run. See the checklist in SHELL-INTEGRATION.md.
- **Mouse interaction inside the tool dialogs** (drawing in Annotate/Redact, dragging crop handles, trim handles, dropping files into lists) is not covered by automated tests. Dialogs were rendered offscreen with preset data and their headless operations were run.
- **Multi-monitor and mixed DPI**: layouts were rendered at 96 and 192 dpi (`FILO_TEST_DPI`) and windows clamp to the monitor's work area, but moving a window between monitors of different scale, and the wheel on a second monitor, have not been tried on real hardware.
- **Audio/video playback** inside the Trim/Bleep/Normalize/Join previews was not played during testing (no sound was to be produced on the development PC). The ffmpeg-pipe + waveOut player code is exercised only through its decode path.
- **Installer**: built (`dist\Filo-Setup-0.1.3.exe`) but not installed/uninstalled on a machine; the context-menu registry entries are therefore untested in Explorer.
- Language packs: face detection and OCR use Windows components; the "no OCR language installed" path is only tested for its message.

## Product gaps

- **UI text is English only.** `language = ar` mirrors layouts (RTL) but no Arabic strings are shipped.
- **Progress HUD** (wheel conversions): a glass card at the top of the screen with a cancel button, shown for running conversions, "Done" (click to show the file) or the error. Only rendered offscreen and wired into the runtime; it has not been watched during a real drag-and-drop conversion.
- **Float** has no edge-docked "N items" pill (the reference video shows one), and no multi-select drag-reorder.
- **Windows 11 context menu**: the Filo cascade is under *Show more options*; a modern (package-registered) menu entry is not implemented.
- **Elevated windows**: a non-elevated Filo cannot see drags that start in an administrator-level Explorer window (Windows UI privilege isolation).
- **Annotate** cannot select, move or edit a mark after drawing it (Undo removes the last one); **Collage** uses at most 9 pictures and saves JPEG; **Redact** detection finds frontal faces and OCR-readable text only and never claims completeness - always review the regions.
- **Visual fidelity** is approximate. The October 21 shared material correction fixes the dark-background contrast failure, but does not establish pixel equivalence for all windows. Official Join Videos and metadata screenshots are unavailable.
- **Live glass requires Windows 11 build 22000+** and an available host backdrop compositor. Windows 10 and compositor creation failures use opaque surfaces. The native live path was tested on this PC (Windows build 26200), including changing backgrounds, resizing, opacity, simulated graphics-device reset, and the 0.1.3 live LayerVisual refraction path. Optical checks force native 96/120/144 DPI geometry; physical mixed-monitor transitions remain unqualified. Full Windows high-contrast system colours and Arabic translations remain incomplete.
- The wheel's tool sectors include Windows-specific additions (Create PDF, Read QR, Collage), so an image shows more than the reference's seven.
- Sector availability for videos asks `ffprobe` whether a clip has audio (cached); the first wheel opening for a new video can take a moment.

## Formats

- **Pages (`.pages`) output is not supported** (undocumented format). Reading a `.pages` file extracts the preview PDF stored inside it.
- Not offered because no path exists: PDF/Word/etc. -> **Pages**, video -> FLAC/OGG/OPUS/AIFF/WMA, same-format "conversions", m4v/mpg/flv/3gp/ts/aac inputs.
- **PDF -> DOCX / PPTX / XLSX / EPUB are offered but best-effort**: a PDF stores glyphs at coordinates, so paragraphs, tables and columns are reconstructed heuristically and will differ on complex pages
  (the dialog's limitation text says so). They were run on real fixtures and checked structurally only (see the manual checks in FORMAT-MATRIX.md); the files were not opened in Word, PowerPoint or Excel.
- **Image -> DOCX** (one page with the picture) was added to match the reference wheel; it was checked structurally with filo-cli (embedded image size and orientation) (see the manual checks in FORMAT-MATRIX.md).
- **Images**: large (about 2000 px and wider) lossy HEIC written by the Windows HEIF encoder decodes with a colour cast in WIC and FFmpeg (a Windows encoder/decoder issue; lossless is exact). HEIC output has no alpha and needs the HEVC extension; AVIF input/output need the AV1 extension or FFmpeg codecs.
  SVG `<text>` is missing from rasterised SVGs (Direct2D SVG limitation; a warning is shown). AVIF output cannot carry EXIF/XMP. GIF is 256 colours; animation is preserved only for GIF <-> WebP (other formats keep the first frame and warn).
- **Video/audio**: hardware encoding uses NVENC only when a 1-second trial encode succeeds (setting "GPU encoding"); AMF/QSV are not used. Compress to a target size is 2-pass ABR (H.264, VP9, MPEG-4, WMV2). Normalising a video processes the first audio track only and warns.
  WMA/ASF copy-trim reports slightly longer durations (preroll). Frame scrubbing starts a short ffmpeg process per frame (about 100 ms).
- **PDF**: Split and Organize drop bookmarks, forms and attachments; outputs of password-protected sources are written unencrypted (with a warning); Compress skips images with masks/SMask, indexed/CMYK/Lab colour, 1-bit, JBIG2, CCITT and JPX;
  tables with only horizontal rules are not detected in PDF -> Markdown; headers/footers are not stripped; files over 4 GB are rejected.
- **Documents**: conversions that need LibreOffice (for example legacy DOC/PPT inputs) are offered only when LibreOffice is installed. LibreOffice contacts the default printer when it starts; an offline printer makes Windows show "Waiting for printer connection" pop-ups during those conversions.
  `FORMAT-MATRIX.md` lists exactly which document paths were tested.

## Licensing consequences

The bundled FFmpeg is a GPL v3 build run as a separate process; anyone redistributing the installer must follow LICENSES.md. The executables are not code-signed, so SmartScreen warns on first run.

## Developer test harness

`--tool-run edit-metadata` can attempt its save before the asynchronous metadata read is applied. It returned failure for the personal-cleanup preset during UI review. The visible metadata window is tested separately after loading; the engine's 27 tests pass. This headless harness race is not counted as a passed metadata UI save.
