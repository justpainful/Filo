# Tools

26 tools, each a real dialog backed by a headless operation. Everything writes a **new file** (the original is never modified) through
`OutputTransaction` (temp -> verify -> atomic move) and appears only for selections it accepts. Wheel order = registration order.
"Run" = executed headless with `Filo.exe --tool-run <id> <preset-json|-> <files>` on real files (results in `out/` during development).
"Shot" = the dialog was rendered with `--tool-shot` and inspected. Mouse interaction inside the dialogs (drawing, dragging handles) was **not**
exercised by an automated test; it was only checked by reading the code paths and rendering states with preset data.

## Image / general

| Id | Wheel | Accepts | Output | Verified |
|---|---|---|---|---|
| `compress` | Compress | images, videos, audio, PDFs (batch) | `<name> (compressed).<ext>`; modes: preset, percent, target size; refuses to write a file that is not smaller | Run (JPEG 3 modes, MP4); engine tests for image/pdf/media compress |
| `edit-metadata` | Metadata | one file | rewrites chosen EXIF/XMP/ID3/Office/PDF fields, can strip GPS | Run + Shot; meta engine tests (27) |
| `edit-photo` | Edit | one image | `(edited)`; adjustment sliders in four sections (light, colour, detail...), live preview rendered off the UI thread | Run + Shot |
| `annotate` | Annotate | one image | `(annotated)`; pen, marker, arrow, box, oval, text; burned in at full resolution | Run + Shot (drawing by mouse not automated) |
| `add-background` | Add BG | one image | `(background)`; 20 presets or own picture, padding, corners, shadow, ratio | Run + Shot |
| `crop-image` | Crop | images | `(cropped)`; free/preset/custom ratio, exact size, same relative crop for a batch | Run + Shot |
| `redact-photo` | Redact | one image | `(redacted)`; solid or mosaic+blur regions, face detection and text detection (Windows APIs), metadata is dropped | Run + Shot; text detection verified on a generated text image (both lines covered, pixels burned in); face detection not run on a real photo of a face |
| `images-to-pdf` | PDF | images | `(PDF)`; A4/A3/A5/Letter/Legal/same-as-image, orientation, fit/fill/stretch, margin; JPEGs embedded untouched | Run (2 images) + pdf engine tests |
| `read-qr` | QR | one image | none (shows results, copy, optional "Open Link" on request) | Shot with a two-code image + 9 engine tests |
| `collage` | Collage | 2+ images | `(collage).jpg`; grid/rows/columns/feature layouts, spacing, corners, background | Run + Shot |

## Video

| Id | Wheel | Output |
|---|---|---|
| `join-videos` | Join | one MP4; reorder, letterboxing of mixed sizes, silence added for clips without audio |
| `trim-video` | Trim | `(trimmed)`; fast (keyframe copy, reports the snapped start) or exact re-encode |
| `crop-video` | Crop | `(cropped)`; aspect presets / free rectangle |
| `change-speed` | Speed | `(2x)` etc.; pitch preserved or changed, optional smoothing |
| `split-video` | Split | folder `(split)` with numbered parts: N parts, markers, or segments |
| `save-frames` | Frames | folder `(frames)` of PNGs at chosen times |
| `redact-video` | Redact | `(redacted)`; solid or blur regions with start/end times |
| `remove-audio` | Mute | `(no audio)`; video stream copied bit-exact; offered only when the video has an audio track |

Verified: Run on a generated 4 s clip for all eight (headless), plus the media engine's own tests (37, last run by the engine author).
The preview player (ffmpeg pipes + waveOut clock) was not played during testing on this machine: audio output was deliberately not exercised.

## Audio

| Id | Wheel | Output |
|---|---|---|
| `normalize-volume` | Normalize | `(normalized)`; loudness target in LUFS, true peak, LRA; re-measures the result |
| `audio-visualizer` | Visualizer | MP4 with an animated waveform, 3 layouts x 3 styles, colours/background picture |
| `trim-audio` | Trim | `(trimmed)`; drag handles, optional automatic silence trimming |
| `convert-channels` | Channels | `(mono)` / stereo with per-channel gain |
| `bleep-audio` | Bleep | `(bleeped)`; tone or silence over chosen ranges, everything else bit-identical |

Verified: Run on a generated 5 s WAV for all five; Shot for Trim/Bleep (compared with `audio-tools.png`).

## PDF

| Id | Wheel | Accepts | Output |
|---|---|---|---|
| `merge-pdf` | Merge | 2+ PDFs | `(merged)`; reorder, per-file password, bookmarks kept grouped per source |
| `organize-pdf` | Organize | one PDF | `(organized)`; page grid: rotate, delete, reorder |
| `split-pdf` | Split | one PDF | folder `(split)`; every page / ranges / every N pages |

Verified: Run on generated PDFs, page counts and rotation checked with PyMuPDF (merge 4+3=7, organize rotate 90, split 4 files).

## Actions (Actions wheel)

Share (Windows share UI), **Float** (always-on-top shelf, see SHELL-INTEGRATION.md), Copy (files on the clipboard as a file copy), Move (folder
picker then `IFileOperation`), Copy Path (Shift = quoted), Terminal (Windows Terminal / PowerShell / cmd at the file's folder).
Copy Path and the clipboard formats were implemented from the documented APIs; they were not exercised in an Explorer session.

## Presets

Each dialog saves its last settings to `Settings.toolPresets` (`applyLastUsed`) so the next run starts where the last one ended; Settings > Files > Forget clears them.
