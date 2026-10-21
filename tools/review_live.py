"""Verify native compositor captures from filo_e2e_live (Pillow required).

The harness owns the foreground and background HWNDs. These are actual desktop
composition results, not swap-chain readbacks or simulated backdrop bitmaps.
Run the native harness from the repository root before running this script.
"""
import csv
import json
import sys
from pathlib import Path

from PIL import Image

out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("out/live-glass")
result = json.loads((out / "result.json").read_text())


def luminance(rgb):
    def linear(v):
        v /= 255
        return v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4
    return sum(w * linear(v) for w, v in zip((.2126, .7152, .0722), rgb))


def contrast(a, b):
    a, b = sorted((luminance(a), luminance(b)))
    return (b + .05) / (a + .05)


palettes = {
    False: [(28, 28, 30), (37, 40, 47), (37, 40, 47)],
    True: [(245, 245, 247), (194, 201, 212), (181, 190, 204)],
}
report = {"native_samples": 0, "themes": []}
samples = list(csv.DictReader((out / "samples.csv").open()))
assert len(samples) == 162, f"Incomplete native sweep: {len(samples)} samples"
for theme in result["themes"]:
    dark = theme["dark"]
    name = "dark" if dark else "light"
    assert all(theme[k] for k in ("live", "opaque", "resized", "device_recovered"))
    assert len(set(theme["frames"])) == 1, "Foreground repainted during live proof"
    surfaces = [Image.open(out / f"{name}-{i}.png").convert("RGB") for i in range(4)]
    pixels = [im.getpixel((290, 280)) for im in surfaces]
    assert len(set(pixels)) == 4, "Backdrop did not change independently"
    half = Image.open(out / f"{name}-half.png").convert("RGB").getpixel((290, 280))
    opaque = [Image.open(out / f"{name}-opaque-{i}.png").convert("RGB").getpixel((290, 280)) for i in range(2)]
    assert opaque[0] == opaque[1], "Reduced transparency still samples desktop"
    assert all(min(a, b) <= h <= max(a, b) for a, b, h in zip(pixels[0], opaque[0], half))
    recovered = Image.open(out / f"{name}-recovered.png").convert("RGB")
    assert recovered.size == (630, 470), "Resize failed"
    assert max(abs(a-b) for a,b in zip(recovered.getpixel((290,280)), pixels[3])) <= 2, "Device recovery lost the material"
    assert half not in (pixels[0], opaque[0]), "Glass intensity has no effect"
    native = [tuple(int(row[c]) for c in ("R", "G", "B")) for row in samples if bool(int(row["dark"])) == dark]
    ratios = [min(contrast(text, colour) for colour in native + pixels + [half, opaque[0]]) for text in palettes[dark]]
    assert min(ratios) >= 4.5, f"{name}: text contrast failed: {ratios}"
    report["native_samples"] += len(native)
    report["themes"].append({"dark": dark, "frames": theme["frames"], "changing_pixels": pixels,
                             "half_intensity": half, "opaque_pixel": opaque[0], "minimum_text_contrast": ratios})
    surfaces[0].save(out / f"{name}-live-proof.gif", save_all=True, append_images=surfaces[1:], duration=1000, loop=0)

assert all(result[k] for k in ("reference_photo_live", "wheel_live", "wheel_opaque", "wheel_device_recovered"))
report["passed"] = True
(out / "contrast.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
