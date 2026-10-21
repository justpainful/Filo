# Generates the Filo app icon (src/app/filo.ico + a 512px PNG). Original artwork: blue glass tile with a 8-petal wheel.
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024


def make():
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    grad = Image.new('RGBA', (S, S))
    px = grad.load()
    c0 = (96, 170, 255)
    c1 = (40, 112, 235)
    for y in range(S):
        t = y / (S - 1)
        for x in range(S):
            u = min(1, max(0, 0.6 * t + 0.4 * x / (S - 1)))
            px[x, y] = (int(c0[0] + (c1[0] - c0[0]) * u), int(c0[1] + (c1[1] - c0[1]) * u), int(c0[2] + (c1[2] - c0[2]) * u), 255)
    mask = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(mask)
    m = int(S * 0.04)
    d.rounded_rectangle([m, m, S - m, S - m], radius=int(S * 0.225), fill=255)
    im.paste(grad, (0, 0), mask)

    ov = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    od = ImageDraw.Draw(ov)
    cx = cy = S / 2
    n = 8
    ro = S * 0.36
    ri = S * 0.13
    gap = math.radians(7)
    for k in range(n):
        a0 = math.radians(-90 + 360 * k / n) - math.pi / n + gap / 2
        a1 = a0 + 2 * math.pi / n - gap
        pts = []
        steps = 18
        for i in range(steps + 1):
            a = a0 + (a1 - a0) * i / steps
            pts.append((cx + ro * math.cos(a), cy + ro * math.sin(a)))
        for i in range(steps + 1):
            a = a1 - (a1 - a0) * i / steps
            pts.append((cx + ri * math.cos(a), cy + ri * math.sin(a)))
        od.polygon(pts, fill=(255, 255, 255, 255 if k == 1 else 235))
    ov = ov.filter(ImageFilter.GaussianBlur(S * 0.006))
    im.alpha_composite(ov)

    hl = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse([-S * 0.2, -S * 0.55, S * 1.2, S * 0.45], fill=(255, 255, 255, 46))
    hl = Image.composite(hl, Image.new('RGBA', (S, S), (0, 0, 0, 0)), mask)
    im.alpha_composite(hl)
    return im


big = make()
sizes = [16, 24, 32, 48, 64, 128, 256]
big.save('src/app/filo.ico', sizes=[(s, s) for s in sizes])
big.resize((512, 512), Image.LANCZOS).save('src/app/filo-icon-512.png')
print('icon ok')
