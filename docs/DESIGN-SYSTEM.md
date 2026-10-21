# Design system

The wheel, dialogs, settings and floating tools share `src/design/tokens.h`, the immediate-mode toolkit in `src/ui`, and the native compositor in `src/gfx`. Reference measurements describe geometry; Windows-specific choices protect reading and interaction.

## Identity and typography

Filo Blue `#3B8BFF` is the selection/focus accent, with hover `#5BA0FF` and pressed `#2E78EA`. Body text uses Segoe UI Variable Text, titles use Segoe UI Variable Display, and both fall back to Segoe UI. Cascadia Mono is used for timecodes. Typical body text is 14.5 DIP and titles are 18.5 DIP bold. Light is the default theme; dark and system modes are selectable.

## Glass materials

**Liquid Glass** is the default. **Soft Glass** preserves the 0.1.2 material. **Windows Acrylic** adds stronger frost and live colour transfer with a light coating and fine static grain. All three remain selectable in Settings → Appearance → Glass style. Reflections, edge refraction and glass intensity are independent controls.

Liquid Glass has a milky centre, clearer rolled lip, directional white highlights, opposing shade and a quiet broad reflection. Five native `LayerVisual` passes flatten a live host-backdrop child, apply small affine displacements and mask the results using HLSL fields derived from the visible geometry. Maximum displacement is 2.4 pixels at 96 DPI and scales with DPI. The source remains live without desktop capture or an app refresh timer. The wheel's gaps preserve the natural desktop, with subtle contact shading. Text, icons and previews are drawn after the optical passes.

Dialog coatings are quieter than wheel lighting. Their outer shadows are cleared out of the reading surface, and their rims use directional gradient strokes. Wheel lighting follows hover over 160–180 ms without tracking the pointer as a spotlight. Soft Glass keeps the earlier circular backdrop, colour bounds, shading and motion values.

Geometry masks regenerate only when geometry or optical parameters change. Background-only updates come from Windows and do not repaint the foreground. Reduced transparency, disabled system transparency and high contrast disable the live material. Device recovery rebuilds the compositor and masks. Host backdrops require Windows 11 build 22000+; earlier Windows and setup failure use opaque rendering.

See [Liquid Glass verification](LIQUID-GLASS-2026-10-21.md) for native pixel tests and limits. Hidden `FILO_TEST_BACKDROP` fixtures use the older CPU material and do not prove the live optical path.

## Wheel geometry

The reference radius is 265 DIP, capped to fit the monitor work area and scaled by the user's wheel scale. Sector outer radius is 0.955R, inner radius 0.389R, hole radius 0.362R, corner radius 0.095R and angular gap 4.5 degrees. Sector zero starts at 12 o'clock and order is clockwise. Hover grows a sector by 1.045 about its own centre. Rendering, optical masks and hit testing use the same rounded annular-sector geometry. The new outer glass rim is 7 DIP; Soft Glass retains its earlier circular rim treatment.

## Dialog metrics

Body corner radius is 26 DIP, title bar 54.5 DIP, close button 32 DIP and content inset 16.5 DIP. Typical control rows are 30 DIP, footer height 58 DIP, slider tracks 5 DIP and thumbs 24×18 DIP. Segmented controls are 26 DIP tall and buttons have 8 DIP corners. The preview well uses warm near-black `#12100D` and a 24 DIP corner radius. A 30 DIP transparent margin contains the exterior shadow. Text polarity follows the selected theme and never flips with the desktop colour.

## Toolkit

`Ui` owns IDs, input state, layout columns, RTL mirroring, control animation and scrolling. `Painter` renders Direct2D/DirectWrite shapes, rounded clips, images, gradients, directional glass rims and text. Widgets include buttons, segmented controls, toggles, sliders, steppers, dropdowns, text fields and scroll panels. The D3D wheel shader renders geometry and material, while Direct2D/DirectWrite paints its sharp foreground. Translucent targets use premultiplied alpha and grayscale text antialiasing.

## Icon languages

Settings → Appearance offers the preserved Classic icons and the Pebble, Circuit and Facet data plugins. Icons inherit foreground theme/interaction colour and stay above all material effects. See [plugin format and verification](ICON-PACKS-2026-10-21.md).


## Tool close controls and Acrylic tuning (0.1.5)

Tools use one 28 DIP floating glass close control, with an independent live 8 DIP host backdrop, asymmetric rim, radial coating and thin rounded ×. Hover/press scale is 1.03/0.97; reduced motion and opaque fallbacks apply. Settings keeps separate standard chrome. Acrylic exposes percentage transparency and independent extra Gaussian blur; foreground text/icon opacity remains fixed. Float exposes close/removal at rest and handles structural mutations after rendering. See CONTROLS-2026-10-21.md.
