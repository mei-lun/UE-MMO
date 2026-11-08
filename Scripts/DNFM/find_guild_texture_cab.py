"""M8-024 step 1: locate the .npk that packs CAB-6af7f146... (the guild
scene's texture container)."""
import sys
import UnityPy
from pathlib import Path

SRC = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\downloaded_130.5.8.202')
TARGET = 'cab-6af7f14628a7155654ed172c566e3677'

bundle_dir = SRC / 'Android' / 'bundles'
names = sys.argv[1:] if len(sys.argv) > 1 else None
files = sorted(bundle_dir.glob('*.npk')) if names is None else [bundle_dir / n for n in names]
print('scanning %d npk files for %s' % (len(files), TARGET))
for p in files:
    try:
        env = UnityPy.load(str(p))
        for k, inner in getattr(env.file, 'files', {}).items():
            if k.endswith('.resS'):
                continue
            nm = str(getattr(inner, 'name', '')).lower()
            if nm == TARGET:
                objs = list(inner.objects)
                print('FOUND:', p.name, 'inner', k, 'objects:', len(objs))
                from collections import Counter
                print(Counter(o.type.name for o in objs).most_common(8))
                sys.exit(0)
    except Exception as ex:
        print('skip', p.name, type(ex).__name__)
print('not found in this set')
