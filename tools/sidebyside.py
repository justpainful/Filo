# Side-by-side of a reference crop and our composite (coordinates in reference pixels).
#   python sidebyside.py ref.png ours.png offx offy x0,y0,x1,y1 out.png [scale]
import sys
from PIL import Image
ref = Image.open(sys.argv[1]).convert('RGB')
ours = Image.open(sys.argv[2]).convert('RGB')
ox, oy = int(sys.argv[3]), int(sys.argv[4])
x0, y0, x1, y1 = [int(v) for v in sys.argv[5].split(',')]
sc = float(sys.argv[7]) if len(sys.argv) > 7 else 0.5
a = ref.crop((x0, y0, x1, y1))
b = ours.crop((x0 - ox, y0 - oy, x1 - ox, y1 - oy))
w, h = a.size
out = Image.new('RGB', (w * 2 + 10, h), (20, 20, 20))
out.paste(a, (0, 0)); out.paste(b, (w + 10, 0))
out = out.resize((int(out.width * sc), int(out.height * sc)), Image.LANCZOS)
out.save(sys.argv[6])
