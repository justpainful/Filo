# Glassy UI material (0.1.7)

An optional fourth glass material, **Settings > Appearance > Glass style > Glassy UI**. It sits beside Liquid Glass, Soft Glass and Windows Acrylic; none of them was removed or changed, and
Liquid Glass stays the default. Windows keep their layout, icon pack and every other setting when the style is switched.

## Source

[alexburlis/glassyui](https://github.com/alexburlis/glassyui) (commit `174548a`, MIT, "Glass components for React and React Native"). The project is a React 19 + Tailwind component library; it has no native
Windows code, and Filo is a native Win32/Direct2D application with no web view, so the React components themselves cannot run inside it. What Filo uses is the library's **glass material, taken from its source
files unchanged in value**: `registry/glassyui/styles/liquid.css` (the `--lg-*` tokens and the `.lg-glass-surface` / `.lg-dialog-content` rules) and the lens parameters in
`registry/glassyui/lib/`. Nothing was redesigned or approximated by eye. The MIT notice is in LICENSES.md.

## What is applied, and where each number comes from

Filo windows are dialog slabs in glassyui terms (`GlassDialogContent` = `GlassPanel lens preset="prominent" elevation="raised"` + the slab rules), so the dialog recipe is used.

| Layer | Dark theme | Light theme | glassyui token |
|---|---|---|---|
| Live backdrop blur (Gaussian) | 3 px | 3 px | `--lg-slab-blur` |
| Saturation of the backdrop | 1.8 | 1.6 | `--lg-glass-sat` (dark default, light override) |
| Body brightness | 0.38 | 1 | `--lg-slab-brightness` |
| Rim brightness (the edge where the lens bends the backdrop) | 0.46 | 1 | `--lg-slab-rim-brightness` |
| Slab tint over the glass | rgb(28 28 30) at 30 % | white at 80 % | `--lg-slab-tint` |
| Sheen (white, top to 45 %) | 5 % | 8 % | `--lg-sheen-alpha` |
| Glow just inside the edge | 6 % | 14 % | `--lg-glow-alpha` |
| Lit bevel (top/left 0.7, bottom/right 0.55 of reflex-light) | 0.42 | 1.0 | `--lg-reflex-light` |
| Shaded inner edge | 1.4 | 0.8 | `--lg-reflex-dark` |
| Chromatic edge (blue left, pink right, 2 px) | 6 % | 5 % | `--lg-rim-ca-alpha` |
| Specular ring, 1 px (peak 0 %, floor 32-68 %, counter 100 %) | .45 / .07 / .22 | .70 / .35 / .55 | `--lg-specular-peak/floor/counter` |
| Raised shadow | 0 2px 6px .20 + 0 30px 60px -18px .55 | .06 + .26 | `--lg-shadow-raised` |
| Text colours | #F5F5F7 / #E5E5EA | #222224 / #505055 | `--lg-foreground`, `--lg-muted-foreground` |

The blue accent stays Filo's (`#3B8BFF`); glassyui's own accent (`#0067d9`) is not substituted.

Implementation: `src/gfx/live_composition.cpp` (blur, CSS-equivalent saturate and brightness folded into one colour matrix, separate body and rim layers, using the compositor's host backdrop like the other materials),
`src/ui/painter.cpp` (`GlassyShadow`, `GlassyBody`, `GlassyRim`), `src/ui/dialog_window.cpp` (selection and ink colours), `src/ui/settings_window.cpp` (the new choice and its **Edge refraction** slider).
Offscreen visual tests apply the same blur/saturate/brightness to the test backdrop on the CPU (`--glassy` with `--tool-shot`).

## Where it is not identical to the library (honest list)

- **Edge lens.** glassyui bends the rim with an SVG displacement map (IOR 1.5, dispersion 0.16, edge-focused) that only Chromium can run. Filo applies the *edge refraction* it already had for Liquid Glass (offset, masked copies of the live backdrop at the rim) with the Glassy rim brightness; the IOR/dispersion numbers are not used because there is no equivalent filter in the Windows compositor path. The **Edge refraction** slider scales it.
- **CSS box-shadow insets** are reproduced with gradient strokes, not blurred inset shadows; the 60 px shadow blur is shortened to fit Filo's 30 dip window margin.
- Not used: the film-grain noise layer (3 % / 2 %), `thickness`, the `GlassScene` canvas renderer, hover "lift" on surfaces, and the React components and their behaviour.
- The Tools/Convert **wheel** keeps its own optics; with Glassy UI selected it uses the Liquid optics and the Glassy backdrop tone behind it.
- Glass intensity does not apply to this material (the library has no such control); Reduce transparency and high contrast still switch it off to an opaque surface.

## Verification

- Compiled in Debug and Release; settings round-trip (`glassStyle` is clamped to 0..3, so old settings files load unchanged).
- Offscreen renders in light and dark (`out/glassy_crop--dark.png`, `out/glassy_crop--light.png`) show the tint, sheen, rim, ring and text colours of the table above.
- **Not verified:** the live compositor path (the real glass with the desktop moving behind it) was not looked at, because a visible window was not opened on the owner's desktop during testing. Please open Settings > Appearance > Glassy UI and check it over a few wallpapers.
