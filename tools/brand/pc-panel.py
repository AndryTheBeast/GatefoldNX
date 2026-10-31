"""The installer's last page: the PC icon on the icon's own dark, for platform/windows/ (Inno Setup picks the size nearest the screen's scaling)."""
import os
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'platform', 'windows')
ico = Image.open(os.path.join(ROOT, 'songate.ico'))
ico.size = max(ico.ico.sizes())
ico.load()
icon = ico.convert('RGBA')
dark = icon.getpixel((icon.width // 2, icon.height // 8))[:3]
for name, (w, h) in {'songate-panel.bmp': (164, 314), 'songate-panel-2x.bmp': (410, 797)}.items():
    panel = Image.new('RGB', (w, h), (13, 12, 20))
    side = w * 3 // 5
    i = icon.resize((side, side), Image.LANCZOS)
    panel.paste(i, ((w - side) // 2, (h - side) // 2), i)
    panel.save(os.path.join(ROOT, name))
print('dark', dark)
