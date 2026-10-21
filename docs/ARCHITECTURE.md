# Architecture

Filo is one native Windows executable (C++20, Win32, COM, Direct3D 11 / Direct2D / DirectComposition / DirectWrite). No web view,
no managed runtime, no UI framework. Conversion and editing work is done by in-process engines (static libraries) and three bundled
helper programs (FFmpeg, ExifTool, and the optional LibreOffice) that run as separate processes.

## Modules (`src/`, one static library each, see `CMakeLists.txt`)

| Module | Responsibility |
|---|---|
| `core` | utilities, logging, crash handler, COM helpers (`ComPtr`, no ATL), JSON, process spawning (`SpawnPiped`, `RunCapture`), the **Job manager** (queue, progress, cancel, results), **output** (`OutputTransaction`: temp file -> verify -> atomic commit, unique names, long paths), settings store |
| `design` | design tokens (`tokens.h`) and the theme (light / dark / system, glass intensity, reduce transparency / motion) |
| `gfx` | D3D/D2D device and shared factories, `Surface` (DirectComposition window base), native host backdrop composition + fixture blur, icon cache (SVG-path icons), image IO helpers, HLSL shaders compiled with `fxc` at build time |
| `wheel` | the radial wheel: geometry (`geometry.cpp`, hit testing), the window (`wheel_window.cpp`), the pixel shader (`wheel.hlsl`) |
| `shell` | everything that touches Explorer: low-level input hooks, drag state machine, Explorer selection reader, tray icon, shell thumbnails, the six action implementations, the wheel controller (opens the wheel, owns the drop target) |
| `registry` | file type detection, the **capability registry** (input format -> output format -> `run`), selection availability (what each wheel shows for a selection) |
| `ui` | immediate-mode UI toolkit (`Ui`, `Painter`, widgets), `DialogWindow` chrome with the glass look, zoom/pan canvas, settings window, widget gallery |
| `tools` | the tool dialogs (`ToolDialog` base: jobs, progress, thumbnails, presets) and their registration |
| `engines/<name>` | `common` (ffmpeg wrapper), `image`, `media`, `pdf`, `doc`, `archive`, `meta`; each registers capabilities and exposes headless operation APIs |
| `app` | `main.cpp` (modes: runtime, `--wheel`, `--settings`, `--quit`, visual-test modes), `runtime.cpp` (message window, tray, hooks, job feedback) |

Dependencies point downward only: `app -> tools -> ui/engines -> registry -> core`. Engines never include UI code; the UI never
contains conversion logic. `docs/ENGINE-CONTRACT.md` is the rulebook engines follow.

## Threads

- **UI thread**: runtime message window, tray, the wheel window, all dialogs, D2D rendering. Dialog work is never done here beyond cheap
  CPU steps; anything long runs as a Job.
- **Hook thread**: owns the `WH_MOUSE_LL` and `WH_KEYBOARD_LL` hooks and a small message loop. It only posts compact events
  (`WM_FILO_HOOK`) to the UI thread, so the hooks always return immediately (Windows removes slow hooks).
- **Job workers** (`JobManager`, `concurrentJobs` setting, default 2): run engine code, report progress, honour cancellation.
- Short-lived workers for thumbnails, QR scanning, face/text detection, PDF page thumbnails; they post results to the owning window.

## Drag, wheel, drop (summary; details in SHELL-INTEGRATION.md)

1. Hooks report left-button down, movement past the drag threshold, button up and modifier changes.
2. `DragSession` (Idle -> Pressed -> Dragging -> WheelOpen -> Closing) decides when a real Explorer/desktop drag is under way
   (a `SysDragImage` window exists, or the press started on a shell view) and which wheel a modifier opens.
3. The selection is prefetched from Explorer (`IShellWindows` -> `IFolderView2::GetSelection`) and is confirmed by the `IDataObject`
   when the wheel's drop target receives `DragEnter`.
4. The wheel window (topmost, no-activate, DirectComposition) is also an OLE drop target (`RegisterDragDrop` + `IDropTargetHelper`).
   Dropping on a sector posts a dispatch message and `Drop()` returns at once (Explorer's drag loop is blocked until then), advertising
   COPY only so the source files can never be moved or deleted by a drop.
5. `Dispatch` starts a conversion Job, opens a tool dialog, or runs an action.

## Wheel rendering

The wheel is one fullscreen-quad pixel shader (`wheel.hlsl`) evaluating the same rounded annular-sector signed distance field as the
C++ hit test. A Windows.UI.Composition host backdrop samples live content behind the HWND, under the UI swap chain; the shader draws translucent sector tint, fresnel and interaction over it. Soft Glass clips the source to the animated circle. Liquid Glass uses five native LayerVisual passes with HLSL geometry masks for live edge refraction, while retaining natural gaps and drawing reflections above them. Visible dialogs share this compositor and use rounded body clips. Desktop capture is absent from the material path; bitmap fixtures are only used by hidden visual tests.
Labels and icons are D2D/DWrite layers composed above. Hover scale, rim, specular and bevel are shader parameters from `tokens.h`.

## Jobs and output safety

Every conversion/edit is a `Job`. Engines write to `<name>.filo-<id>.tmp<ext>` in the destination folder, the result is verified
(decoder / ffprobe / structural checks), then moved into place atomically (`MoveFileEx`). On failure or cancel nothing is left at the
target. Names never overwrite unless the user chose "Replace". Inputs are only ever read.

## Capability registry

`CapabilityRegistry` maps `input format -> output format` to a `Capability` (`run`, optional `verify`, option specs, `Runnable()`).
A path is registered only when it works on this PC (for example HEIC/AVIF encoders are probed at start-up; LibreOffice-based paths need
LibreOffice). The wheel offers the intersection of outputs for the selected files, ordered as in the reference wheel.

## Tool dialogs

`ToolDialog` derives from `DialogWindow` (glass chrome) and adds job running (progress / cancel / done / error panel), async thumbnails,
saved presets (`Settings.toolPresets`) and a standard footer. Each tool implements `Draw`, optional `SavePreset/LoadPreset`, and `TestApply`
so `Filo.exe --tool-run <id> <preset> <files>` can run it headless.

### Appearance plugins and Acrylic

`gfx/icon_packs` discovers bounded data-only JSON manifests from the installed and per-user icon-packs directories. `IconCache` resolves the current ThemeState pack, caches geometry by pack/key/layer, and inherits missing keys from Classic. Explicit reload invalidates geometry; runtime settings changes repaint open surfaces. The Acrylic material reuses a single masked native backdrop layer with a stronger Gaussian frost and colour-preserving luminosity bounds. Its light coating and static grain remain below crisp foreground UI. See ICON-PACKS-2026-10-21.md.
