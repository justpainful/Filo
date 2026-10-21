# Testing

Everything below was actually run; "not run" is stated where that applies. Test binaries are built with the app (`scripts\build.ps1`), live in `build\<config>\bin`
and use a tiny framework (`tests/test.h`: `TEST(name)`, `CHECK`, `CHECK_EQ`, an optional substring filter as the first argument). Assertion dialogs are disabled in
`tests/test_main.cpp` (failures go to stderr, exit code 1), so a test run can never block on a pop-up.

## Suites

| Suite | Exe | Tests | What it covers |
|---|---|---|---|
| Core | `filo_tests` | 20 | wheel geometry (rounded annular-sector SDF, gaps, hover scale, one owner per point) and the drag state machine (Shift/Alt/Ctrl, switching wheels, OLE-drag detection, grace after button-up, Esc, custom shortcuts, disabled) |
| Image | `filo_tests_image` | 64 | decode/encode of every raster format, EXIF orientation (all 8), ICC, metadata round trip, compress modes, SVG, animated GIF/WebP, PDF writer, QR reading (9 variants), corrupt/truncated/odd-size inputs, long and Unicode paths, cancellation |
| Media | `filo_tests_media` | 37 | every video/audio capability through `RunConversion`, edge cases (rotation, VFR, HDR, odd sizes, multi-audio), trim/crop/speed/join/split/frames/redact/mute/normalize/bleep/visualizer accuracy, robustness, cancellation |
| PDF | `filo_tests_pdf` | 41 | open statuses, rendering sizes, text extraction (columns, Arabic, rotation), tables, OCR, merge/split/organize/compress/images->PDF, fuzzing, 300-page performance, cancellation |
| Archive | `filo_tests_archive` | 76 | zip/tar/gz/7z paths, extraction safety (zip-slip, bombs), Unicode and long names, writers read back by independent tools |
| Metadata | `filo_tests_meta` | 27 | ExifTool-backed read/write/strip for images, media, Office files, PDFs; GPS handling; verification |
| Documents | `filo_tests_doc` | about 65 | CSV/XLSX/ODS, DOCX/EPUB/RTF/Markdown/HTML/subtitle writers and readers, hostile inputs, LibreOffice-based conversions |

Independent verification is the rule: outputs are read back with a different implementation (Pillow, PyMuPDF, ffprobe + full decode, WIC, PDFium, bsdtar, 7-Zip, LibreOffice) rather than with the code that wrote them.

## Results on the development PC (2026-10-20)

| Suite | Last result I observed | Build |
|---|---|---|
| Core | 20 / 0 failures | Debug and Release |
| Image | 64 / 0 failures (after my changes to `output.cpp` and `ffmpeg.cpp`) | Release |
| Media | 37 / 0 failures (after the same changes; about 5 minutes) | Release |
| PDF | 41 / 0 failures | Release |
| Archive | 76 / 0 failures | Debug and Release |
| Metadata | 27 / 0 failures | Debug and Release |
| Documents | **Partly run**: the 24 tests that do not start LibreOffice passed (zip, text/encoding, Markdown, HTML, subtitles, TextClipping). The LibreOffice-dependent tests were **not run by me**: LibreOffice contacts the default printer and made Windows show printer pop-ups on the owner's PC. An earlier run by the engine's author recorded 77 passing conversion paths (`testdata/doc/matrix-results.json`, LibreOffice 26.8.1.1); that was before the author's work was stopped, and I have not repeated it. |

## End-to-end and manual

- `filo_e2e_drag` is a harness that acts as a real OLE drag *source* (a window with a shell data object, drag image helper) and drives the mouse and keyboard with `SendInput` against a running `Filo.exe`: hold the left button, press Shift/Ctrl/Alt, move over a sector, release.
  It was used during development to prove the wheel opens, hover hit testing works, drops are received with the right file list, and COPY-only effects are returned. It takes control of the mouse, so run it only on a machine you are not using
  (`filo_e2e_drag.exe --file <path> --mod shift --sector 0`, Esc aborts). It was not run again for this release.
- **Not automated and not verified by me**: dragging from the real File Explorer / Desktop, the right-click cascade, Send To, the global shortcut, Float dragging into other apps, tray menu, autostart, high-DPI moves between monitors. Use the checklist in SHELL-INTEGRATION.md.
- Visual regression: `Filo.exe --render-wheel`, `--tool-shot`, `--settings-shot`, `--float-shot`, `--hud-shot` render hidden windows to PNG; `tools\*.py` compare them with the reference screenshots. These are reviewed by eye and by sampled-region statistics, not by an automatic pass/fail threshold.
- Headless tool runs: `Filo.exe --tool-run <tool> <preset> <files>` (all 26 tools were run at least once; see TOOLS.md) and `filo-cli convert <file> <format>` / `filo-cli list`.
- `Filo.exe --wheel-dump <files>` prints what each wheel offers for a selection (used to check ordering and applicability rules).

## Bugs found by testing in this pass (fixed)

