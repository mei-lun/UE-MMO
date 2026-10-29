"""Clone one combat-room umap for the M6 multi-level production: the source
map (default /Game/UEMMO/Maps/L_CombatRoom01) duplicates to the target with
its full orchestration (RoomTrigger, RoomExit, floor, PlayerStart). Run
inside the UE editor (commandlet) only.

Set UEMMOCloneStripRoomActors=1 for a MENU-surface clone (M6-005): the
duplicated RoomTrigger/RoomExit actors are destroyed before the save, so the
clone carries only the walkable shell (floor + PlayerStart + lighting) - the
pure menu boot surface with no combat orchestration.

The clone is the MAP production step only: a combat clone's duplicated
trigger still references the SOURCE row's definition asset -
Scripts/PrepareRooms.ps1 owns the per-row DA generation and the trigger
rewiring (its verify pass asserts every wired reference).

Two modes, selected by a token on the UE command line:
  generate (default) - duplicate the map when the target is absent; an
                       existing target re-strips/re-saves untouched
                       (idempotent rerun) and reports created=false. Writes
                       Artifacts/Logs/room-map-clone-report.json.
  verify (UEMMORoomMapCloneVerify) - a fresh process reloads the target from
                       disk and asserts the cloned orchestration: a combat
                       clone carries exactly one RoomTrigger, one RoomExit
                       and one PlayerStart; a stripped menu clone carries
                       ZERO RoomTrigger/RoomExit and one PlayerStart. Writes
                       Artifacts/Logs/room-map-clone-verify.json.

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
SOURCE_MAP = os.environ.get('UEMMOCloneSourceMap', '/Game/UEMMO/Maps/L_CombatRoom01')
TARGET_MAP = os.environ.get('UEMMOCloneTargetMap', '/Game/UEMMO/Maps/L_CombatRoom02')
STRIP_ROOM_ACTORS = os.environ.get('UEMMOCloneStripRoomActors', '') == '1'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'room-map-clone-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'room-map-clone-verify.json'

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMORoomMapCloneVerify' in command_line()


def count_class(class_name):
    return len([actor for actor in actors.get_all_level_actors()
                if actor.get_class().get_name() == class_name])


def write_report(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding='utf-8')


def run_generate():
    created = False
    if unreal.EditorAssetLibrary.does_asset_exist(TARGET_MAP):
        # Idempotent rerun: the clone already exists - never duplicate again
        # (an auto-suffixed copy would be a second source of truth).
        loaded = unreal.load_asset(TARGET_MAP)
        if loaded is None:
            raise RuntimeError('The existing clone exists on disk but could not be loaded: ' + TARGET_MAP)
    else:
        if not unreal.EditorAssetLibrary.does_asset_exist(SOURCE_MAP):
            raise RuntimeError('The source map does not exist: ' + SOURCE_MAP)
        duplicated = unreal.EditorAssetLibrary.duplicate_asset(SOURCE_MAP, TARGET_MAP)
        if not duplicated:
            raise RuntimeError('duplicate_asset failed for ' + TARGET_MAP)
        created = True

    # Touch-save the clone (a stripped clone re-runs its destruction pass, so
    # a re-save of an existing target can never resurrect the room actors).
    if not levels.load_level(TARGET_MAP):
        raise RuntimeError('Could not load the clone: ' + TARGET_MAP)
    if STRIP_ROOM_ACTORS:
        destroyed = 0
        for actor in list(actors.get_all_level_actors()):
            if actor is not None and actor.get_class().get_name() in ('RoomTrigger', 'RoomExit'):
                actors.destroy_actor(actor)
                destroyed += 1
        if count_class('RoomTrigger') != 0 or count_class('RoomExit') != 0:
            raise RuntimeError('The strip pass left room actors behind (target=' + TARGET_MAP + ')')
    if not levels.save_current_level():
        raise RuntimeError('Could not save the clone: ' + TARGET_MAP)

    write_report(GENERATE_REPORT, {
        'mode': 'generate',
        'source': SOURCE_MAP,
        'target': TARGET_MAP,
        'created': created,
        'stripped': STRIP_ROOM_ACTORS,
        'trigger_count': count_class('RoomTrigger'),
        'exit_count': count_class('RoomExit'),
        'success': True,
    })
    unreal.log('UEMMO_ROOM_MAP_CLONED target=' + TARGET_MAP + ' created=' + str(created)
               + ' stripped=' + str(STRIP_ROOM_ACTORS))


def run_verify():
    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    record('clone_asset_exists', unreal.EditorAssetLibrary.does_asset_exist(TARGET_MAP), 'target=' + TARGET_MAP)
    loaded = unreal.load_asset(TARGET_MAP)
    record('clone_loads_from_disk', loaded is not None, 'target=' + TARGET_MAP)

    if not levels.load_level(TARGET_MAP):
        raise RuntimeError('Verify: could not load ' + TARGET_MAP)
    if STRIP_ROOM_ACTORS:
        record('clone_has_no_room_trigger', count_class('RoomTrigger') == 0,
               'count=' + str(count_class('RoomTrigger')))
        record('clone_has_no_room_exit', count_class('RoomExit') == 0,
               'count=' + str(count_class('RoomExit')))
    else:
        record('clone_exactly_one_room_trigger', count_class('RoomTrigger') == 1,
               'count=' + str(count_class('RoomTrigger')))
        record('clone_exactly_one_room_exit', count_class('RoomExit') == 1,
               'count=' + str(count_class('RoomExit')))
    record('clone_has_player_start', count_class('PlayerStart') >= 1,
           'count=' + str(count_class('PlayerStart')))

    report = {
        'mode': 'verify',
        'target': TARGET_MAP,
        'checks': checks,
        'success': all(check['passed'] for check in checks),
    }
    write_report(VERIFY_REPORT, report)
    if not report['success']:
        failed = [check['check'] for check in checks if not check['passed']]
        raise RuntimeError('Room map clone verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_ROOM_MAP_CLONE_VERIFIED ' + str(VERIFY_REPORT))


if is_verify_mode():
    run_verify()
else:
    run_generate()
