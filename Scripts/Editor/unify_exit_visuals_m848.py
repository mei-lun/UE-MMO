"""M8-048: apply the M_ExitPortal visual to the remaining room exits."""
import unreal

MAPS = ('/Game/UEMMO/Maps/L_TrainingArena',
        '/Game/UEMMO/Maps/L_CombatRoom01',
        '/Game/UEMMO/Maps/L_CombatRoom02',
        '/Game/UEMMO/Maps/L_MainMenu')
EXIT_MAT = '/Game/UEMMO/Rooms/M_ExitPortal'
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
mat = unreal.load_asset(EXIT_MAT)
result = []
for map_path in MAPS:
    if not LEVELS.load_level(map_path):
        result.append('%s LOAD FAILED' % map_path)
        continue
    n = 0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == 'RoomExit':
            for c in a.get_components_by_class(unreal.StaticMeshComponent):
                c.set_material(0, mat)
            n += 1
    if not LEVELS.save_current_level():
        raise RuntimeError('save failed ' + map_path)
    result.append('%s exits=%d' % (map_path.split('/')[-1], n))
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-048/exits-result.txt', 'w') as fh:
    fh.write('\n'.join(result) + '\n')
