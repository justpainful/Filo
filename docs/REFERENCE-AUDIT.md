# Reference audit

The reference material is the macOS app Tangerine (https://tangerineformac.com): its marketing screenshots (2x retina, so dip = px / 2) in
`reference\`, its product video, and its icon. Filo is an independent Windows implementation; no code, assets or text from Tangerine are used.
Screenshots were used only to **measure** geometry, colour and layout.

## Inventory

| File | Shows | Used for |
|---|---|---|
| `conversion-wheel.png` | the Convert wheel over a Finder window, 8 sectors (JPG WEBP HEIC TIFF AVIF BMP PDF DOCX) around a centre pill (thumbnail + "2.2 MB") | wheel geometry, glass alphas, sector order and labels |
| `advanced-tools.png` | the Tools wheel for an image: Compress, Metadata, Edit, Annotate, Add BG, Crop, Redact (clockwise from 12), upper-case bold letter-spaced labels, file-name chip under the centre | tools order, label style, icon set |
| `edit-photo.png`, `add-background.png` | two tool windows | dialog chrome, glass lift, palette, slider/swatch metrics |
| `audio-tools.png` | Trim Audio, Convert Audio Channels, Bleep Audio | audio tool layouts, waveform styling |
| `backdrop-*.png` | **derived by us**: the screenshots with the window removed and the hole filled (push-pull inpainting, `tools/make_backdrop.py`) | a stand-in desktop so renders can be compared pixel-wise |
| `tangerine-demo.mp4`, `tangerine-app-icon.png` | the product video (22.5 s: drag a MOV, Shift -> Convert wheel, "Converting to MP4" card with progress bar, Option+Shift -> video Tools wheel, Crop Video dialog, "Cropping video" card) and icon | behaviour (drag, hold a modifier, wheel opens near the cursor), the **progress card** (close button at the left, bold title, file name, thin progress bar) that became `ProgressHud` |
| (screenshot of the product's X post) | Float: a fanned stack of file thumbnails with a paper clip that opens into a tall glass list, and collapses to a small pill | Float design |

No reference exists for: Join Videos, the video tools, Crop, Redact, Annotate, Collage, QR, PDF tools, Settings. Those use the same system
(tokens, glass, controls) and follow the layouts' conventions but are **not** pixel-matched to anything.

## What was matched, with numbers

- Wheel: sector outer/inner radii, gap angle, centre pill size, sector fill alpha at both radii, rim and hole fills, fitted on the reference pixels
  (see the `[M]` tags in `tokens.h`). The fitted values were checked by rendering the wheel on the reference backdrop and comparing sampled regions
  (rim/gap pixels, sector tops) with `tools/region_stats.py` / `tools/compare.py`; it is a close match in those regions, not a pixel-identical copy.
- Historical dialog glass: the +0.205 fit was limited to sampled marketing-wallpaper colours. It failed on the dark desktop in the reported screenshot and has been replaced by a contrast-bounded frosted material. The previous sample error must not be interpreted as overall fidelity.
- Dialog chrome metrics: title bar 54.5, close button 32, content inset 16.5, radius 26, well radius 24, row 30, footer 58.
- Edit Photo and Add Background were rebuilt after the first attempt was far from the original; side-by-side comparisons are in `out/edit-sbs.png` and
  `out/addbg-sbs.png`. These historical comparisons predate the October 21 material correction; they do not validate the current build. See the current UI review for remaining differences.

## Deliberate differences from the reference

- The video Tools wheel in the demo shows eight sectors (Compress, Remove Meta, Mute, Trim, Crop, Speed, Snapshot, Split). Filo's video wheel has more (Metadata, Frames, Redact, plus Join and Normalize when applicable) and uses the labels Metadata / Frames instead of Remove Meta / Snapshot.

- Accent colour: blue (`#3B8BFF`) instead of orange, at the project owner's request.
- The macOS app's tools for non-image files and several Windows-specific additions (Collage, Read QR Code, Create PDF, PDF tools) add sectors to some
  Tools wheels, so a single image shows more than the reference's 7.
- Conversion wheel: "DOCX" from an image is offered (one page holding the picture); "Pages" outputs are not (undocumented format).
- Fonts: Segoe UI Variable instead of SF Pro. Icons are our own vector icons, not SF Symbols.
- Windows-native behaviour where macOS differs: the wheel is a topmost no-activate window; Float is a normal always-on-top window.

## Not verifiable from screenshots

Animation timing and easing (taken from the video by eye, not measured), the exact blur radius of the wheel's backdrop, and behaviour of the real app
in situations the material does not show.

## Re-audit, 21 October 2026

Reopened the official product site and press page and visually inspected the local official Conversion Wheel and Edit Photo screenshots. The linked ChatGPT conversation exposed message references but no usable reference image bodies; no additional visual conclusions were inferred from that conversation.

The supplied Filo metadata screenshot shows dark text over a medium-grey body and saturated icons bleeding through behind the controls. This is reproduced by the previous executable with a deterministic dark-desktop test image. The fix uses stronger frosting and a bounded light/dark surface, stable text colours, larger default wheel geometry, quieter bevels and clear control states. These changes are verified against dark, white and coloured backgrounds. They improve readability; they are not a claim of pixel-identical macOS material rendering.

Join Videos still has no supplied original screenshot in this workspace; its layout is reviewed for spacing and readability only.
