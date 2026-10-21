# Measures radial structure of a reference wheel screenshot (estimates, see docs/REFERENCE-AUDIT.md)
import sys, math
import numpy as np
from PIL import Image
im = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(float)
cx, cy = float(sys.argv[2]), float(sys.argv[3])
lum = im @ np.array([0.2126, 0.7152, 0.0722])
def prof(angle_deg, rmax=620):
    a = math.radians(angle_deg)
    out = []
    for r in range(0, rmax):
        x = cx + r*math.cos(a); y = cy - r*math.sin(a)
        out.append(lum[int(round(y)), int(round(x))])
    return np.array(out)
def edges(p, thr=12):
    d = np.diff(p)
    idx = [i for i in range(1, len(d)) if abs(d[i]) > thr and abs(d[i]) >= abs(d[i-1]) and abs(d[i]) >= abs(d[min(i+1, len(d)-1)])]
    return idx
for ang in [90, 67.5, 22.5, 0, -90]:
    p = prof(ang)
    print('angle', ang, 'edges(r):', edges(p))
