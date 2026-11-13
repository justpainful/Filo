# Link downloads (0.1.8)

Drag a link out of the browser (the address bar, a link on a page, a tab), press **Right Ctrl** while you hold it, and drop it on the small
glass card that appears under the cursor. A **Download from Link** window opens and shows what the post contains; you choose what to
save and press *Download*. TikTok, YouTube, X, Instagram, Facebook, Reddit, Vimeo, Pinterest and everything else yt-dlp understands.
**Easier than dragging:** press **Ctrl+Alt+D** (Settings > Wheels > *Download the page open in the browser*). Filo uses the link you copied in the last 30 seconds,
otherwise the address of the page open in the browser in front (read through UI Automation; nothing is typed or clicked), otherwise the clipboard.
So: open the post, press Ctrl+Alt+D. Or right-click a link > *Copy link address*, then Ctrl+Alt+D.
The same window opens from the tray menu (*Download from link...*, uses the link on the clipboard) and from `Filo.exe --download <url>`.

## What can be saved

| Post | Items shown |
|---|---|
| a video (TikTok, X, YouTube, ...) | the video, plus an **Audio only** choice (original codec, nothing re-encoded) |
| a TikTok slideshow | every picture (tick any), and the slideshow's sound as a separate item |
| an X post | every photo and every video of the post |
| several videos in one post | each one, plus audio of the chosen ones |
| an audio track or album (for example Bandcamp) | tracks available to the downloader, selectable individually |
| a supported playlist | up to the first 80 entries, selectable individually |

Pictures are fetched in their original resolution (TikTok: the largest JPEG; X: `name=orig`). Videos use the best resolution the site serves, then
frame rate, then bitrate (*Best available*), or H.264 + AAC in MP4 (*Most compatible*, in the window and in Settings > Files).
The audio format selector offers the original format, MP3, M4A, and WAV; conversion requires FFmpeg. Sites may restrict individual items or require authentication. Playlist probing can take longer, especially for large collections, and only the first 80 entries are shown.
Video format can be set to Auto, MP4, MKV, or WebM. MP4 and WebM require suitable streams from the site; MKV remuxes the streams without re-encoding. Choosing a container does not guarantee that the source provides compatible streams.

## Where files go and how they are named

* Folder: the Windows **Downloads** folder (change it in the window or in Settings > Files). Nothing is written there until a download finishes:
  files are first fetched into a scratch folder and moved in under their final names.
* Names: `Filo_<Platform>_<Author>_<Id>` plus a suffix, e.g. `Filo_TikTok_cin0matic_8365640980_1080p.mp4`, `Filo_X_nitishkmrk_9964042240_960p.mp4`,
  `Filo_YouTube_Multiicord_6Eu2wkEc5xM_audio.opus`, `Filo_TikTok_someone_0000000001_07.jpg`. The id is the last 10 digits of a numeric post id
  (the whole id otherwise), so a name is unique per post; if the file exists anyway a ` (1)` is added.
* **Folders**: when a download produces **4 files or more** (setting *Use a folder from*, 0 = never) the files go into their own folder
  `Filo_<Platform>_<Author>_<Id>` inside Downloads, so a 30-picture slideshow does not scatter.

## How it works

* `src/engines/web` - link parsing, naming, probe-JSON parsing (`web.cpp`, unit-tested without a network) and the yt-dlp runner (`ytdlp.cpp`).
  yt-dlp runs as a separate process: `-J --no-playlist --flat-playlist` to probe, then one run per video/audio item; pictures and slideshow sound
  are fetched directly with WinHTTP (`src/core/http.cpp`).
* `assets/yt-dlp-plugins/filo` - a yt-dlp plugin. Upstream skips photos, so it adds the pictures of TikTok slideshows and X posts as playlist
  entries marked `_filo_kind: image`. Everything that is not a photo post behaves like upstream.
* `src/ui/link_zone.*` - the drop target card; `src/shell/drag_session.*` decides when it opens (Right Ctrl, real OLE drag, press did **not** start
  in File Explorer / the Desktop); `src/tools/download_link.cpp` - the window.
* Right Ctrl on files dragged from Explorer still opens the Actions wheel; Left Ctrl is unchanged.
* YouTube's best formats need a JavaScript runtime: Filo passes `--js-runtimes node` when `node.exe` is on `PATH`.
* Tools: `tools\yt-dlp.exe`, `tools\yt-dlp-plugins\`, `tools\ffmpeg.exe` next to `Filo.exe` (development builds find yt-dlp on `PATH` and the plugin in
  `assets\`). `scripts\package.ps1` stages them (`-YtDlpExe` if the standalone exe is not found automatically).

## Whole accounts and channels (0.3.1)

Everything a profile or channel has posted can be saved in one go. Paste (or drop) the address of an account, a channel or a playlist in the **Download from Link** window:
Filo lists every post (with a live count and *Use what was found* for huge accounts), then you choose how many of the latest to take, what to save (videos, pictures, post text),
the quality, an optional sign-in (Firefox / Chrome / Edge / cookies.txt) and press *Download*. The same thing from the command line:

```powershell
filo-cli account https://www.tiktok.com/@nasa            [--limit N] [--out dir] [--list]
filo-cli account https://www.youtube.com/@NASA            # Videos, Shorts and Live tabs
filo-cli account https://x.com/NASA                       # every post, with its full text
        [--no-videos] [--no-pictures] [--no-text] [--redo] [--parallel 1..4] [--reposts] [--cookies firefox|chrome|edge|cookies.txt]
```

* Recognised: TikTok `@name`, YouTube `@handle` / `/channel/` / `/c/` / `/user/` / playlists, X `name`, Instagram, Twitch, Vimeo, SoundCloud, Bandcamp, Bilibili, Dailymotion, Rumble.
* Everything lands in `Downloads\Filo_<Platform>_<name>\`. Each post is saved as its media files plus a `.txt` with the **full post text**, author, address, date and counts
  (UTF-8; line breaks kept). A post with no picture or video (a tweet) is saved as the text alone.
* Running it again only fetches new posts: saved ids are remembered in `.filo-saved.txt` inside the account folder (`--redo` ignores it). Several posts download at once (default 3).
* **X** has no public timeline for visitors, so Filo's yt-dlp plugin (`filo_twitter_user.py`) reads it from the public FixTweet API (`api.fxtwitter.com`). Only the account handle is sent there,
  no sign-in is needed, and reposts are left out unless `--reposts`. The service sometimes answers slowly or says 404 for a moment; Filo asks again.
* Instagram and other sites that hide accounts from visitors need a sign-in: `--cookies firefox` (or chrome / edge, or a `cookies.txt` file).
* Single posts: the window has *Also save the post text*; `filo-cli download <url> --text` does the same. A tweet with no media is offered as *Text only*.

## Headless checks

```powershell
filo-cli probe <url>                                   # what the link holds (JSON)
filo-cli download <url> [--out dir] [--items 0,1|all] [--audio|--audio-only] [--audio-format original|mp3|m4a|wav] [--video-format auto|mp4|mkv|webm] [--compat] [--folder N]
Filo.exe --download-shot out.png fixture.json [--dark] # the window, hidden, from a yt-dlp JSON file
Filo.exe --link-zone-shot out.png [0|1|2]              # the drop card (waiting / link over it / not a link)
```
Tests: `filo_tests_web` (platforms, address extraction, names, JSON parsing) and the link-zone cases in `filo_tests` (`drag_*link*`).

## Limits

See KNOWN-LIMITATIONS.md ("Link downloads"). Only content you are allowed to save should be downloaded; sites' terms apply.
