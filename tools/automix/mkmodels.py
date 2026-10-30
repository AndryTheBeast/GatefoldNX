"""songate-automix-models.json: the list of Automix models Songate downloads (updater.hpp, UpdModel).

    python tools/automix/mkmodels.py <out folder> <model file>... [--notes <model file>=<notes.txt>]...

Each model file (automix::ModelFile's text) is read for its name, format, lowest Songate and analysis; its size and SHA-256
are measured; it is copied beside the manifest. Attach the manifest and every model file to a release of the model
repository: Songate reads them from its latest release (releases/latest/download/<name>). A model, once published, never
changes: a new one gets a new name and file.
"""
import argparse, datetime, hashlib, json, os, shutil, sys


def read_model(path):
    head, fields = None, {}
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        k, _, v = line.partition(' ')
        if head is None:
            if k != 'songate-automix-model':
                sys.exit(f'{path}: not an Automix model')
            head = int(v)
            continue
        fields[k] = v.strip()
    if not fields.get('name'):
        sys.exit(f'{path}: a model without a name')
    return head, fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('models', nargs='+')
    ap.add_argument('--notes', action='append', default=[])
    ap.add_argument('--date', default=datetime.date.today().isoformat())
    a = ap.parse_args()
    notes = {}
    for n in a.notes:
        m, _, f = n.partition('=')
        notes[os.path.abspath(m)] = [l.strip() for l in open(f, encoding='utf-8') if l.strip()]
    os.makedirs(a.out, exist_ok=True)
    out = {'format': 1, 'models': []}
    for path in a.models:
        fmt, f = read_model(path)
        data = open(path, 'rb').read()
        name = os.path.basename(path)
        if not name.endswith('.model') or '/' in name or name.startswith('.'):
            sys.exit(f'{path}: a model file is named <something>.model')
        shutil.copyfile(path, os.path.join(a.out, name))
        out['models'].append({
            'name': f['name'], 'file': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'format': fmt, 'app': f.get('app', '2.0.0-beta'), 'mixan': int(f.get('mixan', '0')), 'date': a.date,
            'notes': notes.get(os.path.abspath(path), []),
        })
        print(f"{f['name']}: {name}, {len(data)} bytes, format {fmt}, Songate {f.get('app', '2.0.0-beta')}+")
    with open(os.path.join(a.out, 'songate-automix-models.json'), 'w', encoding='utf-8', newline='\n') as fp:
        json.dump(out, fp, indent=2, ensure_ascii=False)
        fp.write('\n')
    print('wrote', os.path.join(a.out, 'songate-automix-models.json'))


if __name__ == '__main__':
    main()
