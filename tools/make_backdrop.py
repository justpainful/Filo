# Builds a clean "wallpaper" backdrop from a reference screenshot by removing the wheel/window region and filling it with
# a smooth normalised-convolution estimate of the surrounding gradient. Used ONLY for local visual comparison.
import sys
import numpy as np
from PIL import Image, ImageFilter
src, dst, cx, cy, r = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
im = Image.open(src).convert('RGB')
w, h = im.size
yy, xx = np.mgrid[0:h, 0:w]
mask = ((xx - cx) ** 2 + (yy - cy) ** 2 > r * r).astype(np.float32)   # 1 = keep
# also drop the blue Finder label chip & file thumbnail at the right of the wheel if requested
extra = sys.argv[6:] 
for e in extra:
    x0, y0, x1, y1 = [int(v) for v in e.split(',')]
    mask[y0:y1, x0:x1] = 0
arr = np.asarray(im).astype(np.float32)
def pushpull(img, m):
    # img: HxWx3 (valid where m==1). Multi-scale fill: average valid pixels downwards, then fill holes from coarser levels.
    levels = [(img * m[:, :, None], m)]
    while min(levels[-1][1].shape) > 2:
        c, mm = levels[-1]
        h2, w2 = (c.shape[0] + 1) // 2, (c.shape[1] + 1) // 2
        cp = np.pad(c, ((0, h2 * 2 - c.shape[0]), (0, w2 * 2 - c.shape[1]), (0, 0)))
        mp = np.pad(mm, ((0, h2 * 2 - mm.shape[0]), (0, w2 * 2 - mm.shape[1])))
        cs = cp.reshape(h2, 2, w2, 2, 3).sum(axis=(1, 3)); ms = mp.reshape(h2, 2, w2, 2).sum(axis=(1, 3))
        levels.append((cs / np.maximum(ms[:, :, None], 1e-6) * (ms[:, :, None] > 0), (ms > 0).astype(np.float32)))
    # coarse -> fine
    cur, curm = levels[-1]
    for c, mm in reversed(levels[:-1]):
        up = np.repeat(np.repeat(cur, 2, 0), 2, 1)[:c.shape[0], :c.shape[1]]
        # smooth the upsampled estimate a little
        k = np.array([1, 2, 1], np.float32) / 4
        for ax in (0, 1):
            up = np.apply_along_axis(lambda v: np.convolve(np.pad(v, 1, mode='edge'), k, mode='valid'), ax, up)
        cur = c + up * (1 - mm[:, :, None])
        curm = np.maximum(mm, 1.0)
    return cur
res = pushpull(arr, mask)
# gentle blur only inside the filled region for smoothness
Image.fromarray(np.clip(res, 0, 255).astype(np.uint8)).save(dst)
print('saved', dst)
