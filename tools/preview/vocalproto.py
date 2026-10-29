# Gatefold NX · tools/preview/vocalproto.py (PC only, never in a Switch build)
# Automix v3, steps 2-3: where the singing is.
#   truth    the FLACs' synced lyrics (LYRICS tag, LRC [mm:ss.xx]) as the moments someone sings
#   features each FLAC decoded once (ffmpeg, 11025 Hz stereo) into per-frame features, cached (.npz)
#   train    a detector trained on part of the tracks, scored on the rest per 0.5 s window and per bar
# Usage: python -I vocalproto.py truth|features|train  (FLAC_ROOT, CACHE environment variables)
import os, re, struct, sys, subprocess, json, math
import numpy as np

ROOT = os.environ.get('FLAC_ROOT', r'C:\Users\andre\Desktop\FLACs')
CACHE = os.environ.get('CACHE', os.path.join(os.path.dirname(__file__), '..', '..', 'automix-demo', 'vocals2'))
FFMPEG = os.path.join(os.path.dirname(__file__), 'deps', 'ffmpeg', 'bin', 'ffmpeg.exe')
SR, N, HOP = 11025, 1024, 512          # as mixan: ~11 kHz; a 1024-point spectrum every 512 samples (46 ms)
TS = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\]')


def vorbis(path):
    with open(path, 'rb') as f:
        if f.read(4) != b'fLaC':
            return {}
        out = {}
        while True:
            h = f.read(4)
            if len(h) < 4:
                break
            last, typ, n = h[0] & 0x80, h[0] & 0x7f, int.from_bytes(h[1:4], 'big')
            if typ != 4:
                f.seek(n, 1)
            else:
                b = f.read(n)
                vl = struct.unpack('<I', b[:4])[0]
                p = 4 + vl
                cnt = struct.unpack('<I', b[p:p + 4])[0]
                p += 4
                for _ in range(cnt):
                    l = struct.unpack('<I', b[p:p + 4])[0]
                    p += 4
                    k, _, v = b[p:p + l].partition(b'=')
                    out.setdefault(k.upper().decode('ascii', 'replace'), v.decode('utf-8', 'replace'))
                    p += l
            if last:
                break
        return out


def lrc(text):
    """[(start s, line text)] in time order."""
    out = []
    for raw in text.splitlines():
        stamps = TS.findall(raw)
        words = TS.sub('', raw).strip()
        for m, s in stamps:
            out.append((int(m) * 60 + float(s), words))
    return sorted(out)


def sung(lines, dur):
    """Spans someone sings: each line from its start to the next line's, but no longer than the line could take
    (1 s + 0.09 s a character, at most 8 s): the last line of a verse does not run into the solo after it.
    Also the uncertain edges: a span's estimated end +- 1 s is not scored."""
    spans = []
    for i, (t, w) in enumerate(lines):
        if not w or w in ('♪', '...'):
            continue
        nxt = lines[i + 1][0] if i + 1 < len(lines) else dur
        est = 1.0 + 0.09 * len(w)
        end = min(nxt, t + min(est, 8.0))
        spans.append((t, end, end < nxt - 0.5))   # True: the end is estimated, not the next line's start
    return spans


def tracks():
    for d in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, d)
        if not os.path.isdir(p):
            continue
        for fn in sorted(os.listdir(p)):
            if fn.lower().endswith('.flac'):
                yield d, os.path.join(p, fn)


def key(path):
    import hashlib
    return hashlib.md5(path.encode('utf-8')).hexdigest()[:16]


def truth():
    os.makedirs(CACHE, exist_ok=True)
    rows = []
    for d, path in tracks():
        t = vorbis(path)
        lines = lrc(t.get('LYRICS', '') or t.get('UNSYNCEDLYRICS', ''))
        if len([1 for _, w in lines if w]) < 5:
            continue
        rows.append({'playlist': d, 'path': path, 'key': key(path), 'lines': lines})
    with open(os.path.join(CACHE, 'truth.json'), 'w', encoding='utf-8') as f:
        json.dump(rows, f, ensure_ascii=False)
    per = {}
    for r in rows:
        per[r['playlist']] = per.get(r['playlist'], 0) + 1
    print(len(rows), 'tracks with synced lyrics', per)


