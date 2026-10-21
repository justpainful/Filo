# Engine contract

How engines plug into Filo. Read this before writing code under `src/engines/<name>/`.

## What an engine is

A static library (`filo_eng_<name>`, auto-created from every `.cpp` under `src/engines/<name>/`) that
1. registers **capabilities** (`registry/capability.h`) - one per advertised `input format -> output format` path, and/or
2. exposes **headless operation APIs** (headers under `src/engines/<name>/`) that tool dialogs call (trim, crop, merge...).

Nothing in an engine may touch UI, windows or the shell. Engines run on worker threads inside a `JobContext`.

## Hard rules

* **Real work only.** No extension renaming, no fake output, no placeholder success. If a path cannot be done truthfully on
  this machine, do not register it (or register it with `run == nullptr` and a `limitation` so FORMAT-MATRIX.md documents it).
* **Never write the final path.** The registry's `RunConversion` hands you a temp path (`ConvertTask::output`). For
  `producesFolder` capabilities `output` is a temp *directory*. Operations you expose directly must use
  `core/output.h` (`OutputTransaction` / `OutputDirTransaction`) the same way.
* **Never modify the input file.**
* **Progress + cancellation.** Call `ctx.SetProgress()` with real measurements when available and check `ctx.Cancelled()`
  between steps. Child processes go through `core/process.h` (`RunProcess`, quoted args, cancel flag, job-object cleanup).
* **No shell.** Never build command lines by string concatenation; pass arguments as vectors. File names are untrusted
  (spaces, quotes, Arabic, emoji, `\\?\` long paths, names starting with `-`; use `--` before paths).
* **Local only.** No network access, ever.
* **Errors are user-facing sentences** in `EngineResult::error` (English; no file contents, no raw stderr dumps).
  Warnings (lossy steps: transparency flattened, animation dropped, metadata dropped) go in `EngineResult::warnings`.
* **Detect real formats** with `DetectFile()` (`registry/filetype.h`), not the extension. Handle: corrupt file, empty file,
  wrong extension, no extension, Unicode names, long paths, read-only folders, a file that vanishes mid-run.
* **Licences.** Only permissive/LGPL libraries linked in-process; GPL tools (ffmpeg) are separate processes only. Record
  every dependency and what you verified in `docs/fragments/<name>.md`.
* **Windows API strings are UTF-16** (`std::wstring`, `fs::path`). Use `LongPath()` for `CreateFileW` & friends.

## Files you own / may touch

* `src/engines/<name>/**`, `tests/<name>/**`, `testdata/<name>/**`, `docs/fragments/<name>*.md`
* optional `cmake/engines/<name>.cmake` (extra `target_link_libraries(filo_eng_<name> ...)` etc.)
* Do **not** edit `core/`, `registry/`, `gfx/`, `CMakeLists.txt`, other engines. If you need a change there, put the request
  in your final report (or implement it additively inside your own directory).
* Exception: you *may read* anything.

## Registering

Define exactly one `void filo::Register<Name>Engine()` (declared in `engines/engines.h`) that calls
`CapabilityRegistry::Get().Add(Capability{...})` for each path. Fill in: `id` (`"png>webp"`), `group`, `in`, `out`
(canonical lower-case ids from `CanonicalExt()`), `engine`, `lossy`, `limitation`, `options` (UI sliders/choices),
`available` (runtime prerequisites), `run`, `verify` (opens the produced file for real: WIC decode, ffprobe, PDFium...).

`run(const ConvertTask&, JobContext&)` receives the detected input kind, merged options (`quality`, `crf`,
`audioBitrateKbps`, `preserveMetadata` already defaulted from Settings), and writes `task.output`.

## Shared helpers

* `core/`: `util.h` (strings, `LongPath`, `UniquePath`), `json.h`, `process.h`, `job.h`, `output.h`, `settings.h`, `log.h`
* `engines/common/ffmpeg.h`: locate ffmpeg/ffprobe, `Probe()`, `Run()` with real progress, capability queries
  (`HasEncoder("libwebp")`). ffmpeg at dev time: PATH (winget Gyan full build, GPL); in the installer: `tools\`.
* Third party in `third_party/`: PDFium (`pdfium.dll`, headers in `third_party/pdfium/include`), miniz, ZXing-cpp source,
  ExifTool (`third_party/exiftool/exiftool-13.59_64/exiftool(-k).exe` + `exiftool_files/`), LibreOffice (installed system-wide via winget,
  detect with `FindLibreOffice()` - see doc engine), Windows built-ins (WIC, Windows.Media.Ocr / FaceAnalysis through C++/WinRT headers in the SDK, `tar.exe` = bsdtar/libarchive).

## Tests

* `tests/<name>/*.cpp` become `filo_tests_<name>` (framework: `tests/test.h`, `TEST(name){ CHECK(...) }`).
* Generate fixtures programmatically (ffmpeg `lavfi`, PIL via `python -I`, WIC, hand-written bytes) into `testdata/<name>/`
  (create at run time; keep committed fixtures tiny).
* **Verify outputs by opening them with an independent decoder** (WIC / ffprobe / PDFium), not just by checking they exist.
* Every registered capability needs at least one real conversion test. Write results with `ftest` plus a machine-readable
  line per capability to `testdata/<name>/matrix-results.json` (`{"results": {"<capability id>": {"status": "pass|fail", "note": "..."}}}`).
* Build only your own targets (other engines may be mid-edit): `pwsh scripts/build.ps1 -Config Debug -Suffix _<name> -Target filo_tests_<name>`
  then run `build\Debug_<name>\bin\filo_tests_<name>.exe`.

## Fragments you must leave behind

`docs/fragments/<name>.md` with: supported paths table (in > out, engine, options, lossy?, status), known limitations (be blunt),
dependency + licence notes, how you verified, anything you could not verify.
