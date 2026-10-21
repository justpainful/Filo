# Media engine (video + audio)

Source: `src/engines/media/**`. Everything runs `ffmpeg` / `ffprobe` as **separate processes** (never linked). Entry: `filo::RegisterMediaEngine()`.

## Supported conversion paths (116 registered, all runnable here, all `pass` in `testdata/media/matrix-results.json`)

| In | Out | Engine / codecs | Lossy | Notes |
|---|---|---|---|---|
| mp4, mov, mkv, webm, avi, wmv | each other (5 each) | ffmpeg | yes | mp4/mov/mkv: H.264 + AAC; webm: VP9 (libvpx, row-mt) + Opus; avi: MPEG-4 (xvid tag) + MP3; wmv: WMV2 + WMAv2 (ASF) |
| mp4, mov, mkv, webm, avi, wmv | gif | palettegen/paletteuse, 2 passes | yes | options gifFps/gifWidth/gifLoop; no sound |
| gif | mp4, mov, mkv, webm, avi, wmv | ffmpeg | yes | registered with group Image (input is an image by content); overrides any image-engine registration of the same id |
| the 6 video containers | mp3, m4a, wav | ffmpeg | mp3/m4a | first audio track |
| mp3, m4a, wav, flac, ogg, opus, aiff, wma | each other (7 each) | libmp3lame, aac/alac, PCM, flac, libvorbis, libopus, pcm_s16be/s24be, wmav2 | lossy targets | flac/wav/aiff chains are bit-exact (tested by PCM md5) |

Options (OptionSpecs): `crf`, `audioBitrateKbps`, `codec` (h264/hevc/av1 for mp4/mkv, vp9/av1 for webm/mkv, mpeg4/h264 for avi), `hardwareAcceleration` (NVENC only if listed AND a 1 s trial encode succeeds, cached), `copyStreams` (remux when codecs fit the target, verified/warned), `preserveMetadata`, `sampleRate`, `channels`, `bitDepth`, `lossless` (m4a = ALAC), `gifFps`, `gifWidth`, `gifLoop`.

Behaviour: autorotate bakes rotation metadata into the pixels (copy mode keeps the metadata); odd sizes are cropped by one pixel to even (warning); VFR kept for mp4/mov/mkv/webm, converted to CFR at the average rate for avi/wmv (warning); every audio track kept (all six containers accept several); text subtitles -> mov_text (mp4/mov), srt/ass copied (mkv), WebVTT (webm), dropped with a warning for avi/wmv/gif; cover art kept for mp3/m4a/flac, dropped with a warning for wav/ogg/opus/aiff/wma (ffmpeg would otherwise write a bogus video stream); HDR input gets a "no tone mapping" warning; mp3/wma mix >2 channels down (warning); sample rates the codec cannot take are mapped to the nearest legal one.

## Operation APIs (`media_ops.h`, `media_info.h`)
Compress (preset CRF / two-pass target size / percent, `notSmaller` keeps nothing), Trim video (frame accurate re-encode, or keyframe copy that reports the real start), Trim audio + silence detection, Crop (+ aspect presets), Speed (atempo chain / pitch change / frame blending), Join (concat filter, first clip's frame), Split, Save frames / ExtractFrame / FrameScrubber, Redact (time-windowed blur / solid), Remove audio (stream copy), Normalise (EBU R128 two-pass loudnorm, re-measured, one corrective retry for lossy encoders), Audio visualiser, Channels, Bleep, Waveform (+ per channel), audio preview wav.

## Known limitations (blunt)
* No persistent decoder process: `FrameScrubber` caches the probe and the last 24 frames, but every new frame is a short ffmpeg process (~100 ms incl. keyframe seek on a laptop-class CPU).
* Compress target-size mode uses two-pass ABR (H.264, VP9, MPEG-4, WMV2); it does not try HEVC/AV1 targets. Result typically lands within 3 % under the target; no CRF search.
* Normalise processes the first audio track only (others are dropped with a warning on video inputs).
* Redact: coordinates are rounded outwards to even pixels; regions below 8x8 are refused. Blur is a box blur (not a pixelate).
* WMV output is the old WMV2 codec (soft picture, big files) - chosen for compatibility; AVI defaults to MPEG-4 part 2.
* FLAC "copy" trim is a lossless re-encode (stream copy would keep a wrong STREAMINFO length).
* ASF/WMA copy-trim reports a slightly longer duration (container preroll).
* Hardware encoding is only used for single-pass CRF-style work (convert, trim, crop...), never for two-pass target-size compression.
* `gif -> video` is registered here (group Image).
* Not verified: playback in Windows Media Player / MPC of every produced container (only ffprobe + a full ffmpeg decode were used); behaviour on non-NVIDIA GPUs (AMF/QSV never used).

## Dependencies and licences
* **FFmpeg 9.0 (Gyan "full" build, `--enable-gpl --enable-version3`)** - GPL; used only as a separate executable (`ffmpeg.exe`, `ffprobe.exe`) found by `ff::FfmpegPath()`. Filo's own code is not linked to it. It bundles x264 (GPL), x265 (GPL), libvpx (BSD), SVT-AV1 / libaom (BSD), libmp3lame (LGPL), libopus (BSD), libvorbis (BSD), libxvid (GPL), etc.: ship the Gyan licence text and offer the source next to the binaries in the installer.
* No other third-party code. Tests additionally use Python 3 + Pillow + NumPy (`python -I`) only as an independent pixel reader; not shipped.
* Windows HEVC/AV1/H.264 NVENC use the installed NVIDIA driver through ffmpeg; nothing is redistributed.

## How it was verified (all run, see `tests/media`)
`filo_tests_media` (37 tests): every registered capability converted through `RunConversion` inside a `JobManager` job and checked with ffprobe + a full null decode (codec, size, duration, channels, tone frequencies of the L/R channels, PCM md5 for lossless chains); special cases (rotation, VFR, odd size, 2 audio tracks, subtitles, cover art/tags, HDR, 5.1, 96 kHz, 8 kHz, tiny clips); a 189-run audio and 72-run video parameter sweep; all operation APIs, also on every container/audio format; frame count / pixel comparisons for trim, crop, split, redact (solid + blur, only inside the time window), join (size, fps, duration, silence for clips without audio, letterboxing of a rotated clip); independent EBU R128 re-measurement (ffmpeg ebur128) within 0.5 LU; bleep (outside region md5-identical); corrupt / 0-byte / text-as-mp4 / wrong extension / no extension / Arabic, emoji, `-leading`, quote and 360-character paths; cancellation mid-encode (stops in < 0.2 s, no `.filo-*` leftovers, nothing at the target path); input hashes unchanged.
