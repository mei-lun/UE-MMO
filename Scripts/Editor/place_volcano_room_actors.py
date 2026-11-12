"""M8-028: place the RoomTrigger and RoomExit on the volcano map."""
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
TRIGGER_CLS = '/Script/UEMMO.RoomTrigger'
EXIT_CLS = '/Script/UEMMO.RoomExit'
GOBLIN_ENEMY = '/Game/UEMMO/Enemy/Definitions/DA_monster_goblinthrower.DA_monster_goblinthrower'

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find_by_class(cls_name):
    return [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == cls_name]


def ensure_actor(cls_path, label, loc):
    cls = unreal.load_class(None, cls_path)
    found = [a for a in ACTORS.get_all_level_actors() if a.get_class() == cls]
    if len(found) > 1:
        raise RuntimeError('multiple %s found' % cls_path)
    if len(found) == 0:
        spawned = ACTORS.spawn_actor_from_class(cls,
            unreal.Vector(x=loc[0], y=loc[1], z=loc[2]),
            unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
        if spawned is None:
            raise RuntimeError('spawn failed: ' + cls_path)
        spawned.set_actor_label(label)
        return spawned, True
    found[0].set_actor_location(
        unreal.Vector(x=loc[0], y=loc[1], z=loc[2]), sweep=False, teleport=False)
    return found[0], False


if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)
trigger, t_created = ensure_actor(TRIGGER_CLS, 'VolcanoRoomTrigger_01', (900.0, 1050.0, 100.0))
exit_actor, e_created = ensure_actor(EXIT_CLS, 'VolcanoRoomExit_01', (1900.0, 1050.0, 110.0))
enemy_def = unreal.load_asset(GOBLIN_ENEMY)
if enemy_def is None:
    raise RuntimeError('missing enemy def ' + GOBLIN_ENEMY)
trigger.set_editor_property('enemy_definition_path', enemy_def)
if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
print('UEMMO_VOLCANO_ACTORS: trigger=%s exit=%s enemy=%s' % (t_created, e_created, GOBLIN_ENEMY))
