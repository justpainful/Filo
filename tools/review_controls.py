"""Review native controls captures from filo_e2e_controls."""
from pathlib import Path
import json
from PIL import Image,ImageChops,ImageStat,ImageDraw,ImageFont
root=Path(__file__).resolve().parents[1]
out=root/'out/controls'
scenes=json.loads((out/'native-control-scenes.json').read_text(encoding='utf-8'))
regression=json.loads((out/'regression-final/float-regression.json').read_text(encoding='utf-8'))
assert regression['passed'] and regression['checks']>=100
assert len(scenes)==30
metrics=[]
for dpi in (96,120,144):
 for dark in (False,True):
  mode='dark' if dark else 'light';scale=dpi/96
  selected=[s for s in scenes if s['dpi']==dpi and s['dark']==dark]
  assert next(s for s in selected if s['transparency']==0)['surface_colour_difference']<=1
  assert next(s for s in selected if s['transparency']==1)['surface_colour_difference']>=25
  assert all(s['bubble_colour_difference']>=10 for s in selected)
  a=Image.open(out/f'{mode}-{dpi}-blur0.png').convert('RGB')
  b=Image.open(out/f'{mode}-{dpi}-blur48.png').convert('RGB')
  box=tuple(round(v*scale) for v in (90,250,490,310))
  aa=a.crop(box);bb=b.crop(box)
  change=sum(ImageStat.Stat(ImageChops.difference(aa,bb)).mean)/3
  detail0=sum(ImageStat.Stat(aa).stddev)/3;detail48=sum(ImageStat.Stat(bb).stddev)/3
  assert change>1,(mode,dpi,change)
  assert detail48<detail0,(mode,dpi,detail0,detail48)
  metrics.append(dict(dpi=dpi,dark=dark,blur_mean_difference=change,detail_without_extra_blur=detail0,detail_at_48=detail48))
font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',22)
for mode in ('light','dark'):
 images=[Image.open(out/f'{mode}-96-t{v}-warm.png').convert('RGB') for v in (0,50,100)]
 w,h=images[0].size;canvas=Image.new('RGB',(w*3,h+48),(24,27,34));draw=ImageDraw.Draw(canvas)
 for k,(value,im) in enumerate(zip((0,50,100),images)):
  canvas.paste(im,(k*w,48));draw.text((k*w+24,10),f'Transparency {value}%',font=font,fill='white')
 canvas.save(out/f'transparency-{mode}.png')
 # Enlarge only an actual native button crop for detail inspection.
 im=Image.open(out/f'{mode}-96-t85-warm.png').convert('RGB').crop((36,34,85,83))
 im.resize((294,294),Image.Resampling.NEAREST).save(out/f'close-detail-{mode}.png')
report=dict(passed=True,regression=regression,native_scenes=len(scenes),blur_metrics=metrics)
(out/'review.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
