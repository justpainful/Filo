# Image engine (`src/engines/image`)

Entry point `filo::RegisterImageEngine()`. Public headers (all under `engines/image/`):

| Header | What it is |
|---|---|
| `bitmap.h` | `Bitmap` (**8-bit BGRA, straight alpha**, top-down, stride = width*4), `LoadBitmap` / `LoadBitmapFromMemory`, `SaveBitmap`, `ResizeBitmap`, `ConvertToSRGB`, `ApplyOrientation`, `Premultiply/Unpremultiply`, `CanEncode/CanDecode/FormatSupportsAlpha`, `ReadFileBytes/WriteFileBytes`, `Scratch` |
| `image_convert.h` | `ConvertImageFile(in, out, outFormat, ConvertOptions, ctx)` - what every registered capability runs |
| `image_compress.h` | `CompressImage(CompressRequest, JobContext&)`, `EstimateCompressedSize(...)` (a tool, not a capability) |
| `qr.h` | `ReadQrCodes(path)` -> `QrResult{text, format, corners[4], isUrl, url, kind}` |
| `pdf_writer.h` | `WritePdf(out, pages, opts)` (multi-page, JPEG pass-through, Flate+SMask) |
| `png_writer.h` | own PNG encoder (auto gray/RGB/RGBA/exact palette, lossy median-cut palette, eXIf/iTXt/iCCP) |
| `imgmeta.h` | container parsing and EXIF/XMP/ICC surgery for JPEG, PNG, WebP, HEIF/AVIF; generated sRGB ICC profile |

## Supported paths (73 capabilities, all runnable and tested on this machine)

`in > out` for in, out in {jpg, png, webp, heic, tiff, avif, bmp, gif} (56 paths), `<raster> > pdf` (8), `svg > <raster>` (8), `svg > pdf` (1).

| Output | Encoder | Options | Lossy | Alpha | EXIF/XMP | ICC |
|---|---|---|---|---|---|---|
| jpg | WIC JPEG (gray images -> 8-bit gray JPEG; 4:4:4 at quality >= 90) | quality, preserveMetadata | yes | flattened on white + warning | yes (APP1 injected) | sRGB tag if source had a profile |
| png | own encoder (miniz) | preserveMetadata | no | yes | yes (eXIf, iTXt XMP) | sRGB iCCP if source had a profile |
| webp | ffmpeg `libwebp` (`libwebp_anim` for GIF<->WebP) | quality, lossless, preserveMetadata | yes / lossless | yes | yes (VP8X + EXIF/XMP/ICCP chunks written by us) | yes |
| avif | ffmpeg `libaom-av1` + avif muxer; alpha as 2nd stream; BT.709 full-range tagged | quality, lossless (true RGB lossless, verified bit-exact) | yes / lossless | yes | **no** (warning) | no (pixels are sRGB) |
| heic | WIC HEIF encoder (HEVC extension) | quality, lossless, preserveMetadata | yes / lossless | flattened + warning | EXIF tags replayed through the WIC metadata writer | no |
| tiff | WIC (LZW, Deflate in Compress) | preserveMetadata | no | yes | EXIF tags replayed | yes |
| bmp | WIC (24-bit) | - | no | flattened + warning | no (warning) | no |
| gif | own median-cut + Floyd-Steinberg palette, WIC GIF writer | - | yes (256 colours) | on/off (warning when soft edges are reduced) | no (warning) | no |
| pdf | own writer | - | no | SMask | no (warning) | JPEG's ICC kept as /ICCBased |

Behaviour that applies to every path: format is detected from content (`DetectFile`/`ClassifyBytes`), never the extension; EXIF orientation is baked into the pixels and the written EXIF orientation is reset to 1 (PixelXDimension/YDimension are patched, the IFD1 thumbnail is unlinked); embedded RGB ICC profiles are converted to sRGB with `IWICColorTransform` and an sRGB profile (generated in code, `SrgbIccProfile()`) is embedded where the target allows; dimensions are never changed; animated GIF<->WebP keep every frame, delay, transparency and loop count (ffmpeg), every other target gets frame 0 plus "Animation was reduced to the first frame"; multi-page TIFF converts page 0 plus a warning; truncated JPEG/PNG/WebP/BMP files are rejected ("incomplete or damaged") instead of silently producing a half-grey picture.

