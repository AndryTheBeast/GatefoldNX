#!/usr/bin/env python3
"""Records the PC preview: its real frames and real sound, driven by a timed storyboard (the README's tour video).

    python tools/preview/record.py <card-root> <storyboard.txt> <out-folder>

Speaks the live environment's protocol (streamshim.cpp): runs tools/preview/out/gatefold-preview.exe with GF_STREAM,
asks for every frame at high quality, and saves <out>/frames.bin ([time f64][length u32][JPEG]…), <out>/audio.s16 and
<out>/info.txt (rate, channels, when the sound began). Dev only. Then make_film.py cuts takes into the README's film.

A storyboard has lines of "<seconds from the start> <command>", # starts a comment:
    tap <x> <y>        a finger down and up           hold <x> <y> <seconds>   a long touch
    press <button>     A B X Y L R ZL ZR PLUS MINUS LEFT RIGHT UP DOWN        wheel <x> <y> <notches>
    key <text>         the keyboard's answer          mark <name>   notes this moment in <out>/marks.txt
    end                stops the recording
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
PORT = 8799
BUTTONS = {'A': 0, 'B': 1, 'X': 2, 'Y': 3, 'L': 6, 'R': 7, 'ZL': 8, 'ZR': 9, 'PLUS': 10, 'MINUS': 11,
           'LEFT': 12, 'UP': 13, 'RIGHT': 14, 'DOWN': 15}


def reader(conn, out, info):
    frames = open(out / 'frames.bin', 'wb')
    audio = open(out / 'audio.s16', 'wb')
    buf = bytearray()
    while True:
        try:
            data = conn.recv(1 << 20)
        except OSError:
            break
        if not data:
            break
        buf += data
        at = 0
        while len(buf) - at >= 5:
            kind, n = buf[at], struct.unpack_from('<I', buf, at + 1)[0]
            if len(buf) - at < 5 + n:
                break
            t = struct.unpack_from('<d', buf, at + 5)[0]
            body = bytes(buf[at + 13:at + 5 + n])
            if kind == 1:
                frames.write(struct.pack('<dI', t, len(body)))
                frames.write(body)
                info['frames'] = info.get('frames', 0) + 1
            elif kind == 2:
                if 'rate' not in info:
                    info['rate'] = struct.unpack_from('<I', body, 0)[0]
                    info['channels'] = body[4]
                    info['audio_at'] = t
                audio.write(body[5:])
            at += 5 + n
        del buf[:at]
    frames.close()
    audio.close()


def main():
    card, story, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', PORT))
    srv.listen(1)
    env = os.environ.copy()
    for k in ('GF_SHOW', 'GF_SCRIPT', 'SDL_AUDIODRIVER'):
        env.pop(k, None)
    env.update(GF_ROOT=card, GF_ROMFS=str(PROJECT / 'romfs'), GF_STREAM=f'127.0.0.1:{PORT}', SDL_AUDIODRIVER='dummy')
    app = subprocess.Popen([str(HERE / 'out' / 'gatefold-preview.exe')], cwd=str(HERE / 'out'), env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    conn, _ = srv.accept()
    conn.sendall(b'q 92\nf 1\nz 100\n')
    info = {}
    th = threading.Thread(target=reader, args=(conn, out, info), daemon=True)
    th.start()
    send = lambda s: conn.sendall((s + '\n').encode('utf-8'))
    t0 = time.monotonic()
    marks = []
    for raw in story.read_text(encoding='utf-8').splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        when, cmd = line.split(None, 1)
        wait = float(when) - (time.monotonic() - t0)
        if wait > 0:
            time.sleep(wait)
        w = cmd.split()
        if w[0] == 'tap':
            send(f'd {w[1]} {w[2]}')
            time.sleep(0.05)
            send(f'u {w[1]} {w[2]}')
        elif w[0] == 'hold':
            send(f'd {w[1]} {w[2]}')
            time.sleep(float(w[3]))
            send(f'u {w[1]} {w[2]}')
        elif w[0] == 'press':
            send(f'b {1 << BUTTONS[w[1]]}')
            time.sleep(0.12)
            send('b 0')
        elif w[0] == 'wheel':
            send(f'p {w[1]} {w[2]}')
            send(f'w {w[1]} {w[2]} {w[3]}')
        elif w[0] == 'key':
            send('k ' + cmd.split(None, 1)[1])
        elif w[0] == 'mark':
            marks.append(f'{time.monotonic() - t0:.2f} {w[1]}')
        elif w[0] == 'end':
            break
    (out / 'marks.txt').write_text('\n'.join(marks) + '\n', encoding='utf-8')
    send('quit')
    try:
        app.wait(10)
    except subprocess.TimeoutExpired:
        app.kill()
    th.join(5)
    (out / 'info.txt').write_text(''.join(f'{k} {v}\n' for k, v in info.items()), encoding='utf-8')
    print(info)


if __name__ == '__main__':
    main()
