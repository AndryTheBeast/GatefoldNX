"""What a training round's ratings say (self-serve training, 2026-10-30): for anyone training an Automix model of their own.

    python tools/mixtrain-fit.py <round folder> [earlier round folders…] [--settled N] [--share 0.75] [--gain 1]

Each round folder holds round.json (mixtrain build or mixtrain round) and the ratings saved from its page
(ratings-round-N.json). Every version is compared with its own join's reference (some joins are easier to mix than others,
so never across joins), kind of join by kind, and the numbers it changed are reported with the notes written about it.
Across all the rounds given, a number is SETTLED when it was tried on at least N joins (4), was better on at least that
share of them (three quarters) and gained at least that much on average (+1). Nothing is changed: the report goes to the
screen and to fit-round-N.md in the newest round's folder, and the settled numbers to candidate-round-N.txt, ready for
`mixtrain freeze` (edit it first if you disagree: it is your model).
"""
import glob, json, os, sys
from collections import defaultdict

TUNE_KEYS = None   # the numbers a version can change (from the rounds' own records)


def load(folder):
    r = json.load(open(os.path.join(folder, 'round.json'), encoding='utf-8'))
    rated = {}
    for f in sorted(glob.glob(os.path.join(folder, 'ratings-*.json'))):
        data = json.load(open(f, encoding='utf-8'))
        items = data.get('ratings', data)
        for x in (items if isinstance(items, list) else [dict(file=k, **(v if isinstance(v, dict) else {'rating': v})) for k, v in items.items()]):
            if x.get('rating') is not None:
                rated[x['file']] = (float(x['rating']), (x.get('note') or '').strip())
    return r, rated


