"""M8-024 step 2: extract the guild scene's missing textures.

The M6-011 intermediates marked 78 materials' _MainTex as missing with the
source key cab-6af7f14628a7155654ed172c566e3677:<path_id>; the container
rides in prefabs_3d_guildagit_textures.npk (located by the step-1 scan).
Extracts every Texture2D from that cab to PNG under
SourceAssets/Unpacked/DNFM130_5_8/guild_textures/, and rewrites the manifest:
the textures rows grow, the materials' missing_textures entries flip to
resolved with the local file path.
"""
import hashlib
import json
from pathlib import Path

import UnityPy

SRC = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\downloaded_130.5.8.202')
TEXTURE_NPK = SRC / 'Android' / 'bundles' / 'prefabs_3d_guildagit_textures.npk'
CAB_NAME = 'cab-6af7f14628a7155654ed172c566e3677'
STAGE = Path(r'D:\Github-Poj\UE-MMO\SourceAssets\Unpacked\DNFM130_5_8')
MANIFEST = STAGE / 'manifest.json'
OUT_DIR = STAGE / 'guild_textures'
TARGET_SLOTS = ('_MainTex', '_BaseMap', '_BaseColorMap', '_DiffuseTex')


def main():
    env = UnityPy.load(str(TEXTURE_NPK))
    textures = {}
    for obj in env.objects:
        af_name = str(getattr(getattr(obj, 'assets_file', None), 'name', '')).lower()
        if af_name.startswith(CAB_NAME) and obj.type.name == 'Texture2D':
            textures[str(obj.path_id)] = obj
    print('texture objects in the cab:', len(textures))

    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    needed = {}
    for mat in manifest['materials']:
        for slot, ref in (mat.get('missing_textures') or {}).items():
            if slot in TARGET_SLOTS and str(ref.get('status')) == 'missing':
                key = str(ref.get('source_key', ''))
                if key.startswith(CAB_NAME + ':'):
                    needed.setdefault(key.split(':', 1)[1], []).append((mat['id'], slot))
    print('distinct needed path ids:', len(needed),
          'refs:', sum(len(v) for v in needed.values()))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    resolved_rows = []
    missing_after = []
    existing_ids = {row['id'] for row in manifest['textures']}
    for path_id, users in sorted(needed.items(), key=lambda kv: int(kv[0])):
        obj = textures.get(path_id)
        if obj is None:
            missing_after.append({'path_id': path_id, 'users': users})
            continue
        tex = obj.read()
        img = tex.image
        clip = users[0][0].split('_')[1][:5] if False else ''
        out_name = 'GA_%s' % tex.m_Name if tex.m_Name else 'GA_pathid_%s' % path_id
        out_name = ''.join(c if c.isalnum() or c in '_' else '_' for c in out_name)
        png = OUT_DIR / (out_name + '.png')
        img.save(str(png))
        data = png.read_bytes()
        resolved_rows.append({
            'id': out_name,
            'file': str(png),
            'sha256': hashlib.sha256(data).hexdigest(),
            'width': img.width, 'height': img.height,
            'source_cab': CAB_NAME, 'source_path_id': path_id,
            'used_by_materials': sorted({m for m, _ in users}),
        })
        for mat_id, slot in users:
            pass
    print('extracted:', len(resolved_rows), 'still missing:', len(missing_after))

    # rewrite the manifest: append the texture rows; flip the material refs
    by_cab_path = {r['source_path_id']: r for r in resolved_rows}
    for mat in manifest['materials']:
        for slot, ref in (mat.get('missing_textures') or {}).items():
            key = str(ref.get('source_key', ''))
            if not key.startswith(CAB_NAME + ':'):
                continue
            pid = key.split(':', 1)[1]
            row = by_cab_path.get(pid)
            if row is None:
                continue
            ref['status'] = 'resolved_m8_024'
            ref['resolved_texture_id'] = row['id']
            ref['resolved_file'] = row['file']
    for row in resolved_rows:
        if row['id'] not in existing_ids:
            manifest['textures'].append({
                'id': row['id'], 'file': row['file'],
                'sha256': row['sha256'], 'width': row['width'], 'height': row['height'],
                'source': 'm8_024_guild_texture_completion',
            })
    manifest['m8_024_guild_texture_completion'] = {
        'extracted': len(resolved_rows),
        'still_missing': len(missing_after),
        'out_dir': str(OUT_DIR),
    }
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding='utf-8')
    for r in resolved_rows[:12]:
        print('  %s %dx%d used_by=%d' % (r['id'], r['width'], r['height'], len(r['used_by_materials'])))
    if missing_after:
        print('STILL MISSING path ids:', [m['path_id'] for m in missing_after][:10])


if __name__ == '__main__':
    main()
