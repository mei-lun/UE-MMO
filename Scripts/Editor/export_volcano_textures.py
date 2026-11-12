"""Export ALL imported volcano OBJ textures to PNG for offline alpha analysis (M8-043 pass 2)."""
import os

import unreal

OUT = 'D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-043/tex-export'
os.makedirs(OUT, exist_ok=True)
tools = unreal.AssetToolsHelpers.get_asset_tools()
suffixes = []
for i in range(1, 45):
    found = None
    for s in ('NORMAL', 'MIDDLEBACK', 'DISTANTBACK', 'CONTACT'):
        p = '/Game/UEMMO/DNFM/Volcano/T/T_OBJ_%02d_%s' % (i, s)
        if unreal.EditorAssetLibrary.does_asset_exist(p):
            found = p
            break
    if found:
        suffixes.append(found)
print('UEMMO_TEX_EXPORT: found=%d' % len(suffixes))
ok = tools.export_assets(suffixes, export_path=OUT)
print('UEMMO_TEX_EXPORT: ok=%s' % ok)
