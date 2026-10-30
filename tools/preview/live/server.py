#!/usr/bin/env python3
"""Gatefold NX live test environment: the PC preview, in real time, streamed to a web page.

    python tools/preview/live/server.py            (or preview_start "gatefold-live" from .claude/launch.json)

Runs tools/preview/out/gatefold-preview.exe (copied to out-live/run, so a new build can be made while it runs) with
GF_STREAM, on a card of its own (out-live/card: sdmc/Music is a junction to the music folder, GF_PROTECT keeps it
read-only; the config is the card's own). The page (index.html) shows its frames, plays its sound and sends back
touches, the buttons and the stick. A new build in tools/preview/out restarts it by itself.

Dev only: nothing here is in the Switch build. Standard library only.
  GF_LIVE_MUSIC  the music folder (default D:\\Music)    GF_LIVE_MIX  its folder made an Automix playlist (Melatonin)
  GF_LIVE_PORT   the page's port (8790; the app talks to 8791 on 127.0.0.1)
"""
import asyncio
import base64
import hashlib
import json
import os
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
PREVIEW = HERE.parent
PROJECT = PREVIEW.parent.parent
OUT = PREVIEW / 'out'
LIVE = PREVIEW / 'out-live'
RUN = LIVE / 'run'
CARD = LIVE / 'card'
CFG = CARD / 'sdmc' / 'config' / 'gatefold-nx'
MUSIC = Path(os.environ.get('GF_LIVE_MUSIC', r'D:\Music'))
MIX = os.environ.get('GF_LIVE_MIX', 'Melatonin')
PORT = int(os.environ.get('GF_LIVE_PORT', '8790'))
APP_PORT = PORT + 1
AUDIO_EXT = ('.m4a', '.mp3', '.flac')


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


# ---------------------------------------------------------------- the card

def prepare_card(reset=False):
    (CARD / 'sdmc').mkdir(parents=True, exist_ok=True)
    music = CARD / 'sdmc' / 'Music'
    if not music.exists():
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(music), str(MUSIC)], check=True, capture_output=True)
    if reset and CFG.exists():
        shutil.rmtree(CFG)   # the config only: never anything through the junction
    if (CFG / 'settings.txt').exists():
        return
    CFG.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / 'card-settings.txt', CFG / 'settings.txt')
    folder = MUSIC / MIX
    if folder.is_dir():   # an Automix playlist to try joins with, ordered by Gatefold
        name = MIX + ' Mix'
        tracks = sorted(p.name for p in folder.iterdir() if p.suffix.lower() in AUDIO_EXT)
        (CFG / 'playlists.txt').write_text(name + '\t\n', encoding='utf-8', newline='\n')
        (CFG / 'lists.txt').write_text('#' + name + '\n' + ''.join(f'sdmc:/Music/{MIX}/{t}\n' for t in tracks),
                                       encoding='utf-8', newline='\n')
        (CFG / 'automix-lists.tsv').write_text(name + '\t0\t0\t\n', encoding='utf-8', newline='\n')
    log('card ready:', CFG)


# ---------------------------------------------------------------- the app