Inputs: JPG/PNG/GIF/BMP/TIFF via WIC (CMYK JPEGs through the CMYK profile when present, 16-bit sources to 8-bit); WebP via WIC (animated WebP via ffmpeg); **AVIF via ffmpeg first** (libdav1d: correct matrix/range tagging, alpha via `alphamerge`, `irot`) with WIC as fallback (WIC drops AVIF alpha and mis-decodes some matrices); HEIC via WIC first (tiles/grids composed by the OS, `irot` applied) with ffmpeg as fallback; the decoded size is checked against the container's `ispe`/`clap` so a single tile is never returned as "the image"; SVG via Direct2D (`ID2D1DeviceContext5::CreateSvgDocument`).

### Image -> PDF
Own writer (`pdf_writer.cpp`): page = image at 96 dpi -> 72 pt (pt = px * 0.75, capped at the 14400 pt PDF limit by scaling the mapping, never the pixels). JPEG input is embedded **untouched** (DCTDecode, byte-identical - verified with PyMuPDF), EXIF orientation is applied through the content-stream matrix, an embedded RGB/Gray ICC profile becomes `/ICCBased`. Everything else (and CMYK/arithmetic JPEGs) is Flate with PNG predictors; alpha becomes an SMask. SVG -> PDF is **rasterised** (3072 px long side, page size = intrinsic size), not vector.

### Compress (`image_compress.h`)
Modes `TargetBytes`, `PercentReduction`, `Preset` (Low/Medium/High); optional `maxSide` / `resizePercent`; keeps the input format unless `outFormat` is set (jpg png webp avif heic tiff). Lossy formats: binary search over quality (floor `minQuality`, default 35), then - only if `allowDownscale` - dimensions are reduced by `sqrt(target/size)` steps (aspect ratio kept, never upscales). PNG: lossless first (level 9, exact palette when <= 256 colours), then 256..16-colour dithered palettes, then downscale. TIFF is lossless (Deflate) so only resizing helps. If the result is not smaller than the input, `CompressResult::notSmaller` is set and **no output file is written**. `targetMet=false` + warning when the target cannot be reached. The ICC profile is always kept (pixels stay in their own colour space); EXIF/XMP follow `preserveMetadata`. `EstimateCompressedSize` really encodes images up to 1.3 MP and extrapolates from a full-resolution 1024x1024 crop for larger ones (measured 0.89 - 0.98 of the real size on the test photos, 0.04 - 0.5 s). BMP and GIF are refused with a message that names the formats to choose instead; animated inputs are refused.

### QR (`qr.h`)
ZXing-cpp 3.1.1 (`third_party/zxing/zxing-cpp-zxing-cpp-287c85d`, **Apache-2.0**) built by `cmake/engines/image.cmake` as a static library from `core/` only: readers on, writers off, C API off, only the QR-code family enabled (QR / Micro QR / rMQR), no examples/tests/python. Multiple codes per image, rotated/mirrored/inverted/transparent/JPEG/SVG inputs, EXIF orientation honoured, corners are returned in the upright image's pixel coordinates. `isUrl` is only a classification (http/https/ftp or bare `www.`); **nothing is ever opened** (a test asserts the module contains no ShellExecute/CreateProcess).

## Dependencies and licences
* WIC, Direct2D (SVG), D3D11/WARP: Windows components (no redistribution).
* ffmpeg (GPL build, separate process only): libwebp, libwebp_anim, libaom-av1, libdav1d, avif muxer. Dev machine: Gyan 9.0 full build.
* miniz (MIT) - deflate/CRC; PDFium (BSD-3/Apache-2) - only used in `verify` and the tests; ZXing-cpp (Apache-2.0, includes libzueci, MIT-style).
* Tests only (not shipped): Python 3.12 with Pillow 12 (HPND; AVIF via libavif is an *independent* encoder/decoder), OpenCV (Apache-2.0, QR *encoder* for fixtures), numpy, **PyMuPDF (AGPL-3.0 - dev machine only, never linked or shipped)**.