def changes_of(demo, ref):
    """What a version changed: its own record (mixtrain build), or its numbers against its reference's (mixtrain round)."""
    if 'changes' in demo:
        return {k: str(v) for k, v in demo['changes'].items()}
    out = {}
    for k, v in demo['tune'].items():
        if ref['tune'].get(k) != v:
            out[k] = (str(v).lower() if isinstance(v, bool) else ('%g' % v if isinstance(v, float) else str(v)))
    return out


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')   # (the report has arrows; Windows' console is not UTF-8 by default)
    args, opts = [], {'--settled': 4, '--share': 0.75, '--gain': 1.0}
    it = iter(sys.argv[1:])
    for a in it:
        if a in opts:
            opts[a] = type(opts[a])(next(it))
        else:
            args.append(a)
    if not args:
        sys.exit(__doc__)
    rounds = [(f,) + load(f) for f in args]
    rounds.sort(key=lambda x: x[1]['round'])
    newest_folder, newest, _ = rounds[-1]
    lines = []
    say = lambda s='': lines.append(s)
    across = defaultdict(list)       # (kind, number, value) → [(gain, round, join)]
    together = defaultdict(list)     # (kind, the whole change) → gains
    base_file = newest.get('baseFile', '')
    for folder, r, rated in rounds:
        demos = r['demos']
        by_join = defaultdict(dict)
        for d in demos:
            by_join[d['join']][d['version']] = d
        scores = [rated[d['file']][0] for d in demos if d['file'] in rated]
        if not scores:
            say(f"Round {r['round']} ({folder}): no ratings found (save them from its page into this folder)")
            continue
        avg = lambda v: sum(v) / len(v) if v else float('nan')
        per_version = [avg([rated[d['file']][0] for d in demos if d['version'] == v and d['file'] in rated]) for v in (0, 1, 2)]
        say(f"## Round {r['round']}: {len(scores)} of {len(demos)} demos rated, average {avg(scores):.2f} "
            f"(reference {per_version[0]:.2f}, versions {per_version[1]:.2f} / {per_version[2]:.2f})")
        kinds = defaultdict(list)
        for j, vs in sorted(by_join.items()):
            kinds[vs[0]['kind']].append((j, vs))
        for kind, js in sorted(kinds.items()):
            say(f"\n### {kind.upper()} ({len(js)} join{'s' if len(js) > 1 else ''})")
            rows = defaultdict(list)
            for j, vs in js:
                ref = vs.get(0)
                if not ref or ref['file'] not in rated:
                    continue
                r0 = rated[ref['file']][0]
                for v in (1, 2):
                    d = vs.get(v)
                    if not d or d['file'] not in rated:
                        continue
                    ch = changes_of(d, ref)
                    label = ', '.join(f'{k} {x}' for k, x in sorted(ch.items())) or '(as the reference)'
                    g = rated[d['file']][0] - r0
                    rows[label].append((g, f"{d['aName']} -> {d['bName']}", rated[d['file']][1]))
                    together[(kind, label)].append(g)
                    for k, x in ch.items():
                        across[(kind, k, x)].append((g, r['round'], j))
                if rated[ref['file']][1]:
                    rows['(reference)'].append((0, f"{ref['aName']} -> {ref['bName']}", rated[ref['file']][1]))
            for label, gs in rows.items():
                if label == '(reference)':
                    for _, name, note in gs:
                        say(f"  reference on {name}: \"{note}\"")
                    continue
                vals = [g for g, _, _ in gs]
                better = sum(g > 0 for g in vals)
                verdict = ('better on all' if better == len(vals) else 'never worse' if better and all(g >= 0 for g in vals)
                           else 'worse on all' if all(g < 0 for g in vals) else 'mixed')
                say(f"  {label}: " + ', '.join(f'{g:+g}' for g in vals) + f"  → {verdict}, mean {avg(vals):+.2f}")
                for g, name, note in gs:
                    if note:
                        say(f"      {g:+g} on {name}: \"{note}\"")
        say()
    say(f"## Across the rounds given (settled: tried on {opts['--settled']}+ joins, better on {opts['--share']:.0%}+, mean {opts['--gain']:+g}+)")
    settled = {}
    for (kind, k, x), gs in sorted(across.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        vals = [g for g, _, _ in gs]
        share = sum(g > 0 for g in vals) / len(vals)
        mean = sum(vals) / len(vals)
        ok = len(vals) >= opts['--settled'] and share >= opts['--share'] and mean >= opts['--gain']
        state = 'SETTLED' if ok else 'not yet' if mean > 0 else 'worse'
        say(f"  {kind:10} {k} {x}: {len(vals)} joins in round(s) {sorted(set(r for _, r, _ in gs))}, better on {share:.0%}, mean {mean:+.2f} → {state}")
        if ok:
            settled.setdefault(k, []).append((kind, x, mean))
    # the candidate: each settled number once (a model's numbers are for every kind): one settled at two values, on one kind
    # or on two, is left out for you to decide
    cand, conflicts, notes = {}, [], []
    for k, found in settled.items():
        values = {x for _, x, _ in found}
        if len(values) == 1:
            cand[k] = values.pop()
        else:
            conflicts.append(f"{k}: " + ', '.join(f'{x} on {kind}' for kind, x, _ in found))
    # the gate's numbers only with the gate itself: its level, duck and pace do nothing while it is off
    gate_parts = [k for k in ('gateLevel', 'gateDuck', 'gateEvery') if k in cand]
    if gate_parts and cand.get('gate') not in ('1', '2'):
        for k in gate_parts:
            del cand[k]
        notes.append('the gate\'s ' + ', '.join(gate_parts) + ' settled but the gate itself did not: left out with it')
    # the kinds the gate plays on: those the base model gates already, and those where it settled now (a gate settled on
    # beats adds them; it never takes the drops' stabs away)
    base_gate = set()
    try:
        for line in open(base_file, encoding='utf-8'):
            if line.startswith('gateOn '):
                base_gate = {k for k in line.split(None, 1)[1].strip().split(',') if k and k != 'none'}
    except OSError:
        pass
    if 'gateOn' in cand or (cand.get('gate') in ('1', '2') and 'gate' in settled):
        kinds = set(cand.get('gateOn', '').split(',')) - {'', 'none'}
        if 'gate' in settled and cand.get('gate') in ('1', '2'):
            kinds |= {kind for kind, x, _ in settled['gate'] if x == cand['gate']} & {'drop', 'structure', 'beat'}
        cand['gateOn'] = ','.join(sorted(base_gate | kinds)) or 'none'
    if any(k in cand for k in ('gateLevel', 'gateDuck', 'gateEvery')) and base_gate:
        notes.append('the gate\'s numbers are shared by every kind it plays on (' + cand.get('gateOn', ','.join(sorted(base_gate)))
                     + '): check what freeze says it changes')
    say()
    if conflicts:
        say('Settled at two values (left out of the candidate; decide yourself): ' + '; '.join(conflicts))
    for n_ in notes:
        say('Note: ' + n_)
    say('Each version is measured against the reference of its own round: when rounds had different references, read the '
        'older rounds\' numbers with that in mind (a round changing one or two numbers per version reads best).')
    n = newest['round']
    cpath = os.path.join(newest_folder, f'candidate-round-{n}.txt')
    with open(cpath, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f"# The numbers settled by round {n}'s fit (tools/mixtrain-fit.py). Edit it as you like, then:\n")
        f.write(f"#   mixtrain freeze {os.path.basename(cpath)} \"<your model's name>\" <cache.tsv> <your-model>.model\n")
        f.write(f"base {base_file or '<the model file your rounds started from>'}\n")
        for k, x in sorted(cand.items()):
            f.write(f'{k} {x}\n')
    say(f"Candidate: {cpath} ({len(cand)} number{'s' if len(cand) != 1 else ''})" + ('' if cand else ': nothing settled yet: another round'))
    report = '\n'.join(lines) + '\n'
    open(os.path.join(newest_folder, f'fit-round-{n}.md'), 'w', encoding='utf-8', newline='\n').write(report)
    print(report)


if __name__ == '__main__':
    main()
