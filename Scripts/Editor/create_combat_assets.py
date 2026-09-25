"""Create /Game/UEMMO/Combat/Definitions attack data assets from Data/combat-attacks.json.

Run inside the UE editor only (UnrealEditor-Cmd -run=pythonscript). Two modes,
selected by a token on the UE command line (same pattern as create_training_room.py):
  generate (default) - validate the source JSON and load all four animations
                       BEFORE writing anything; then create or update the four
                       DA_* UAttackDefinition assets in place (never duplicate
                       assets with _1/_2 suffixes), save them, and write
                       Artifacts/Logs/combat-assets-report.json.
  verify (UEMMOCombatAssetsVerify) - after a fresh process start, reload the
                       saved assets from disk and compare every field against
                       the same JSON; writes
                       Artifacts/Logs/combat-assets-verify.json plus a copy as
                       json-vs-assets.json under UEMMOCombatVerifyDir.

Optional environment overrides:
  UEMMOCombatDataJson  - source JSON path. Default Data/combat-attacks.json.
                         Lets a red-line test point at a bad sample COPY without
                         touching the official file.
  UEMMOCombatVerifyDir - extra directory that receives the verify report copy.

Atomicity contract: structural validation and all four animation loads happen
before the first create_asset call; any error aborts the whole run (nonzero
exit via exception) and leaves the existing assets and every other directory
untouched. The report is written in both outcomes: success=false on failure.

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
DEFINITIONS_PATH = '/Game/UEMMO/Combat/Definitions'
ASSET_PREFIX = 'DA_'
EXPECTED_IDS = ('light_01', 'light_02', 'launcher', 'aerial_01')
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-assets-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-assets-verify.json'

STRING_FIELDS = ('attack_id', 'animation_path')
NUMBER_FIELDS = (
    'duration_frames', 'active_start_frame', 'active_end_frame',
    'cancel_start_frame', 'cancel_end_frame', 'base_damage', 'attack_coefficient',
    'knockback_cm_per_s', 'launch_cm_per_s', 'hit_stun_seconds', 'hit_stop_seconds',
    'clip_start_seconds', 'clip_end_seconds')
VECTOR_FIELDS = ('hit_offset_cm', 'hit_half_extent_cm')
STRING_ARRAY_FIELDS = ('next_attack_ids', 'cancel_window_allows')
FLOAT_TOLERANCE = 1e-6


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMOCombatAssetsVerify' in command_line()


def source_json_path():
    override = os.environ.get('UEMMOCombatDataJson')
    if override:
        return override
    return str(ROOT / 'Data' / 'combat-attacks.json')


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def is_integral(value):
    return is_number(value) and float(value).is_integer()


def validate_entry(entry, index, id_set, problems):
    """Mirrors the M1-008 structural rules for one attacks[] entry."""
    label = 'attacks[' + str(index) + ']'
    attack_id = entry.get('attack_id') if isinstance(entry, dict) else None
    if isinstance(attack_id, str) and attack_id:
        label = "attack '" + attack_id + "'"

    def problem(field, reason):
        problems.append("FAIL " + label + " field '" + field + "': " + reason)

    for field in STRING_FIELDS:
        value = entry.get(field) if isinstance(entry, dict) else None
        if not isinstance(value, str):
            problem(field, 'required string field is missing or not a string.')
    for field in NUMBER_FIELDS:
        value = entry.get(field) if isinstance(entry, dict) else None
        if not is_number(value):
            problem(field, 'required number field is missing or not a number.')
    for field in STRING_ARRAY_FIELDS:
        value = entry.get(field) if isinstance(entry, dict) else None
        if not isinstance(value, list) or not all(isinstance(x, str) for x in value):
            problem(field, 'required string array field is missing or holds a non-string element.')
    if not isinstance(entry.get('placeholder_animation'), bool):
        problem('placeholder_animation', 'required boolean field is missing or not a boolean.')
    for field in VECTOR_FIELDS:
        value = entry.get(field) if isinstance(entry, dict) else None
        if not isinstance(value, list) or len(value) != 3 or not all(is_number(x) for x in value):
            problem(field, 'must hold exactly 3 numeric components.')
        elif field == 'hit_half_extent_cm' and any(x <= 0 for x in value):
            problem(field, 'every component must be > 0.')

    duration = entry.get('duration_frames')
    active_start = entry.get('active_start_frame')
    active_end = entry.get('active_end_frame')
    cancel_start = entry.get('cancel_start_frame')
    cancel_end = entry.get('cancel_end_frame')
    if is_number(duration):
        if not is_integral(duration):
            problem('duration_frames', 'must be an integer.')
        elif duration <= 0:
            problem('duration_frames', 'must be > 0.')
    if is_number(active_start) and active_start < 0:
        problem('active_start_frame', 'must be >= 0.')
    if is_number(active_start) and is_number(active_end) and active_end <= active_start:
        problem('active_end_frame', 'must be > active_start_frame.')
    if is_number(active_end) and is_number(duration) and active_end > duration:
        problem('active_end_frame', 'must be <= duration_frames.')
    if is_number(cancel_start) and cancel_start < 0:
        problem('cancel_start_frame', 'must be >= 0.')
    if is_number(cancel_start) and is_number(cancel_end) and cancel_end <= cancel_start:
        problem('cancel_end_frame', 'must be > cancel_start_frame.')
    if is_number(cancel_end) and is_number(duration) and cancel_end > duration:
        problem('cancel_end_frame', 'must be <= duration_frames.')

    if is_number(entry.get('base_damage')) and entry['base_damage'] < 0:
        problem('base_damage', 'must be >= 0.')
    if is_number(entry.get('attack_coefficient')) and entry['attack_coefficient'] <= 0:
        problem('attack_coefficient', 'must be > 0.')
    for field in ('knockback_cm_per_s', 'launch_cm_per_s', 'hit_stun_seconds', 'hit_stop_seconds',
                  'clip_start_seconds', 'clip_end_seconds'):
        if is_number(entry.get(field)) and entry[field] < 0:
            problem(field, 'must be >= 0.')
    if entry.get('placeholder_animation') is False and is_number(entry.get('clip_start_seconds')) \
            and is_number(entry.get('clip_end_seconds')) and entry['clip_end_seconds'] < entry['clip_start_seconds']:
        problem('clip_end_seconds', 'must be >= clip_start_seconds for a real (non-placeholder) animation.')

    animation_path = entry.get('animation_path')
    if isinstance(animation_path, str):
        if not animation_path.strip():
            problem('animation_path', 'must be a non-empty animation path.')
        elif not animation_path.startswith('/Game/'):
            problem('animation_path', "must start with '/Game/'.")

    next_ids = entry.get('next_attack_ids')
    if isinstance(next_ids, list):
        for element in next_ids:
            if isinstance(element, str) and element not in id_set:
                problem('next_attack_ids', "references unknown attack id '" + element + "'.")
    allows = entry.get('cancel_window_allows')
    if isinstance(allows, list):
        for element in allows:
            if isinstance(element, str) and element not in ('attack', 'jump'):
                problem('cancel_window_allows', "unknown cancel kind '" + element + "' (allowed: attack, jump).")


def validate_document(doc, source_path):
    """Returns (entries, animations, problems).

    animations maps attack_id to the loaded UAnimSequence; the load happens
    only after the structure is sound, still before any asset write. Every
    path must resolve to a loaded UAnimSequence.
    """
    problems = []
    entries = []
    animations = {}
    if not isinstance(doc, dict):
        problems.append('FAIL: top-level JSON value must be an object.')
        return entries, animations, problems
    if doc.get('schema_version') != 1:
        problems.append("FAIL field 'schema_version': must be the number 1 (got "
                        + repr(doc.get('schema_version')) + ').')
    if doc.get('logic_fps') != 60:
        problems.append("FAIL field 'logic_fps': must be the number 60 (got "
                        + repr(doc.get('logic_fps')) + ').')
    attacks = doc.get('attacks')
    if not isinstance(attacks, list):
        problems.append("FAIL field 'attacks': must be a JSON array.")
        return entries, animations, problems
    if len(attacks) != 4:
        problems.append("FAIL field 'attacks': must hold exactly 4 entries (got " + str(len(attacks)) + ').')
    id_list = []
    for index, entry in enumerate(attacks):
        if not isinstance(entry, dict):
            problems.append('FAIL attacks[' + str(index) + ']: must be a JSON object.')
            continue
        attack_id = entry.get('attack_id')
        if not isinstance(attack_id, str) or not attack_id:
            problems.append("FAIL attacks[" + str(index) + "] field 'attack_id': must be a non-empty string.")
            continue
        id_list.append(attack_id)
    duplicates = sorted({x for x in id_list if id_list.count(x) > 1})
    for duplicate in duplicates:
        problems.append("FAIL field 'attack_id': duplicate id '" + duplicate + "' (ids must be unique).")
    for expected in EXPECTED_IDS:
        if expected not in id_list:
            problems.append("FAIL field 'attack_id': expected attack '" + expected + "' is missing from the catalog.")
    for attack_id in id_list:
        if attack_id not in EXPECTED_IDS:
            problems.append("FAIL field 'attack_id': unexpected attack id '" + attack_id + "'.")
    if problems:
        return entries, animations, problems

    id_set = set(id_list)
    for index, entry in enumerate(attacks):
        validate_entry(entry, index, id_set, problems)
    if problems:
        return entries, animations, problems

    # Animation loads happen only after the structure is sound, still before
    # any asset write. Every path must resolve to a loaded UAnimSequence.
    for entry in attacks:
        path = entry['animation_path']
        loaded = None
        try:
            loaded = unreal.load_asset(path)
        except Exception as error:
            problems.append("FAIL attack '" + entry['attack_id'] + "' field 'animation_path': load raised: " + str(error))
            continue
        if loaded is None:
            problems.append("FAIL attack '" + entry['attack_id'] + "' field 'animation_path': asset could not be loaded: " + path)
        elif not isinstance(loaded, unreal.AnimSequence):
            problems.append("FAIL attack '" + entry['attack_id'] + "' field 'animation_path': asset is not an AnimSequence: " + path)
        else:
            animations[entry['attack_id']] = loaded
    return attacks, animations, problems


def write_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2), encoding='utf-8')


def asset_name_for(attack_id):
    return ASSET_PREFIX + attack_id


def apply_window(asset, property_name, start_frame, end_frame):
    """Fills one FAttackFrameWindow member of the asset.

    The window UPROPERTYs are EditDefaultsOnly, which the Python wrappers only
    allow setting when the owner object counts as a template - and UPROPERTY
    assets do (PropertyAccessUtil::IsObjectTemplate treats assets as editable
    defaults). So the struct is taken as a reference FROM the asset (its owner
    is then the asset) instead of mutating a standalone struct wrapper, which
    would raise 'cannot be edited on instances'.
    """
    window = asset.get_editor_property(property_name)
    window.set_editor_property('start_frame', int(start_frame))
    window.set_editor_property('end_frame', int(end_frame))
    asset.set_editor_property(property_name, window)


def apply_fields(asset, entry, animations):
    """Fills every UAttackDefinition UPROPERTY from one JSON entry."""
    offset = entry['hit_offset_cm']
    extent = entry['hit_half_extent_cm']
    asset.set_editor_property('attack_id', entry['attack_id'])
    asset.set_editor_property('duration_frames', int(entry['duration_frames']))
    apply_window(asset, 'active_window', entry['active_start_frame'], entry['active_end_frame'])
    apply_window(asset, 'cancel_window', entry['cancel_start_frame'], entry['cancel_end_frame'])
    asset.set_editor_property('allowed_next_attacks', [str(x) for x in entry['next_attack_ids']])
    asset.set_editor_property('base_damage', float(entry['base_damage']))
    asset.set_editor_property('attack_coefficient', float(entry['attack_coefficient']))
    asset.set_editor_property('hit_offset_from_feet', unreal.Vector(float(offset[0]), float(offset[1]), float(offset[2])))
    asset.set_editor_property('hit_half_extent', unreal.Vector(float(extent[0]), float(extent[1]), float(extent[2])))
    asset.set_editor_property('knockback_speed', float(entry['knockback_cm_per_s']))
    asset.set_editor_property('launch_speed', float(entry['launch_cm_per_s']))
    asset.set_editor_property('hit_stun_seconds', float(entry['hit_stun_seconds']))
    asset.set_editor_property('hit_stop_seconds', float(entry['hit_stop_seconds']))
    # TSoftObjectPtr<UAnimSequence> has no SoftObjectPath nativization in
    # UE 5.8 Python: the property is set from the already-loaded AnimSequence
    # object, which stores the same FSoftObjectPath on disk (no hard
    # dependency is serialized).
    asset.set_editor_property('animation', animations[entry['attack_id']])
    asset.set_editor_property('clip_start_seconds', float(entry['clip_start_seconds']))
    asset.set_editor_property('clip_end_seconds', float(entry['clip_end_seconds']))
    asset.set_editor_property('placeholder_animation', bool(entry['placeholder_animation']))


def run_generate():
    source_path = source_json_path()
    doc = json.loads(Path(source_path).read_text(encoding='utf-8'))
    entries, animations, problems = validate_document(doc, source_path)
    if problems:
        write_report(GENERATE_REPORT, {
            'mode': 'generate',
            'source_json': source_path,
            'created': [],
            'updated': [],
            'failed': [],
            'errors': problems,
            'success': False,
        })
        raise RuntimeError('combat-attacks.json validation failed; no asset was written.\n'
                           + '\n'.join(problems))

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = []
    updated = []
    for entry in entries:
        asset_name = asset_name_for(entry['attack_id'])
        asset_path = DEFINITIONS_PATH + '/' + asset_name
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            asset = unreal.load_asset(asset_path)
            if asset is None:
                raise RuntimeError('Existing asset exists on disk but could not be loaded: ' + asset_path)
            updated.append(asset_name)
        else:
            asset = asset_tools.create_asset(asset_name, DEFINITIONS_PATH, unreal.AttackDefinition, unreal.DataAssetFactory())
            if asset is None:
                raise RuntimeError('create_asset returned null for ' + asset_path)
            created.append(asset_name)
        apply_fields(asset, entry, animations)

    # UE 5.8 signature: save_directory(directory_path, only_if_is_dirty=True,
    # recursive=True); only dirty packages (the ones this run touched) save.
    if not unreal.EditorAssetLibrary.save_directory(DEFINITIONS_PATH):
        raise RuntimeError('save_directory failed for ' + DEFINITIONS_PATH)
    saved = [asset_name_for(x) for x in EXPECTED_IDS
             if unreal.EditorAssetLibrary.does_asset_exist(DEFINITIONS_PATH + '/' + asset_name_for(x))]
    if len(saved) != 4:
        raise RuntimeError('Expected 4 saved assets, found: ' + repr(saved))
    write_report(GENERATE_REPORT, {
        'mode': 'generate',
        'source_json': source_path,
        'created': created,
        'updated': updated,
        'failed': [],
        'errors': [],
        'saved': saved,
        'success': True,
    })
    unreal.log('UEMMO_COMBAT_ASSETS_SUCCESS created=' + repr(created) + ' updated=' + repr(updated))


def floats_match(expected, actual):
    return abs(expected - actual) <= FLOAT_TOLERANCE * max(1.0, abs(expected))


def check_field(checks, asset_name, field, expected, actual, match):
    checks.append({
        'asset': asset_name,
        'field': field,
        'json_value': expected,
        'asset_value': actual,
        'match': bool(match),
    })


def verify_entry(asset, entry, checks):
    """Compares one loaded asset against its JSON entry; returns True on full match."""
    asset_name = asset_name_for(entry['attack_id'])
    ok = True

    def record(field, expected, actual, match):
        nonlocal ok
        check_field(checks, asset_name, field, expected, actual, match)
        if not match:
            ok = False

    record('attack_id', entry['attack_id'], str(asset.get_editor_property('attack_id')),
           str(asset.get_editor_property('attack_id')) == entry['attack_id'])
    record('duration_frames', entry['duration_frames'], asset.get_editor_property('duration_frames'),
           int(asset.get_editor_property('duration_frames')) == int(entry['duration_frames']))

    active = asset.get_editor_property('active_window')
    cancel = asset.get_editor_property('cancel_window')
    record('active_window.start_frame', entry['active_start_frame'], active.get_editor_property('start_frame'),
           int(active.get_editor_property('start_frame')) == int(entry['active_start_frame']))
    record('active_window.end_frame', entry['active_end_frame'], active.get_editor_property('end_frame'),
           int(active.get_editor_property('end_frame')) == int(entry['active_end_frame']))
    record('cancel_window.start_frame', entry['cancel_start_frame'], cancel.get_editor_property('start_frame'),
           int(cancel.get_editor_property('start_frame')) == int(entry['cancel_start_frame']))
    record('cancel_window.end_frame', entry['cancel_end_frame'], cancel.get_editor_property('end_frame'),
           int(cancel.get_editor_property('end_frame')) == int(entry['cancel_end_frame']))

    next_ids = [str(x) for x in asset.get_editor_property('allowed_next_attacks')]
    expected_next = [str(x) for x in entry['next_attack_ids']]
    record('allowed_next_attacks', expected_next, next_ids, next_ids == expected_next)

    for field, property_name in (
            ('base_damage', 'base_damage'),
            ('attack_coefficient', 'attack_coefficient'),
            ('knockback_cm_per_s', 'knockback_speed'),
            ('launch_cm_per_s', 'launch_speed'),
            ('hit_stun_seconds', 'hit_stun_seconds'),
            ('hit_stop_seconds', 'hit_stop_seconds'),
            ('clip_start_seconds', 'clip_start_seconds'),
            ('clip_end_seconds', 'clip_end_seconds')):
        expected = float(entry[field])
        actual = float(asset.get_editor_property(property_name))
        record(field, expected, actual, floats_match(expected, actual))

    for field, property_name in (('hit_offset_cm', 'hit_offset_from_feet'),
                                 ('hit_half_extent_cm', 'hit_half_extent')):
        vector = asset.get_editor_property(property_name)
        actual = [float(vector.x), float(vector.y), float(vector.z)]
        expected = [float(x) for x in entry[field]]
        match = len(actual) == 3 and all(floats_match(e, a) for e, a in zip(expected, actual))
        record(field, expected, actual, match)

    # Make sure the referenced animation is in memory so the TSoftObjectPtr
    # resolves in this fresh process; a wrong saved path yields None or a
    # different object and fails the comparison.
    unreal.load_asset(entry['animation_path'])
    soft_object = asset.get_editor_property('animation')
    if soft_object is None:
        record('animation_path', entry['animation_path'], None, False)
    else:
        actual_path = str(soft_object.get_path_name())
        asset_base = entry['animation_path'].rsplit('/', 1)[-1]
        # GetPathName is Package.Object; FSoftObjectPath hides the suffix when
        # it matches the package name, so both forms are accepted.
        match = actual_path in (entry['animation_path'], entry['animation_path'] + '.' + asset_base)
        record('animation_path', entry['animation_path'], actual_path, match)

    record('placeholder_animation', entry['placeholder_animation'], asset.get_editor_property('placeholder_animation'),
           bool(asset.get_editor_property('placeholder_animation')) == bool(entry['placeholder_animation']))
    return ok


def run_verify():
    source_path = source_json_path()
    doc = json.loads(Path(source_path).read_text(encoding='utf-8'))
    entries, animations, problems = validate_document(doc, source_path)
    if problems:
        raise RuntimeError('combat-attacks.json validation failed during verify.\n' + '\n'.join(problems))

    checks = []
    all_ok = True
    for entry in entries:
        asset_path = DEFINITIONS_PATH + '/' + asset_name_for(entry['attack_id'])
        asset = unreal.load_asset(asset_path)
        if asset is None:
            all_ok = False
            check_field(checks, asset_name_for(entry['attack_id']), 'asset_loaded', True, False, False)
            continue
        if not verify_entry(asset, entry, checks):
            all_ok = False

    listed = unreal.EditorAssetLibrary.list_assets(DEFINITIONS_PATH, recursive=False, include_folder=False)
    # UE 5.8 returns object paths ("/Game/.../DA_light_01.DA_light_01");
    # keep the object name only.
    directory_listing = sorted(Path(str(p)).name.split('.')[0] for p in listed)
    expected_listing = sorted(asset_name_for(x) for x in EXPECTED_IDS)
    directory_ok = directory_listing == expected_listing
    if not directory_ok:
        all_ok = False

    report = {
        'mode': 'verify',
        'source_json': source_path,
        'asset_count': len(directory_listing),
        'expected_asset_count': 4,
        'directory_listing': directory_listing,
        'directory_matches_expected': directory_ok,
        'checks': checks,
        'failed_checks': [c for c in checks if not c['match']],
        'success': all_ok,
    }
    write_report(VERIFY_REPORT, report)
    verify_dir = os.environ.get('UEMMOCombatVerifyDir')
    if verify_dir:
        write_report(Path(verify_dir) / 'json-vs-assets.json', report)
    unreal.log('UEMMO_COMBAT_ASSETS_VERIFY_SUCCESS' if all_ok else 'UEMMO_COMBAT_ASSETS_VERIFY_FAILED')
    if not all_ok:
        raise RuntimeError('Verify: loaded assets do not match the source JSON; see combat-assets-verify.json.')


def main():
    if is_verify_mode():
        run_verify()
    else:
        run_generate()


main()
