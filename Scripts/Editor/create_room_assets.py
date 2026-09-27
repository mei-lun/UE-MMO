"""Create /Game/UEMMO/Maps/L_CombatRoom01 from L_TrainingArena and place the
M2-009 room actors. Run inside the UE editor (commandlet) only.

Two modes, selected by a token on the UE command line:
  create (default)  - duplicate the training arena into L_CombatRoom01, place
                      exactly one ARoomTrigger and one ARoomExit, save.
                      Idempotent: a repeated run reuses the existing actors
                      and normalizes their locations.
  verify            - after a fresh process start, reload both maps and assert
                      the actor inventory; writes
                      Artifacts/room-assets-verify-report.json.

The training map L_TrainingArena is never modified: it is only duplicated.
The duplicate inherits the training enemy (reported as-is; it is inert
scaffolding in the combat copy). The activation trigger exists ONLY on the
combat map - the training arena must keep its render-only, never-auto-fighting
semantics (M2-009 explicit render vs wave-spawn mode separation).
ASCII only: this file must not contain non-ASCII characters.
"""
import json
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
CREATE_REPORT = ROOT / 'Artifacts' / 'room-assets-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'room-assets-verify-report.json'
BASE_MAP = '/Game/UEMMO/Maps/L_TrainingArena'
COMBAT_MAP = '/Game/UEMMO/Maps/L_CombatRoom01'
TRIGGER_CLASS_PATH = '/Script/UEMMO.RoomTrigger'
EXIT_CLASS_PATH = '/Script/UEMMO.RoomExit'
TRIGGER_LABEL = 'RoomTrigger_01'
EXIT_LABEL = 'RoomExit_01'
# Player spawn (-400, 0): the trigger activation zone covers x in [-250, 50]
# (box half extents 150/150/90 at (-100, 0, 88)) and the exit sits near the
# far end of the arena (box half extents 80/80/110 at (900, 0, 110)). Both
# stay inside the room_training_01 bounds (x +-1200, y +-500, z 0..180).
TRIGGER_LOCATION = unreal.Vector(-100, 0, 88)
EXIT_LOCATION = unreal.Vector(900, 0, 110)

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMORoomAssetsVerify' in command_line()


def actor_class(path, python_name):
    candidate = getattr(unreal, python_name, None)
    if candidate is not None:
        return candidate
    loaded = unreal.load_class(None, path)
    if loaded is None:
        raise RuntimeError('UEMMO room class not found (' + path + '); build the UEMMO module first.')
    return loaded


def inventory():
    rows = []
    for actor in actors.get_all_level_actors():
        rows.append({'label': actor.get_actor_label(), 'class': actor.get_class().get_name()})
    return rows


def count_class(rows, class_name):
    return len([row for row in rows if row['class'] == class_name])


def find_by_class(class_name):
    found = []
    for actor in actors.get_all_level_actors():
        if actor.get_class().get_name() == class_name:
            found.append(actor)
    return found


def box_extent_of(actor):
    # Best effort: the extents prove the volume geometry survived the save.
    try:
        box = actor.get_component_by_class(unreal.BoxComponent)
        if box is None:
            return None
        extent = box.get_editor_property('box_extent')
        return {'x': extent.x, 'y': extent.y, 'z': extent.z}
    except Exception:
        return None


def ensure_actor(class_name, python_name, class_path, label, location):
    """Spawns exactly one actor of the class, or reuses and normalizes one."""
    existing = find_by_class(class_name)
    if len(existing) > 1:
        raise RuntimeError('Map already holds ' + str(len(existing)) + ' ' + class_name + ' actors; refusing to add more.')
    if len(existing) == 0:
        spawned = actors.spawn_actor_from_class(
            actor_class(class_path, python_name), location,
            unreal.Rotator(pitch=0, yaw=0, roll=0))
        if spawned is None:
            raise RuntimeError('Could not spawn ' + class_name + '.')
        spawned.set_actor_label(label)
        return spawned, True
    # Idempotent update: normalize the stored location on repeated runs.
    existing[0].set_actor_location(location)
    return existing[0], False


