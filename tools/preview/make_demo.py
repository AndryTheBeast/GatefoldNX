"""Gatefold NX preview: a small fake SD card with synthesised tracks and covers.

Usage: python tools/preview/make_demo.py <covers-folder> <out-root>
Writes <out-root>/sdmc/music/Artist/Album/NN Title.wav (+ cover.jpg), using the
JPEGs in <covers-folder> (Gatefold's tests/fixtures/covers work well).
"""
import math, os, random, shutil, struct, sys, wave

covers_dir, root = sys.argv[1], sys.argv[2]
covers = sorted(f for f in os.listdir(covers_dir) if f.lower().endswith('.jpg'))
RATE = 22050

ALBUMS = [
    ('Nina Chuba', 'Lips Shut', 2019, ['Lips Shut', 'Neon Rain', 'Glasshouse']),
    ('Kai Engel', 'Calls and Echoes', 2018, ['Moonlight Reprise', 'Interception', 'Brooks', 'Snowfall']),
    ('Tigerberry', 'Cold Wave', 2020, ['Cold Wave', 'Afterglow', 'Static Bloom']),
    ('Broke For Free', 'Slam Funk', 2012, ['Night Owl', 'Something Elated', 'Covered in Oil']),
    ('rolemusic', 'The Black Dot', 2014, ['The Black Kitty', 'Leafless Quince Tree', 'Pokemon']),
]

def tone(path, seed, seconds=40):
    rnd = random.Random(seed)
    base = rnd.choice([110, 130.8, 146.8, 164.8, 98])
    chord = [1, 1.25, 1.5, 2, 3]
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        frames = bytearray()
        for i in range(RATE * seconds):
            t = i / RATE
            beat = (t * 2) % 1
            env = math.exp(-beat * 4) * 0.6 + 0.25 + 0.15 * math.sin(t * 0.3 + seed)
            swell = 0.55 + 0.45 * math.sin(t * 0.21 + seed) ** 2
            v = sum(math.sin(2 * math.pi * base * c * t + k) / (k + 1) for k, c in enumerate(chord))
            v += 0.25 * math.sin(2 * math.pi * base * 8 * t) * math.exp(-beat * 9)
            v += (rnd.random() - 0.5) * 0.3 * math.exp(-beat * 14)
            frames += struct.pack('<h', int(max(-1, min(1, v * env * swell * 0.35)) * 32767))
        w.writeframes(bytes(frames))

music = os.path.join(root, 'sdmc', 'music')
for ai, (artist, album, year, titles) in enumerate(ALBUMS):
    d = os.path.join(music, artist, album)
    os.makedirs(d, exist_ok=True)
    shutil.copy(os.path.join(covers_dir, covers[ai % len(covers)]), os.path.join(d, 'cover.jpg'))
    for ti, title in enumerate(titles):
        p = os.path.join(d, '%02d %s.wav' % (ti + 1, title))
        if not os.path.exists(p):
            tone(p, ai * 10 + ti)
    if ai == 0:
        with open(os.path.join(d, '01 Lips Shut.lrc'), 'w', encoding='utf-8') as f:
            lines = ['Lights go down on the boulevard', 'I keep my lips shut, keep it hard', 'Every colour bleeding through',
                     'Every echo sounds like you', '', 'Turn it up and let it ring', 'Neon hearts on everything',
                     'Hold the note until it breaks', 'Hold the night for both our sakes']
            for i, l in enumerate(lines):
                f.write('[%02d:%05.2f]%s\n' % ((i * 4) // 60, (i * 4) % 60, l))
print('demo library in', music)
