#!/usr/bin/env python3
"""The README's product film: two record.py takes (Joy-Con only) cut into a presentation in the app's own style.

    python tools/preview/make_film.py <take1> <take1.abs> <take2> <take2.abs> <out.mp4> [--mb 9.6] [--still 12.5,40]

Takes: tour-film-1.txt (everything) and tour-film-2.txt (Automix, 58 s before the song's end, left alone through the join).
Looks like the app: its blurred cover as the background, the name in its own magical letters (Gfx::wordmark: Fraunces Black,
the gradient, the glow that breathes, the shimmer), Inter, the kicker, and its Joy-Con key caps as each button is pressed.
60 frames a second, every move sub-pixel. The sound is Seasons, whole, then the real Automix join (synced to the picture by
correlating the take with the song). --still writes PNGs of those seconds. Needs ffmpeg with libx264, Pillow, numpy.
The music is NCS's (ncs.io), credited on the end card and in the README.
"""
import io
import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
FONTS = PROJECT / 'romfs' / 'fonts'
SHOTS = PROJECT / 'docs' / 'screenshots'
SEASONS = HERE / 'deps' / 'demo-ncs' / 'sdmc' / 'Music' / 'NCS' / '121 - Seasons.flac'
W, H, FPS, RATE = 1280, 720, 60, 48000
# Seasons' palette, as the app measured it (Now playing › Palette)
ACCENT, ACCENT2, TXT, GLOW = (251, 105, 90), (233, 181, 137), (249, 242, 236), (197, 55, 45)
INK, TEXT2 = (255, 138, 120), (214, 204, 208)
FADE = 0.7    # a dissolve between scenes
BEAT = 3.0    # a chapter's title: the window small beside it, then growing to the whole screen
GROW = 1.5

SCENES = [
    # take, from, to (seconds of the take), speed (the menus move a little quicker; the music does not); the kicker, title, words;
    # cam: (seconds of the take, zoom, x, y); caps: (from, to, text), in seconds of the take from its start
    dict(kind='intro', len=6.0),
    dict(take=1, t0=11.0, t1=22.3, speed=1, kicker='Now playing', title='Every album,\nits own colours', sub='Songate takes its whole interface from the cover.',
         cam=[(0, 1, .5, .5), (3.0, 1.45, .74, .68), (9.4, 1, .5, .5)],
         caps=[(3.3, 6.2, 'Synced lyrics that follow the music'), (6.7, 9.4, 'The palette, measured from the cover')]),
    dict(take=1, t0=22.3, t1=34.6, speed=1, kicker='Canvas', title='The whole screen\nfor the music', sub='Three layouts, one button away.',
         cam=[(0, 1, .5, .5)], caps=[(3.6, 11, 'Cover, Lyrics and Minimal')]),
    dict(take=1, t0=40.6, t1=64.4, speed=1.4, kicker='Library', title='Your music,\nby its tags', sub='Never by how your folders are arranged.',
         cam=[(0, 1, .5, .5)], caps=[(4.8, 7.2, 'Albums, sorted your way'), (8, 15, 'Artists, genres and folders'), (15.5, 18.4, 'Sort, filter and jump by letter'),
                                     (19, 23.2, 'Every track has a menu')]),
    dict(take=1, t0=64.7, t1=86.3, speed=1.3, kicker='Playlists', title='Lists that\nfill themselves', sub='Your own, smart and Mood.',
         cam=[(0, 1, .5, .5)], caps=[(4.3, 6.8, 'Your own, each with its icon'), (7, 10.2, 'Move anything with the D-pad'), (11.4, 13.8, 'Smart playlists stay up to date'),
                                     (14, 16.8, 'Mood playlists sort by how songs sound'), (17.4, 21.2, 'Five ways to start a new one')]),
    dict(take=1, t0=91.6, t1=111.2, speed=1.35, kicker='Settings', title='Make it\nyours', sub='Sound, looks and controls.',
         cam=[(0, 1, .5, .5)], caps=[(4.4, 8.2, 'A ten-band equalizer'), (8.8, 12, 'A visualizer you can tune'), (12.2, 14.6, 'Updates, only if you turn them on'),
                                     (14.8, 17.6, 'Report a bug from the console')]),
    dict(take=1, t0=111.6, t1=122.0, speed=1, kicker='Guided tour', title='Songate shows\nyou around', sub='On its own screens, with your music.',
         cam=[(0, 1, .5, .5)], caps=[(3.2, 10, 'Basic, Advanced or Complete')]),
    dict(take=2, t0=6.4, t1=10.4, speed=1, kicker='Automix', title='Your songs,\nlike a DJ set', sub='Songate listens, orders and joins them.', cam=[(0, 1, .5, .5)], caps=[]),
    dict(take=2, t0=49.0, t1=65.0, speed=1, cam=[(0, 1, .5, .5)], caps=[(.7, 3.4, 'It reads each song: tempo, key, bars and bass'), (3.6, 5.7, 'Then plans the join')]),
    dict(kind='overlay', len=10.0),
    dict(kind='end', len=5.6),
]


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


