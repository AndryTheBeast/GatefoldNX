"""A local Subsonic fixture; never connects to an external server."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

salts = set()


def response(body):
    return ('<subsonic-response xmlns="http://subsonic.org/restapi" status="ok">'
            + body + '</subsonic-response>').encode()


class Server(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urlsplit(self.path)
        q = {key: values[0] for key, values in parse_qs(url.query).items()}
        assert q['u'] == 'user & é'
        assert 'p' not in q
        assert len(q['s']) >= 6 and q['s'] not in salts
        salts.add(q['s'])
        assert q['t'] == hashlib.md5(('secret+&é' + q['s']).encode()).hexdigest()
        assert q['f'] == 'xml' and q['v'] == '1.16.1' and q['c'] == 'Songate'
        method = url.path.rsplit('/', 1)[-1]
        if url.path.startswith('/redirect/'):
            self.send_response(302)
            self.send_header('Location', '/must-not-follow')
            self.end_headers()
            return
        if url.path.startswith('/bad/'):
            data = b'<subsonic-response status="failed"><error code="40" message="secret+&amp;\xc3\xa9"/></subsonic-response>'
        elif url.path.startswith('/malformed/'):
            data = b'<html>not Subsonic</html>'
        elif url.path.startswith('/dtd/'):
            data = b'<!DOCTYPE subsonic-response [<!ENTITY x SYSTEM "file:///etc/passwd">]><subsonic-response status="ok"><albumList2>&x;</albumList2></subsonic-response>'
        elif url.path.startswith('/missing/'):
            data = response('')
        elif method == 'getAlbumList2.view':
            offset = int(q['offset'])
            if url.path.startswith('/repeated/'):
                data = response('<albumList2><album id="same"/><album id="same"/></albumList2>')
            else:
                assert offset in (0, 500) and q['size'] == '500'
                ids = range(offset, min(offset + 500, 501))
                data = response('<albumList2>' + ''.join(f'<album id="{i}"/>' for i in ids) + '</albumList2>')
        elif method == 'getAlbum.view':
            song = (f'<song id="../song &amp; {q["id"]}" title="Song &amp; café" artist="Artist" album="Album" '
                    'duration="123" track="2" genre="Rock" year="2026" coverArt="cover &amp; 1"/>')
            if q['id'] == '500':
                song = song.replace('duration="123" track="2"', 'duration="NaN" track="2147483648"')
            data = response('<album artist="Album artist">' + song + song + '<song id="video" isVideo="true"/></album>')
        elif method == 'getCoverArt.view':
            assert q['id'] == 'cover & 1' and q['size'] == '512'
            if url.path.startswith('/cover-error/'):
                self.send_response(404)
                self.end_headers()
                return
            data = b'\xff\xd8\xff\xd9'
        elif method == 'stream.view':
            assert q['id'].startswith('../song & ') and q['format'] == 'mp3' and q['maxBitRate'] == '192'
            if url.path.startswith('/audio-error/'):
                data = b'<subsonic-response status="failed"><error code="50"/></subsonic-response>'
            elif url.path.startswith('/slow/'):
                self.send_response(200)
                self.end_headers()
                try:
                    for _ in range(100):
                        self.wfile.write(b'ID3' * 1024)
                        self.wfile.flush()
                        time.sleep(0.02)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            else:
                data = b'ID3' + b'\0' * 8192
        else:
            raise AssertionError(method)
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


with ThreadingHTTPServer(('127.0.0.1', 0), Server) as server:
    threading.Thread(target=server.serve_forever, daemon=True).start()
    result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'])
    server.shutdown()
    sys.exit(result.returncode)
