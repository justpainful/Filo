# Shell integration

Filo uses only documented, supported Windows APIs. It does **not** inject into Explorer, patch it, or use undocumented interfaces.

> **Honest status:** the whole drag pipeline is verified with a test harness (`filo_e2e_drag`, see TESTING.md) that performs a real OLE drag with
> `DoDragDrop` and a real shell data object. **A drag that starts in the real Windows File Explorer has not been verified by me**: the
> automation available while building could only click, not press-and-drag across applications. Before release, do the manual check in the
> checklist at the end of this file.

## Detecting "a file is being dragged"

1. **Global low-level hooks** (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`) on a dedicated thread observe: left button down/up (with position), movement beyond
   the drag threshold (system setting, or `Settings.dragThresholdPx`), key down, and modifier changes. The hook procedures only post small
   messages to the UI thread, so they never block input. Nothing is logged or stored.
2. A **real OLE drag** is recognised by the drag-image window class `SysDragImage` that the shell creates while a drag is in progress (a sticky probe, because the window
   only exists while the cursor is over a drop target). A press that began on an Explorer/desktop list view is the other signal (`IsShellViewAt`).
3. State machine `DragSession`: **Idle -> Pressed -> Dragging -> WheelOpen -> Closing**. The wheel opens when a wheel modifier is pressed during a drag (Shift = Convert, Alt = Tools, Ctrl = Actions by default;
   configurable in Settings > Wheels; switching modifiers switches wheels without dropping the drag). The left button is released *after* the OLE drop is delivered, so the session keeps a
   350 ms grace after the hook sees button-up.
4. Pressing Alt would normally activate a menu on release; Filo injects a harmless unused virtual key (`VK_E8`) so the Alt/Win key-up does not trigger menu activation.

## Knowing which files are being dragged

- **Prefetch** (before the wheel is shown): `IShellWindows` -> `IServiceProvider(SID_STopLevelBrowser)` -> `IShellBrowser` -> `IFolderView2::GetSelection` for the Explorer window under the cursor
  (also works for the desktop). This gives the selection while the wheel is already animating in.
- **Authoritative**: when the cursor enters the wheel, its `IDropTarget::DragEnter` receives the real `IDataObject`; its `CF_HDROP` / shell ID list replaces the prefetched list.
  If the two differ, the data object wins.

## The wheel as a drop target

The wheel window is `WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, composed with DirectComposition, registered with `RegisterDragDrop`, and uses `IDropTargetHelper` so Explorer's drag image
keeps rendering over it. The drop target answers `DROPEFFECT_COPY` (or LINK) only - **never MOVE** - so a drop on Filo can never cause Explorer to delete the source files. `Drop()` posts a
message and returns immediately, because Explorer's drag loop is blocked until it returns; the work happens afterwards.

## Keyboard / context-menu / Send To paths

- **Global shortcut** (default Ctrl+Alt+F, `RegisterHotKey`): opens the Convert wheel for the files selected in the foreground Explorer window (same `IFolderView2` path).
- **Context menu**: the installer adds a per-user cascade **Filo > Convert... / Tools... / Actions...** (`HKCU\Software\Classes\*\shell\Filo` with `SubCommands`, verbs in
  `HKCU\...\Explorer\CommandStore\shell\Filo.*`) and **Filo > Actions...** for folders. Each verb runs `Filo.exe --wheel --kind <convert|tools|actions> "%1"`. When several files are selected Explorer starts one
  process per file; the running instance collects the paths for 180 ms (`WM_COPYDATA`) and opens one wheel for all of them. On Windows 11 the cascade is under **Show more options**
  (the compact menu only shows package-registered extensions); a package-based modern menu entry is not implemented.
- **Send To**: a `Filo` shortcut in `shell:sendto` runs `Filo.exe --wheel <paths...>`.
- All of these start the same runtime; if Filo is already running (tray) the paths are forwarded to it.

## Actions

Share = Windows share UI (`IDataTransferManagerInterop`), Copy = `CF_HDROP` + `Preferred DropEffect = copy` (Ctrl+V in Explorer copies), Move = folder picker + `IFileOperation` (progress, conflicts, undo, cross-volume),
Copy Path = `CF_UNICODETEXT`, Terminal = Windows Terminal / PowerShell / cmd in the folder, Float = the Float shelf (below).

## Float

A small always-on-top window (`src/tools/float_shelf.cpp`). Collapsed: a fanned stack of up to three thumbnails with a paper clip and a count badge; click to expand into a tall glass list; the "N items" header collapses it again;
drag the stack or the list header to move it. Rows can be dragged into any application: the drag is a standard shell drag (`SHCreateDataObject` of the selected paths + `SHDoDragDrop` offering copy, move and link), so the target decides.
Dropping files on Float adds them. Entries whose file disappears are removed. Files are only referenced, never copied or moved by Float.
Not implemented: docking the stack to a screen edge as a small pill (the reference video shows one).

## Failure behaviour

If hooks cannot be installed, the tray menu says so and the keyboard/context-menu/Send To paths still work. If Explorer's selection cannot be read the wheel opens with the data object from `DragEnter`; if neither is available it shows
"Nothing selected." Elevated windows (running as administrator) do not receive hook events from a non-elevated Filo; this is a Windows isolation rule and is listed in KNOWN-LIMITATIONS.md.

## Manual verification checklist (to do on a real desktop)

1. Start Filo (tray icon appears). Open Explorer on a folder with an image.
2. Drag the image; while dragging, hold **Shift** -> the Convert wheel opens near the cursor; move onto a sector (blue hover) and drop. A new file appears next to the original, and the original is untouched.
3. Repeat with **Alt** (Tools) and **Ctrl** (Actions). Release the modifier without dropping: the wheel stays or closes as set in Settings > Wheels.
4. Drag a file from the **Desktop** and from a folder in a second monitor with different DPI.
5. Press **Esc** or drop outside the wheel: nothing happens, no files move.
6. Select several files and use the right-click Filo menu; use Send To; press Ctrl+Alt+F.
7. Float: choose Actions > Float, expand, drag a row into another folder.
