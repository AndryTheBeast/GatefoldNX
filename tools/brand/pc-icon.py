"""Songate's icon for the PC: the app's own mark (the gate and its note, icons.inc's ICON_BRAND_ARCH and ICON_BRAND_NOTE) in the neon
blue to red of the console's icon, with no console in it. Writes platform/windows/songate.ico and the installer's small picture.

    python tools/brand/pc-icon.py          (Pillow, PyMuPDF)
"""
import io
import os

import fitz
import numpy as np
from PIL import Image, ImageChops, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'platform', 'windows')
S = 1024


def svg(body):
    doc = fitz.open('svg', f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 100 100">{body}</svg>'.encode())
    pix = doc[0].get_pixmap(alpha=True)
    return Image.open(io.BytesIO(pix.tobytes('png'))).convert('RGBA')


def painted(mask, stops, ax, ay):
    """A shape in white (MuPDF's SVG has no gradients) coloured along the direction (ax, ay), its stops as (place, r, g, b)."""
    yy, xx = np.mgrid[0:S, 0:S] / (S - 1)
    t = (xx * ax + yy * ay) / (ax + ay)
    rgb = np.stack([np.interp(t, [s[0] for s in stops], [s[c] for s in stops]) for c in (1, 2, 3)], axis=2)
    out = np.dstack([rgb, np.asarray(mask.getchannel('A'), dtype=float)])
    return Image.fromarray(np.uint8(np.round(out)), 'RGBA')


NEON = [(0.18, 22, 214, 255), (0.45, 106, 123, 255), (0.66, 255, 63, 192), (0.86, 255, 45, 74)]
# the mark, a little inside the tile
PLACE = 'transform="translate(11.5 10.5) scale(0.77)"'
ARCH = f'<g {PLACE}><path d="M16 91V47a34 34 0 0 1 68 0V91" fill="none" stroke="#ffffff" stroke-width="17.5" stroke-linecap="round"/></g>'
NOTE = (f'<g {PLACE}><circle cx="43.5" cy="72.5" r="11.5" fill="#ffffff"/>'
        '<path d="M51 72V40.5l13.5 6" fill="none" stroke="#ffffff" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"/></g>')

tile = painted(svg('<rect x="3" y="3" width="94" height="94" rx="21" fill="#ffffff"/>'), [(0, 16, 26, 66), (1, 5, 7, 15)], 1, 1)
arch = painted(svg(ARCH), NEON, 1, 0.55)
note = svg(NOTE)
glow = arch.filter(ImageFilter.GaussianBlur(S * 0.035))
glow.putalpha(glow.getchannel('A').point(lambda a: int(a * 0.75)))

art = tile.copy()
art.alpha_composite(glow)
art.putalpha(ImageChops.multiply(art.getchannel('A'), tile.getchannel('A')))   # the glow stays on the tile
art.alpha_composite(arch)
art.alpha_composite(note)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
art.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, 'songate.ico'), sizes=[(s, s) for s in sizes])
# the wizard's corner: on white, as its page is
small = Image.new('RGBA', (S, S), (255, 255, 255, 255))
small.alpha_composite(art)
small.convert('RGB').resize((165, 165), Image.LANCZOS).save(os.path.join(OUT, 'songate-wizard.bmp'))
print('written')
