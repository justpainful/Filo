"""Fixture generator for the image engine tests. Run as: python -I gen_fixtures.py <outdir>

Display image D (320x240): TL red, TR green, BL blue, BR yellow. The alpha variant makes TR fully transparent and BL 50 %.
Every source format is written from D (or an inverse-oriented copy of D) with libraries that are independent of Filo.
"""
import io
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageCms, ImageOps, ExifTags

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
W, H = 320, 240


def master(alpha=False):
    a = np.zeros((H, W, 4), np.uint8)
    a[:H // 2, :W // 2] = (255, 0, 0, 255)
    a[:H // 2, W // 2:] = (0, 255, 0, 255)
    a[H // 2:, :W // 2] = (0, 0, 255, 255)
    a[H // 2:, W // 2:] = (255, 255, 0, 255)
    if alpha:
        a[:H // 2, W // 2:, 3] = 0
        a[H // 2:, :W // 2, 3] = 128
    return Image.fromarray(a, 'RGBA')


D = master(False)
DA = master(True)
P = lambda n: os.path.join(out, n)

# ---- stored (inverse-oriented) images + EXIF for all 8 orientations ----------------------------------------------------------
XMP = (b'<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">'
       b'<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" '
       b'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:creator><rdf:Seq><rdf:li>FiloTestXmp</rdf:li></rdf:Seq></dc:creator>'
       b'</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>')
INV = {1: None, 2: Image.Transpose.FLIP_LEFT_RIGHT, 3: Image.Transpose.ROTATE_180, 4: Image.Transpose.FLIP_TOP_BOTTOM,
       5: Image.Transpose.TRANSPOSE, 6: Image.Transpose.ROTATE_90, 7: Image.Transpose.TRANSVERSE, 8: Image.Transpose.ROTATE_270}


def stored_for(orientation, img):
    m = INV[orientation]
    return img if m is None else img.transpose(m)


def exif_for(orientation, w, h):
    ex = Image.Exif()
    ex[271] = 'FiloTest'
    ex[272] = 'Model X'
    ex[274] = orientation
    ifd = ex.get_ifd(0x8769)
    ifd[36867] = '2024:05:06 07:08:09'
    ifd[0xA002] = w
    ifd[0xA003] = h
    return ex


for o in range(1, 9):
    S = stored_for(o, D.convert('RGB'))
    # independent self check: PIL's own exif_transpose of the stored image must give D back
    ex = exif_for(o, S.width, S.height)
    probe = S.copy()
    probe.info['exif'] = ex.tobytes()
    S.save(P(f'orient_{o}.jpg'), quality=95, subsampling=0, exif=ex.tobytes(), xmp=XMP)
    back = ImageOps.exif_transpose(Image.open(P(f'orient_{o}.jpg')))
    diff = np.abs(np.asarray(back.convert('RGB'), int) - np.asarray(D.convert('RGB'), int)).max(axis=2)
    assert back.size == D.size and (diff > 30).mean() < 0.02, f'orientation {o} fixture self-check failed'

# ---- one source per format ---------------------------------------------------------------------------------------------------
S6 = stored_for(6, D.convert('RGB'))
S6.save(P('src_jpg.jpg'), quality=95, subsampling=0, exif=exif_for(6, S6.width, S6.height).tobytes(), xmp=XMP)
png_exif = exif_for(1, W, H)
DA.save(P('src_png.png'), exif=png_exif.tobytes())
DA.save(P('src_webp.webp'), lossless=True, exif=png_exif.tobytes(), xmp=XMP)
DA.save(P('src_tiff.tiff'), compression='tiff_lzw')
D.convert('RGB').save(P('src_bmp.bmp'))
# GIF: binary transparency (TR), BL opaque
g = master(False).convert('RGBA')
ga = np.asarray(g).copy()
ga[:H // 2, W // 2:, 3] = 0
gi = Image.fromarray(ga, 'RGBA')
pal = gi.convert('RGB').quantize(colors=16, method=Image.Quantize.FASTOCTREE)
mask = Image.fromarray(np.where(ga[:, :, 3] == 0, 255, 0).astype(np.uint8))
tr_index = 15
pal_img = pal.copy()
pa = np.asarray(pal_img).copy()
pa[ga[:, :, 3] == 0] = tr_index
pal_img = Image.fromarray(pa, 'P')
pl = pal.getpalette()
pl = (pl + [0] * 768)[:768]
pal_img.putpalette(pl)
pal_img.save(P('src_gif.gif'), transparency=tr_index)
DA.save(P('src_avif.avif'), quality=95)

# ---- animations --------------------------------------------------------------------------------------------------------------
frames = []
for c in [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]:
    a = np.zeros((120, 160, 4), np.uint8)
    a[:, :] = (*c, 255)
    a[:30, :40] = (0, 0, 0, 0)
    frames.append(Image.fromarray(a, 'RGBA'))
frames[0].save(P('anim.gif'), save_all=True, append_images=frames[1:], duration=[100, 200, 300, 400], loop=0, disposal=2)
frames[0].save(P('anim.webp'), save_all=True, append_images=frames[1:], duration=[100, 200, 300, 400], loop=0, lossless=True)

# ---- SVG ---------------------------------------------------------------------------------------------------------------------
open(P('src_svg.svg'), 'w', encoding='utf-8').write(
    '<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240">\n'
    '<rect x="0" y="0" width="160" height="120" fill="#ff0000"/>\n'
    '<rect x="160" y="0" width="160" height="120" fill="#00ff00" fill-opacity="0"/>\n'
    '<rect x="0" y="120" width="160" height="120" fill="#0000ff" fill-opacity="0.5"/>\n'
    '<rect x="160" y="120" width="160" height="120" fill="#ffff00"/>\n</svg>\n')
open(P('tiny_icon.svg'), 'w', encoding='utf-8').write(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" fill="#e91e63"/></svg>')
open(P('arabic_text.svg'), 'w', encoding='utf-8').write(
    '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="120"><rect width="400" height="120" fill="#ffffff"/>'
    '<text x="20" y="80" font-size="60" fill="#000000">Filo فيلو</text></svg>')

# ---- ICC (Adobe RGB 1998 like matrix profile) ----------------------------------------------------------------------------------
def s15(v):
    return struct.pack('>i', int(round(v * 65536)))


def xyz_tag(x, y, z):
    return b'XYZ ' + b'\0\0\0\0' + s15(x) + s15(y) + s15(z)


def make_matrix_profile(name, rxyz, gxyz, bxyz, gamma):
    desc = b'desc' + b'\0\0\0\0' + struct.pack('>I', len(name) + 1) + name.encode() + b'\0' + b'\0' * 8 + b'\0\0' + b'\0' + b'\0' * 67
    cprt = b'text' + b'\0\0\0\0' + b'Public domain test profile\0'
    curv = b'curv' + b'\0\0\0\0' + struct.pack('>I', 1) + struct.pack('>H', int(round(gamma * 256)))
    tags = [(b'desc', desc), (b'cprt', cprt), (b'wtpt', xyz_tag(0.9642, 1.0, 0.8249)), (b'rXYZ', xyz_tag(*rxyz)),
            (b'gXYZ', xyz_tag(*gxyz)), (b'bXYZ', xyz_tag(*bxyz)), (b'rTRC', curv), (b'gTRC', curv), (b'bTRC', curv)]
    pos = 128 + 4 + 12 * len(tags)
    table = struct.pack('>I', len(tags))
    body = b''
    for sig, data in tags:
        while (pos + len(body)) % 4:
            body += b'\0'
        table += sig + struct.pack('>II', pos + len(body), len(data))
        body += data
    while len(body) % 4:
        body += b'\0'
    size = 128 + len(table) + len(body)
    hdr = bytearray(128)
    hdr[0:4] = struct.pack('>I', size)
    hdr[8:12] = struct.pack('>I', 0x02100000)
    hdr[12:16] = b'mntr'
    hdr[16:20] = b'RGB '
    hdr[20:24] = b'XYZ '
    hdr[24:36] = struct.pack('>HHHHHH', 2024, 1, 1, 0, 0, 0)
    hdr[36:40] = b'acsp'
    hdr[68:80] = s15(0.9642) + s15(1.0) + s15(0.8249)
    return bytes(hdr) + table + body


adobe = make_matrix_profile('Adobe RGB Test', (0.60974, 0.31111, 0.01947), (0.20528, 0.62567, 0.06087), (0.14919, 0.06322, 0.74457), 2.19921875)
open(P('adobe_test.icc'), 'wb').write(adobe)
patch = Image.new('RGB', (64, 64), (200, 100, 50))
patch.save(P('icc_adobe.png'), icc_profile=adobe)
patch.save(P('icc_adobe.jpg'), quality=98, subsampling=0, icc_profile=adobe)
ap = ImageCms.ImageCmsProfile(io.BytesIO(adobe))
sp = ImageCms.createProfile('sRGB')
conv = ImageCms.profileToProfile(patch, ap, sp, outputMode='RGB')
json.dump({'src': [200, 100, 50], 'srgb': list(conv.getpixel((10, 10)))}, open(P('icc_expected.json'), 'w'))

# ---- CMYK jpeg ---------------------------------------------------------------------------------------------------------------
cm = np.zeros((H, W, 4), np.uint8)
cm[:H // 2, :W // 2] = (0, 255, 255, 0)      # red
cm[:H // 2, W // 2:] = (255, 0, 255, 0)      # green
cm[H // 2:, :W // 2] = (255, 255, 0, 0)      # blue
cm[H // 2:, W // 2:] = (0, 0, 255, 0)        # yellow
cmi = Image.fromarray(cm, 'CMYK')
cmi.save(P('cmyk.jpg'), quality=95)
Image.open(P('cmyk.jpg')).convert('RGB').save(P('cmyk_pil_rgb.png'))

# ---- photo-like images for compression -----------------------------------------------------------------------------------------
rng = np.random.default_rng(7)
PW, PH = 1600, 1200
yy, xx = np.mgrid[0:PH, 0:PW]
base = np.stack([128 + 100 * np.sin(xx / 170.0) * np.cos(yy / 230.0), 128 + 90 * np.sin((xx + yy) / 260.0), 120 + 110 * np.cos(xx / 310.0 - yy / 190.0)], axis=2)
base += rng.normal(0, 9, base.shape)
# a few hard-edged shapes
base[300:520, 200:700] = (30, 60, 160)
base[700:1000, 900:1400] = (220, 200, 40)
photo = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), 'RGB')
photo.save(P('photo.jpg'), quality=96, subsampling=0, exif=exif_for(1, PW, PH).tobytes())
photo.save(P('photo.png'))
photo.save(P('photo.webp'), quality=95)
photo.save(P('photo.avif'), quality=90)
photo.save(P('photo.tiff'), compression='tiff_lzw')

# flat graphic (few colours): lossless palette friendly
gr = Image.new('RGB', (800, 600), (255, 255, 255))
from PIL import ImageDraw
d = ImageDraw.Draw(gr)
for i in range(0, 800, 40):
    d.line([(i, 0), (800 - i, 600)], fill=(20 + i % 200, 60, 120), width=3)
d.rectangle([100, 100, 400, 300], fill=(200, 30, 30), outline=(0, 0, 0), width=4)
d.ellipse([450, 200, 700, 450], fill=(30, 160, 60), outline=(0, 0, 0), width=4)
gr.save(P('graphic.png'))
gradient = np.stack([np.tile(np.linspace(0, 255, 800), (600, 1)), np.tile(np.linspace(255, 0, 600)[:, None], (1, 800)), np.full((600, 800), 128.0)], axis=2)
Image.fromarray(gradient.astype(np.uint8), 'RGB').save(P('gradient.png'))

# ---- QR codes (OpenCV's encoder = independent of ZXing) ----------------------------------------------------------------------------
import cv2


def qr_image(text, module=8, quiet=4):
    enc = cv2.QRCodeEncoder.create()
    m = enc.encode(text)              # uint8 0/255, one pixel per module (+ border handled below)
    m = np.where(m > 127, 255, 0).astype(np.uint8)
    m = np.pad(m, quiet, constant_values=255)
    m = np.kron(m, np.ones((module, module), np.uint8))
    return Image.fromarray(m, 'L').convert('RGB')


URL = 'https://example.com/filo?x=1&y=two'
TXT = 'Hello Filo QR'
ARB = 'مرحبا فيلو'
qr_image(URL).save(P('qr_url.png'))
qr_image(TXT).save(P('qr_text.png'))
try:
    qr_image(ARB).save(P('qr_arabic.png'))
except Exception as e:  # encoder lacks UTF-8 support in this build
    print('arabic qr skipped', e)
a, b = qr_image(URL, 6), qr_image(TXT, 6)
canvas = Image.new('RGB', (a.width + b.width + 120, max(a.height, b.height) + 80), (255, 255, 255))
canvas.paste(a, (40, 40))
canvas.paste(b, (a.width + 80, 40))
canvas.save(P('qr_two.png'))
r = qr_image(URL, 6).rotate(23, expand=True, fillcolor=(255, 255, 255), resample=Image.Resampling.BICUBIC)
r.save(P('qr_rotated.png'))
qr_image(URL, 6).transpose(Image.Transpose.FLIP_LEFT_RIGHT).save(P('qr_mirrored.png'))
qa = qr_image(URL, 6)
qa.save(P('qr_url.jpg'), quality=70)
Image.fromarray(rng.integers(0, 255, (300, 300, 3), dtype=np.uint8), 'RGB').save(P('noise.png'))
# QR stored sideways with EXIF orientation 6 (the code is only readable in the upright, displayed orientation)
qexif = Image.Exif()
qexif[274] = 6
qa.transpose(Image.Transpose.ROTATE_90).save(P('qr_exif6.jpg'), quality=92, exif=qexif.tobytes())
# QR small inside a big busy photo-like image
bigscene = Image.fromarray(np.clip(rng.normal(150, 40, (1500, 2000, 3)), 0, 255).astype(np.uint8), 'RGB')
bigscene.paste(qr_image(URL, 4), (1300, 700))
bigscene.save(P('qr_in_scene.jpg'), quality=90)
# inverted (light on dark)
ImageOps.invert(qr_image(TXT, 8)).save(P('qr_inverted.png'))
# transparent PNG: black QR modules on a fully transparent background
tq = np.asarray(qr_image(TXT, 8).convert('L'))
ta = np.zeros((tq.shape[0], tq.shape[1], 4), np.uint8)
ta[tq < 128] = (0, 0, 0, 255)
Image.fromarray(ta, 'RGBA').save(P('qr_transparent.png'))
# QR drawn inside an SVG (as <rect>s)
m = np.asarray(qr_image(TXT, 1, 0).convert('L')) < 128
rects = ''.join(f'<rect x="{x + 4}" y="{y + 4}" width="1" height="1"/>' for y in range(m.shape[0]) for x in range(m.shape[1]) if m[y, x])
open(P('qr_text.svg'), 'w').write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {m.shape[1] + 8} {m.shape[0] + 8}" width="{(m.shape[1] + 8) * 10}" height="{(m.shape[0] + 8) * 10}"><rect x="0" y="0" width="100%" height="100%" fill="white"/><g fill="black" shape-rendering="crispEdges">{rects}</g></svg>')
# ---- more edge-case sources ----------------------------------------------------------------------------------------------------------
photo.resize((4000, 3000), Image.Resampling.BILINEAR).save(P('big_12mp.jpg'), quality=88)
g16 = np.zeros((60, 80), np.uint16)
g16[:, :40] = 0x2000
g16[:, 40:] = 0xC000
Image.fromarray(g16, 'I;16').save(P('gray16.png'))
pages = [D.convert('RGB'), Image.new('RGB', (320, 240), (10, 10, 10))]
pages[0].save(P('multipage.tiff'), save_all=True, append_images=pages[1:])
open(P('svg_percent_only.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="100%" height="100%" viewBox="0 0 200 100"><rect width="200" height="100" fill="#336699"/></svg>')
open(P('svg_units.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="2in" height="1in"><rect width="100%" height="100%" fill="#cc3333"/></svg>')
open(P('svg_px.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="48px" height="24px"><rect width="48" height="24" fill="#33cc33"/></svg>')
open(P('svg_external.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100"><rect width="100" height="100" fill="#ffffff"/><image xlink:href="http://127.0.0.1:9/never.png" width="100" height="100"/><script>alert(1)</script><circle cx="50" cy="50" r="30" fill="#0000ff"/></svg>')
open(P('svg_broken.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect width="100" height="100" fill="red"')
open(P('svg_huge.svg'), 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="30000" height="15000"><rect width="30000" height="15000" fill="#222222"/></svg>')

# ---- odd sizes (must never be distorted) -------------------------------------------------------------------------------------------
oddsrc = Image.fromarray(np.asarray(master(False).resize((321, 241), Image.Resampling.NEAREST)), 'RGBA')
oddsrc.save(P('odd_321x241.png'))
Image.new('RGB', (1, 1), (10, 200, 30)).save(P('one_px.png'))
wide = Image.new('RGB', (4000, 30), (200, 30, 30))
wide.save(P('wide_4000x30.png'))

# ---- AVIF with an 'irot' property (ISO-BMFF surgery; PIL cannot write rotations) ---------------------------------------------------
def box_find(b, s, e, t):
    p = s
    while p + 8 <= e:
        sz, tt = struct.unpack('>I4s', b[p:p + 8])
        if tt == t:
            return p, sz
        p += sz
    return None


def add_property(src, dst, prop):
    b = bytearray(open(src, 'rb').read())
    mp, msz = box_find(b, 0, len(b), b'meta')
    ip, isz = box_find(b, mp + 12, mp + msz, b'iprp')
    cp, csz = box_find(b, ip + 8, ip + isz, b'ipco')
    ap, asz = box_find(b, ip + 8, ip + isz, b'ipma')
    lp, lsz = box_find(b, mp + 12, mp + msz, b'iloc')
    pp, psz = box_find(b, mp + 12, mp + msz, b'pitm')
    n = 0
    p = cp + 8
    while p < cp + csz:
        n += 1
        p += struct.unpack('>I', b[p:p + 4])[0]
    pid = struct.unpack('>H', b[pp + 12:pp + 14])[0] if b[pp + 8] == 0 else struct.unpack('>I', b[pp + 12:pp + 16])[0]
    ver = b[ap + 8]
    flags = int.from_bytes(b[ap + 9:ap + 12], 'big')
    cnt = struct.unpack('>I', b[ap + 12:ap + 16])[0]
    q = ap + 16
    found = None
    for _ in range(cnt):
        iid = struct.unpack('>H', b[q:q + 2])[0] if ver == 0 else struct.unpack('>I', b[q:q + 4])[0]
        q2 = q + (2 if ver == 0 else 4)
        k = b[q2]
        endq = q2 + 1 + k * (2 if flags & 1 else 1)
        if iid == pid:
            found = (q2, k, endq)
            break
        q = endq
    q2, k, endq = found
    newassoc = struct.pack('>H', 0x8000 | (n + 1)) if flags & 1 else bytes([0x80 | (n + 1)])
    delta = len(prop) + len(newassoc)
    # iloc offsets (absolute, method 0) that point behind the meta box move by delta
    ver_l = b[lp + 8]
    offsz, lensz = b[lp + 12] >> 4, b[lp + 12] & 15
    basesz, idxsz = b[lp + 13] >> 4, (b[lp + 13] & 15) if ver_l in (1, 2) else 0
    r = lp + 14
    icount = struct.unpack('>H', b[r:r + 2])[0] if ver_l < 2 else struct.unpack('>I', b[r:r + 4])[0]
    r += 2 if ver_l < 2 else 4
    meta_end = mp + msz
    for _ in range(icount):
        r += 2 if ver_l < 2 else 4
        method = 0
        if ver_l in (1, 2):
            method = struct.unpack('>H', b[r:r + 2])[0] & 15
            r += 2
        r += 2
        bpos = r
        base = int.from_bytes(b[r:r + basesz], 'big') if basesz else 0
        r += basesz
        ec = struct.unpack('>H', b[r:r + 2])[0]
        r += 2
        for _e in range(ec):
            if idxsz:
                r += idxsz
            off = int.from_bytes(b[r:r + offsz], 'big')
            if method == 0 and base + off >= meta_end:
                if basesz:
                    b[bpos:bpos + basesz] = (base + delta).to_bytes(basesz, 'big')
                    base += delta
                else:
                    b[r:r + offsz] = (off + delta).to_bytes(offsz, 'big')
            r += offsz + lensz
    out = bytearray(b)
    out[endq:endq] = newassoc
    out[q2] = k + 1
    out[cp + csz:cp + csz] = prop

    def addsz(pos, d):
        out[pos:pos + 4] = struct.pack('>I', struct.unpack('>I', out[pos:pos + 4])[0] + d)

    addsz(cp, len(prop))
    addsz(ap + len(prop), len(newassoc))
    addsz(ip, delta)
    addsz(mp, delta)
    open(dst, 'wb').write(out)


D.convert('RGB').save(P('plain.avif'), quality=95)
add_property(P('plain.avif'), P('irot90ccw.avif'), struct.pack('>I4sB', 9, b'irot', 1))
check = Image.open(P('irot90ccw.avif'))
print('irot avif opens in PIL as', check.size)
print('ok')
