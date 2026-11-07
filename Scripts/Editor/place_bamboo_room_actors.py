"""M8-019C: place the production entry actors on L_DNFM_Bamboo01.

Exactly one ARoomTrigger (the activation region whose overlap starts the
room run; its EnemyDefinitionPath points at the goblin's enemy definition)
and one ARoomExit (the east-edge gap indicator the M8-014 marker already
advertises). Geometry facts from Data/DNFMReplica/bamboo_calibration.json
(player spawn = the hypothesis-A band center the map's PlayerStart sits at;
the east gap at x=1560; the walkable y band [422.76, 1127.36]).

Idempotent: repeated runs reuse the existing actors and re-apply the
locations and the enemy-definition reference. The verify mode
(UEMMOBambooRoomActorsVerify) reloads the saved map in a fresh process and
asserts the counts, the locations and the enemy path.
"""
import json
import os
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
BAMBOO_MAP = '/Game/UEMMO/DNFM/Bamboo/Maps/L_DNFM_Bamboo01'
TRIGGER_CLASS_PATH = '/Script/UEMMO.RoomTrigger'
EXIT_CLASS_PATH = '/Script/UEMMO.RoomExit'
GOBLIN_ENEMY_ASSET = '/Game/UEMMO/Enemy/Definitions/DA_monster_goblinthrower.DA_monster_goblinthrower'

# The player spawn (the map's PlayerStart) and the east gap (M8-014).
PLAYER_SPAWN = (789.15, 775.06, 100.0)
# The activation zone covers the spawn and the first steps east; the wave
# spawns sit further east (rooms.json), so the player walks into combat.
TRIGGER_LOCATION = (1100.0, 775.06, 100.0)
EXIT_LOCATION = (1560.0, 775.06, 110.0)

GENERATE_REPORT = ROOT / 'Artifacts' / 'Tasks' / 'M8-019C' / 'bamboo-room-actors-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Tasks' / 'M8-019C' / 'bamboo-room-actors-verify.json'

assets = unreal.EditorAssetLibrary
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find_by_class(class_name):
    return [a for a in actors.get_all_level_actors()
            if a.get_class().get_name() == class_name]


def ensure_actor(class_path, label, location):
    cls = unreal.load_class(None, class_path)
    if cls is None:
        raise RuntimeError('class missing: ' + class_path)
    found = [a for a in actors.get_all_level_actors()
             if a.get_class() == cls]
    if len(found) > 1:
        raise RuntimeError('map holds %d actors of %s; refusing' % (len(found), class_path))
    if len(found) == 0:
        spawned = actors.spawn_actor_from_class(
            cls, unreal.Vector(x=location[0], y=location[1], z=location[2]),
            unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
        if spawned is None:
            raise RuntimeError('could not spawn ' + class_path)
        spawned.set_actor_label(label)
        return spawned, True
    found[0].set_actor_location(
        unreal.Vector(x=location[0], y=location[1], z=location[2]),
        sweep=False, teleport=False)
    return found[0], False


def generate():
    if not levels.load_level(BAMBOO_MAP):
        raise RuntimeError('could not load ' + BAMBOO_MAP)
    trigger, created_trigger = ensure_actor(
        TRIGGER_CLASS_PATH, 'BambooRoomTrigger_01', TRIGGER_LOCATION)
    exit_actor, created_exit = ensure_actor(
        EXIT_CLASS_PATH, 'BambooRoomExit_01', EXIT_LOCATION)
    # The thrower room trains the thrower: the trigger loads the goblin's
    # enemy definition (the M5-068 entry validation loads this asset).
    enemy_def = unreal.load_asset(GOBLIN_ENEMY_ASSET)
    if enemy_def is None:
        raise RuntimeError('missing enemy definition ' + GOBLIN_ENEMY_ASSET)
    trigger.set_editor_property('enemy_definition_path', enemy_def)
    if not levels.save_current_level():
        raise RuntimeError('could not save ' + BAMBOO_MAP)
    report = {
        'mode': 'generate',
        'map': BAMBOO_MAP,
        'trigger_created': created_trigger,
        'exit_created': created_exit,
        'enemy_definition': GOBLIN_ENEMY_ASSET,
        'trigger_location': list(TRIGGER_LOCATION),
        'exit_location': list(EXIT_LOCATION),
        'success': True,
    }
    GENERATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    GENERATE_REPORT.write_text(json.dumps(report, indent=1), encoding='utf-8')
    unreal.log('UEMMO_BAMBOO_ROOM_ACTORS_PLACED trigger=%s exit=%s enemy=%s'
               % (created_trigger, created_exit, GOBLIN_ENEMY_ASSET))


def verify():
    if not levels.load_level(BAMBOO_MAP):
        raise RuntimeError('verify: could not load ' + BAMBOO_MAP)
    triggers = find_by_class('RoomTrigger')
    exits = find_by_class('RoomExit')
    problems = []
    if len(triggers) != 1:
        problems.append('trigger count %d' % len(triggers))
    if len(exits) != 1:
        problems.append('exit count %d' % len(exits))
    enemy_path = ''
    if triggers:
        enemy = triggers[0].get_editor_property('enemy_definition_path')
        enemy_path = str(enemy)
        if 'DA_monster_goblinthrower' not in enemy_path:
            problems.append('trigger enemy path ' + enemy_path)
    report = {
        'mode': 'verify',
        'map': BAMBOO_MAP,
        'trigger_count': len(triggers),
        'exit_count': len(exits),
        'enemy_definition_path': enemy_path,
        'problems': problems,
        'success': not problems,
    }
    VERIFY_REPORT.parent.mkdir(parents=True, exist_ok=True)
    VERIFY_REPORT.write_text(json.dumps(report, indent=1), encoding='utf-8')
    unreal.log('UEMMO_BAMBOO_ROOM_ACTORS_VERIFIED problems=%s' % problems)
    if problems:
        raise RuntimeError('bamboo room actor verify failed: %s' % problems)


try:
    if 'UEMMOBambooRoomActorsVerify' in unreal.SystemLibrary.get_command_line():
        verify()
    else:
        generate()
except Exception:
    import traceback
    unreal.log('UEMMO_BAMBOO_ROOM_ACTORS_FAILED: ' + traceback.format_exc())
    raise