## How it was verified
`build\Debug_image\bin\filo_tests_image.exe` - 64 tests, 0 failures (about 3.5 min in Debug, 2.7 min in Release; most of that is python/ffmpeg start-up and AV1 encodes). Fixtures are generated at run time into `testdata/image/src` by `tests/image/gen_fixtures.py` (PIL, OpenCV, hand-written ICC and ISO-BMFF bytes) and WIC. `testdata/image/matrix-results.json` has one entry per capability (73 pass).
* Every capability converts a real file through `JobManager -> RunConversion` (temp file, verify, atomic commit) and the output is opened by an independent decoder: PIL for jpg/png/webp/tiff/bmp/gif/avif, WIC for heic, PDFium (render + page size) for pdf. Checked: dimensions, orientation (the JPEG source is stored rotated with EXIF orientation 6), quadrant colours, alpha preservation/flattening, flatten warning.
* All 8 EXIF orientations -> upright output (jpg->png) and upright PDF pages; ICC Adobe-like profile -> sRGB within 1 level of lcms2; CMYK JPEG; animated GIF<->WebP (frame count, per-frame delays via ffprobe/PIL, transparency, loop); metadata round trip (Make, DateTimeOriginal, XMP, PixelX/YDimension patched, orientation 1) for png/webp/tiff/heic; lossless webp/avif/heic are bit-exact; odd sizes (321x241), 1x1, 4000x30; 12 MP JPEG; corrupt/truncated/empty/junk files; PNG named .jpg; Arabic/space/emoji/quote names; > 260 character paths; unicode names reaching ffmpeg (staged copy); cancellation leaves nothing behind. The container parsers (`imgmeta.cpp`) were also fuzzed with AddressSanitizer outside the repo (60,000 truncated/bit-flipped copies of 10 fixtures: clean); `parsers_and_loader_survive_damaged_files` repeats a smaller version in the suite. The container parsers (`imgmeta.cpp`) were also fuzzed with AddressSanitizer outside the repo (60,000 truncated/bit-flipped copies of 10 fixtures: clean), and `parsers_and_loader_survive_damaged_files` repeats a smaller version in the test suite.

## Known limitations (be blunt)
* **Windows' HEIF encoder + decoder disagree on large pictures.** For images that the encoder stores as a tile grid (observed from about 2000 px wide; 1000x750 is a single item) a lossy HEIC written by WIC decodes through WIC *and* ffmpeg with a colour cast (pure red 255 -> 232, pure green -> (19,255,6)); ffmpeg with an explicit BT.709/full-range decode returns the right colours, i.e. the stream is fine but the grid carries no usable colour signalling. Lossless HEIC (`lossless=true`) is exact. We cannot fix the OS encoder (adding a `colr nclx` box did not change WIC's or ffmpeg's decoding, so it was not shipped). Photos are visually fine; saturated flat colours shift.
* HEIC output has no alpha (WIC HEIF accepts only 24-bit BGR) and needs HEIF >= 8x8 px; EXIF is limited to what the WIC metadata writer accepts. AVIF output cannot carry EXIF/XMP (ffmpeg's avif muxer); a warning says so.
* **Direct2D's SVG renderer has no `<text>`** - text in an SVG is missing from the raster (a warning is added); `<foreignObject>` likewise. External references are never fetched.
* GIF output is 256-colour with on/off transparency; animated sources only keep animation for GIF<->WebP (APNG is treated as a still PNG; animated AVIF/HEIC sequences give frame 0).
* HDR / 10-bit AVIF and HEIC are tone-mapped by nothing: they are decoded to 8-bit sRGB-ish values. nclx primaries (e.g. Display P3 without an ICC profile) are not converted.
* Last-frame delay of GIF -> animated WebP is ffmpeg's (the final frame gets the previous frame's duration).
* `ff::HasMuxer()` in `engines/common/ffmpeg.cpp` is always false (see report); the image engine uses its own `FfHasMuxer()` (`ffmpeg -h muxer=...`).
* `OutputTransaction::Commit` uses plain `std::filesystem` calls, which fail for paths > 260 characters unless the OS has `LongPathsEnabled` (registry-level, lead's file). The engine itself handles long paths (inputs via the registry path, outputs via `ConvertImageFile`).
* Not verified: real iPhone/Samsung HEIC files (none on this machine; verified with WIC-written HEIC including a 4000x3000 tile grid and `irot`), AVIF `irot` is verified with a hand-built file, Debug build timings only (12 MP JPEG -> PNG 8 s, -> TIFF 15 s in Debug; Release not measured), behaviour on a machine without the HEVC/AV1 extensions (capabilities report `Runnable()==false` from runtime probes, not exercised here).