class App:
    def __init__(self):
        self.proc = None
        self.writer = None          # the app's stream connection
        self.build = ''
        self.built_at = 0.0
        self.state = 'stopped'
        self.last_frame = None      # sent to a page as it connects
        self.keyboard = None        # the keyboard open, if it is (a page that connects is asked too)
        self.wanted = True          # restart it when it closes by itself

    def stamp(self):
        exe = OUT / 'gatefold-preview.exe'
        st = exe.stat()
        return st.st_mtime, st.st_size

    def copy_build(self):
        RUN.mkdir(parents=True, exist_ok=True)
        for f in OUT.iterdir():
            if f.suffix.lower() != '.dll':
                continue
            d = RUN / f.name
            if not d.exists() or d.stat().st_size != f.stat().st_size or d.stat().st_mtime < f.stat().st_mtime:
                shutil.copy2(f, d)
        exe = OUT / 'gatefold-preview.exe'
        shutil.copy2(exe, RUN / exe.name)
        self.built_at = exe.stat().st_mtime
        md5 = hashlib.md5((RUN / exe.name).read_bytes()).hexdigest()[:8]
        self.build = time.strftime('%d %b %H:%M', time.localtime(self.built_at)) + ' · ' + md5

    async def start(self):
        if self.proc and self.proc.returncode is None:
            return
        prepare_card()
        self.copy_build()
        env = os.environ.copy()
        for k in ('GF_SHOW', 'GF_SCRIPT', 'SDL_AUDIODRIVER'):
            env.pop(k, None)
        env.update(GF_ROOT=str(CARD), GF_ROMFS=str(PROJECT / 'romfs'), GF_STREAM=f'127.0.0.1:{APP_PORT}',
                   GF_PROTECT='sdmc:/Music')
        out = open(LIVE / 'app-log.txt', 'ab')
        out.write(f'\n==== {time.ctime()} build {self.build}\n'.encode())
        out.flush()
        self.proc = await asyncio.create_subprocess_exec(str(RUN / 'gatefold-preview.exe'), cwd=str(RUN), env=env,
                                                         stdout=out, stderr=subprocess.STDOUT)
        self.wanted = True
        self.state = 'starting'
        log('app started, build', self.build)
        await hub.status()
        asyncio.ensure_future(self.watch(self.proc))

    async def watch(self, proc):
        code = await proc.wait()
        if proc is not self.proc:
            return
        log('app closed, code', code)
        self.state = 'stopped'
        self.writer = None
        self.keyboard = None
        await hub.status()
        if self.wanted and code != 0:   # a crash: say so, and do not loop
            self.state = f'crashed ({code}); see tools/preview/out-live/app-log.txt'
            await hub.status()

    async def stop(self):
        self.wanted = False
        p = self.proc
        if not p or p.returncode is not None:
            return
        if self.writer:
            try:
                self.writer.write(b'quit\n')
                await self.writer.drain()
            except Exception:
                pass
        try:
            await asyncio.wait_for(p.wait(), 8)
        except asyncio.TimeoutError:
            p.kill()
            await p.wait()

    async def restart(self, reset=False):
        await self.stop()
        if reset:
            prepare_card(reset=True)
        await self.start()

    def send(self, line):
        if self.writer:
            self.writer.write(line.encode() + b'\n')

    async def connected(self, reader, writer):
        """The app's stream: [type u8][length u32][payload], forwarded to every page as [type][payload]."""
        self.writer = writer
        self.state = 'running'
        log('app connected')
        await hub.status()
        hub.replay_settings()
        try:
            while True:
                head = await reader.readexactly(5)
                kind, n = head[0], struct.unpack('<I', head[1:])[0]
                body = await reader.readexactly(n)
                msg = bytes([kind]) + body
                if kind == 1:
                    self.last_frame = msg
                elif kind == 3:
                    self.keyboard = msg
                hub.forward(kind, msg)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        if self.writer is writer:
            self.writer = None

    async def watch_builds(self):
        """A new build in tools/preview/out (still for two looks, so the linker is done) restarts the app."""
        seen = None
        html = (HERE / 'index.html').stat().st_mtime
        while True:
            await asyncio.sleep(2)
            if (HERE / 'index.html').stat().st_mtime != html:   # a new page: open ones reload
                html = (HERE / 'index.html').stat().st_mtime
                await hub.status()
            try:
                s = self.stamp()
            except OSError:
                continue
            if s[0] <= self.built_at:
                seen = None
                continue
            if seen != s:
                seen = s
                continue
            log('new build: restarting')
            await self.restart()
            await hub.status(updated=True)
            seen = None


# ---------------------------------------------------------------- pages (WebSocket)

class Page:
    def __init__(self, writer):
        self.writer = writer

    def buffered(self):
        t = self.writer.transport
        return t.get_write_buffer_size() if t and not t.is_closing() else 1 << 30

    def send(self, data, text=False):
        n = len(data)
        head = bytes([0x81 if text else 0x82])
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack('>H', n)
        else:
            head += bytes([127]) + struct.pack('>Q', n)
        try:
            self.writer.write(head + data)
        except Exception:
            pass


class Hub:
    def __init__(self):
        self.pages = set()
        self.settings = {}   # the stream's quality and frame rate, sent to every new app

    def forward(self, kind, msg):
        for p in list(self.pages):
            b = p.buffered()
            if kind == 1 and b > 300_000:      # that page is behind: it misses this frame
                continue
            if kind == 2 and b > 4_000_000:
                continue
            p.send(msg)

    def info(self, updated=False):
        page = page_version()   # a page that differs reloads itself
        return json.dumps({'state': app.state, 'build': app.build, 'updated': updated, 'pages': len(self.pages), 'page': page,
                           'settings': self.settings})

    async def status(self, updated=False):
        s = self.info(updated).encode()
        for p in list(self.pages):
            p.send(s, text=True)

    def replay_settings(self):
        for k, v in self.settings.items():
            app.send(f'{k} {v}')

    async def message(self, page, text):
        for line in text.split('\n'):
            if not line:
                continue
            if line[0] == '!':
                cmd = line[1:]
                if cmd == 'restart':
                    await app.restart()
                elif cmd == 'reset':
                    await app.restart(reset=True)
                elif cmd == 'start':
                    await app.start()
                continue
            if line[0] in 'kK' and app.keyboard:   # answered: every other page closes its keyboard
                app.keyboard = None
                for p in list(self.pages):
                    p.send(b'{"keyboardClosed": true}', text=True)
            if line[0] in "qfz" and line[1:2] == " ":
                self.settings[line[0]] = line[2:]
            app.send(line)


