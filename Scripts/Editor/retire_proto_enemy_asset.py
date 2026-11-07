"""M8-019C one-off: retire the stale DA_dungeon_goblin_proto (its enemies.json
row was superseded by monster_goblinthrower at M8-019A; no code path loads the
asset - the M7_004_GoblinId branch keys on an id no row carries)."""
import unreal
ASSETS = unreal.EditorAssetLibrary
PATH = '/Game/UEMMO/Enemy/Definitions/DA_dungeon_goblin_proto'
if ASSETS.does_asset_exist(PATH):
    ok = ASSETS.delete_asset(PATH)
    unreal.log('UEMMO_M8_019C_RETIRE %s -> %s' % (PATH, ok))
else:
    unreal.log('UEMMO_M8_019C_RETIRE %s absent' % PATH)
