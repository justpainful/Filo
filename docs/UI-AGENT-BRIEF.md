# Tool-dialog brief (read fully before writing code)

Filo is a native Windows (C++20/MSVC/CMake) drag-and-drop converter + editor that reproduces the macOS app *Tangerine* with
the highest achievable visual and behavioural fidelity. **The owner rejected the first UI as "nothing like the original"**, so
fidelity is the top acceptance criterion: every dialog must look like it belongs in the reference screenshots
(`reference/*.png`, 2x retina captures; `dip = px / 2`) - vivid liquid-glass window, warm backdrop-tinted text, big radii, blue
accent (Filo Blue replaces the original orange accent everywhere, incl. hover/selection).

## Work area and rules
* You work in YOUR copy of the repo: `C:\Users\kuroi\OneDrive\Desktop\Filo\_wt\<name>` (never touch the parent repo or the other
  copies). The lead merges your files afterwards.
* You may only ADD/EDIT files under `src/tools/<group>/` (new sub-folder; CMake globs it recursively into `filo_tools`), plus
  `docs/fragments/ui-<name>.md` and test fixtures under `testdata/ui/`. Do NOT edit `src/ui`, `src/gfx`, `src/design`, `src/core`,
  `src/registry`, `src/shell`, `src/app`, `src/engines/*`, `CMakeLists.txt`, `scripts/*`, `tools/*.py`. If you need something there
  (a generic widget, a token, an engine fix) implement it locally under your folder or list it in your report.
* Never run `Stop-Process` on `Filo` (other agents share the machine). Run executables only with
  `Start-Process -Wait` / foreground and they must exit by themselves.
* Never open real windows that need user interaction. Everything is verified headless (see below). No git. No network.
* No placeholders: every control must do real work with the engines (headers in `src/engines/*/*.h`). Honest limits go into
  your fragment and into on-screen messages.

## Build
```
pwsh scripts\build.ps1 -Config Debug -Suffix _<name> -NoEngines -ToolEngines "media;image;pdf;meta;archive;doc" -ExcludeSrc "<engine source basenames that do not compile yet>" -Target Filo
```
Engines are being written by other agents: if an engine `.cpp` does not compile in your snapshot, pass its basename in
`-ExcludeSrc` (semicolon list) and avoid calling what it implements; do not edit it. Output: `build\Debug_<name>\bin\Filo.exe`.

## Toolkit you must use (read these files)
* `src/design/tokens.h` - all visual constants (measured from the reference). `src/design/theme.h`.
* `src/ui/ui.h`, `src/ui/painter.h`, `src/ui/canvas.h` - the immediate-mode widget layer: `SliderRow`, `Slider`, `Segmented`, `Toggle`,
  `Checkbox`, `TextInput`, `Stepper`, `Dropdown`, `Swatch`, `Button`, `IconButton`, `ProgressBar`, `BeginScroll`, layout (`Take`,
  `Cols`, `Mirror`), `Behavior()` for custom widgets, `AutoId`/`PushId`. Icons: `src/gfx/icons.cpp` names (`ui.P().Icon("trim", ...)`).
* `src/ui/dialog_window.h` - window chrome (glass backdrop, title bar, close button, resizing, file drop, RTL).
* `src/tools/tool_dialog.h` - `ToolDialog` base: `RunJob`, `DrawJobPanel` (progress/cancel/done/error), `DrawFooter`, `Thumb()` async
  thumbnails, presets (`SavePreset/LoadPreset`), `TestApply()` + `RunForTest` hook.
* `src/tools/media_player.h` (ffmpeg-based audio/video preview player, waveOut audio clock), `src/tools/widgets_media.h`
  (`Timeline`, `WaveformTrim`, `WaveformRegions`), `src/tools/photo_adjust.h`.
* Exemplars: `src/tools/join_videos.cpp` (list dialog) and `src/tools/edit_photo.cpp` (canvas + slider sections + async preview).
* Registration: `src/tools/tools.h` (`FILO_TOOL_GROUP(<Group>) { add(ToolInfo{...}, &MakeXyz); }`, accepts-rules, wheel labels/icons).
  Wheel order = registration order. Never offer a tool that cannot run on the selection (`accepts`).

## Fidelity method (mandatory, repeat until the numbers and the eye agree)
1. Where a reference screenshot exists, extract its layout numbers (crop with `python -I tools\crop.py ref out x0 y0 x1 y1 [scale]`,
   sample colours with `tools\region_stats.py`). Use `dip = px/2`. Tokens already capture the common metrics; do not invent new values.
2. Render your dialog hidden at the reference scale over a clean backdrop:
   `$env:FILO_TEST_DPI='192'; $env:FILO_TEST_BACKDROP=(Resolve-Path reference\backdrop-edit.png).Path` then
   `Start-Process build\Debug_<name>\bin\Filo.exe -ArgumentList '--tool-shot','<toolId>','out\x.png',<files...>,'--at','1110,826' -Wait`
   and compose/compare with `tools\compose.py`, `tools\sidebyside.py`, `tools\region_stats.py` (see how `out/run_edit.ps1`-style scripts do it).
   `--light`/`--dark` select the theme. Read the PNGs with the image viewer and LOOK at them against the reference.
3. Without a reference (most tools) keep the exact same design language: same title bar, margins (`dialog::contentInset`), row heights,
   fonts (`dialog::bodyFontSize`), glass, sections with `SectionTitle`, footer via `DrawFooter`, job panel via `DrawJobPanel`.
   Nothing may look like a stock Windows control. Dark and light themes and RTL (`--rtl` not available in tool-shot: set
   `DialogOptions.rtl` from Settings language and use `Cols`/`Mirror`) must not break layouts.
4. Every window needs sane empty / loading / error states and keyboard support (Tab, Enter = primary action when no text field has
   focus, Esc closes). Never block the UI thread for more than a few ms: heavy work in jobs or worker threads (see edit_photo.cpp).

## Verification (mandatory)
* Implement `LoadPreset(json)` (set options) + `TestApply()` (start the job, same code path as the button) in every dialog.
* Test each tool end to end: `build\Debug_<name>\bin\Filo.exe --tool-run <toolId> '<presetJson>' <files...>` prints JSON
  `{ok, error, outputs, warnings}`. Verify the outputs with independent tools (ffprobe, WIC/PIL via `python -I`, PDFium). Generate fixtures
  with ffmpeg lavfi / PIL into `testdata/ui/`. Check: the input is untouched, outputs are next to the input with a unique name, cancel works,
  a corrupt/empty file gives a clear error.
* Write `docs/fragments/ui-<name>.md`: tools delivered (id, wheel label, accepts rule), reference comparison notes (what matched, what did
  not, numbers), tests run + results, known gaps.

## Final report (< 60 lines)
List the files you created, the registration order, screenshots you produced (paths under `out\`), the verification results, and every
request for the lead (core changes, tokens, widgets). Do not claim anything you did not run.
