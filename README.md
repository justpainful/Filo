# Filo

A native Windows tray app that turns dragging a file into a set of fast actions. Drag a file in File Explorer or on the Desktop, press a modifier
key, and a glass radial wheel opens next to the cursor:

- **Shift - Convert**: images, video, audio, PDF, documents, presentations, spreadsheets, subtitles, archives. Drop on a format; a new file appears next to the original.
- **Alt - Tools**: Compress, Metadata, Edit Photo, Annotate, Add Background, Crop, Redact, Create PDF, Read QR, Collage, Join/Trim/Crop/Speed/Split/Frames/Redact/Mute video,
  Normalize/Visualizer/Trim/Channels/Bleep audio, Merge/Organize/Split PDF. Every tool has a real window.
- **Ctrl - Actions**: Share, Float, Copy, Move, Copy Path, Terminal.
- **Ctrl+Alt+D** in a browser (or **Right Ctrl on a link** dragged out of it): *Download from Link* - choose what to save (video, audio, every picture of a slideshow) and get the best quality in Downloads. [Details](docs/LINK-DOWNLOADS.md).

Everything runs locally. Originals are never modified; results are written to a temporary file, verified, then moved into place.
C++20, Win32, Direct3D/Direct2D/DirectComposition; no web view, no framework. Independent implementation inspired by the macOS app Tangerine.

| Read | |
|---|---|
| [docs/BUILD.md](docs/BUILD.md) | build, run, package |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | modules, threads, drag pipeline |
| [docs/DESIGN-SYSTEM.md](docs/DESIGN-SYSTEM.md), [docs/REFERENCE-AUDIT.md](docs/REFERENCE-AUDIT.md) | look and how it was matched to the reference |
| [docs/SHELL-INTEGRATION.md](docs/SHELL-INTEGRATION.md) | how Explorer is integrated, manual checklist |
| [docs/TOOLS.md](docs/TOOLS.md), [docs/FORMAT-MATRIX.md](docs/FORMAT-MATRIX.md) | what each tool and conversion does, and what was verified |
| [docs/TESTING.md](docs/TESTING.md), [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | tests run and numbers measured |
| [docs/KNOWN-LIMITATIONS.md](docs/KNOWN-LIMITATIONS.md) | **read this** - what is not verified or not supported |
| [docs/LICENSES.md](docs/LICENSES.md), [docs/RELEASE.md](docs/RELEASE.md) | third-party licences (FFmpeg is GPL), release steps |

UI revision **0.1.3**: Liquid Glass with live edge refraction, directional reflections and the previous **Soft Glass** material as a selectable alternative. [Review and verification](docs/LIQUID-GLASS-2026-10-21.md). Installer: `dist/Filo-Setup-0.1.3.exe`.

UI revision **0.1.4**: three independent icon plugins (**Pebble**, **Circuit**, **Facet**) alongside the preserved **Classic** set. **Windows Acrylic** joins Liquid Glass and Soft Glass. Change both independently in Settings → Appearance. [Icon plugin format and verification](docs/ICON-PACKS-2026-10-21.md). Installer: `dist/Filo-Setup-0.1.4.exe`.

UI revision **0.1.5**: responsive Float with persistent close/remove/clear controls, independent Acrylic transparency and blur, and live circular glass close buttons for tools. [Implementation and verification](docs/CONTROLS-2026-10-21.md). Installer: `dist/Filo-Setup-0.1.5.exe`.

UI revision **0.1.6**: the circular glass close button is now on every window (Settings, gallery, progress card, Float, tools). Installer: `dist/Filo-Setup-0.1.6.exe`.

UI revision **0.1.7**: **Glassy UI** is a new optional glass material (Settings > Appearance), using the glass recipe of the open-source glassyui library. Liquid Glass stays the default. [Details and limits](docs/GLASSY-UI.md). Installer: `dist/Filo-Setup-0.1.7.exe`.

UI revision **0.1.8**: **Download from Link** (TikTok, YouTube, X, Instagram and more; slideshows, photos, audio). [Details and limits](docs/LINK-DOWNLOADS.md).

UI revision **0.1.9**: **Symbolic Glass** (the glass of the owner's Symbolic app) and the original **Symbolic Style** icon pack are the new defaults; every earlier material and pack stays selectable in Settings > Appearance. [Details and licence notes](docs/SYMBOLIC-GLASS.md).

UI revision **0.2.3**: the **Symbolic theme**: new tokens, capsule buttons, sliding-lens segmented controls, pop-up steppers, icons on every button, settings row and tray menu item, grouped Settings. [Details](docs/SYMBOLIC-THEME.md).