F_KICK, F_TITLE, F_SUB, F_CAP = font('Inter-Bold.ttf', 15), font('Inter-ExtraBold.ttf', 46), font('Inter-Regular.ttf', 22), font('Inter-SemiBold.ttf', 20)


def clamp(x):
    return min(1.0, max(0.0, x))


def ease(x):
    """In and out, soft at both ends: for everything that moves across the screen."""
    x = clamp(x)
    return 4 * x * x * x if x < .5 else 1 - (-2 * x + 2) ** 3 / 2


def out(x):
    """Quick, then settling for a long time: for words arriving."""
    x = clamp(x)
    return 1 - (1 - x) ** 4


def rounded(w, h, r):
    m = Image.new('L', (w * 4, h * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * 4 - 1, h * 4 - 1), r * 4, fill=255)
    return m.resize((w, h), Image.LANCZOS)


MASK = rounded(W, H, 30)
SHADOW = Image.new('L', (W + 240, H + 240), 0)
SHADOW.paste(MASK, (120, 120))
SHADOW = SHADOW.filter(ImageFilter.GaussianBlur(46))


def placed(img, x, y, w, h):
    """img resized to the rectangle x, y, w, h with fractions kept (so a slow move never steps), and antialiased: the pixels
    it covers, and the part of the source each stands for."""
    ix0, iy0, ix1, iy1 = math.ceil(x), math.ceil(y), math.floor(x + w), math.floor(y + h)
    sx, sy = img.width / w, img.height / h
    box = ((ix0 - x) * sx, (iy0 - y) * sy, (ix1 - x) * sx, (iy1 - y) * sy)
    return img.resize((ix1 - ix0, iy1 - iy0), Image.BILINEAR, box=box), (ix0, iy0)


class Take:
    def __init__(self, folder, story):
        self.dir = Path(folder)
        self.idx = []
        self.f = open(self.dir / 'frames.bin', 'rb')
        while True:
            head = self.f.read(12)
            if len(head) < 12:
                break
            t, n = struct.unpack('<dI', head)
            self.idx.append((t / 1000, self.f.tell(), n))
            self.f.seek(n, 1)
        self.times = np.array([i[0] for i in self.idx])
        self.cache = {}
        info = dict(l.split() for l in (self.dir / 'info.txt').read_text().splitlines())
        self.audio_at = float(info['audio_at']) / 1000
        self.presses = []
        for l in open(story, encoding='utf-8'):
            w = l.split()
            if len(w) == 3 and w[1] == 'press':
                self.presses.append((float(w[0]), w[2]))

    def frame(self, t):
        k = int(np.searchsorted(self.times, t))
        if k and (k == len(self.times) or t - self.times[k - 1] < self.times[k] - t):
            k -= 1   # the nearest frame, so an uneven frame now and then does not show
        if k not in self.cache:
            if len(self.cache) > 12:
                self.cache.clear()
            self.f.seek(self.idx[k][1])
            self.cache[k] = Image.open(io.BytesIO(self.f.read(self.idx[k][2]))).convert('RGB')
        return self.cache[k]

    def audio(self, t0, t1):
        a = np.memmap(self.dir / 'audio.s16', dtype='<i2', mode='r')
        s0, s1 = int((t0 - self.audio_at) * RATE) * 2, int((t1 - self.audio_at) * RATE) * 2
        return np.asarray(a[max(0, s0):s1], dtype=np.float32) / 32768


