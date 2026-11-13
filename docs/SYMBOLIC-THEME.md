# Symbolic theme (0.2.3)

Filo's interface follows the rules of the owner's **Symbolic** app (`src/design/tokens.css`, `src/ui/controls`, `src/ui/menu`, `src/ui/dialog`, read only). It is one theme:
the glass material (SYMBOLIC-GLASS.md), the colours and metrics below, the controls, the icons in every place and the settings layout. Earlier materials and icon packs
remain selectable in Settings > Appearance.

## Tokens (src/design/tokens.h)

| Role | Symbolic | Filo |
|---|---|---|
| Accent | #007AFF / #0A84FF | #0A84FF (the brighter one, readable on glass in both themes) |
| Text / secondary / tertiary | #1D1D1F #6E6E73 #8E8E93 (dark #F5F5F7 #AEAEB2) | primary as Symbolic; secondary and tertiary stay darker (light) / lighter (dark) than Symbolic so they keep 4.5:1 over every backdrop (`test_material`) |
| Radii | control 9, group 12, card/popover 16 | buttons are capsules, list rows 12, preview wells 16, windows 18 |
| Title bar | 52 | 52 |
| Type | 13 / 11 | 13.5 / 12 (Segoe UI Variable) |
| Motion | hover 110 ms, popover 180 ms, selection 220 ms | the same constants (`ds::motion`) |

Not taken over: Symbolic's 36 DIP control height and 32 DIP rows. Filo's tool windows are dense and laid out in fixed rows; control heights stay at 26-30 DIP.

## Controls (src/ui/ui.cpp)

- **Buttons**: quiet glass capsules with a hairline; one accent capsule per view (the primary action). Hover is a soft platter, press shrinks to 0.96. Icon buttons are round.
- **Segmented control**: a track with a glass lens that slides to the selected item (220 ms); optional icons per item (the Settings tabs use them).
- **Pop-up buttons**: a capsule with the value and a small round up/down stepper badge; the list is a rounded surface with a soft platter on the hovered row.
- **Switches**: blue track, white knob. **Section titles**: small, secondary ink.
- **Icons on buttons**: every button gets the best-fitting icon from its label (`IconForLabel` in `ui.cpp`), so the same word always has the same glyph (Apply/Save check, Cancel/Close x, Download, Paste, Browse/Change folder, Reset, ...).

## Icons everywhere

- Default pack **Symbolic Style**: original rounded line icons of one weight (SF Symbols are not bundled, see SYMBOLIC-GLASS.md and LICENSES.md). Eleven icons were added for the new places:
  download, clipboard, sliders, palette, link, keyboard, bell, globe, power, eye, info. Pebble, Circuit and Facet inherit them from Classic.
- Settings: tabs with icons, every section heading and every row has one (`RowIcon` / `HeadingIcon`).
- Tray menu: every item has an icon (`ui/menu_icons`, drawn in the system menu ink).
- Buttons, Download from Link, link zone, wheel and tool windows: through the label rule above and the existing wheel icons.

## Settings layout

- A section is one rounded container (radius 12) with a subtle fill and **no border**; headers are small secondary text only (no icon: the icons belong to the rows).
- Rows are 40 DIP high with controls of one height (30): icon, label, control. Hairlines between rows run from the label to the trailing edge, at low alpha.
- Every control ends on the same trailing inset (toggle, pop-up, segmented control, stepper, shortcut field, slider); segmented controls are sized to their labels.
- Shortcut fields are compact capsules sized to their text; the explanation sits under the row it belongs to.
- Icons beside a label are 18 DIP with a stroke that matches the 13.5 pt text (about 1.3x the text size); tab icons 18, button icons 17.
- Equal space above every header (20) and below it (8). The window shadow is Symbolic's near + far pair, spread out (no hard band under the edge).
- The Wheels tab uses a new `wheel` icon (ring, hub and spokes).

## Not done / honest list

- The other tool windows keep their layouts; they got the new controls, colours, radii and button icons but were not redrawn row by row.
- The tray menu is the native Windows menu (icons added), not a glass menu; shortcut hints are not shown in it.
- Per-icon hover motion (Symbolic's choreographies) is not implemented.
- Verified with hidden renders in light and dark (out/redesign/before and /after) and unit tests; the live compositor and real pointer interaction were not exercised.

## 0.2.2 review fixes

- Scroll areas fade into their edges (14 DIP at the top once scrolled, 26 at the bottom while more follows) with bottom padding, so no row is ever sliced; the scroll bar has its own gutter.
- **Segmented lens bug**: two segmented controls in one scope shared one animation state, so a lens could rest between two segments (Compress: "Light" under "Balanced"). Every control now has its own state.
- The convert wheel shows a format icon above each label (image, video, audio, document, archive); wheel labels are sentence case at semibold, and every wheel icon is scaled to one optical size (`IconCache::OpticalScale`).
- Number fields are a capsule plus one round up/down badge; text fields use the quiet track surface ("Optional suffix" placeholder).
- Compress: icons on every row, a quiet file card, a compact pop-up, a switch for "Keep metadata", a shorter window, and "already as small as it gets" instead of an estimate that grows the file.
- The Symbolic Style pack is visibly its own: line weight 2.0 (Classic 1.7) and a soft second tone behind file-like glyphs.

## 0.2.3 fixes

- **Segmented lens off its tab**: the lens position used `Ui::Anim`, which returns an eased *progress* and therefore only rests exactly on 0 or 1. In a four-item tab bar the second and third tabs (targets 1/3 and 2/3) rested 28 DIP off. Positions now use `Ui::Follow` (exponential approach that snaps onto any target); unit-tested (`test_follow.cpp`), and a render of all four Settings tabs shows the lens centred on each.
- **Tray menu**: the "Enable drag wheels" check mark was hidden by the item's icon, so it always looked off. The icon is now the state: a blue check when on, an empty ring when off.
