"""Independent PDF inspector (PyMuPDF). python -I check_pdf.py <pdf> [<original.jpg>]
Prints one JSON object: page count, page sizes (pt), repaired flag, images per page with their embedded format and, if a JPEG
is given, whether the embedded DCT stream is byte-identical to that file."""
import json
import sys

try:
    import pymupdf as fitz
except ImportError:
    import fitz

pdf = sys.argv[1]
orig = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 else None
info = {'ok': False}
try:
    doc = fitz.open(pdf)
    info['pages'] = doc.page_count
    info['repaired'] = bool(doc.is_repaired)
    info['sizes'] = [[round(p.rect.width, 2), round(p.rect.height, 2)] for p in doc]
    imgs = []
    identical = None
    for p in doc:
        for im in p.get_images(full=True):
            xref = im[0]
            ex = doc.extract_image(xref)
            imgs.append({'ext': ex.get('ext'), 'w': ex.get('width'), 'h': ex.get('height'), 'cs': ex.get('colorspace'), 'smask': im[1] != 0, 'bpc': ex.get('bpc')})
            if orig is not None and ex.get('ext') == 'jpeg':
                identical = ex['image'] == orig
    info['images'] = imgs
    info['jpeg_identical'] = identical
    info['producer'] = doc.metadata.get('producer')
    info['ok'] = True
except Exception as e:  # noqa
    info['error'] = repr(e)
print(json.dumps(info))
