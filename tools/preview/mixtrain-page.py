# Gatefold NX · tools/preview/mixtrain-page.py: the rating page of a training round (mixtrain round), rate.html beside its
# demos. Blind: it shows the two songs of each demo, never which version it is or its numbers.
#   python tools/preview/mixtrain-page.py <round dir> [--claude]
# "Save the ratings" saves ratings-round-N.json (the browser's downloads) and copies it too; put it in the round's folder for
# tools/mixtrain-fit.py. --claude: Songate's own rounds, rated for Claude to read ("Send to Claude").
import html, json, os, sys

args = [a for a in sys.argv[1:] if a != '--claude']
for_claude = '--claude' in sys.argv[1:]
d = args[0]
r = json.load(open(os.path.join(d, 'round.json'), encoding='utf-8'))
n = r['round']
# the model the round trains: Songate's own rounds (V1.1.0 from rounds 1-4, V1.2.0 from round 5), or a round of your own
EYEBROW = ('Training from ' + r['base']) if r.get('base') else 'Automix ' + ('V1.1.0' if n <= 4 else 'V1.2.0') + ' · training'
BUTTON = 'Send to Claude' if for_claude else 'Save the ratings'
AFTER = ("'. Tell Claude it is sent.'" if for_claude else
                   json.dumps(". Put it in the round's folder, then run tools/mixtrain-fit.py on it."))
demos = r['demos']
e = lambda s: html.escape(str(s), quote=True)

cards = []
for i, x in enumerate(demos):
    cards.append(f'''<article class="demo" data-file="{e(x['file'])}" data-join="{x['joinAt']}">
  <div class="top"><span class="num">{i + 1:02d}</span><div class="songs"><span class="a">{e(x['aName'])}</span><span class="arrow">→</span><span class="b">{e(x['bName'])}</span></div></div>
  <div class="player">
    <button class="play" type="button" aria-label="Play demo {i + 1}"><svg viewBox="0 0 24 24" aria-hidden="true"><path class="ip" d="M8 5v14l11-7z"/><path class="iz" d="M7 5h4v14H7zM13 5h4v14h-4z"/></svg></button>
    <div class="track" role="slider" aria-label="Position in demo {i + 1}" tabindex="0"><div class="fill"></div><div class="mark" title="The join starts here"></div></div>
    <span class="time">0:00</span>
    <button class="jump" type="button">Jump to the join</button>
  </div>
  <div class="rate" role="radiogroup" aria-label="Rating for demo {i + 1}">{''.join(f'<button type="button" role="radio" aria-checked="false" data-v="{v}">{v}</button>' for v in range(11))}</div>
  <input class="note" type="text" placeholder="A note (optional): what you heard" aria-label="Note for demo {i + 1}">
  <audio preload="metadata" src="{e(x['file'])}"></audio>
</article>''')

