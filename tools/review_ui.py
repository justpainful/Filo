"""Render the real executable against deterministic dark, bright and coloured backdrops.
Usage: python -I tools/review_ui.py <exe> <output-directory>
These images are QA fixtures, never shipped application assets.
"""
import os
from pathlib import Path
import subprocess
import sys
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent.parent
exe, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=True)
dark = Image.new('RGB', (1600, 1200), '#18191c')
d = ImageDraw.Draw(dark)
d.rectangle((0, 0, 1600, 80), fill='#292b30')
for i, color in enumerate(('#ff583e', '#39e48b', '#47a8ff', '#ffc853')):
    x = 540 + i * 105
    d.rounded_rectangle((x, 310, x + 64, 380), 13, fill=color)
    d.text((x, 390), 'Desktop file', fill='white')
dark.save(output / 'dark-desktop.png')
Image.new('RGB', dark.size, 'white').save(output / 'white-desktop.png')
env = dict(os.environ, FILO_TEST_DPI='96')
photo = root / 'testdata/ui/flower.png'

def run(args, backdrop):
    e = dict(env, FILO_TEST_BACKDROP=str(backdrop))
    subprocess.run([str(exe), *map(str, args)], cwd=root, env=e, check=True, timeout=60)

for name, backdrop, theme in [
    ('dark-desktop', output / 'dark-desktop.png', '--light'),
    ('white-desktop', output / 'white-desktop.png', '--light'),
    ('colour', root / 'reference/backdrop-edit.png', '--light'),
    ('dark-theme', output / 'white-desktop.png', '--dark')]:
    run(['--tool-shot', 'edit-metadata', output / f'metadata-{name}.png', photo, '--at', '800,600', theme], backdrop)

for name, kind, hover in [('convert', 'convert', -1), ('tools', 'tools9', -1), ('hover', 'convert', 1), ('actions', 'actions', -1)]:
    run(['--render-wheel', kind, output / f'wheel-{name}.png', '--backdrop', output / 'dark-desktop.png', '--cursor', '800,600', '--light', '--hover', hover], output / 'dark-desktop.png')

run(['--tool-shot', 'edit-photo', output / 'edit-photo.png', photo, '--at', '800,600', '--light'], root / 'reference/backdrop-edit.png')
run(['--tool-shot', 'add-background', output / 'add-background.png', photo, '--at', '800,600', '--light'], root / 'reference/backdrop-edit.png')
run(['--tool-shot', 'join-videos', output / 'join-videos.png', root / 'testdata/ui/clip1.mp4', root / 'testdata/ui/clip2.mp4', '--at', '800,600', '--light'], output / 'dark-desktop.png')
run(['--tool-shot', 'trim-audio', output / 'trim-audio.png', root / 'testdata/media/work/trimaudio/s.wav', '--at', '800,600', '--light'], output / 'dark-desktop.png')
run(['--gallery', '--rtl', '--light', '--offscreen', output / 'gallery-rtl.png'], output / 'dark-desktop.png')
for tab in range(4):
    run(['--settings-shot', output / f'settings-{tab}.png', tab], output / 'dark-desktop.png')
for mode in ['running', 'done', 'failed']:
    run(['--hud-shot', output / f'progress-{mode}.png', mode], output / 'dark-desktop.png')
print(f'UI renders saved to {output}')
