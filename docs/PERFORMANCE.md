# Performance

Only numbers that were measured are listed; "reported" marks numbers taken from an engine author's test log that I did not re-measure.
Machine: Windows 11 Pro, NVIDIA RTX 4070 SUPER (NVENC available), Release build (`/O2`, static CRT), 2026-10-20.

## Background footprint (the part that matters because Filo runs all day)

Measured with `Filo.exe --background` left idle for 36 s (tray icon + global hooks, no wheel open):

| Metric | Value |
|---|---|
| Working set | 46.9 MB (unchanged between 6 s and 36 s) |
| CPU time over 30 idle seconds | 109 ms (about 0.4 % of one core) |
| Threads / handles | 34 / 451 |
| Quit | `Filo.exe --quit` ended the process cleanly |

The hook procedures only post a message and return; no work happens on the hook thread.

## Tool runs (headless `--tool-run`, includes process start and engine registration, so real dialog use is a little faster)

| Run | Time |
|---|---|
| Crop a 494x741 PNG | 523 ms |
| Redact (mosaic + blur) one region | 599 ms |
| Annotate, 2 marks burned in | 565 ms |
| Collage of 3 images, 3072 px long side | 803 ms |
| Merge two small PDFs | 440 ms |
| Trim a 4 s H.264 clip (re-encode, NVENC) | 1.43 s |
| Normalize volume of a 5 s WAV (measure + 2 passes) | 1.17 s |

## Engines (reported by the engine authors, Release build, same machine)

- Image: 12 MP JPEG -> PNG 1.0 s, -> WebP 2.2 s, -> TIFF 0.8 s.
- PDF: 300-page text extraction 0.75 s (+5 MB); 300 pages to PNG at 60 dpi 1.4 s; cancellation reacts in under 10 ms.
- Media: cancel stops a conversion in under 0.2 s with no temporary files left.

## Design choices that keep it fast

- One shader draws the whole wheel; textures are uploaded once per opening.
- The desktop backdrop is captured once per window (not per frame) and blurred on the CPU.
- Dialogs render only when something changed or an animation runs (`Invalidate` on input/timers); preview work (Edit Photo, thumbnails, QR scan, detection) runs on worker threads and posts results back.
- Output files are streamed to a temp file and moved into place; nothing is held in memory beyond the working images (the collage and annotate tools decode at most 2600 px / one full-size image).

## Not measured

Wheel open latency from the first modifier press, GPU frame times on integrated graphics, memory while a 12 MP image is open in Edit Photo, behaviour with thousands of selected files. These have not been profiled.
