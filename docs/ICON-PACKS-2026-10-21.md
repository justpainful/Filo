# Icon plugins and Windows Acrylic — 0.1.4

## Available choices

Settings → Appearance contains four independent icon packs and three materials. Classic remains the default for existing preferences; all 68 original paths are unchanged.

| Pack | Visual language |
|---|---|
| Classic | Original light, rounded outlines |
| Pebble | Rounded silhouettes, quiet two-tone bodies, solid operation marks |
| Circuit | Square terminals, orthogonal routes and input/output structure |
| Facet | Angular planes, directional folds and inset marks |

Each added pack supplies all 68 existing icon keys. Shared transport, direction and editing controls retain familiar semantic conventions. Main tool and file icons have different geometry and construction. These are original vector assets, rendered by Direct2D after the glass passes, with no bitmap scaling.

## Plugin format

Bundled manifests live in `assets/icon-packs` and are copied to `icon-packs` next to Filo.exe in builds and installations. Additional manifests can be placed in `%LOCALAPPDATA%/Filo/icon-packs`; the Open packs folder button opens that location. Click Reload packs after adding or changing a file. No rebuild or executable plugin loading is required.

```json
{
  "schema": 1,
  "id": "my-pack",
  "name": "My pack",
  "description": "A short description.",
  "round": true,
  "icons": {
    "image": [
      { "path": "M4 4h16v16H4z", "fill": true, "opacity": 0.2 },
      { "path": "M4 16l5-6 5 5 3-3 3 4", "fill": false, "stroke": 1.7 }
    ]
  }
}
```

Paths use the 24×24 icon grid; allowed commands are M/L/H/V/C/S/Q/T/A/Z, absolute or relative. `fill` is required per layer, opacity defaults to 1, and stroke to 1.7. At least one layer of each glyph must be fully opaque. The foreground brush provides theme, hover and disabled colour; plugin opacity multiplies its existing opacity and is restored after drawing.

IDs are stable lowercase ASCII letters, digits and hyphens, at most 48 characters. Classic is reserved; installed IDs take precedence over duplicate user IDs. Files are limited to 512 KiB, packs to 128 known icon keys, glyphs to six layers and paths to 8192 characters. Opacity is .12–1 and stroke .5–3.5. Unknown keys, unsupported schema, malformed/nonfinite/out-of-grid paths and invalid layer values reject the pack. A missing pack or missing glyph inherits Classic. A partial pack can override a single glyph. The registry is explicitly reloaded; it performs no file polling during rendering.

`src/gfx/icons.cpp` lists the 68 canonical keys. `tools/generate_icon_packs.py` is the reproducible source authoring tool for the three bundled JSON manifests.

## Windows Acrylic

Acrylic is a distinct live material, independent from icon choice. It uses a Windows Composition host backdrop, 30 DIP Gaussian frost, slight desaturation, exclusion blending, controlled luminosity, a light colour coating and static fine grain. It has no liquid edge refraction or directional lens reflection. Sector and rounded-window masks preserve the natural background in gaps.

The first preview used a coating that was too dense. The revised material lets substantially more desktop colour through: the coating is 18% in light mode and 16% in dark mode at full glass intensity. Background luminosity is bounded in the compositor to preserve legibility instead of relying on a thick uniform overlay. Dark mode has a 0.30 per-channel pre-coating ceiling; light mode has a 0.68 luminosity floor. Glass intensity changes coating opacity independently of foreground text. Noise is static and weaker than in the first preview. Changing themes rebuilds the Acrylic effect graph; changing only the desktop does not repaint the application.

This is an Acrylic recipe implemented with Windows Composition, following the [Microsoft material ingredients](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic). It is not the whole-HWND `DWMSBT_TRANSIENTWINDOW` controller: that controller does not provide Filo's individual sector geometry. No desktop capture or bitmap refresh is part of the runtime material. Host backdrops require Windows 11 support; unavailable host composition, high contrast and reduced transparency retain the existing opaque fallback. There is no new Windows App SDK dependency.

## Verification and scope

- 29 core/settings/plugin tests; all original paths preserved.
- 2448 native Direct2D glyph draws: 4 packs × 68 icons × 3 sizes (16/24/32 DIP) × 3 DPI values (96/120/144).
- Cache isolation, selection changes, missing pack, partial external plugin discovery/removal, cache recreation and brush-opacity restoration.
- 36 visible native wheel combinations: 3 materials × 4 packs × 3 DPI values. The harness owns its synthetic background and foreground windows.
- Acrylic RGB/intensity/theme sweep, independent desktop updates, opacity fallback, resizing and simulated device reset. See `out/acrylic/contrast.json` for the final measured contrast and `out/acrylic/result.json` for frame counters.
- Actual Settings UI selected and persisted each new icon pack, reloaded packs, switched light/dark and Liquid/Soft, and restored the same saved selection after process restart. The final three-material selector is additionally rendered in `out/icon-packs/settings-final.png`.
- The original and new Classic actions/tools wheel renders are pixel-identical, and all 68 source paths match: `out/icon-packs/classic-preservation.json`.

Evidence is under `out/icon-packs` and `out/acrylic`. These checks cover the icon/material change, not all processing engines or installer install/uninstall. Physical multi-monitor DPI migration and Windows 10 native transparency are not newly qualified.


### Final Acrylic colour-response check

For warm versus cool synthetic desktops, the measured surface RGB difference increased from 27 to 115 in light mode and from 21 to 50 in dark mode after the opacity correction. The final measured minimum text contrast across the native sweep is 7.13:1 light and 5.29:1 dark. Foreground counters remain `[2,2,2,2]`, with one geometry-mask update during each background-only proof. The Acrylic wheel changes by 127 RGB levels between the warm/cool backgrounds at a point inside a sector at all three tested DPIs. Clear gap differences from the uncovered background are at most 6/3/2 levels at 96/120/144 DPI, including the intended faint contact shading. Actual warm/cool window captures are in `out/acrylic/background-response.png`.
