"""Independent pixel checks of the owned native Liquid Glass scenes."""
from pathlib import Path
import json
import sys
import numpy as np
from PIL import Image, ImageDraw

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("out/liquid-glass")
scene = root / "liquid"

def pixels(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(int)

optics = json.loads((scene / "optics.json").read_text())
report = {"dpi": []}
background = pixels(scene / "optics-background.png")
for case in optics["dpi"]:
    assert case["live"] and case["soft_live"] and case["idle"], case
    dpi = case["dpi"]
    a = pixels(scene / f"dpi-{dpi}-liquid.png")
    no_light = pixels(scene / f"dpi-{dpi}-no-reflection.png")
    no_bend = pixels(scene / f"dpi-{dpi}-no-refraction.png")
    restored = pixels(scene / f"dpi-{dpi}-restored.png")
    reflected = np.max(abs(a - no_light), axis=2)
    bent = np.max(abs(a - no_bend), axis=2)
    assert (reflected > 2).sum() > 1000, "Reflection slider did not change the rim"
    assert reflected.max() > 10
    assert np.max(abs(a - restored)) <= 2, "Material switch did not restore Liquid Glass"
    # Fully opaque foreground ink remains sharp and unchanged by optical passes.
    ink = a.max(axis=2) < 26
    assert ink.sum() > 1000
    assert np.max(abs(a - no_light)[ink]) <= 1
    scale = dpi / 96
    x, y = round(510 + 100 * scale), round(403 - 173.205 * scale)
    gap = np.max(abs(a[y-1:y+2, x-1:x+2] - background[y-1:y+2, x-1:x+2]))
    assert gap <= 5, f"Blur leaked into the gap at {dpi} DPI: {gap}"
    report["dpi"].append({**case, "reflection_changed_pixels": int((reflected > 2).sum()),
                           "refraction_changed_pixels": int((bent > 0).sum()),
                           "gap_max_difference": int(gap)})

# The raw compositor calibration removes the milky coat and tests an exaggerated
# offset as a diagnostic only. Production remains at 2.4 physical pixels at 96 DPI.
cal = pixels(scene / "refraction-calibration-none.png")
for strength, threshold in (("2.4", 2), ("24", 10)):
    delta = np.max(abs(cal - pixels(scene / f"refraction-calibration-{strength}.png")), axis=2)
    assert delta.max() > threshold and (delta > 0).sum() > 100, "Backdrop displacement has no measured effect"
    report[f"calibration_{strength}"] = {"max_difference": int(delta.max()), "changed_pixels": int((delta > 0).sum())}

live_red=pixels(scene / "refraction-calibration-live-red.png")[403,510]
live_blue=pixels(scene / "refraction-calibration-live-blue.png")[403,510]
assert np.max(abs(live_red-live_blue))>100, "Refraction source froze instead of following the live desktop"
report["refraction_source_updates_live"]=True

for theme in json.loads((scene / "result.json").read_text())["themes"]:
    assert len(set(theme["frames"])) == 1 and theme["mask_updates"] == 1
assert json.loads((scene / "contrast.json").read_text())["passed"]
soft = json.loads((root / "soft/contrast.json").read_text())
assert soft["passed"]
baseline = json.loads(Path("out/live-glass/contrast.json").read_text())
assert [t["changing_pixels"] for t in soft["themes"]] == [t["changing_pixels"] for t in baseline["themes"]]
report["soft_material_matches_previous_pixels"] = True
hover=pixels(scene / "wheel-hover-white.png")[313,600]
def relative(rgb):
    v=np.asarray(rgb)/255
    v=np.where(v<=.04045,v/12.92,((v+.055)/1.055)**2.4)
    return float(np.dot(v,[.2126,.7152,.0722]))
hover_contrast=1.05/(relative(hover)+.05)
assert hover_contrast>=4.5, f"White hover label contrast failed: {hover_contrast}"
report["hover_contrast_on_white"]=hover_contrast


# Actual native screenshots side by side; no simulated glass is introduced here.
left = Image.open(scene / "dpi-96-soft.png").convert("RGB").crop((220, 115, 805, 695))
right = Image.open(scene / "dpi-96-liquid.png").convert("RGB").crop((220, 115, 805, 695))
comparison = Image.new("RGB", (left.width * 2, left.height + 32), "#f4f5f8")
comparison.paste(left, (0, 32)); comparison.paste(right, (left.width, 32))
labels = ImageDraw.Draw(comparison)
labels.text((14, 10), "Soft Glass - previous material", fill="#15151a")
labels.text((left.width + 14, 10), "Liquid Glass - live optics", fill="#15151a")
comparison.save(root / "materials-comparison.png")
frames = [Image.open(scene / f"dpi-96-{name}.png").convert("RGB").crop((220, 115, 805, 695)) for name in ("warm", "cool")]
frames[0].save(root / "live-liquid-proof.gif", save_all=True, append_images=frames[1:], duration=1100, loop=0)
report["passed"] = True
(root / "optical-verification.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