def page_version():
    return hashlib.md5((HERE / 'index.html').read_bytes()).hexdigest()[:8]


app = App()
hub = Hub()
TOKEN = None


def lan_addresses():
    out = set()
    try:
        for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            out.add(a[4][0])
    except OSError:
        pass
    return sorted(a for a in out if not a.startswith('127.'))


async def http(reader, writer):
    try:
        request = await reader.readuntil(b'\r\n\r\n')
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
        writer.close()
        return
    lines = request.decode('latin-1').split('\r\n')
    try:
        method, target, _ = lines[0].split(' ', 2)
    except ValueError:
        writer.close()
        return
    headers = {}
    for l in lines[1:]:
        if ':' in l:
            k, v = l.split(':', 1)
            headers[k.strip().lower()] = v.strip()
    url = urllib.parse.urlsplit(target)
    query = urllib.parse.parse_qs(url.query)
    peer = writer.get_extra_info('peername')[0]
    allowed = peer in ('127.0.0.1', '::1') or query.get('k', [''])[0] == TOKEN

    def reply(code, kind, body):
        writer.write(f'HTTP/1.1 {code}\r\nContent-Type: {kind}\r\nContent-Length: {len(body)}\r\n'
                     'Cache-Control: no-store\r\nConnection: close\r\n\r\n'.encode() + body)

    if not allowed:
        reply('403 Forbidden', 'text/plain', b'Open the link the server printed (it has ?k=...).\n')
        await writer.drain()
        writer.close()
        return
    if url.path == '/ws' and headers.get('upgrade', '').lower() == 'websocket':
        key = headers.get('sec-websocket-key', '')
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        writer.write(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                     b'Sec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
        sock = writer.get_extra_info('socket')
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await websocket(reader, writer)
        return
    if url.path in ('/', '/index.html'):
        html = (HERE / 'index.html').read_bytes()
        reply('200 OK', 'text/html; charset=utf-8', html.replace(b"'__PAGE__'", b"'" + page_version().encode() + b"'", 1))
    else:
        reply('404 Not Found', 'text/plain', b'')
    await writer.drain()
    writer.close()


async def websocket(reader, writer):
    page = Page(writer)
    hub.pages.add(page)
    log('page connected,', len(hub.pages), 'open')
    page.send(hub.info().encode(), text=True)
    if app.last_frame:
        page.send(app.last_frame)
    if app.keyboard:
        page.send(app.keyboard)
    try:
        while True:
            b0, b1 = await reader.readexactly(2)
            op, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack('>H', await reader.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', await reader.readexactly(8))[0]
            mask = await reader.readexactly(4) if b1 & 0x80 else b'\0\0\0\0'
            data = bytearray(await reader.readexactly(n))
            for i in range(n):
                data[i] ^= mask[i & 3]
            if op == 8:
                break
            if op == 9:
                writer.write(bytes([0x8A, len(data)]) + data)
            elif op == 1:
                await hub.message(page, data.decode('utf-8', 'replace'))
            if app.writer:
                await app.writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        hub.pages.discard(page)
        writer.close()
        log('page closed,', len(hub.pages), 'open')
        if not hub.pages:
            app.send('b 0')       # nothing held once nobody is there
            app.send('s 0 0')


async def main():
    global TOKEN
    LIVE.mkdir(parents=True, exist_ok=True)
    tok = LIVE / 'token.txt'
    if not tok.exists():
        tok.write_text(secrets.token_urlsafe(12))
    TOKEN = tok.read_text().strip()
    if not MUSIC.is_dir():
        sys.exit(f'no music folder at {MUSIC} (set GF_LIVE_MUSIC)')
    if not (OUT / 'gatefold-preview.exe').exists():
        sys.exit('build the preview first: FFMPEG=tools/preview/deps/ffmpeg bash tools/preview/build.sh tools/preview/deps/sdl')
    await asyncio.start_server(app.connected, '127.0.0.1', APP_PORT)
    await asyncio.start_server(http, '0.0.0.0', PORT)
    log(f'Gatefold live on http://localhost:{PORT}/')
    for a in lan_addresses():
        log(f'  from another device: http://{a}:{PORT}/?k={TOKEN}')
    await app.start()
    await app.watch_builds()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
