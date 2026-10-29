"""Clone one combat-room umap for the M6-004 multi-level production: the
source map (default /Game/UEMMO/Maps/L_CombatRoom01) duplicates to the target
(default /Game/UEMMO/Maps/L_CombatRoom02) with its full orchestration
(RoomTrigger, RoomExit, floor, PlayerStart). Run inside the UE editor
(commandlet) only.

The clone is the MAP production step only: the duplicated trigger still
references the SOURCE row's definition asset - Scripts/PrepareRooms.ps1 owns
the per-row DA generation and the trigger rewiring (its verify pass asserts
every wired reference).

Two modes, selected by a token on the UE command line:
  generate (default) - duplicate the map when the target is absent; an
                       existing target re-saves untouched (idempotent rerun)
                       and reports created=false. Writes
                       Artifacts/Logs/room-map-clone-report.json.
  verify (UEMMORoomMapCloneVerify) - a fresh process reloads the target from
                       disk and asserts the cloned orchestration: exactly one
                       RoomTrigger, one RoomExit and one PlayerStart. Writes
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

    # Touch-save the clone so the on-disk package is this process's product
    # (a fresh process reload in the verify pass then reads what we wrote).
    if not levels.load_level(TARGET_MAP):
        raise RuntimeError('Could not load the clone: ' + TARGET_MAP)
    if not levels.save_current_level():
        raise RuntimeError('Could not save the clone: ' + TARGET_MAP)

    write_report(GENERATE_REPORT, {
        'mode': 'generate',
        'source': SOURCE_MAP,
        'target': TARGET_MAP,
        'created': created,
        'trigger_count': count_class('RoomTrigger'),
        'exit_count': count_class('RoomExit'),
        'success': True,
    })
    unreal.log('UEMMO_ROOM_MAP_CLONED target=' + TARGET_MAP + ' created=' + str(created))


def run_verify():
    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    record('clone_asset_exists', unreal.EditorAssetLibrary.does_asset_exist(TARGET_MAP), 'target=' + TARGET_MAP)
    loaded = unreal.load_asset(TARGET_MAP)
    record('clone_loads_from_disk', loaded is not None, 'target=' + TARGET_MAP)

    if not levels.load_level(TARGET_MAP):
        raise RuntimeError('Verify: could not load ' + TARGET_MAP)
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
