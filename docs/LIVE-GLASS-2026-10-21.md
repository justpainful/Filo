# Filo 0.1.2 — native live glass

The visible application now uses Windows.UI.Composition `CreateHostBackdropBrush`, enabled by the public `DWMWA_USE_HOSTBACKDROPBRUSH` attribute. A compositor backdrop visual sits below the UI swap chain, clipped to the dialog body or animated wheel circle. The background is sampled before Filo's HWND, so background motion updates without Filo repainting or photographing the desktop. The previous runtime `CaptureBackdrop` API was removed. Bitmap fixtures remain only for explicit hidden visual tests.

The material retains the reference's +0.20 light lift and saturated yellow/orange colours. A luminosity blend raises dim regions for text readability while preserving chrominance; dark mode limits brightness. The former shadow was also cleared out of the transparent body, retaining only the exterior shadow. Wheel sectors draw translucent tints and quiet highlights above the live material. This is an implementation on Windows, not a claim of exact macOS optical or pixel equivalence.

## Native verification

`tests/e2e/live_backdrop.cpp` owns both the changing background and the foreground Filo windows. It captures the real composed desktop scene, not a readback of a simulated material. Run from the project root on a Windows 11 desktop with transparency enabled:

```powershell
pwsh scripts/build.ps1 -Config Release -Suffix _rel -Target filo_e2e_live
build/Release_rel/bin/filo_e2e_live.exe out/live-glass
python tools/review_live.py out/live-glass
```

The harness and review check:

- Four distinct background colours in each theme with the foreground frame counter unchanged (`[2,2,2,2]`). Windows independently updates the glass.
- 162 measured GPU/compositor samples: 27 RGB backgrounds × three glass intensities × two themes. Primary, secondary and tertiary text are checked against the measured surface pixels at 4.5:1 or higher. This covers the material/palette pair, not every possible image overlay, disabled control or widget state.
- Half intensity differs from both full glass and the opaque surface. Reduced transparency yields identical interior pixels when the background changes.
- Body resizing, material restoration and simulated graphics-device loss/recreation for dialogs and the wheel.
- Actual Edit Photo and wheel windows over the reference backgrounds, captured in `out/live-glass/reference-photo-live.png` and `reference-wheel-live.png`.

The measured minimum across primary/secondary/tertiary text was **7.106:1 in light mode** and **4.654:1 in dark mode**.

Machine-readable evidence is in `out/live-glass/result.json`, `samples.csv` and `contrast.json`; native animations are `light-live-proof.gif` and `dark-live-proof.gif`. Core and metadata test results are saved beside them. The older 0.1.1 review describes a historical snapshot implementation and is superseded by this revision.

The actual Release `Edit Metadata` window was also opened on the Unicode PDF fixture, inspected in View and Edit modes, and dragged over a different desktop region. Text remained clear and the material followed the live background. No document edits were saved.

## Runtime and compatibility

Appearance changes are applied to already-open windows. Size/DPI changes update composition clipping; graphics-device recovery creates a new composition target and restores the material. A static dialog does not run a background-refresh timer. Filo still renders when controls, media previews or appearance animations need a frame.

Public host backdrops require Windows 11 build 22000+. This PC runs build 26200. Earlier Windows, a failed compositor/effect setup, reduced transparency, disabled system transparency or high contrast use opaque material. Mixed-monitor DPI transitions and installer install/uninstall have not been requalified. The remaining product gaps in KNOWN-LIMITATIONS.md still apply.

Microsoft API references: [host backdrop attribute](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute), [CreateHostBackdropBrush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createhostbackdropbrush), [composition effects](https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-effects).


## Delivered build

Full MSVC Release build and executable resources are version 0.1.2. Core tests: **25 passed**; metadata tests: **27 passed**. Native verification and all 162 measured compositor samples passed. The installer is `dist/Filo-Setup-0.1.2.exe` (compiled successfully; not installed/uninstalled in this review). The staged executable SHA-256 matches the verified Release binary. Hashes and results are saved in `out/live-glass/release-manifest.json`.
