"""Create /Game/UEMMO/Combat/Data combat data assets from Data/CombatSystem.

Run inside the UE editor only (UnrealEditor-Cmd -run=pythonscript). This is the
M5-008 editor entry: it calls the C++ builder
UCombatDataAssetBuilderLibrary.build_from_source_directory, which parses every
required table of the source directory, validates it with the full cross-table
reference gate (including the presentations table) and then builds one data
asset per row of the six tables under /Game/UEMMO/Combat/Data
(DA_Damage_*, DA_Reaction_*, DA_Target_*, DA_Weapon_*, DA_Ammo_*,
DA_Projectile_*), created or updated in place, never duplicated.

The heavy lifting (strict table schema, local row validators, reference gate,
idempotent asset build, package save) lives in
Source/UEMMO/Combat/Data/CombatDataAssetBuilder.h/.cpp; this script is the
editor wrapper that reports the outcome to
Artifacts/Logs/combat-system-assets-report.json:
  {mode, source_dir, success, created, updated, config_revision, errors}

Optional environment overrides:
  UEMMOCombatDataSourceDir - source directory. Default Data/CombatSystem
                             (project-relative paths resolve against the
                             project directory).
  UEMMOCombatReportDir     - extra directory that receives a report copy.

Failure contract: the builder validates everything before touching anything,
so any parse/validation/save failure leaves the existing assets untouched and
reports success=false with the builder's error text naming the offending
table, row and field. The script raises after writing the report so the UE
process exits nonzero.

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
DEFAULT_SOURCE_DIR = 'Data/CombatSystem'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-system-assets-report.json'


def source_dir():
    override = os.environ.get('UEMMOCombatDataSourceDir')
    if override:
        return override
    return DEFAULT_SOURCE_DIR


def write_report(report):
    GENERATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(report, indent=2)
    GENERATE_REPORT.write_text(payload, encoding='utf-8')
    extra = os.environ.get('UEMMOCombatReportDir')
    if extra:
        extra_dir = Path(extra)
        extra_dir.mkdir(parents=True, exist_ok=True)
        (extra_dir / GENERATE_REPORT.name).write_text(payload, encoding='utf-8')


def build_system_test_room_map():
    """M5-018A: create (or verify) the L_SystemTestRoom map.

    The map is a duplicate of the M0 arena (floor, walls, PlayerStart) with
    exactly one ACombatTestRoomDriver actor placed on it; the driver loads
    Data/test_room.json at BeginPlay and spawns the configured targets, so
    the map itself carries no per-content content (the config is the source).
    Modes via the UEMMO_SYSTEM_TEST_ROOM env var: build (default) / verify
    (fresh-process reload check of the saved map and its driver actor).
    """
    mode = (os.environ.get('UEMMO_SYSTEM_TEST_ROOM') or 'build').lower()
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    system_test_map = '/Game/UEMMO/Maps/L_SystemTestRoom'
    base_map = '/Game/UEMMO/Maps/L_PrototypeArena'
    driver_class = getattr(unreal, 'CombatTestRoomDriver', None)
    if driver_class is None:
        loaded = unreal.load_class(None, '/Script/UEMMO.CombatTestRoomDriver')
        if loaded is None:
            raise RuntimeError('UEMMO.CombatTestRoomDriver not found; build the UEMMO module first.')
        driver_class = loaded
    report = {'mode': mode, 'map': system_test_map, 'success': False, 'errors': ''}
    try:
        if not assets.does_asset_exist(system_test_map):
            if not assets.does_asset_exist(base_map):
                raise RuntimeError('base map missing: ' + base_map)
            if not unreal.EditorAssetLibrary.duplicate_asset(base_map, system_test_map):
                raise RuntimeError('could not duplicate the arena into ' + system_test_map)
        if not levels.load_level(system_test_map):
            raise RuntimeError('could not load ' + system_test_map)
        drivers = [actor for actor in actor_sub.get_all_level_actors()
                   if actor.get_class().get_name() == 'CombatTestRoomDriver']
        if mode == 'verify':
            if len(drivers) != 1:
                raise RuntimeError('expected exactly one CombatTestRoomDriver, found ' + str(len(drivers)))
            if 'PlayerStart' not in [a.get_actor_label() for a in actor_sub.get_all_level_actors()]:
                raise RuntimeError('PlayerStart is missing from the system test room')
        else:
            if len(drivers) == 0:
                driver = actor_sub.spawn_actor_from_class(
                    driver_class, unreal.Vector(600.0, 600.0, 200.0), unreal.Rotator(0.0, 0.0, 0.0))
                if driver is None:
                    raise RuntimeError('could not spawn the CombatTestRoomDriver')
                driver.set_actor_label('SystemTestRoomDriver')
            elif len(drivers) > 1:
                raise RuntimeError('the room already holds ' + str(len(drivers)) + ' drivers; refusing to add more')
            if 'PlayerStart' not in [a.get_actor_label() for a in actor_sub.get_all_level_actors()]:
                raise RuntimeError('PlayerStart is missing from the base arena')
            if not levels.save_current_level():
                raise RuntimeError('could not save ' + system_test_map)
        report['success'] = True
        unreal.log('UEMMO_M5_018A_SYSTEM_TEST_ROOM_' + mode.upper() + '_SUCCESS')
    except Exception as error:  # noqa: BLE001 - reported then raised
        report['errors'] = str(error)
        write_report_named(report, 'system-test-room-report.json')
        unreal.log_error('UEMMO_M5_018A system test room failed: ' + str(error))
        raise
    write_report_named(report, 'system-test-room-report.json')


def write_report_named(report, file_name):
    payload = json.dumps(report, indent=2)
    target = GENERATE_REPORT.parent / file_name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(payload, encoding='utf-8')


def main():
    # The UFUNCTION returns the error text (empty on success); the out
    # parameters come back with it as one tuple:
    # (errors, created, updated, config_revision).
    errors, created, updated, revision = (
        unreal.CombatDataAssetBuilderLibrary.build_from_source_directory(source_dir()))
    success = not (errors or '').strip()
    report = {
        'mode': 'generate',
        'source_dir': source_dir(),
        'success': bool(success),
        'created': int(created),
        'updated': int(updated),
        'config_revision': revision or '',
        'errors': errors or '',
    }
    write_report(report)
    if not success:
        unreal.log_error(
            'combat system asset generation failed; no asset was touched: ' + (errors or 'unknown error'))
        raise RuntimeError('combat system asset generation failed')
    unreal.log('combat system asset generation succeeded: created=%d updated=%d revision=%s'
               % (int(created), int(updated), revision or ''))


if os.environ.get('UEMMO_SYSTEM_TEST_ROOM'):
    # M5-018A: the map build/verify mode replaces the table generation for
    # this invocation (one editor process, one write surface).
    build_system_test_room_map()
else:
    main()
