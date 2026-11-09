"""M8-027: the determinism check - hash the volcano_assets tree, re-run the
builder, hash again, compare."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(r'D:\Github-Poj\UE-MMO')
ASSETS = ROOT / 'Data' / 'DNFMReplica' / 'volcano_assets'


def dirsha(d):
    h = {}
    for p in sorted(d.rglob('*')):
        if p.is_file():
            h[str(p.relative_to(d))] = hashlib.sha256(p.read_bytes()).hexdigest()[:16]
    return h


a = dirsha(ASSETS)
subprocess.run([sys.executable, str(ROOT / 'Scripts' / 'DNFM' / 'build_volcano_assets.py')],
               check=True, capture_output=True)
b = dirsha(ASSETS)
print('deterministic:', a == b, '| files:', len(b))
m = json.loads((ASSETS / 'manifest.json').read_text(encoding='utf-8'))
print('manifest keys:', list(m.keys())[:10])
print('objects:', len(m.get('objects', [])), 'tiles:', len(m.get('tiles', [])))
if a != b:
    diff = [k for k in set(a) | set(b) if a.get(k) != b.get(k)]
    print('DIFF:', diff[:8])
    sys.exit(1)
