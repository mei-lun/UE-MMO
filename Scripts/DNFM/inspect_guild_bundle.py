"""M8-024 step 1b: inspect the UnityPy object structure of the guild npk."""
import UnityPy
from pathlib import Path

npk = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\downloaded_130.5.8.202\Android\bundles\prefabs_3d_guildagit.npk')
env = UnityPy.load(str(npk))
f = env.file
print('env.file type:', type(f).__name__)
print('env.file attrs:', [a for a in dir(f) if not a.startswith('_')][:20])
files = getattr(f, 'files', {})
print('files count:', len(files))
for k, inner in list(files.items())[:6]:
    print('inner key:', k, 'type:', type(inner).__name__)
    af = getattr(inner, 'assets_file', None)
    if af is not None:
        print('  assets_file name:', getattr(af, 'name', '?'))
        print('  externals:', [e.path for e in af.externals][:6])
# some versions: env.file itself is the SerializedFile
print('env objects:', len(list(env.objects)))
# find Texture2D/Material objects directly
from collections import Counter
print(Counter(o.type.name for o in env.objects).most_common(12))
