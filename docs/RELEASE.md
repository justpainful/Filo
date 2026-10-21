# Release checklist

## Produce the installer

1. `pwsh scripts\build.ps1 -Config Release -Suffix _rel -Reconfigure`
2. Run the test suites (TESTING.md). Do not ship with a failing suite.
3. `pwsh scripts\package.ps1 -Version <x.y.z>` -> `dist\Filo-Setup-<x.y.z>.exe` (per-user install, no administrator rights, LZMA2).
   The 1.0.0 build produced on the development PC is about 131 MB; almost all of it is the two FFmpeg executables (about 220 MB each uncompressed).
4. Check the installer contents: `licenses\` must contain FFmpeg's `LICENSE` (GPL), PDFium's licences, ExifTool's README and the Strawberry Perl licence list, and `THIRD-PARTY-NOTICES.md`.

## What the installer does

- Installs to `%LOCALAPPDATA%\Programs\Filo` (the user can choose another folder).
- Optional tasks (all on by default): start Filo when signing in (`HKCU\...\Run`), the right-click **Filo** cascade (`HKCU\Software\Classes\*\shell\Filo` + CommandStore verbs; folders get Actions only),
  the **Send to > Filo** shortcut.
- Start-menu entries (Filo, Filo Settings), uninstaller (stops the running instance with `Filo.exe --quit`, removes the registry keys and the shortcut). User settings in `%LOCALAPPDATA%\Filo` are kept.
- No services, no drivers, no firewall rules, no network access.

## Signing and distribution

The installer and `Filo.exe` are **not code-signed** (no certificate was available). Windows SmartScreen will warn on first run until the files are signed. To sign:
`signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256 /a Filo.exe` before `package.ps1` copies it, then sign `Filo-Setup-*.exe`.

## GPL note for redistribution

The FFmpeg binaries are GPL v3 and are shipped unmodified as separate programs. Redistributing the installer requires keeping the licence texts with it and providing the
corresponding FFmpeg source (or a written offer) - details in LICENSES.md.

## Before announcing

- Do the manual shell checklist at the end of SHELL-INTEGRATION.md on a real desktop (the real-Explorer drag is the one thing not verified by automation).
- Test install and uninstall on a clean user account / VM: right-click cascade appears (under "Show more options" on Windows 11), Send To works, the uninstall removes both.
- Test on a second monitor with different scaling, and with Windows set to Arabic / right-to-left.
- Re-run `python -I tools\gen_matrix.py` so FORMAT-MATRIX.md matches the latest test results.

## Versioning

`project(Filo VERSION ...)` in `CMakeLists.txt` and the `-Version` argument of `package.ps1` should be changed together; the version string is shown by the installer only (the app has no update checker and never contacts a server).

## UI revision 0.1.1 (21 October 2026)

The source project and executable resource version are 0.1.1. `scripts/package.ps1` now defaults to 0.1.1 and verifies the resolved staging directory before clearing it. The earlier `Filo-Setup-1.0.0.exe` is a historical package and does not contain the UI fixes. Use `dist/Filo-Setup-0.1.1.exe` for this revision. This package number does not imply completion of the production requirements listed in KNOWN-LIMITATIONS.md.


## Live glass revision 0.1.2 (21 October 2026)

Native Windows.UI.Composition host backdrops replace runtime desktop snapshots in tool windows and the wheel. The compositor updates behind static UI; text brightness bounds, clipped outer shadows, intensity and opaque accessibility modes remain. Already-open windows apply appearance changes, and graphics-device recovery rebuilds their compositor targets. Public host backdrops require Windows 11 build 22000+; earlier Windows uses opaque surfaces. `dist/Filo-Setup-0.1.2.exe` is the package for this revision. See LIVE-GLASS-2026-10-21.md for native capture verification and limits.


## Liquid Glass revision 0.1.3 (21 October 2026)

New default Liquid Glass adds a milky surface, rolled rims, directional reflections, a quiet surface sheen and local live refraction. The former material is retained as Soft Glass under Settings > Appearance. Reflections, refraction and intensity are independent controls. Live optical sources and foreground/mask idling are checked in the native Windows compositor, with no desktop capture backend. Packaging uses build/stage-0.1.3 to preserve the older running staging application. The previous 0.1.2 installer and source baseline remain available. See LIQUID-GLASS-2026-10-21.md for verification and limits.


## 0.1.4 appearance assets

The installer must include `icon-packs/01-pebble.json`, `02-circuit.json`, and `03-facet.json`. Classic remains compiled into the executable. Packaging reads the source `assets/icon-packs` folder into the isolated `build/stage-0.1.4` directory. User plugins and preferences in `%LOCALAPPDATA%/Filo` survive updates. Do not clean the legacy running `build/stage` checkout.


## 0.1.6 close button everywhere

Settings, the widget gallery and the progress card now use the same circular glass close control as the tool windows (28 DIP, top left, hover and pressed states, live local blur when the host backdrop is available). `DialogOptions::glassCloseButton` defaults to true. The progress card's button is aligned with it (28 DIP, never mirrored in RTL). Package: `dist/Filo-Setup-0.1.6.exe` (staged in `build/stage-0.1.6`; the running older stages were not touched). Checked by rendering Settings, the gallery, Float and the progress card offscreen; not clicked in a live session.
