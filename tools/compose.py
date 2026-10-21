# Composites an offscreen Filo window render (RGBA PNG, transparent margin) over a wallpaper crop for design review.
# The live app gets its backdrop from DWM blur-behind; here the same effect is emulated with a gaussian blur restricted to
# the window body (the inner rounded rectangle, margin = 30 dip).
#   python compose.py frame.png wallpaper.png out.png [--sigma 18] [--x 400] [--y 100] [--scale 1.0] [--margin 30]
import sys
import numpy as np
from PIL import Image, ImageFilter, ImageDraw

args = sys.argv[1:]
frame_p, wall_p, out_p = args[:3]
opt = {'--sigma': 18.0, '--x': 100, '--y': 60, '--scale': 1.0, '--margin': 30.0, '--radius': 26.0}
i = 3
while i < len(args):
    opt[args[i]] = float(args[i + 1]); i += 2
frame = Image.open(frame_p).convert('RGBA')
wall = Image.open(wall_p).convert('RGBA')
x, y = int(opt['--x']), int(opt['--y'])
sc = opt['--scale']
crop = wall.crop((x, y, x + frame.width, y + frame.height))
blur = crop.filter(ImageFilter.GaussianBlur(opt['--sigma'] * sc))
m = int(opt['--margin'] * sc)
mask = Image.new('L', frame.size, 0)
ImageDraw.Draw(mask).rounded_rectangle([m, m, frame.width - m - 1, frame.height - m - 1], radius=int(opt['--radius'] * sc), fill=255)
mask = mask.filter(ImageFilter.GaussianBlur(0.6))
base = Image.composite(blur, crop, mask)
base.alpha_composite(frame)
base.convert('RGB').save(out_p)
print('composed', out_p)