def backdrop(img, dim=0.42):
    """The app's own background: the picture, blurred away."""
    b = img.resize((48, 27), Image.BILINEAR).filter(ImageFilter.GaussianBlur(3)).resize((W, H), Image.BICUBIC)
    return Image.eval(b, lambda v: int(v * dim))


def faded(layer, a):
    if a >= 1:
        return layer
    out_ = layer.copy()
    out_.putalpha(Image.eval(layer.getchannel('A'), lambda v: int(v * a)))
    return out_


_lines = {}


def line(text, f, fill):
    """A line of text as an image of its own, kept: drawn once, moved and faded after."""
    key = (text, id(f), fill)
    if key not in _lines:
        d = ImageDraw.Draw(Image.new('L', (1, 1)))
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        im = Image.new('RGBA', (int(r - l) + 8, int(f.size * 1.5)), (0, 0, 0, 0))
        ImageDraw.Draw(im).text((4 - l, int(f.size * .2)), text, font=f, fill=fill + (255,))
        _lines[key] = im
    return _lines[key]


def spaced(text, f, fill, spacing=2.6):
    key = ('sp', text, id(f), fill)
    if key not in _lines:
        d = ImageDraw.Draw(Image.new('L', (1, 1)))
        w = int(sum(d.textlength(c, font=f) + spacing for c in text)) + 8
        im = Image.new('RGBA', (w, int(f.size * 1.6)), (0, 0, 0, 0))
        di, x = ImageDraw.Draw(im), 2.0
        for c in text:
            di.text((x, 2), c, font=f, fill=fill + (255,))
            x += d.textlength(c, font=f) + spacing
        _lines[key] = im
    return _lines[key]


def reveal(frame, im, x, y, t, a=1.0):
    """A line rising out of its own baseline, as if from behind a mask; t from 0 to 1."""
    h = im.height
    p = out(t)
    off = int(round((1 - p) * h))
    if off >= h:
        return
    part = faded(im.crop((0, 0, im.width, h - off)), a)
    frame.paste(part, (x, y + off), part)


def wrapped(text, f, width):
    d = ImageDraw.Draw(Image.new('L', (1, 1)))
    lines, cur = [], ''
    for w in text.split():
        if d.textlength((cur + ' ' + w).strip(), font=f) > width:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + ' ' + w).strip()
    return lines + [cur]


def title_block(frame, kick, title, sub, t, end):
    """The chapter's words on the left, each line rising in after the last; they fade away as the window grows."""
    leave = 1 - ease((t - end) / 0.55)
    if leave <= 0:
        return
    y = 236 - int(14 * ease((t - end) / 0.55))
    reveal(frame, spaced(kick.upper(), F_KICK, INK), 64, y, (t - .1) / .8, leave)
    y += 30
    d = 0.2
    for l in title.split('\n'):
        reveal(frame, line(l, F_TITLE, (255, 255, 255)), 60, y, (t - d) / .9, leave)
        y += 56
        d += .1
    y += 12
    for l in wrapped(sub, F_SUB, 340):
        reveal(frame, line(l, F_SUB, TEXT2), 60, y, (t - d - .1) / .9, leave)
        y += 31


