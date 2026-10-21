"""Review native icon/Acrylic evidence and compose actual-capture comparison panels."""
from pathlib import Path
import json,math
from PIL import Image,ImageDraw,ImageFont
root=Path(__file__).resolve().parents[1]
out=root/'out/icon-packs';native=out/'native'
report=json.loads((native/'native-icons.json').read_text(encoding='utf-8'))
assert report['native_wheel_cases']==36 and report['passed']
glyphs=json.loads((out/'render-verification.json').read_text(encoding='utf-8'))
assert glyphs['passed'] and glyphs['glyph_renders']==2448
assert glyphs['partial_plugin_discovery_and_removal']
checks=[]
for dpi in (96,120,144):
 R=220*dpi/96
 bg=Image.open(native/'icons-background.png').convert('RGB')
 wheel=Image.open(native/f'wheel-2-facet-{dpi}.png').convert('RGB')
 diffs=[]
 for angle in (30,90,150,210,270,330):
  a=math.radians(angle);x=round(510+R*.65*math.sin(a));y=round(403-R*.65*math.cos(a))
  diffs.append(max(abs(a-b) for a,b in zip(bg.getpixel((x,y)),wheel.getpixel((x,y)))))
 warm=Image.open(native/f'wheel-2-facet-{dpi}-warm.png').convert('RGB')
 cool=Image.open(native/f'wheel-2-facet-{dpi}-cool.png').convert('RGB')
 # Sample the outer part of the Float sector, away from labels and gaps.
 point=(round(510+R*.85*math.sin(math.pi/3)),round(403-R*.85*.5))
 delta=max(abs(a-b) for a,b in zip(warm.getpixel(point),cool.getpixel(point)))
 assert max(diffs)<=8,(dpi,diffs)
 assert delta>=25,(dpi,delta)
 assert report[f'idle-{dpi}']
 checks.append(dict(dpi=dpi,gap_max_difference=max(diffs),surface_warm_cool_delta=delta))
report['acrylic_geometry_and_live_checks']=checks
report['glyphs']=glyphs
(native/'scene-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',25)
for name,entries,cols in [('materials-comparison',[(str(i),label,'facet') for i,label in enumerate(['Liquid Glass','Soft Glass','Windows Acrylic'])],3),('packs-on-liquid',[('0',label,pack) for label,pack in [('Classic (original)','classic'),('Pebble','pebble'),('Circuit','circuit'),('Facet','facet')]],2)]:
 rows=(len(entries)+cols-1)//cols
 result=Image.new('RGB',(cols*500,rows*550),(243,245,249));draw=ImageDraw.Draw(result)
 for k,(style,label,pack) in enumerate(entries):
  im=Image.open(native/f'wheel-{style}-{pack}-96.png').convert('RGB').crop((260,150,760,650))
  x=k%cols*500;y=k//cols*550;result.paste(im,(x,y+50));draw.text((x+22,y+10),label,font=font,fill=(28,33,45))
 result.save(native/(name+'.png'))
print(json.dumps(report,indent=2))