- `ff::HasMuxer` always returned false (wrong header separator) - fixed in `engines/common/ffmpeg.cpp`.
- `OutputTransaction::Commit` used non-long-path file checks, so outputs deeper than 260 characters failed - fixed with `LongPath` in `core/output.cpp`.
- PDF -> PPTX verification always failed (slide relationship read from the wrong attribute) - fixed in `engines/doc/pptx.cpp`.
- A debug-assertion dialog in the PDF tests (empty-output vector) - fixed by the PDF engine author and by the runner's assertion handling.

## Adding tests

Engine tests: `tests/<engine>/*.cpp` (picked up by the glob; each engine gets `filo_tests_<engine>`). Record per-path results in `testdata/<engine>/matrix-results.json`
(`{"results": {"<in>><out>": {"status": "pass", "note": "..."}}}`) and regenerate `docs/FORMAT-MATRIX.md` with `python -I tools/gen_matrix.py`.

## UI correction — 21 October 2026

The shared material and native window lifetime tests are in `tests/test_material.cpp` and `tests/test_dialog.cpp`. Material tests check 729 RGB colours x 4 intensity settings x 2 themes (5,832 surfaces), primary/secondary/tertiary contrast >=4.5:1, opaque-mode independence from background colour, and preservation of backdrop hue. Native window tests verify that Escape and closing during rendering defer object deletion until the active dispatch/frame has returned, and that WM_UNICHAR preserves Arabic and supplementary Unicode codepoints.

The live Windows check used the standalone metadata window, focused search, entered a physical key with the active Arabic keyboard layout, observed the filtered empty state, and closed the window. This exposed a posted-frame loop starving hardware input; frame scheduling now uses a low-priority timer. The final build also passed literal `type_text` of "File size", filtered to the size row, and switched View/Edit afterwards. Standard WM_PASTE/WM_COPY/WM_CUT and WM_UNICHAR input are routed into the custom widgets; Arabic plus supplementary codepoints have a regression test. Do not confuse the offscreen stress renders with a complete manual acceptance run of every editor.

`tools/review_ui.py` runs the real executable and writes dark/white/colour background metadata screenshots, dark-theme metadata, four wheel states, Edit Photo, Add Background, Join Videos, Trim Audio and the RTL widget gallery. Outputs are under `out/ui-review/after/`; `docs/UI-REVIEW-2026-10-21.md` records the final build/test result.


## Native live glass (21 October 2026, 0.1.2)

Build/run `filo_e2e_live` from the repository root on a visible Windows 11 desktop with system transparency enabled. It owns both background and foreground windows, covers four changing backgrounds without foreground repaint, 162 measured RGB/intensity/theme samples, reduced transparency, resize and simulated device loss. Review with `python tools/review_live.py out/live-glass`. The native captures, CSV, JSON and GIF evidence are described in LIVE-GLASS-2026-10-21.md. Offscreen `--tool-shot` captures do not include the compositor-owned live backdrop and cannot validate this feature.


## Liquid Glass (21 October 2026, 0.1.3)

26 core/settings tests passed. Both native materials passed 162 measured background/intensity/theme samples each (324 total), opaque/recovery/resize checks, and the optical scene passed at forced native 96/120/144 DPI. LayerVisual refraction calibration shows real local displacement and ongoing background updates; foreground and mask counters remain unchanged during background-only changes. Reflection-off/refraction-off comparisons and unchanged foreground ink are checked by tools/review_liquid.py. The actual Settings UI was used to select both materials and verify persistence without changing other preferences. See LIQUID-GLASS-2026-10-21.md and out/liquid-glass/release-manifest.json. Engine suites were not rerun for this material-only revision.


## Icon plugins and Acrylic (21 October 2026, 0.1.4)

Run `filo_tests`, `filo_e2e_icons out/icon-packs`, and `filo_e2e_live out/icon-packs/native --icons`. The new icon harness draws all 68 icons from four packs at 16/24/32 DIP and 96/120/144 DPI (2448 renders), checks fallback, cache isolation, theme selection and external partial-plugin discovery/removal, and exports light/dark atlases. The native scene covers 36 pack/material/DPI combinations. Run `filo_e2e_live out/acrylic --acrylic` then `python tools/review_live.py out/acrylic` for live background updates, 162 RGB/intensity/theme samples, reduced transparency, resize and device reset. See ICON-PACKS-2026-10-21.md for results and scope.

Final 0.1.4 results: 29 core tests, 2448 glyph renders, 36 native wheel combinations and 162 revised Acrylic samples passed. The corrected Acrylic material passes 7.13:1 light and 5.29:1 dark minimum measured text contrast; `python tools/review_icon_packs.py` checks sector colour response, gap clarity and idle counters and composes the review panels. Classic actions/tools renders remain pixel-identical to 0.1.3.


## Float and glass controls (0.1.5)

`filo_e2e_controls out/controls/regression-final --regression-only` performs the final 116 owned-window interaction checks. The full `filo_e2e_controls out/controls` run also captures 30 native warm/cool transparency pairs and four blur levels per theme/DPI; review with `python tools/review_controls.py`. Core tests: 30 passed. Native Acrylic: 162 measured samples passed. Native wheels: 36 combinations passed. See CONTROLS-2026-10-21.md for detail and scope; the harness uses synthetic background windows and keeps its original fixtures intact.
