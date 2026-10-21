# Filo 0.1.3 — Liquid Glass and the preserved Soft Glass material

The new default is **Liquid Glass**. The previous 0.1.2 rendering path remains selectable as **Soft Glass** under **Settings → Appearance → Glass style**. Reflection strength, edge refraction and overall glass intensity are separate controls. The 0.1.2 installer is retained, and its source/executable baseline is preserved in `out/liquid-glass/baseline-0.1.2`.

## Material

The wheel has a milky reading surface, a clearer rolled lip, directional white highlights, a shaded opposing edge, a broad quiet sheen, contact shadows and a narrow outer glass rim. Highlights respond to hover over 160–180 ms without following the cursor like a spotlight. Light and dark materials use different coatings. The file preview, labels and icons are drawn after the optical passes and remain sharp. Dialogs use a quieter coating and directional rim; their image previews keep their own pixels.

Liquid Glass blurs only the sectors, centre and outer glass rim. The intervening gaps reveal the desktop, with a small contact shadow. Soft Glass retains its earlier circular backdrop, tint, bevel and motion values.

The reference is the supplied image and the official [conversion wheel](https://tangerineformac.com/press/screenshots/conversion-wheel.png) and [tools wheel](https://tangerineformac.com/press/screenshots/advanced-tools.png). This revision approximates the material and depth on Windows; it does not claim exact macOS rendering or physical ray tracing.

## Live optical path

The source is the public Windows host backdrop. Each of five compositor `LayerVisual` instances flattens a live backdrop child, applies a small affine displacement, and masks the result with an HLSL field derived from the same rounded sector geometry used by rendering and hit testing. Conditional source-over weights preserve the desired mixture and premultiplied coverage. The centre has no displacement; displacement increases only within the narrow rolled edge, with a production maximum of 2.4 pixels at 96 DPI, scaled with DPI.

This layering matters: directly transforming a host backdrop cancelled the intended displacement on this PC. Rendering it through `CompositionVisualSurface` displaced it but froze background updates. Neither experimental path is used by the delivered material. The accepted `LayerVisual` path demonstrates both real displacement and independent background updates. No runtime desktop capture, capture-exclusion flag, screen readback, background-refresh timer or persistent composition animation is used.

Geometry masks regenerate only for geometry/material changes, not background changes. Windows updates the background itself. The native harness records unchanged foreground frame and mask-update counters while warm and cool backgrounds change. Reduced transparency and high contrast disable live glass. Device recovery rebuilds the source, masks and compositor layers.

## Native verification

```powershell
pwsh scripts/build.ps1 -Config Release -Suffix _rel
build/Release_rel/bin/filo_tests.exe
build/Release_rel/bin/filo_e2e_live.exe out/liquid-glass/liquid
python tools/review_live.py out/liquid-glass/liquid
build/Release_rel/bin/filo_e2e_live.exe out/liquid-glass/soft --soft
python tools/review_live.py out/liquid-glass/soft
python tools/review_liquid.py out/liquid-glass
```

The test owns the real foreground and background HWNDs. PNGs show the native composed result, not an image simulation. Verification covers:

- 162 RGB/intensity/theme samples per material; 324 in total. Active text/material contrast is checked against measured pixels. This does not certify every possible disabled control or image overlay.
- Real displacement with the coating removed for calibration, including an exaggerated **24-pixel diagnostic only**. Production remains at 2.4 pixels. Red/blue scene changes verify that this exact source continues updating live.
- Reflection-off and refraction-off comparisons, unchanged opaque text, natural gaps, and restoration after switching to Soft Glass and back.
- Native rendering with forced 96, 120 and 144 DPI geometry paths. This is not a claim of physical mixed-monitor transition testing.
- Reduced transparency, resizing, simulated graphics-device reset, and unchanged foreground/mask counters while the background changes.

Evidence is in `out/liquid-glass/liquid`, `soft`, `optical-verification.json`, `materials-comparison.png` and `live-liquid-proof.gif`. Core/settings tests are in `core-tests.txt`. The settings regression checks migration of existing preferences, both materials, independent strengths, round trips and invalid-value bounds.

## Delivery and limits

The installer is `dist/Filo-Setup-0.1.3.exe`. Packaging uses the version-specific `build/stage-0.1.3` and refuses to clean a staging directory with a running executable. The already-running 0.1.2 executable in `build/stage` is preserved, with its support files restored and its SHA-256 checked against the saved baseline.

Windows 11 build 22000+ is required for host backdrops; the native tests run on build 26200. Compositor/API failure falls back to opaque material. Installer installation/uninstallation and physical mixed-monitor DPI transitions remain unqualified. Existing product limitations in KNOWN-LIMITATIONS.md still apply.

API references: [LayerVisual effects](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.layervisual.effect), [host backdrop](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createhostbackdropbrush), [affine transforms](https://learn.microsoft.com/en-us/windows/win32/direct2d/2d-affine-transform).


## Observed results

All native pixel checks passed, including live refraction, 96/120/144 DPI geometry, unchanged sharp foreground ink, natural gaps, idle frame/mask counters and both material switches. The 324 measured colour/intensity/theme samples passed. Liquid Glass minimum active-text contrast measured 8.460:1 in light mode and 5.538:1 in dark mode; the retained Soft Glass values remained 7.106:1 and 4.654:1. These readings cover the sampled material/palette pair, not every possible control state.

The actual Settings window was inspected and both style buttons were clicked. The selected style persisted and other preferences remained unchanged. Core/settings tests: 26 passed, zero failures. The full MSVC Release build and installer compile succeeded. The versioned staged executable matches the verified Release SHA-256. Exact hashes and machine-readable evidence are in out/liquid-glass/release-manifest.json.


Liquid Glass selected-sector text was also measured over a white background: **4.619:1** after the blue hover tint adjustment. This hover change is restricted to Liquid Glass; Soft Glass retains its previous appearance.
