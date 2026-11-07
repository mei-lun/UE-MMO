"""M8-019A one-off: wipe the goblin visual assets so the next import starts
from the same fresh-asset conditions the proven fighter import had (the
repeated delete+reimport cycles are the remaining suspect for the -game
texture mismatch)."""
from pathlib import Path

import unreal

ASSETS = unreal.EditorAssetLibrary
ROOTS = [
    '/Game/UEMMO/DNFM/Monsters/Goblinthrower',
]

for root in ROOTS:
    if ASSETS.does_directory_exist(root):
        ok = ASSETS.delete_directory(root)
        unreal.log('UEMMO M8-019A wipe %s -> %s' % (root, ok))
    else:
        unreal.log('UEMMO M8-019A wipe %s absent' % root)
unreal.log('UEMMO M8-019A wipe done')
