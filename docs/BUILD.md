# Building Filo

## Requirements

- Windows 10 1809+ or Windows 11, x64
- Visual Studio 2022 with the *Desktop development with C++* workload (MSVC v143, Windows 10/11 SDK; `fxc.exe` from the SDK compiles the shaders)
- CMake 3.25+ and Ninja on `PATH`
- PowerShell 7 (`pwsh`)
- `third_party\` contents are fetched once (not in source control): PDFium prebuilt (`pdfium-win-x64.tgz` -> `third_party\pdfium`), ExifTool for Windows
  (`third_party\exiftool`), miniz 3.1.2 (`third_party\miniz`), ZXing-cpp 3.1.1 (`third_party\zxing`). Archives live in `third_party\_dl\`; `cmake\thirdparty.cmake` documents the layout.
- To run the full engine test-suites: FFmpeg "full" build on `PATH` (or in `tools\`), Python 3 with Pillow and NumPy (`python -I`), optionally PyMuPDF and LibreOffice (tests that need an absent tool are skipped or report it).
  **Warning:** the document tests start LibreOffice, which contacts the default printer; on a PC with an offline printer Windows shows "Waiting for printer connection" pop-ups while they run.

## Build

```powershell
pwsh scripts\build.ps1 -Config Release -Suffix _rel          # everything: Filo.exe, engines, all test executables
pwsh scripts\build.ps1 -Config Debug  -Suffix _full           # debug build with every engine
pwsh scripts\build.ps1 -Config Debug -Suffix _ui -NoEngines -ToolEngines "media;image;pdf;meta"   # fast UI iteration build
```

`scripts\env.ps1` loads the MSVC environment (it is dot-sourced by `build.ps1`). Output goes to `build\<Config><Suffix>\bin`. Use `-Reconfigure` after adding source files.
Source files are picked up by `GLOB_RECURSE ... CONFIGURE_DEPENDS` per module (`src/<module>`) and per engine (`src/engines/<name>`); an engine may add a hook `cmake\engines\<name>.cmake`
for extra link libraries.

Targets: `Filo` (the app), `filo-cli`, `filo_tests` (geometry, drag state machine), `filo_tests_<engine>`, `filo_e2e_drag` (OLE drag harness).
The CRT is linked statically (`/MT`), so the executable needs no Visual C++ redistributable.

## Run

```powershell
build\Release_rel\bin\Filo.exe                 # tray app
build\Release_rel\bin\Filo.exe --settings      # settings window
build\Release_rel\bin\Filo.exe --gallery       # widget gallery
build\Release_rel\bin\Filo.exe --tool edit-metadata <file>  # standalone native tool window
build\Release_rel\bin\Filo.exe --tool-run <id> <preset.json|-> <files...>     # headless run of a tool, prints JSON
build\Release_rel\bin\Filo.exe --tool-shot <id> <out.png> <files...> [--at x,y] [--light|--dark]   # hidden render of a tool dialog
build\Release_rel\bin\Filo.exe --render-wheel <convert|tools|actions> <out.png> [--backdrop img.png] [--cursor x,y] [--hover n]
build\Release_rel\bin\Filo.exe --settings-shot <out.png> [tab]   |   --float-shot <out.png> [--expanded] <files...>
```

Visual tests at the reference scale: set `FILO_TEST_DPI=192` and `FILO_TEST_BACKDROP=<image>` (a picture that stands in for the desktop behind the glass).
Helper scripts for comparing against the reference screenshots are in `tools\*.py` (run with `python -I`).

## Installer

```powershell
pwsh scripts\package.ps1 [-Config Release] [-Suffix _rel] [-Version 1.0.0] [-FfmpegDir <dir>]
```

Stages `Filo.exe`, `pdfium.dll`, `tools\ffmpeg.exe`, `tools\ffprobe.exe`, `tools\exiftool\...` and `licenses\` into `build\stage`, then compiles `installer\filo.iss`
with Inno Setup 6 into `dist\Filo-Setup-<version>.exe`. See RELEASE.md.