def create_room_assets():
    created_map = False
    if not assets.does_asset_exist(COMBAT_MAP):
        if not assets.does_asset_exist(BASE_MAP):
            raise RuntimeError('Base map missing: ' + BASE_MAP)
        if not unreal.EditorAssetLibrary.duplicate_asset(BASE_MAP, COMBAT_MAP):
            raise RuntimeError('Could not duplicate ' + BASE_MAP + ' into ' + COMBAT_MAP)
        created_map = True
    if not levels.load_level(COMBAT_MAP):
        raise RuntimeError('Could not load ' + COMBAT_MAP)

    trigger, created_trigger = ensure_actor(
        'RoomTrigger', 'RoomTrigger', TRIGGER_CLASS_PATH, TRIGGER_LABEL, TRIGGER_LOCATION)
    exit_actor, created_exit = ensure_actor(
        'RoomExit', 'RoomExit', EXIT_CLASS_PATH, EXIT_LABEL, EXIT_LOCATION)

    rows = inventory()
    labels = [row['label'] for row in rows]
    if 'PlayerStart' not in labels:
        raise RuntimeError('PlayerStart is missing from the combat room.')
    if count_class(rows, 'TrainingEnemy') != 1:
        raise RuntimeError('The duplicated combat room should carry exactly the inherited training enemy.')
    if not levels.save_current_level():
        raise RuntimeError('Could not save ' + COMBAT_MAP)

    report = {
        'mode': 'create',
        'created': created_map or created_trigger or created_exit,
        'updated': not (created_map or created_trigger or created_exit),
        'combat_map': {
            'path': COMBAT_MAP,
            'duplicated_from': BASE_MAP if created_map else '',
            'actor_count': len(rows),
            'room_trigger_count': count_class(rows, 'RoomTrigger'),
            'room_exit_count': count_class(rows, 'RoomExit'),
            'training_enemy_count': count_class(rows, 'TrainingEnemy'),
            'trigger_box_extent': box_extent_of(trigger),
            'exit_box_extent': box_extent_of(exit_actor),
        },
        'success': True,
    }
    CREATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    CREATE_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    unreal.log('UEMMO_ROOM_ASSETS_CREATED map=' + COMBAT_MAP
               + ' new_map=' + str(created_map)
               + ' new_trigger=' + str(created_trigger)
               + ' new_exit=' + str(created_exit))


def verify_maps():
    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    if not levels.load_level(COMBAT_MAP):
        raise RuntimeError('Verify: could not load ' + COMBAT_MAP)
    combat_rows = inventory()
    triggers = find_by_class('RoomTrigger')
    exits = find_by_class('RoomExit')
    trigger_extent = triggers[0] if triggers else None
    exit_extent = exits[0] if exits else None
    record('combat_map_exactly_one_room_trigger', len(triggers) == 1,
           'count=' + str(len(triggers)))
    record('combat_map_exactly_one_room_exit', len(exits) == 1,
           'count=' + str(len(exits)))
    record('combat_map_inherited_training_enemy',
           count_class(combat_rows, 'TrainingEnemy') == 1,
           'count=' + str(count_class(combat_rows, 'TrainingEnemy')))
    record('combat_map_player_start_present',
           any(row['class'] == 'PlayerStart' or row['label'] == 'PlayerStart' for row in combat_rows))
    record('combat_map_trigger_box_extent', trigger_extent is not None and box_extent_of(trigger_extent) is not None,
           'extent=' + json.dumps(box_extent_of(trigger_extent) if trigger_extent else None))

    if not levels.load_level(BASE_MAP):
        raise RuntimeError('Verify: could not load ' + BASE_MAP)
    base_rows = inventory()
    record('training_map_has_no_room_trigger', count_class(base_rows, 'RoomTrigger') == 0,
           'count=' + str(count_class(base_rows, 'RoomTrigger')))
    record('training_map_has_no_room_exit', count_class(base_rows, 'RoomExit') == 0,
           'count=' + str(count_class(base_rows, 'RoomExit')))
    record('training_map_still_exactly_one_training_enemy',
           count_class(base_rows, 'TrainingEnemy') == 1,
           'count=' + str(count_class(base_rows, 'TrainingEnemy')))

    report = {
        'mode': 'verify',
        'combat_map': {
            'path': COMBAT_MAP,
            'actor_count': len(combat_rows),
            'room_trigger_count': count_class(combat_rows, 'RoomTrigger'),
            'room_exit_count': count_class(combat_rows, 'RoomExit'),
            'training_enemy_count': count_class(combat_rows, 'TrainingEnemy'),
            'actors': combat_rows,
        },
        'training_map': {
            'path': BASE_MAP,
            'actor_count': len(base_rows),
            'room_trigger_count': count_class(base_rows, 'RoomTrigger'),
            'room_exit_count': count_class(base_rows, 'RoomExit'),
            'training_enemy_count': count_class(base_rows, 'TrainingEnemy'),
        },
        'checks': checks,
        'success': all(check['passed'] for check in checks),
    }
    VERIFY_REPORT.parent.mkdir(parents=True, exist_ok=True)
    VERIFY_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not report['success']:
        failed = [check['check'] for check in checks if not check['passed']]
        raise RuntimeError('Room assets verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_ROOM_ASSETS_VERIFIED ' + str(VERIFY_REPORT))


if is_verify_mode():
    verify_maps()
else:
    create_room_assets()
