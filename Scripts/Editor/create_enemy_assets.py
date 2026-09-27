"""Create /Game/UEMMO/Enemy/Definitions enemy data assets from Data/enemies.json.

Run inside the UE editor only (UnrealEditor-Cmd -run=pythonscript). Two modes,
selected by a token on the UE command line (the M1-009 create_combat_assets.py
pattern):
  generate (default) - validate the source JSON BEFORE writing anything; then
                       create or update the DA_* UEnemyDefinition assets in
                       place (never duplicating assets with _1/_2 suffixes),
                       save them, and write
                       Artifacts/Logs/enemy-assets-report.json.
  verify (UEMMOEnemyAssetsVerify) - after a fresh process start, reload the
                       saved assets from disk and compare every field against
                       the same JSON; writes
                       Artifacts/Logs/enemy-assets-verify.json.

Optional environment override:
  UEMMOEnemyDataJson - source JSON path. Default Data/enemies.json.

Atomicity contract: structural validation happens before the first
create_asset call; any error aborts the whole run (nonzero exit via
exception) and leaves the existing assets and every other directory
untouched. The report is written in both outcomes: success=false on failure.
Idempotent: a repeated run loads the existing asset and re-applies every
field (update, not duplicate).

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
DEFINITIONS_PATH = '/Game/UEMMO/Enemy/Definitions'
ASSET_PREFIX = 'DA_'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'enemy-assets-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'enemy-assets-verify.json'

STRING_FIELDS = ('enemy_id', 'melee_attack_id')
NUMBER_FIELDS = (
    'max_hp', 'attack_power', 'move_speed', 'attack_range_x',
    'align_y_tolerance', 'telegraph_seconds', 'spawn_grace_seconds')
FLOAT_TOLERANCE = 1e-6


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMOEnemyAssetsVerify' in command_line()


def source_json_path():
    override = os.environ.get('UEMMOEnemyDataJson')
    if override:
        return override
    return str(ROOT / 'Data' / 'enemies.json')


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def enemy_class():
    """The UEMMO UEnemyDefinition class (build the UEMMO module first)."""
    candidate = getattr(unreal, 'EnemyDefinition', None)
    if candidate is not None:
        return candidate
    loaded = unreal.load_class(None, '/Script/UEMMO.EnemyDefinition')
    if loaded is None:
        raise RuntimeError('UEMMO.EnemyDefinition not found; build the UEMMO module first.')
    return loaded


def asset_name_for(enemy_id):
    return ASSET_PREFIX + enemy_id


def validate_document(doc, source_path):
    """Structural check of Data/enemies.json (schema_version 1, enemies[])."""
    problems = []
    entries = []
    if not isinstance(doc, dict):
        raise RuntimeError(source_path + ': the document is not a JSON object.')
    if doc.get('schema_version') != 1:
        problems.append("FAIL schema_version: expected 1.")
    rows = doc.get('enemies')
    if not isinstance(rows, list) or not rows:
        raise RuntimeError(source_path + ": 'enemies' is missing or empty; nothing to generate.")
    id_set = set()
    for index, entry in enumerate(rows):
        label = 'enemies[' + str(index) + ']'
        if not isinstance(entry, dict):
            problems.append('FAIL ' + label + ': entry is not a JSON object.')
            continue
        enemy_id = entry.get('enemy_id')
        if isinstance(enemy_id, str) and enemy_id:
            label = "enemy '" + enemy_id + "'"
        if not isinstance(enemy_id, str) or not enemy_id:
            problems.append("FAIL " + label + " field 'enemy_id': required non-empty string.")
        elif enemy_id in id_set:
            problems.append("FAIL " + label + " field 'enemy_id': duplicate enemy id.")
        else:
            id_set.add(enemy_id)
        for field in STRING_FIELDS:
            value = entry.get(field)
            if not isinstance(value, str) or not value:
                problems.append("FAIL " + label + " field '" + field
                                + "': required non-empty string field is missing or empty.")
        for field in NUMBER_FIELDS:
            value = entry.get(field)
            if not is_number(value):
                problems.append("FAIL " + label + " field '" + field
                                + "': required number field is missing or not a number.")
        if is_number(entry.get('max_hp')) and entry['max_hp'] <= 0:
            problems.append("FAIL " + label + " field 'max_hp': must be > 0.")
        if is_number(entry.get('move_speed')) and entry['move_speed'] <= 0:
            problems.append("FAIL " + label + " field 'move_speed': must be > 0.")
        if is_number(entry.get('attack_range_x')) and entry['attack_range_x'] <= 0:
            problems.append("FAIL " + label + " field 'attack_range_x': must be > 0.")
        if is_number(entry.get('align_y_tolerance')) and entry['align_y_tolerance'] <= 0:
            problems.append("FAIL " + label + " field 'align_y_tolerance': must be > 0.")
        for field in ('attack_power', 'telegraph_seconds', 'spawn_grace_seconds'):
            if is_number(entry.get(field)) and entry[field] < 0:
                problems.append("FAIL " + label + " field '" + field + "': must be >= 0.")
        if all(isinstance(entry.get(f), str) and entry.get(f) for f in STRING_FIELDS) \
                and all(is_number(entry.get(f)) for f in NUMBER_FIELDS):
            entries.append(entry)
    if problems:
        raise RuntimeError('enemies.json validation failed; no asset was written.\n'
                           + '\n'.join(problems))
    if not entries:
        raise RuntimeError('enemies.json holds no valid enemy row; no asset was written.')
    return entries


def apply_fields(asset, entry):
    """Writes every UEnemyDefinition field of one enemies.json row."""
    # UEnemyDefinition is a pure data asset: set_editor_property is the
    # supported write path (the M1-009 attack-definition precedent). FName
    # properties convert from str; floats convert from numbers.
    asset.set_editor_property('enemy_id', str(entry['enemy_id']))
    asset.set_editor_property('max_hp', float(entry['max_hp']))
    asset.set_editor_property('attack_power', float(entry['attack_power']))
    asset.set_editor_property('move_speed', float(entry['move_speed']))
    asset.set_editor_property('attack_range_x', float(entry['attack_range_x']))
    asset.set_editor_property('align_y_tolerance', float(entry['align_y_tolerance']))
    asset.set_editor_property('telegraph_seconds', float(entry['telegraph_seconds']))
    asset.set_editor_property('spawn_grace_seconds', float(entry['spawn_grace_seconds']))
    asset.set_editor_property('melee_attack_id', str(entry['melee_attack_id']))


def run_generate():
    source_path = source_json_path()
    doc = json.loads(Path(source_path).read_text(encoding='utf-8'))
    entries = validate_document(doc, source_path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = []
    updated = []
    for entry in entries:
        asset_name = asset_name_for(entry['enemy_id'])
        asset_path = DEFINITIONS_PATH + '/' + asset_name
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            asset = unreal.load_asset(asset_path)
            if asset is None:
                raise RuntimeError('Existing asset exists on disk but could not be loaded: ' + asset_path)
            updated.append(asset_name)
        else:
            asset = asset_tools.create_asset(asset_name, DEFINITIONS_PATH, enemy_class(),
                                             unreal.DataAssetFactory())
            if asset is None:
                raise RuntimeError('create_asset returned null for ' + asset_path)
            created.append(asset_name)
        apply_fields(asset, entry)

    # UE 5.8 signature: save_directory(directory_path, only_if_is_dirty=True,
    # recursive=True); only dirty packages (the ones this run touched) save.
    if not unreal.EditorAssetLibrary.save_directory(DEFINITIONS_PATH):
        raise RuntimeError('save_directory failed for ' + DEFINITIONS_PATH)
    saved = [asset_name_for(x['enemy_id']) for x in entries
             if unreal.EditorAssetLibrary.does_asset_exist(DEFINITIONS_PATH + '/' + asset_name_for(x['enemy_id']))]
    if len(saved) != len(entries):
        raise RuntimeError('Expected ' + str(len(entries)) + ' saved assets, found: ' + repr(saved))
    write_report(GENERATE_REPORT, {
        'mode': 'generate',
        'source_json': source_path,
        'created': created,
        'updated': updated,
        'saved': saved,
        'success': True,
    })
    unreal.log('UEMMO_ENEMY_ASSETS_SUCCESS created=' + repr(created) + ' updated=' + repr(updated))


def floats_match(expected, actual):
    return abs(expected - actual) <= FLOAT_TOLERANCE * max(1.0, abs(expected))


def verify_entry(asset, entry, checks):
    """Compares one loaded asset against its JSON row; returns True on full match."""
    asset_name = asset_name_for(entry['enemy_id'])
    ok = True

    def record(field, expected, actual, match):
        nonlocal ok
        checks.append({
            'asset': asset_name,
            'field': field,
            'json_value': expected,
            'asset_value': actual,
            'match': bool(match),
        })
        if not match:
            ok = False

    record('enemy_id', entry['enemy_id'], str(asset.get_editor_property('enemy_id')),
           str(asset.get_editor_property('enemy_id')) == entry['enemy_id'])
    for field, property_name in (
            ('max_hp', 'max_hp'),
            ('attack_power', 'attack_power'),
            ('move_speed', 'move_speed'),
            ('attack_range_x', 'attack_range_x'),
            ('align_y_tolerance', 'align_y_tolerance'),
            ('telegraph_seconds', 'telegraph_seconds'),
            ('spawn_grace_seconds', 'spawn_grace_seconds')):
        expected = float(entry[field])
        actual = float(asset.get_editor_property(property_name))
        record(field, expected, actual, floats_match(expected, actual))
    record('melee_attack_id', entry['melee_attack_id'], str(asset.get_editor_property('melee_attack_id')),
           str(asset.get_editor_property('melee_attack_id')) == entry['melee_attack_id'])
    return ok


def run_verify():
    source_path = source_json_path()
    doc = json.loads(Path(source_path).read_text(encoding='utf-8'))
    entries = validate_document(doc, source_path)

    checks = []
    all_ok = True
    for entry in entries:
        asset_path = DEFINITIONS_PATH + '/' + asset_name_for(entry['enemy_id'])
        asset = unreal.load_asset(asset_path)
        if asset is None:
            all_ok = False
            checks.append({
                'asset': asset_name_for(entry['enemy_id']),
                'field': 'asset_loaded',
                'json_value': True,
                'asset_value': False,
                'match': False,
            })
            continue
        if not verify_entry(asset, entry, checks):
            all_ok = False

    listed = unreal.EditorAssetLibrary.list_assets(DEFINITIONS_PATH, recursive=False, include_folder=False)
    # UE 5.8 returns object paths ("/Game/.../DA_melee_grunt.DA_melee_grunt");
    # keep the object name only.
    directory_listing = sorted(Path(str(p)).name.split('.')[0] for p in listed)
    expected_listing = sorted(asset_name_for(x['enemy_id']) for x in entries)
    directory_ok = directory_listing == expected_listing
    if not directory_ok:
        all_ok = False

    report = {
        'mode': 'verify',
        'source_json': source_path,
        'asset_count': len(entries),
        'directory_listing': directory_listing,
        'directory_ok': directory_ok,
        'checks': checks,
        'success': all_ok,
    }
    VERIFY_REPORT.parent.mkdir(parents=True, exist_ok=True)
    VERIFY_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not report['success']:
        failed = [check['field'] for check in checks if not check['match']]
        raise RuntimeError('Enemy asset verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_ENEMY_ASSETS_VERIFIED ' + str(VERIFY_REPORT))


def write_report(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding='utf-8')


if is_verify_mode():
    run_verify()
else:
    run_generate()