page = f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Automix Training Round {n}</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Bricolage+Grotesque:opsz,wght@12..96,600;12..96,800&family=Atkinson+Hyperlegible:wght@400;700&family=JetBrains+Mono:wght@400;600&display=swap">
<style>
:root {{
  --bg: #f6f3f7; --card: #ffffff; --fg: #1c1720; --muted: #6b6273; --line: #e3dce8;
  --accent: #d6337f; --accent-ink: #b0205f; --soft: #f0eaf3;
  --display: "Bricolage Grotesque", "Segoe UI", system-ui, sans-serif;
  --body: "Atkinson Hyperlegible", "Segoe UI", system-ui, sans-serif;
  --mono: "JetBrains Mono", ui-monospace, Consolas, monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  --bg: #141018; --card: #1e1824; --fg: #f1ecf4; --muted: #a79caf; --line: #33293b;
  --accent: #ff5aa5; --accent-ink: #ff86bd; --soft: #261e2d; color-scheme: dark }} }}
:root[data-theme="dark"] {{ --bg: #141018; --card: #1e1824; --fg: #f1ecf4; --muted: #a79caf; --line: #33293b;
  --accent: #ff5aa5; --accent-ink: #ff86bd; --soft: #261e2d; color-scheme: dark }}
* {{ box-sizing: border-box }}
body {{ margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.55 var(--body); padding: 40px 16px 120px }}
.wrap {{ max-width: 820px; margin: 0 auto; display: grid; gap: 28px }}
header {{ display: grid; gap: 10px }}
.eyebrow {{ font: 700 12px/1 var(--body); letter-spacing: .14em; text-transform: uppercase; color: var(--accent-ink) }}
h1 {{ font: 800 clamp(30px, 6vw, 44px)/1.05 var(--display); margin: 0 }}
.lede {{ color: var(--muted); margin: 0; max-width: 64ch }}
.lede strong {{ color: var(--fg) }}
.demos {{ display: grid; gap: 16px }}
.demo {{ background: var(--card); border: 1px solid var(--line); border-radius: 14px; padding: 16px 18px; display: grid; gap: 12px }}
.demo.rated {{ border-color: color-mix(in oklab, var(--accent) 45%, var(--line)) }}
.top {{ display: flex; gap: 14px; align-items: baseline }}
.num {{ font: 600 13px/1 var(--mono); color: var(--accent-ink) }}
.songs {{ display: flex; flex-wrap: wrap; gap: 4px 8px; font-weight: 700; min-width: 0 }}
.songs .arrow {{ color: var(--muted); font-weight: 400 }}
.player {{ display: flex; align-items: center; gap: 12px; flex-wrap: wrap }}
.play {{ width: 40px; height: 40px; border-radius: 50%; border: 0; background: var(--accent); color: #fff; display: grid; place-items: center; cursor: pointer; flex: none }}
.play svg {{ width: 18px; height: 18px; fill: currentColor }}
.play .iz {{ display: none }} .play.on .ip {{ display: none }} .play.on .iz {{ display: inline }}
.track {{ position: relative; flex: 1 1 220px; height: 8px; background: var(--soft); border-radius: 4px; cursor: pointer }}
.fill {{ position: absolute; inset: 0 auto 0 0; width: 0; background: var(--accent); border-radius: 4px }}
.mark {{ position: absolute; top: -5px; width: 2px; height: 18px; background: var(--fg); opacity: .55; border-radius: 1px }}
.time {{ font: 13px var(--mono); color: var(--muted); min-width: 3.2em }}
.jump {{ font: 700 13px/1 var(--body); color: var(--accent-ink); background: transparent; border: 1px solid var(--accent); border-radius: 999px; padding: 8px 12px; cursor: pointer }}
.rate {{ display: grid; grid-template-columns: repeat(11, 1fr); gap: 6px }}
.rate button {{ font: 600 14px/1 var(--mono); padding: 10px 0; border-radius: 9px; border: 1px solid var(--line); background: var(--soft); color: var(--fg); cursor: pointer }}
.rate button[aria-checked="true"] {{ background: var(--accent); border-color: var(--accent); color: #fff }}
.rate button:focus-visible, .play:focus-visible, .jump:focus-visible, .track:focus-visible, .send:focus-visible {{ outline: 2px solid var(--accent); outline-offset: 2px }}
.note {{ font: 15px var(--body); padding: 9px 12px; border-radius: 9px; border: 1px solid var(--line); background: transparent; color: var(--fg) }}
.bar {{ position: fixed; left: 0; right: 0; bottom: 0; background: color-mix(in oklab, var(--card) 92%, transparent); border-top: 1px solid var(--line);
  backdrop-filter: blur(10px); padding: 12px 16px }}
.bar .in {{ max-width: 820px; margin: 0 auto; display: flex; gap: 14px; align-items: center; justify-content: space-between; flex-wrap: wrap }}
.count {{ font-weight: 700 }}
.status {{ color: var(--muted); font-size: 14px }}
.send {{ font: 700 15px/1 var(--body); color: #fff; background: var(--accent); border: 0; border-radius: 999px; padding: 12px 20px; cursor: pointer }}
.send:disabled {{ opacity: .45; cursor: default }}
@media (max-width: 560px) {{ .rate {{ grid-template-columns: repeat(6, 1fr) }} }}
</style></head>
<body>
<div class="wrap">
<header>
  <span class="eyebrow">{EYEBROW}</span>
  <h1>Round {n}: rate the joins</h1>
  <p class="lede">{len(demos)} demos from your library, each a join between two songs. The line on each player marks where the join
  starts (about 10 s in). Rate how good the join sounds, <strong>0 (awful) to 10 (you can't tell where one song ends)</strong>,
  the join itself, not the songs. Some joins come back more than once, made a little differently: rate each as you hear it.
  Your ratings stay in this browser as you go; <strong>{BUTTON}</strong> saves them as a file once they're done.</p>
</header>
<section class="demos">
{chr(10).join(cards)}
</section>
</div>
<div class="bar"><div class="in"><span><span class="count">0 of {len(demos)} rated</span> <span class="status"></span></span>
<button class="send" type="button" disabled>{BUTTON}</button></div></div>
<script>
const ROUND = {n}, KEY = 'gf-train-round-' + ROUND;
const demos = [...document.querySelectorAll('.demo')];
let saved = {{}};
try {{ saved = JSON.parse(localStorage.getItem(KEY) || '{{}}'); }} catch {{}}
const store = () => {{ try {{ localStorage.setItem(KEY, JSON.stringify(saved)); }} catch {{}} }};
const fmt = s => Math.floor(s / 60) + ':' + String(Math.floor(s % 60)).padStart(2, '0');
let current = null;
demos.forEach(card => {{
  const file = card.dataset.file, joinAt = +card.dataset.join;
  const audio = card.querySelector('audio'), play = card.querySelector('.play'), track = card.querySelector('.track');
  const fill = card.querySelector('.fill'), mark = card.querySelector('.mark'), time = card.querySelector('.time');
  const note = card.querySelector('.note'), buttons = [...card.querySelectorAll('.rate button')];
  const s = saved[file] || {{}};
  const show = () => {{
    buttons.forEach(b => b.setAttribute('aria-checked', String(+b.dataset.v === s.rating)));
    card.classList.toggle('rated', s.rating !== undefined);
  }};
  if (s.note) note.value = s.note;
  show();
  audio.addEventListener('loadedmetadata', () => {{ mark.style.left = (joinAt / audio.duration * 100) + '%'; }});
  audio.addEventListener('timeupdate', () => {{
    fill.style.width = (audio.currentTime / (audio.duration || 1) * 100) + '%';
    time.textContent = fmt(audio.currentTime);
  }});
  audio.addEventListener('play', () => {{ if (current && current !== audio) current.pause(); current = audio; play.classList.add('on'); }});
  audio.addEventListener('pause', () => play.classList.remove('on'));
  audio.addEventListener('ended', () => play.classList.remove('on'));
  play.addEventListener('click', () => audio.paused ? audio.play() : audio.pause());
  card.querySelector('.jump').addEventListener('click', () => {{ audio.currentTime = Math.max(0, joinAt - 3); audio.play(); }});
  const seek = x => {{ const r = track.getBoundingClientRect(); audio.currentTime = Math.max(0, Math.min(1, (x - r.left) / r.width)) * (audio.duration || 0); }};
  track.addEventListener('click', ev => seek(ev.clientX));
  track.addEventListener('keydown', ev => {{
    if (ev.key === 'ArrowRight') audio.currentTime = Math.min(audio.duration || 0, audio.currentTime + 5);
    if (ev.key === 'ArrowLeft') audio.currentTime = Math.max(0, audio.currentTime - 5);
  }});
  buttons.forEach(b => b.addEventListener('click', () => {{ s.rating = +b.dataset.v; saved[file] = s; store(); show(); update(); }}));
  note.addEventListener('input', () => {{ s.note = note.value; saved[file] = s; store(); }});
}});
const count = document.querySelector('.count'), status = document.querySelector('.status'), send = document.querySelector('.send');
function update() {{
  const done = demos.filter(c => (saved[c.dataset.file] || {{}}).rating !== undefined).length;
  count.textContent = done + ' of ' + demos.length + ' rated';
  send.disabled = done < demos.length;
}}
update();
send.addEventListener('click', async () => {{
  const out = {{ round: ROUND, ratedAt: new Date().toISOString(), ratings: demos.map(c => ({{ file: c.dataset.file,
    rating: saved[c.dataset.file].rating, note: saved[c.dataset.file].note || '' }})) }};
  const text = JSON.stringify(out, null, 2);
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], {{ type: 'application/json' }}));
  a.download = 'ratings-round-' + ROUND + '.json';
  document.body.appendChild(a); a.click(); a.remove();
  let copied = false;
  try {{ await navigator.clipboard.writeText(text); copied = true; }} catch {{}}
  status.textContent = 'Saved to your Downloads as ratings-round-' + ROUND + '.json' + (copied ? ' (and copied)' : '') + {AFTER};
}});
</script>
</body></html>
'''
open(os.path.join(d, 'rate.html'), 'w', encoding='utf-8').write(page)
print('wrote', os.path.join(d, 'rate.html'), len(demos), 'demos')