def caption(frame, text, a0, a1):
    """A pane of frosted glass with its words, above the player bar."""
    a = out(a0 / .45) * ease(a1 / .4)
    if a <= 0:
        return
    im = line(text, F_CAP, (255, 255, 255))
    w, h = im.width + 44, 52
    x, y = (W - w) // 2, H - 150 + int(10 * (1 - out(a0 / .5)))
    glass = frame.crop((x, y, x + w, y + h)).filter(ImageFilter.GaussianBlur(14))
    glass = Image.blend(glass, Image.new('RGB', (w, h), (26, 18, 22)), .62)
    glass.paste(im, (22, (h - im.height) // 2 + 1), im)
    m = rounded(w, h, 18)
    frame.paste(glass, (x, y), Image.eval(m, lambda v: int(v * a)))


_caps = {}


def cap(name):
    """A Joy-Con button as the tour draws it: round for A B X Y, a diamond of four for the D-pad, a solid plus or bar."""
    if name in _caps:
        return _caps[name]
    S = 4
    n = 46
    im = Image.new('RGBA', (n * S, n * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = n * S / 2
    dark = (24, 18, 22, 200)
    if name in ('ZL', 'ZR', 'L', 'R'):
        d.rounded_rectangle((1 * S, 9 * S, (n - 1) * S, (n - 9) * S), 14 * S, fill=dark)
        d.text((c, c), name, font=font('Inter-Bold.ttf', 16 * S), fill=(255, 255, 255, 245), anchor='mm')
    else:
        d.ellipse((1 * S, 1 * S, (n - 1) * S, (n - 1) * S), fill=dark)
        if name in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
            for b, dx, dy in (('UP', 0, -11), ('DOWN', 0, 11), ('LEFT', -11, 0), ('RIGHT', 11, 0)):
                r = 6 * S
                x, y = c + dx * S, c + dy * S
                d.ellipse((x - r, y - r, x + r, y + r), fill=ACCENT + (255,) if b == name else (255, 255, 255, 80))
        elif name in ('PLUS', 'MINUS'):
            d.rounded_rectangle((c - 11 * S, c - 2.5 * S, c + 11 * S, c + 2.5 * S), 2 * S, fill=(255, 255, 255, 240))
            if name == 'PLUS':
                d.rounded_rectangle((c - 2.5 * S, c - 11 * S, c + 2.5 * S, c + 11 * S), 2 * S, fill=(255, 255, 255, 240))
        else:
            d.text((c, c), name, font=font('Inter-Bold.ttf', 20 * S), fill=(255, 255, 255, 245), anchor='mm')
    _caps[name] = im.resize((n, n), Image.LANCZOS)
    return _caps[name]


def pressed_caps(frame, take, t, t0, speed):
    """The latest presses above the player bar, each coming and going softly."""
    show = [(p, n) for p, n in take.presses if t0 <= p <= t and (t - p) / speed < 1.0][-4:]
    x = 28.0
    for p, n in show:
        age = (t - p) / speed
        a = out(age / .16) * (1 - ease((age - .55) / .45))
        s = 1 + .1 * (1 - out(age / .3))
        im = cap(n)
        pic, (px, py) = placed(im, x + 23 - 23 * s, H - 150 + 23 - 23 * s, 46 * s, 46 * s)
        pic = faded(pic, a)
        frame.paste(pic, (px, py), pic)
        x += 54


def camera(img, keys, t):
    z, cx, cy = keys[0][1:]
    for k0, z1, x1, y1 in keys[1:]:
        e = ease((t - k0) / 1.5)
        z, cx, cy = z + (z1 - z) * e, cx + (x1 - cx) * e, cy + (y1 - cy) * e
    if z <= 1.0005:
        return img
    w, h = W / z, H / z
    x = min(W - w, max(0, cx * W - w / 2))
    y = min(H - h, max(0, cy * H - h / 2))
    return img.resize((W, H), Image.BILINEAR, box=(x, y, x + w, y + h))


def windowed(img, bg, scale, cx):
    """The app as a window on its own blurred background."""
    fr = bg.copy()
    w, h = W * scale, H * scale
    x, y = cx - w / 2, (H - h) / 2
    sh, pos = placed(SHADOW, x - 120 * scale, y - 120 * scale + 10, (W + 240) * scale, (H + 240) * scale)
    fr.paste((0, 0, 0), pos + (pos[0] + sh.width, pos[1] + sh.height), Image.eval(sh, lambda v: int(v * .6)))
    pic, pos = placed(img, x, y, w, h)
    m, _ = placed(MASK, x, y, w, h)
    fr.paste(pic, pos, m)
    return fr


def scene_frame(s, t, takes):
    """One frame of a recording scene, t seconds of film into it (it runs on past its end under the dissolve)."""
    take = takes[s['take']]
    rec = s['t0'] + t * s['speed']
    rel = rec - s['t0']
    img = camera(take.frame(rec), s['cam'], rel)
    if 'title' in s and t < BEAT:
        g = ease((t - BEAT + GROW) / GROW)
        drift = 1 + .018 * clamp(t / (BEAT - GROW))   # never quite still
        scale, cx = .6 * drift + (1 - .6 * drift) * g, 856 + (640 - 856) * g
        fr = windowed(img, s['bg'], scale, cx)
        title_block(fr, s['kicker'], s['title'], s['sub'], t, BEAT - GROW - .1)
    else:
        fr = img.copy()
    for c0, c1, text in s.get('caps', []):
        if c0 <= rel <= c1:
            caption(fr, text, (rel - c0) / s['speed'], (c1 - rel) / s['speed'])
    pressed_caps(fr, take, rec, s['t0'], s['speed'])
    return fr


# ---- the name, as the app draws it (Gfx::wordmark and shell.cpp's colours), large

def mix(p, q, t):
    return tuple(p[i] + (q[i] - p[i]) * t for i in range(3))


class Word:
    def __init__(self, px, pad=90):
        f = ImageFont.truetype(str(PROJECT / 'tools' / 'brand' / 'Fraunces.ttf'), px * 3)
        f.set_variation_by_axes([36, 900, 100, 1])   # opsz, wght, SOFT, WONK (tools/brand/wordmark.py)
        l, t, r, b = f.getbbox('Songate')
        big = Image.new('L', (r - l + 60, b - t + 60), 0)
        ImageDraw.Draw(big).text((30 - l, 30 - t), 'Songate', font=f, fill=255)
        big = big.crop(big.getbbox())
        word = big.resize((big.width // 3, big.height // 3), Image.LANCZOS)
        self.w, self.h, self.pad = word.width, word.height, pad
        canvas = Image.new('L', (self.w + 2 * pad, self.h + 2 * pad), 0)
        canvas.paste(word, (pad, pad))
        self.a = np.asarray(canvas, dtype=np.float32) / 255
        glow = np.asarray(canvas.filter(ImageFilter.GaussianBlur(px * .2)), dtype=np.float32) / 255
        self.glow = glow / glow.max()
        tight = np.asarray(canvas.filter(ImageFilter.GaussianBlur(px * .05)), dtype=np.float32) / 255
        self.tight = tight / tight.max()
        # the gradient across: a → b → c, its middle stop at 0.45: "Song" in the accents, "gate" turning light
        a, b, c = mix(ACCENT, TXT, .12), mix(ACCENT2, TXT, .2), mix(ACCENT2, TXT, .78)
        ts = (np.arange(self.a.shape[1]) - pad) / self.w
        self.u = ts
        col = [np.where(ts < .45, a[i] + (b[i] - a[i]) * (ts / .45), b[i] + (c[i] - b[i]) * ((ts - .45) / .55)) for i in range(3)]
        self.col = np.stack(col, 1)[None, :, :].astype(np.float32)

    def draw(self, frame, cx, cy, unfurl, shine, breathe, alpha):
        """On an RGB image: the letters unfurling from the left (0..1), the shimmer at shine (−.25..1.25), the glow."""
        x, y = int(round(cx - self.a.shape[1] / 2)), int(round(cy - self.a.shape[0] / 2))
        box = (x, y, x + self.a.shape[1], y + self.a.shape[0])
        px = np.asarray(frame.crop(box), dtype=np.float32)
        # unfurled from the left, by a soft edge
        edge = np.clip((unfurl * 1.3 - .15 - self.u) / .15, 0, 1)[None, :]
        edge = edge * edge * (3 - 2 * edge)
        band = None
        if -.5 < shine < 1.5:
            d = (self.u - shine) / .16
            band = (np.exp(-4.5 * d * d) * (1 - d * d) * (np.abs(d) < 1))[None, :].astype(np.float32)
        ga = (.34 + .2 * breathe + (0.2 * math.sin(clamp(shine) * math.pi) if band is not None else 0)) * alpha
        glow = self.glow * edge * ga * 1.15
        px += (np.array(GLOW, dtype=np.float32) - px) * glow[:, :, None]
        tight = self.tight * edge * ga * .28
        px += (np.array(ACCENT, dtype=np.float32) - px) * tight[:, :, None]
        col = self.col
        if band is not None:
            col = col + (255 - col) * (band * .85)[:, :, None]
        a = (self.a * edge * alpha)[:, :, None]
        px += (col - px) * a
        frame.paste(Image.fromarray(np.clip(px, 0, 255).astype(np.uint8)), (x, y))


def sparkle(frame, x, y, size, a):
    """The app's four-pointed star, twinkling."""
    if a <= 0 or size < 1:
        return
    n = int(size * 2) | 1
    c = np.linspace(-1, 1, n, dtype=np.float32)
    xx, yy = np.meshgrid(c, c)
    m = np.clip(1 - (np.abs(xx) ** .55 + np.abs(yy) ** .55), 0, 1) ** 1.2 * 2.2
    m = np.clip(m, 0, 1) * a
    box = (int(x - n / 2), int(y - n / 2), int(x - n / 2) + n, int(y - n / 2) + n)
    px = np.asarray(frame.crop(box), dtype=np.float32)
    px += (255 - px) * m[:, :, None]
    frame.paste(Image.fromarray(px.astype(np.uint8)), box[:2])


def name_scene(fr, word, t, cy):
    """The app's arrival: unfurled from the left, the shimmer as it lands, the glow breathing, two sparkles after the light."""
    unfurl = ease(t / 1.5)
    shine = -.25 + 1.5 * (t - 1.0) / 1.5 if 1.0 <= t < 2.5 else -1
    breathe = .5 + .5 * math.sin(t * 2 * math.pi / 4.5)
    word.draw(fr, W / 2, cy, unfurl, shine, breathe, ease(t / .5))
    right = W / 2 + word.w / 2
    for at, x, y, s in ((2.15, right - 8, cy - word.h * .42, 26), (2.4, right - word.w * .36, cy - word.h * .56, 15)):
        k = (t - at) / .9
        if 0 < k < 1:
            sparkle(fr, x, y, s * math.sin(k * math.pi), math.sin(k * math.pi))


def overlay_scene(t, bg, shots, length):
    fr = bg.copy()
    end = length - 1.0
    F = font('Inter-ExtraBold.ttf', 40)
    leave = 1 - ease((t - end) / .6)
    if leave > 0:
        reveal(fr, spaced('BACKGROUND PLAYER', F_KICK, INK), 64, 214, (t - .1) / .8, leave)
        y, d = 244, .2
        for l in ('In every game,', 'on HOME, always'):
            reveal(fr, line(l, F, (255, 255, 255)), 60, y, (t - d) / .9, leave)
            y += 50
            d += .1
        y += 12
        for l in ('The music keeps going after you', 'close Songate. The overlay runs', 'it without leaving your game.'):
            reveal(fr, line(l, F_SUB, TEXT2), 60, y, (t - d) / .9, leave)
            y += 31
            d += .06
    # three shots at a time, each rising in after the last
    half = length / 2
    n = 0 if t < half else 1
    for i, im in enumerate(shots[n]):
        at = (.3 if n == 0 else half) + i * .14
        stop = half if n == 0 else length
        e = out((t - at) / .9) * (1 - ease((t - stop + .55) / .5))
        if e <= 0:
            continue
        pic, pos = placed(im, 470 + i * 262, 84 + 36 * (1 - out((t - at) / .9)), im.width, im.height)
        m, _ = placed(im_mask(im), 470 + i * 262, 84 + 36 * (1 - out((t - at) / .9)), im.width, im.height)
        fr.paste(pic, pos, Image.eval(m, lambda v: int(v * e)))
    return fr


_m = {}


def im_mask(im):
    if im.size not in _m:
        _m[im.size] = rounded(im.size[0], im.size[1], 18)
    return _m[im.size]


def end_scene(t, bg, word):
    fr = bg.copy()
    name_scene(fr, word, t, 296)
    im = line('Free and open source, for the Nintendo Switch', F_SUB, TEXT2)
    reveal(fr, im, (W - im.width) // 2, 372, (t - 1.2) / 1.0)
    s = font('Inter-Regular.ttf', 16)
    a = ease((t - 1.8) / .8)
    if a > 0:
        for text, y in (('Music from NCS (NoCopyrightSounds), ncs.io', 540),
                        ('Rival & CADMIUM ft. Harley Bird: Seasons  ·  Jim Yosef & ROY KNOX: Sun Goes Down  ·  Diviners ft. Harley Bird: Falling', 566)):
            im = faded(line(text, s, (190, 182, 186)), a)
            fr.paste(im, ((W - im.width) // 2, y), im)
    return fr


def scene_start(k):
    return sum(s['len'] for s in SCENES[:k])


def build_audio(takes, out_path):
    """Seasons from the file, unbroken, synced to the first scenes; then the real join from the second take."""
    flac = np.frombuffer(subprocess.run(['ffmpeg', '-v', 'error', '-i', str(SEASONS), '-f', 's16le', '-ar', str(RATE), '-ac', '2', '-'],
                                        check=True, capture_output=True).stdout, dtype='<i2').astype(np.float32) / 32768
    # where the first scene stands in the song: the take's sound found in the file
    probe = takes[1].audio(13, 21)
    p, q = probe[0::2][::4], flac[0::2][::4]
    n = len(q) + len(p)
    c = np.fft.irfft(np.fft.rfft(q, n) * np.conj(np.fft.rfft(p, n)), n)
    at = int(np.argmax(c[:len(q)])) * 4 / RATE - (13 - SCENES[1]['t0'])   # the song's second at the first scene's start
    print('Seasons at', round(at, 3))
    join = next(k for k, s in enumerate(SCENES) if s.get('take') == 2 and 'title' not in s)
    start = at - scene_start(1)
    head = flac[int(start * RATE) * 2:int((start + scene_start(join) + .8) * RATE) * 2]
    tail = takes[2].audio(SCENES[join]['t0'], SCENES[join]['t0'] + sum(s['len'] for s in SCENES[join:]) + .3)
    tail = tail * (np.sqrt(np.mean(head ** 2)) / np.sqrt(np.mean(tail[:RATE * 8] ** 2)))   # as loud as what came before
    x = int(.8 * RATE) * 2
    ramp = np.sin(np.linspace(0, np.pi / 2, x, dtype=np.float32)) ** 2
    mix_ = np.concatenate([head[:-x], head[-x:] * (1 - ramp) + tail[:x] * ramp, tail[x:]])
    fade = int(1.5 * RATE) * 2
    mix_[:fade] *= np.sin(np.linspace(0, np.pi / 2, fade, dtype=np.float32)) ** 2
    end = int(3.5 * RATE) * 2
    mix_[-end:] *= np.cos(np.linspace(0, np.pi / 2, end, dtype=np.float32)) ** 2
    mix_ = np.tanh(mix_ / max(1e-3, np.abs(mix_).max()) * 1.05) * .92
    (mix_ * 32767).astype('<i2').tofile(out_path)


def main():
    a = sys.argv
    takes = {1: Take(a[1], a[2]), 2: Take(a[3], a[4])}
    out_file = Path(a[5])
    mb = float(a[a.index('--mb') + 1]) if '--mb' in a else 9.6
    stills = [float(x) for x in a[a.index('--still') + 1].split(',')] if '--still' in a else None
    for s in SCENES:
        if 'take' in s:
            s['len'] = (s['t1'] - s['t0']) / s['speed']
            s['bg'] = backdrop(takes[s['take']].frame(s['t0'] + 1.5))
    total = sum(s['len'] for s in SCENES)
    bg = backdrop(Image.open(SHOTS / 'now-playing.webp').convert('RGB'))
    word, small = Word(150), Word(120)
    sh = lambda n: Image.open(SHOTS / (n + '.webp')).convert('RGB').resize((246, 398), Image.LANCZOS)
    shots = [[sh('overlay-player'), sh('overlay-games'), sh('overlay-settings')], [sh('overlay-browse'), sh('overlay-playlists'), sh('overlay-presets')]]

    def render(k, t):
        s = SCENES[k]
        if s.get('kind') == 'intro':
            fr = bg.copy()
            name_scene(fr, word, t, 322)
            im = line('A music player for the Nintendo Switch', F_SUB, TEXT2)
            reveal(fr, im, (W - im.width) // 2, 414, (t - 1.5) / 1.0)
            return fr
        if s.get('kind') == 'overlay':
            return overlay_scene(t, bg, shots, s['len'])
        if s.get('kind') == 'end':
            return end_scene(t, bg, small)
        return scene_frame(s, t, takes)

    def at(T):
        t0 = 0
        for k, s in enumerate(SCENES):
            if T < t0 + s['len'] or k == len(SCENES) - 1:
                t = T - t0
                fr = render(k, t)
                if k and t < FADE:
                    # a dissolve, both pictures still moving: the old one easing forward a little, the new one settling
                    e = ease(t / FADE)
                    prev = render(k - 1, SCENES[k - 1]['len'] + t)
                    z = 1 + .022 * e
                    w, h = W / z, H / z
                    prev = prev.resize((W, H), Image.BILINEAR, box=((W - w) / 2, (H - h) / 2, (W + w) / 2, (H + h) / 2))
                    z = 1.018 - .018 * e
                    w, h = W / z, H / z
                    fr = fr.resize((W, H), Image.BILINEAR, box=((W - w) / 2, (H - h) / 2, (W + w) / 2, (H + h) / 2))
                    fr = Image.blend(prev, fr, e)
                if T > total - 1.2:
                    fr = Image.eval(fr, lambda v: int(v * ease((total - T) / 1.2)))
                return fr
            t0 += s['len']

    if stills:
        for T in stills:
            at(T).save(out_file.with_name('%s-%05.1f.png' % (out_file.stem, T)))
        print('total', round(total, 1))
        return
    wav = out_file.with_suffix('.s16')
    build_audio(takes, wav)
    mezz = out_file.with_suffix('.mezz.mkv')
    enc = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(FPS), '-i', '-',
                            '-f', 's16le', '-ar', str(RATE), '-ac', '2', '-i', str(wav), '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '10',
                            '-pix_fmt', 'yuv420p', '-c:a', 'pcm_s16le', '-shortest', str(mezz)], stdin=subprocess.PIPE)
    for i in range(int(total * FPS)):
        enc.stdin.write(at(i / FPS).tobytes())
        if i % 600 == 0:
            print(i, flush=True)
    enc.stdin.close()
    enc.wait()
    kbps = int(mb * 8192 / total) - 112
    base = ['ffmpeg', '-v', 'error', '-y', '-i', str(mezz), '-c:v', 'libx264', '-preset', 'slower', '-tune', 'animation', '-b:v', f'{kbps}k', '-pix_fmt', 'yuv420p',
            '-profile:v', 'high', '-g', '300', '-movflags', '+faststart', '-passlogfile', str(out_file.with_suffix('.pass'))]
    subprocess.run(base + ['-pass', '1', '-an', '-f', 'null', 'NUL'], check=True)
    subprocess.run(base + ['-pass', '2', '-c:a', 'aac', '-b:a', '112k', str(out_file)], check=True)
    print(out_file, round(out_file.stat().st_size / 1048576, 2), 'MB', round(total, 1), 's', kbps, 'kbps')


if __name__ == '__main__':
    main()
