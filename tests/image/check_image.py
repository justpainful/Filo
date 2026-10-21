"""Independent image inspector for the image engine tests. python -I check_image.py <file> [<file>...]
Prints one JSON object per file: format, size, frames, mean RGBA of the centre of each quadrant, EXIF facts, ICC, XMP marker."""
import io
import json
import sys

import numpy as np
from PIL import Image

try:
    from PIL import features
except Exception:
    pass


def quad_means(im):
    rgba = np.asarray(im.convert('RGBA'), float)
    h, w = rgba.shape[:2]
    res = []
    for qy in range(2):
        for qx in range(2):
            x0 = int(qx * w / 2 + w / 8)
            x1 = int(qx * w / 2 + w * 3 / 8)
            y0 = int(qy * h / 2 + h / 8)
            y1 = int(qy * h / 2 + h * 3 / 8)
            x1 = max(x1, x0 + 1); y1 = max(y1, y0 + 1)
            x0 = min(x0, w - 1); y0 = min(y0, h - 1)
            reg = rgba[y0:y1, x0:x1].reshape(-1, 4)
            a = reg[:, 3]
            # colour mean weighted by alpha so transparent pixels do not pollute it
            wsum = a.sum()
            rgb = (reg[:, :3] * a[:, None]).sum(axis=0) / wsum if wsum > 0 else reg[:, :3].mean(axis=0)
            res.append([round(float(rgb[0]), 1), round(float(rgb[1]), 1), round(float(rgb[2]), 1), round(float(a.mean()), 1)])
    return res


for path in sys.argv[1:]:
    info = {'path': path}
    try:
        raw = open(path, 'rb').read()
        im = Image.open(io.BytesIO(raw))
        info['format'] = im.format
        info['mode'] = im.mode
        info['w'], info['h'] = im.size
        info['frames'] = getattr(im, 'n_frames', 1)
        if info['frames'] > 1:
            durs = []
            for i in range(info['frames']):
                im.seek(i)
                durs.append(im.info.get('duration'))
            im.seek(0)
            info['durations'] = durs
            info['loop'] = im.info.get('loop')
        ex = im.getexif()
        info['orientation'] = ex.get(274)
        info['make'] = ex.get(271)
        try:
            sub = ex.get_ifd(0x8769)
            info['datetime_original'] = sub.get(36867)
            info['pixel_x'] = sub.get(0xA002)
            info['pixel_y'] = sub.get(0xA003)
        except Exception:
            pass
        info['icc'] = bool(im.info.get('icc_profile'))
        icc = im.info.get('icc_profile')
        info['icc_len'] = len(icc) if icc else 0
        info['icc_is_srgb'] = bool(icc) and (b'sRGB' in icc[:600] or b'sRGB' in icc)
        info['xmp_marker'] = b'FiloTestXmp' in raw
        info['q'] = quad_means(im)
        info['size_bytes'] = len(raw)
        info['ok'] = True
    except Exception as e:  # noqa
        info['ok'] = False
        info['error'] = repr(e)
    print(json.dumps(info))
