# Mean colour of matching regions in the reference and in our composite (coordinates in reference pixels).
#   python region_stats.py ref.png ours.png offx offy  x0,y0,x1,y1 [x0,y0,x1,y1 ...]
import sys
import numpy as np
from PIL import Image
ref = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(float)
ours = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(float)
ox, oy = int(sys.argv[3]), int(sys.argv[4])
for spec in sys.argv[5:]:
    x0, y0, x1, y1 = [int(v) for v in spec.split(',')]
    a = ref[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)
    b = ours[y0 - oy:y1 - oy, x0 - ox:x1 - ox].reshape(-1, 3).mean(axis=0)
    print(spec, 'ref', np.round(a).astype(int), 'ours', np.round(b).astype(int), 'diff', np.round(b - a).astype(int))