# ---- features: what mixan could compute on the console (one more spectrum per hop, of the side)
MEL = None


def bands():
    """24 bands, mel-spaced 100 Hz-5 kHz, as bin ranges of the 1024-point spectrum at 11025 Hz."""
    def mel(f): return 2595 * math.log10(1 + f / 700)
    def imel(m): return 700 * (10 ** (m / 2595) - 1)
    edges = [imel(mel(100) + (mel(5000) - mel(100)) * i / 24) for i in range(25)]
    b = [int(round(e * N / SR)) for e in edges]
    out = []
    for i in range(24):
        lo, hi = b[i], max(b[i + 1], b[i] + 1)
        out.append((lo, hi))
    return out


def decode(path):
    r = subprocess.run([FFMPEG, '-v', 'error', '-i', path, '-ac', '2', '-ar', str(SR), '-f', 's16le', '-'], capture_output=True)
    x = np.frombuffer(r.stdout, dtype=np.int16).astype(np.float32) / 32768
    return x.reshape(-1, 2)


def frame_features(x):
    """Per hop: 24 log mel bands of the centre (M), and in 300 Hz-3.4 kHz: the centre's level over the side's (dB),
    the centre's spectral flatness, and its 'peakiness' (energy in bins above both neighbours: harmonics)."""
    m = (x[:, 0] + x[:, 1]) * 0.5
    s = (x[:, 0] - x[:, 1]) * 0.5
    nf = max(0, (len(m) - N) // HOP)
    w = np.hanning(N + 1)[:-1].astype(np.float32)
    idx = np.arange(N)[None, :] + HOP * np.arange(nf)[:, None]
    M = np.abs(np.fft.rfft(m[idx] * w, axis=1)).astype(np.float32)
    S = np.abs(np.fft.rfft(s[idx] * w, axis=1)).astype(np.float32)
    lo, hi = int(300 * N / SR), int(3400 * N / SR)
    pm, ps = (M[:, lo:hi] ** 2).sum(1), (S[:, lo:hi] ** 2).sum(1)
    centre = 10 * np.log10((pm + 1e-9) / (ps + 1e-9))
    seg = M[:, lo:hi] + 1e-7
    flat = np.exp(np.log(seg).mean(1)) / seg.mean(1)
    peaks = (seg[:, 1:-1] > seg[:, :-2]) & (seg[:, 1:-1] > seg[:, 2:])
    peaky = ((seg[:, 1:-1] ** 2) * peaks).sum(1) / (seg ** 2).sum(1)
    mel = np.stack([np.log10((M[:, a:b] ** 2).sum(1) + 1e-9) for a, b in bands()], 1)
    side = np.stack([np.log10((S[:, a:b] ** 2).sum(1) + 1e-9) for a, b in bands()], 1)
    lvl = 10 * np.log10(pm + 1e-9)
    # v2: the side's bands too (the centre over the side, band by band: lead vocals sit dead centre)
    return np.concatenate([mel, centre[:, None], flat[:, None], peaky[:, None], lvl[:, None], mel - side], 1).astype(np.float32)


def one(row):
    out = os.path.join(CACHE, row['key'] + '.npz')
    if os.path.exists(out):
        return
    x = decode(row['path'])
    np.savez_compressed(out, f=frame_features(x), dur=len(x) / SR)


def features():
    from concurrent.futures import ProcessPoolExecutor
    rows = json.load(open(os.path.join(CACHE, 'truth.json'), encoding='utf-8'))
    with ProcessPoolExecutor(max_workers=12) as ex:
        for i, _ in enumerate(ex.map(one, rows)):
            if i % 50 == 0:
                print(i, flush=True)
    print('done', len(rows))


# ---- training and scoring
WIN = 0.5            # a window (s); its features: the frames' mean and spread over the second around it
FPS = SR / HOP       # frames a second


def windows(row):
    """Per window: features (mean and spread of each frame feature over 1 s), the label (1 sung, 0 not, -1 not scored)."""
    z = np.load(os.path.join(CACHE, row['key'] + '.npz'))
    f, dur = z['f'], float(z['dur'])
    nw = int(dur / WIN)
    t = (np.arange(nw) + 0.5) * WIN
    X, y = [], np.full(nw, -1, np.int8)
    half = int(FPS * 0.5)
    for i, c in enumerate(t):
        j = int(c * FPS)
        seg = f[max(0, j - half):j + half + 1]
        if len(seg) < 4:
            X.append(np.zeros(f.shape[1] * 4, np.float32))
            continue
        wide = f[max(0, j - 4 * half):j + 4 * half + 1]   # 4 s around it: the phrase, not only the moment
        X.append(np.concatenate([seg.mean(0), seg.std(0), wide.mean(0), wide.std(0)]))
    X = np.array(X, np.float32)
    lvl = X[:, 27]   # the mids' level (dB): quiet windows are not scored
    loud = lvl > np.median(lvl) - 20
    spans = sung(row['lines'], dur)
    first = row['lines'][0][0] if row['lines'] else 0
    for i, c in enumerate(t):
        if not loud[i]:
            continue
        lab, sure = 0, True
        for a, b, est in spans:
            if a - 0.25 <= c <= b + (1.0 if est else 0.25):
                if c < a + 0.25 or (est and c > b - 1.0) or c > b:
                    sure = False
                else:
                    lab = 1
                break
        if c < first - 0.25 or True:
            pass
        y[i] = lab if sure else -1
    # each feature against the track's own spread: a song's sung parts against its own instrumental parts
    mu, sd = X.mean(0), X.std(0) + 1e-6
    return (X - mu) / sd, y, t


def split(rows):
    import hashlib
    tr, te = [], []
    for r in rows:
        (te if int(hashlib.md5(r['key'].encode()).hexdigest(), 16) % 10 < 3 else tr).append(r)
    return tr, te


def stack(rows):
    Xs, ys, ids = [], [], []
    for k, r in enumerate(rows):
        X, y, _ = windows(r)
        Xs.append(X)
        ys.append(y)
        ids.append(np.full(len(y), k))
    return np.concatenate(Xs), np.concatenate(ys), np.concatenate(ids)


def scores(p, y, ids, thr=0.5, block=4):
    """Per window and per block of `block` windows (2 s, about a bar at 120 BPM): precision, recall, F1, accuracy."""
    out = {}
    m = y >= 0
    pr, yy = p[m] >= thr, y[m] == 1
    tp, fp, fn = (pr & yy).sum(), (pr & ~yy).sum(), (~pr & yy).sum()
    out['window'] = (tp / max(1, tp + fp), tp / max(1, tp + fn), (pr == yy).mean())
    bp, by = [], []
    for k in np.unique(ids):
        s = ids == k
        pk, yk = p[s], y[s]
        for a in range(0, len(pk) - block + 1, block):
            yb = yk[a:a + block]
            if (yb < 0).any():
                continue
            by.append(yb.mean() >= 0.5)
            bp.append(pk[a:a + block].mean() >= thr)
    bp, by = np.array(bp), np.array(by)
    tp, fp, fn = (bp & by).sum(), (bp & ~by).sum(), (~bp & by).sum()
    out['block'] = (tp / max(1, tp + fp), tp / max(1, tp + fn), (bp == by).mean())
    out['share'] = yy.mean()
    return out


def auc(s, y):
    m = y >= 0
    s, y = s[m], y[m] == 1
    o = np.argsort(s)
    r = np.empty(len(s))
    r[o] = np.arange(len(s))
    n1, n0 = y.sum(), (~y).sum()
    return (r[y].sum() - n1 * (n1 - 1) / 2) / (n1 * n0)


NAMES = [f'mel{i}' for i in range(24)] + ['centre', 'flat', 'peaky', 'level']


def fit_logistic(X, y, l2=1e-3, steps=300):
    w, b = np.zeros(X.shape[1]), 0.0
    m = y >= 0
    X, t = X[m], (y[m] == 1).astype(np.float64)
    lr = 0.5
    for _ in range(steps):
        p = 1 / (1 + np.exp(-(X @ w + b)))
        g = p - t
        w -= lr * (X.T @ g / len(t) + l2 * w)
        b -= lr * g.mean()
    return w, b


def fit_mlp(X, y, h=16, epochs=12, seed=1):
    rng = np.random.default_rng(seed)
    m = y >= 0
    X, t = X[m].astype(np.float32), (y[m] == 1).astype(np.float32)
    W1 = rng.normal(0, 1 / np.sqrt(X.shape[1]), (X.shape[1], h)).astype(np.float32)
    b1 = np.zeros(h, np.float32)
    W2 = rng.normal(0, 1 / np.sqrt(h), h).astype(np.float32)
    b2 = np.float32(0)
    params = [W1, b1, W2, b2]
    mom = [np.zeros_like(p) for p in params]
    vel = [np.zeros_like(p) for p in params]
    lr, bs, step = 2e-3, 512, 0
    for ep in range(epochs):
        order = rng.permutation(len(t))
        for a in range(0, len(t), bs):
            i = order[a:a + bs]
            x, tt = X[i], t[i]
            hh = np.maximum(0, x @ W1 + b1)
            p = 1 / (1 + np.exp(-(hh @ W2 + b2)))
            g = (p - tt) / len(tt)
            gW2, gb2 = hh.T @ g, g.sum()
            gh = np.outer(g, W2) * (hh > 0)
            gW1, gb1 = x.T @ gh + 1e-4 * W1, gh.sum(0)
            step += 1
            for k, gr in enumerate([gW1, gb1, gW2, gb2]):
                mom[k] = 0.9 * mom[k] + 0.1 * gr
                vel[k] = 0.999 * vel[k] + 0.001 * gr * gr
                upd = lr * (mom[k] / (1 - 0.9 ** step)) / (np.sqrt(vel[k] / (1 - 0.999 ** step)) + 1e-8)
                if k == 3:
                    b2 = np.float32(b2 - upd)
                else:
                    params[k] -= upd
        W1, b1, W2 = params[0], params[1], params[2]
    return W1, b1, W2, b2


def mlp(X, net):
    W1, b1, W2, b2 = net
    return 1 / (1 + np.exp(-(np.maximum(0, X @ W1 + b1) @ W2 + b2)))


def show(name, p, y, ids):
    s = scores(p, y, ids)
    print(f"{name:28s} AUC {auc(p, y):.3f}  window P {s['window'][0]:.2f} R {s['window'][1]:.2f} acc {s['window'][2]:.2f}"
          f"  block P {s['block'][0]:.2f} R {s['block'][1]:.2f} acc {s['block'][2]:.2f}")


def train():
    rows = json.load(open(os.path.join(CACHE, 'truth.json'), encoding='utf-8'))
    tr, te = split(rows)
    Xtr, ytr, itr = stack(tr)
    Xte, yte, ite = stack(te)
    print(f'train {len(tr)} tracks, {int((ytr >= 0).sum())} windows scored; test {len(te)} tracks, {int((yte >= 0).sum())} windows, '
          f'{(yte[yte >= 0] == 1).mean():.2f} of them sung')
    # single measures, as the earlier attempts (one number per window, the track's own z-score)
    for k in (24, 25, 26):
        show(NAMES[k] + ' (mean)', 1 / (1 + np.exp(-Xte[:, k])), yte, ite)
        show('-' + NAMES[k] + ' (mean)', 1 / (1 + np.exp(Xte[:, k])), yte, ite)
    w, b = fit_logistic(Xtr, ytr)
    show(f'logistic, {Xtr.shape[1]} features', 1 / (1 + np.exp(-(Xte @ w + b))), yte, ite)
    net = fit_mlp(Xtr, ytr)
    pte = mlp(Xte, net)
    show(f'MLP {Xtr.shape[1]}-16-1', pte, yte, ite)
    np.savez(os.path.join(CACHE, 'mlp.npz'), W1=net[0], b1=net[1], W2=net[2], b2=net[3])
    # per playlist
    pl = np.array([te[k]['playlist'] for k in ite])
    for name in sorted(set(r['playlist'] for r in te)):
        s = pl == name
        show('  ' + name.encode('ascii', 'replace').decode()[:26], pte[s], yte[s], ite[s])


if __name__ == '__main__':
    {'truth': truth, 'features': features, 'train': train}[sys.argv[1]]()
