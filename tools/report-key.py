"""Settings › Support's key: makes source/report_key.inc (git-ignored) from report.key (git-ignored), for builds that can send
reports (source/report.cpp unscrambles it).

    python tools/report-key.py [report.key]

report.key: two lines, the repository ("AndryTheBeast/songate-reports") and a fine-grained token that can reach only that
repository, with only Issues: read and write. Never commit either file. Without them a build simply cannot send reports.

The header line "Authorization: Bearer <token>" is XORed with a xorshift32 stream (its seed: four random numbers here, and
0x9E3779B9 in report.cpp), each byte then put at a random place among random bytes, the places in another table: no text
of it is in the .nro. This slows someone down; it cannot stop them (see report.hpp)."""
import os, secrets, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'report.key')
lines = [l.strip() for l in open(src, encoding='utf-8').read().splitlines() if l.strip()]
assert len(lines) == 2, 'report.key: the repository, then the token'
repo, token = lines
assert repo.count('/') == 1 and ' ' not in repo, 'the repository as owner/name'
assert token.startswith('github_pat_') or token.startswith('ghp_'), 'a GitHub token'
payload = ('Authorization: Bearer ' + token).encode('ascii')

seed = [secrets.randbits(32) for _ in range(4)]
x = seed[0] ^ seed[1] ^ seed[2] ^ seed[3] ^ 0x9E3779B9
enc = []
for b in payload:
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    enc.append(b ^ ((x >> 8) & 0xFF))
total = len(payload) + 24 + secrets.randbelow(40)
places = list(range(total))
for i in range(total - 1, 0, -1):   # Fisher-Yates with the system's randomness
    j = secrets.randbelow(i + 1)
    places[i], places[j] = places[j], places[i]
data = [secrets.randbits(8) for _ in range(total)]
place = places[:len(payload)]
for i, p in enumerate(place): data[p] = enc[i]

def rows(v, per=16):
    return ',\n    '.join(', '.join(str(a) for a in v[i:i + per]) for i in range(0, len(v), per))

out = os.path.join(root, 'source', 'report_key.inc')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Made by tools/report-key.py from report.key. Git-ignored: never commit it (see report.hpp).\n#pragma once\n#include <cstddef>\n#include <cstdint>\n')
    f.write('namespace report { namespace key {\n')
    f.write(f'constexpr const char* kRepo = "{repo}";\n')
    f.write(f'constexpr size_t kLen = {len(payload)};\n')
    f.write(f'static const uint32_t kSeed[4] = {{ {", ".join(str(s) for s in seed)} }};\n')
    f.write(f'static const uint8_t kData[{total}] = {{\n    {rows(data)} }};\n')
    f.write(f'static const uint16_t kPlace[{len(place)}] = {{\n    {rows(place)} }};\n')
    f.write('} }\n')
print(out, f'{len(payload)} bytes among {total}; repository {repo}')
