"""Keeps the open-source Automix repository (songate-automix) in step with Songate: Songate's files are the source of
truth, copied there as they are (the user's choice, 2026-10-30: a copy kept in step by a script).

    python tools/automix/sync-automix.py [<songate-automix folder>]          copy what changed
    python tools/automix/sync-automix.py --check [<folder>]                  only say what differs (exit 1 if anything)
    python tools/automix/sync-automix.py --freeze <name>                     record a newly frozen model's code (see below)

The rule it guards: a model never changes once it exists. The code of every frozen model (automix.cpp's namespaces v1_0_0,
v1_1_0 and file1, the model-file planner) is hashed and compared with frozen.json beside this script: a change refuses the
sync. A published .model file is never overwritten with other contents. A new built-in model or a new model-file format is
recorded with --freeze once it is frozen.
"""
import hashlib, json, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
FROZEN = os.path.join(HERE, 'frozen.json')

FILES = {   # Songate's file → the repository's
    'source/mixan.hpp': 'src/mixan.hpp', 'source/mixan.cpp': 'src/mixan.cpp',
    'source/automix.hpp': 'src/automix.hpp', 'source/automix.cpp': 'src/automix.cpp',
    'tools/preview/mixtest.cpp': 'tools/mixtest.cpp', 'tools/preview/mixtrain.cpp': 'tools/mixtrain.cpp',
    'tools/preview/mixtrain-page.py': 'tools/mixtrain-page.py', 'tools/automix/mkmodels.py': 'tools/mkmodels.py',
    'tools/preview/mixtrain-fit.py': 'tools/mixtrain-fit.py',
    'source/ffdecoder.cpp': 'tools/support/ffdecoder.cpp', 'source/ffdecoder.hpp': 'tools/support/ffdecoder.hpp',
    'source/loudness.cpp': 'tools/support/loudness.cpp', 'source/loudness.hpp': 'tools/support/loudness.hpp',
    'source/metadata.hpp': 'tools/support/metadata.hpp', 'source/mood.cpp': 'tools/support/mood.cpp',
    'source/mood.hpp': 'tools/support/mood.hpp', 'source/cores.hpp': 'tools/support/cores.hpp',
}
MODELS = 'automix-demo/models'   # the frozen models' files (each published as it is, never changed)
NAMESPACES = ['v1_0_0', 'v1_1_0', 'file1']


def frozen_code(text):
    """Each frozen namespace's code (all of its blocks), line endings aside."""
    text = text.replace('\r\n', '\n')
    out = {}
    for ns in NAMESPACES:
        blocks = re.findall(r'^namespace %s \{.*?^\}  // namespace %s$' % (ns, ns), text, re.S | re.M)
        if not blocks:
            sys.exit(f'automix.cpp has no namespace {ns}: a frozen model is gone')
        out[ns] = hashlib.sha256('\n'.join(blocks).encode()).hexdigest()
    return out


def main():
    args = sys.argv[1:]
    check = '--check' in args
    freeze = None
    if '--freeze' in args:
        i = args.index('--freeze')
        freeze = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if a != '--check']
    repo = os.path.abspath(args[0] if args else os.path.join(ROOT, '..', 'songate-automix'))
    code = frozen_code(open(os.path.join(ROOT, 'source/automix.cpp'), encoding='utf-8').read())
    known = json.load(open(FROZEN, encoding='utf-8')) if os.path.exists(FROZEN) else {}
    if freeze:
        known.update({ns: h for ns, h in code.items() if ns not in known})
        known.setdefault('_log', []).append(freeze)
        json.dump(known, open(FROZEN, 'w', encoding='utf-8', newline='\n'), indent=2)
        print('recorded:', ', '.join(ns for ns in code if ns in known))
        return
    bad = [ns for ns, h in code.items() if ns in known and known[ns] != h]
    if bad:
        sys.exit('REFUSED: frozen model code changed: ' + ', '.join(bad) + ' (a model never changes: copy it into a new one)')
    new = [ns for ns in code if ns not in known]
    if new:
        sys.exit('REFUSED: not recorded as frozen yet: ' + ', '.join(new) + ' (record it with --freeze "<model>")')
    differ = []
    text = lambda p: open(p, 'rb').read().replace(b'\r\n', b'\n')   # (the repository keeps every text file LF: .gitattributes)
    for src, dst in FILES.items():
        a, b = os.path.join(ROOT, src), os.path.join(repo, dst)
        if not os.path.exists(b) or text(a) != text(b):
            differ.append((a, b, dst))
    for f in sorted(os.listdir(os.path.join(ROOT, MODELS))):
        if not f.endswith('.model'):
            continue
        a, b = os.path.join(ROOT, MODELS, f), os.path.join(repo, 'models', f)
        if os.path.exists(b) and open(a, 'rb').read() != open(b, 'rb').read():
            sys.exit(f'REFUSED: models/{f} is published with other contents (a model never changes: give it a new name)')
        if not os.path.exists(b):
            differ.append((a, b, 'models/' + f))
    for _, _, d in differ:
        print(('differs: ' if check else 'copied:  ') + d)
    if check:
        sys.exit(1 if differ else 0)
    for a, b, _ in differ:
        os.makedirs(os.path.dirname(b), exist_ok=True)
        shutil.copyfile(a, b)
    print('in step' if not differ else f'{len(differ)} file(s) copied; frozen models unchanged')


if __name__ == '__main__':
    main()
