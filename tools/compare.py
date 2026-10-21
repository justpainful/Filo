# Visual comparison: composites our offscreen render over the backdrop at the window origin and produces
#   <out>-side.png   reference | ours | abs-diff heat map (cropped to the wheel/window region)
#   <out>-overlay.png 50% blend, and prints several metrics (MAE, RMSE, edge-map IoU, per-channel mean shift).
import sys
import numpy as np
from PIL import Image
ref_p, ours_p, backdrop_p, ox, oy, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
crop = [int(v) for v in sys.argv[7].split(',')] if len(sys.argv) > 7 else None   # x0,y0,x1,y1 in reference coords
ref = Image.open(ref_p).convert('RGB')
bd = Image.open(backdrop_p).convert('RGBA')
ours = Image.open(ours_p).convert('RGBA')
frame = bd.copy()
frame.alpha_composite(ours, (ox, oy))
frame = frame.convert('RGB')
if crop is None:
    crop = [ox, oy, ox + ours.width, oy + ours.height]
a = ref.crop(crop); b = frame.crop(crop)
A = np.asarray(a).astype(np.float32); B = np.asarray(b).astype(np.float32)
d = np.abs(A - B)
mae = d.mean(); rmse = float(np.sqrt(((A - B) ** 2).mean()))
def edges(x):
    g = x.mean(axis=2)
    gx = np.abs(np.diff(g, axis=1, prepend=g[:, :1])); gy = np.abs(np.diff(g, axis=0, prepend=g[:1, :]))
    return (gx + gy) > 18
ea, eb = edges(A), edges(B)
def dil(m, r=3):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out
# tolerant edge IoU (edges may be off by <=3 px)
inter = (ea & dil(eb)).sum() + (eb & dil(ea)).sum()
union = ea.sum() + eb.sum()
print('MAE %.2f  RMSE %.2f  tolerant-edge-F %.3f  mean shift RGB %s' % (mae, rmse, inter / max(union, 1), np.round((B - A).mean(axis=(0, 1)), 1)))
heat = np.clip(d.mean(axis=2) * 3, 0, 255).astype(np.uint8)
side = Image.new('RGB', (a.width * 3, a.height))
side.paste(a, (0, 0)); side.paste(b, (a.width, 0)); side.paste(Image.fromarray(heat).convert('RGB'), (a.width * 2, 0))
side.save(out + '-side.png')
Image.blend(a, b, 0.5).save(out + '-overlay.png')
