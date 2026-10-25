"""Create /Game/UEMMO/Rooms/DA_room_combat_01 from the Data/rooms.json
room_combat_01 row and wire the L_CombatRoom01 ARoomTrigger to it. Run inside
the UE editor (commandlet) only.

Two modes, selected by a token on the UE command line (the
create_enemy_assets.py / create_room_assets.py patterns):
  generate (default) - structurally validate Data/rooms.json BEFORE writing
                       anything (schema 1, unique room ids, the required
                       room_combat_01 row, wave count/location agreement);
                       then create or update DA_room_combat_01 in place
                       (never duplicating; auto-suffixed duplicates abort the
                       run), set the combat map's single ARoomTrigger
                       room_definition_asset soft reference to it, save both,
                       and write Artifacts/Logs/room-definition-assets-report.json.
  verify (UEMMORoomAssetVerify) - after a fresh process start, reload the
                       saved asset from disk and compare every field against
                       the same JSON; reload both maps and assert the combat
                       map carries exactly one trigger (referencing the asset)
                       and one exit while the training map carries neither;
                       write Artifacts/Logs/room-definition-assets-verify.json.

The map_path property stores the FULLY-QUALIFIED object path
(/Game/.../L_CombatRoom01.L_CombatRoom01); the JSON's package path is its
prefix. That is the only normalization this script applies.

Idempotent: a repeated run loads the existing asset and re-applies every
field (update, not duplicate), and re-asserts the trigger reference.
Atomicity: structural validation happens before the first write; any error
aborts the whole run (nonzero exit via exception). The report is written in
both outcomes: success=false on failure.
ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
ROOMS_PATH = '/Game/UEMMO/Rooms'
COMBAT_ASSET_NAME = 'DA_room_combat_01'
COMBAT_ASSET_PATH = ROOMS_PATH + '/' + COMBAT_ASSET_NAME
COMBAT_ASSET_OBJECT_PATH = COMBAT_ASSET_PATH + '.' + COMBAT_ASSET_NAME
COMBAT_ROOM_ID = 'room_combat_01'
BASE_MAP = '/Game/UEMMO/Maps/L_TrainingArena'
COMBAT_MAP = '/Game/UEMMO/Maps/L_CombatRoom01'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'room-definition-assets-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'room-definition-assets-verify.json'
FLOAT_TOLERANCE = 1e-6
SCHEMA_VERSION = 2
RULE_EVENTS = ('PlayerEnteredRegion', 'WaveCleared', 'PlayerDied')
RULE_EFFECTS = ('StartWave', 'MarkCleared', 'FailRun')
RULE_FIELDS = ('rule_id', 'event', 'region_id', 'wave_index', 'effect',
               'next_wave_index', 'delay_s', 'once')
ACTIVATION_REGION = 'activation'

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMORoomAssetVerify' in command_line()


def source_json_path():
    override = os.environ.get('UEMMORoomDataJson')
    if override:
        return override
    return str(ROOT / 'Data' / 'rooms.json')


def room_class():
    candidate = getattr(unreal, 'RoomDefinition', None)
    if candidate is not None:
        return candidate
    loaded = unreal.load_class(None, '/Script/UEMMO.RoomDefinition')
    if loaded is None:
        raise RuntimeError('UEMMO.RoomDefinition not found; build the UEMMO module first.')
    return loaded


def full_object_path(package_path):
    """Normalizes a /Game package path into its fully-qualified object path."""
    if '.' in package_path:
        return package_path
    return package_path + '.' + Path(package_path).name


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def read_vector(object_value, label, problems):
    if not isinstance(object_value, dict):
        problems.append('FAIL ' + label + ': expected an {x, y, z} object.')
        return None
    result = {}
    for axis in ('x', 'y', 'z'):
        axis_value = object_value.get(axis)
        if not is_number(axis_value):
            problems.append('FAIL ' + label + ' field ' + axis + ': required number.')
            return None
        result[axis] = float(axis_value)
    return result


def validate_rule(rule, wave_count, label, problems):
    """Structural check of one schema 2 rule row; returns the parsed dict or None."""
    if not isinstance(rule, dict):
        problems.append('FAIL ' + label + ': entry is not a JSON object.')
        return None
    for key in rule:
        if key not in RULE_FIELDS:
            problems.append('FAIL ' + label + " field '" + str(key)
                            + "': unknown rule field (allowed: " + ', '.join(RULE_FIELDS) + ').')
    rule_id = rule.get('rule_id')
    if not isinstance(rule_id, str) or not rule_id:
        problems.append('FAIL ' + label + " field 'rule_id': required non-empty string.")
        return None
    label = label + " '" + rule_id + "'"
    event = rule.get('event')
    if event not in RULE_EVENTS:
        problems.append('FAIL ' + label + " field 'event': unknown event '" + str(event)
                        + "' (allowed: " + ', '.join(RULE_EVENTS) + ').')
        return None
    effect = rule.get('effect')
    if effect not in RULE_EFFECTS:
        problems.append('FAIL ' + label + " field 'effect': unknown effect '" + str(effect)
                        + "' (allowed: " + ', '.join(RULE_EFFECTS) + ').')
        return None
    parsed = {'rule_id': rule_id, 'event': event, 'effect': effect, 'region_id': '',
              'wave_index': -1, 'next_wave_index': -1, 'delay_s': 0.0, 'once': True}
    if event == 'PlayerEnteredRegion':
        region = rule.get('region_id')
        if region != ACTIVATION_REGION:
            problems.append('FAIL ' + label + " field 'region_id': '" + str(region)
                            + "' is not a defined region (defined: " + ACTIVATION_REGION + ').')
        else:
            parsed['region_id'] = region
    if event == 'WaveCleared':
        wave_index = rule.get('wave_index')
        if not is_number(wave_index) or int(wave_index) != wave_index or not (0 <= int(wave_index) < wave_count):
            problems.append('FAIL ' + label + " field 'wave_index': required whole index in [0, "
                            + str(wave_count - 1) + '] (got ' + str(wave_index) + ').')
        else:
            parsed['wave_index'] = int(wave_index)
    if effect == 'StartWave':
        next_wave = rule.get('next_wave_index')
        if not is_number(next_wave) or int(next_wave) != next_wave or not (0 <= int(next_wave) < wave_count):
            problems.append('FAIL ' + label + " field 'next_wave_index': required whole index in [0, "
                            + str(wave_count - 1) + '] (got ' + str(next_wave) + ').')
        else:
            parsed['next_wave_index'] = int(next_wave)
    delay = rule.get('delay_s', 0.0)
    if not is_number(delay) or delay < 0:
        problems.append('FAIL ' + label + " field 'delay_s': required finite number >= 0 (got "
                        + str(delay) + ').')
    else:
        parsed['delay_s'] = float(delay)
    once = rule.get('once', True)
    if not isinstance(once, bool):
        problems.append('FAIL ' + label + " field 'once': required boolean.")
    else:
        parsed['once'] = once
    return parsed


def validate_document(doc, source_path):
    """Structural check of Data/rooms.json (schema 2 with finite rules);
    returns the room_combat_01 row with parsed rules."""
    problems = []
    if not isinstance(doc, dict):
        raise RuntimeError(source_path + ': the document is not a JSON object.')
    version = doc.get('schema_version')
    if version == 1:
        problems.append("FAIL schema_version: 1 is the legacy no-rules schema - migrate the"
                        " source to schema 2 (BuildLegacySchemaOneDefaultRules documents the"
                        " explicit schema 1 -> 2 mapping); nothing was generated.")
    elif version != SCHEMA_VERSION:
        problems.append('FAIL schema_version: expected ' + str(SCHEMA_VERSION) + ' (got '
                        + str(version) + ').')
    rows = doc.get('rooms')
    if not isinstance(rows, list) or not rows:
        raise RuntimeError(source_path + ": 'rooms' is missing or empty; nothing to generate.")
    id_set = set()
    combat_row = None
    for index, entry in enumerate(rows):
        label = 'rooms[' + str(index) + ']'
        if not isinstance(entry, dict):
            problems.append('FAIL ' + label + ': entry is not a JSON object.')
            continue
        room_id = entry.get('room_id')
        if isinstance(room_id, str) and room_id:
            label = "room '" + room_id + "'"
        if not isinstance(room_id, str) or not room_id:
            problems.append('FAIL ' + label + " field 'room_id': required non-empty string.")
        elif room_id in id_set:
            problems.append('FAIL ' + label + " field 'room_id': duplicate room id.")
        else:
            id_set.add(room_id)
        for field in ('map_path', 'reward_table_id'):
            value = entry.get(field)
            if not isinstance(value, str) or not value:
                problems.append('FAIL ' + label + " field '" + field
                                + "': required non-empty string field is missing or empty.")
        for field in ('player_spawn_location_cm', 'bounds_center_cm', 'bounds_size_cm'):
            read_vector(entry.get(field), label + ' field ' + field, problems)
        if not is_number(entry.get('player_spawn_yaw_degrees')):
            problems.append('FAIL ' + label + " field 'player_spawn_yaw_degrees': required number.")
        waves = entry.get('waves')
        if not isinstance(waves, list) or not waves:
            problems.append('FAIL ' + label + " field 'waves': required non-empty array.")
        else:
            for wave_index, wave in enumerate(waves):
                wave_label = label + ' wave ' + str(wave_index)
                if not isinstance(wave, dict):
                    problems.append('FAIL ' + wave_label + ': entry is not a JSON object.')
                    continue
                if not isinstance(wave.get('enemy_id'), str) or not wave.get('enemy_id'):
                    problems.append('FAIL ' + wave_label + " field 'enemy_id': required non-empty string.")
                count = wave.get('count')
                if not is_number(count) or isinstance(count, bool) or int(count) != count or int(count) <= 0:
                    problems.append('FAIL ' + wave_label + " field 'count': required whole number > 0.")
                    count = None
                locations = wave.get('spawn_locations_cm')
                if not isinstance(locations, list):
                    problems.append('FAIL ' + wave_label + " field 'spawn_locations_cm': required array.")
                elif count is not None and len(locations) != int(count):
                    problems.append('FAIL ' + wave_label + " field 'spawn_locations_cm': expected "
                                    + str(int(count)) + ' entries (count), found ' + str(len(locations)) + '.')
                else:
                    for loc_index, location in enumerate(locations):
                        read_vector(location, wave_label + ' spawn_locations_cm[' + str(loc_index) + ']', problems)
        rules = entry.get('rules', [])
        if not isinstance(rules, list):
            problems.append('FAIL ' + label + " field 'rules': required array (empty = no rule flow).")
            entry['parsed_rules'] = []
        else:
            parsed_rules = []
            rule_ids = set()
            wave_count = len(waves) if isinstance(waves, list) else 0
            for rule_index, rule in enumerate(rules):
                parsed_rule = validate_rule(rule, wave_count,
                                            label + ' rules[' + str(rule_index) + ']', problems)
                if parsed_rule is not None:
                    if parsed_rule['rule_id'] in rule_ids:
                        problems.append('FAIL ' + label + ' rules[' + str(rule_index)
                                        + "] field 'rule_id': duplicate '" + parsed_rule['rule_id'] + "'.")
                    else:
                        rule_ids.add(parsed_rule['rule_id'])
                    parsed_rules.append(parsed_rule)
            entry['parsed_rules'] = parsed_rules
        if room_id == COMBAT_ROOM_ID:
            combat_row = entry
    if problems:
        raise RuntimeError('rooms.json validation failed; nothing was written.\n' + '\n'.join(problems))
    if combat_row is None:
        raise RuntimeError("rooms.json carries no '" + COMBAT_ROOM_ID + "' row; nothing was written.")
    return combat_row


def wave_structs(row):
    waves = []
    for wave in row['waves']:
        struct = unreal.RoomWaveDefinition()
        struct.set_editor_property('enemy_id', str(wave['enemy_id']))
        struct.set_editor_property('count', int(wave['count']))
        struct.set_editor_property('spawn_locations', [
            unreal.Vector(float(location['x']), float(location['y']), float(location['z']))
            for location in wave['spawn_locations_cm']])
        waves.append(struct)
    return waves


def soft_path_string(value):
    """Robust string form of a soft-reference property read (SoftObjectPath
    or a resolved Object - the editor API returns either depending on the
    load state)."""
    if value is None:
        return ''
    if isinstance(value, unreal.SoftObjectPath):
        return str(value)
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    return str(value)


def rule_event_enum(name):
    """JSON event name -> unreal.RoomRuleEvent value (robust member lookup)."""
    token = {'PlayerEnteredRegion': 'PLAYER_ENTERED_REGION',
             'WaveCleared': 'WAVE_CLEARED',
             'PlayerDied': 'PLAYER_DIED'}[name]
    value = getattr(unreal.RoomRuleEvent, token,
                    getattr(unreal.RoomRuleEvent, name, None))
    if value is None:
        raise RuntimeError('unreal.RoomRuleEvent has no member for event ' + name)
    return value


def rule_effect_enum(name):
    """JSON effect name -> unreal.RoomRuleEffect value (robust member lookup)."""
    token = {'StartWave': 'START_WAVE',
             'MarkCleared': 'MARK_CLEARED',
             'FailRun': 'FAIL_RUN'}[name]
    value = getattr(unreal.RoomRuleEffect, token,
                    getattr(unreal.RoomRuleEffect, name, None))
    if value is None:
        raise RuntimeError('unreal.RoomRuleEffect has no member for effect ' + name)
    return value


def rule_event_json_name(value):
    """Enum value -> JSON event name (compares the enum objects themselves, so
    the editor's str() rendering of enum values cannot break the check)."""
    for name in ('PlayerEnteredRegion', 'WaveCleared', 'PlayerDied'):
        if value == rule_event_enum(name):
            return name
    return str(value)


def rule_effect_json_name(value):
    """Enum value -> JSON effect name (object comparison, see above)."""
    for name in ('StartWave', 'MarkCleared', 'FailRun'):
        if value == rule_effect_enum(name):
            return name
    return str(value)


def rule_structs(parsed_rules):
    structs = []
    for parsed in parsed_rules:
        struct = unreal.RoomSceneRule()
        struct.set_editor_property('rule_id', parsed['rule_id'])
        struct.set_editor_property('event', rule_event_enum(parsed['event']))
        if parsed['region_id']:
            struct.set_editor_property('region_id', parsed['region_id'])
        if parsed['wave_index'] >= 0:
            struct.set_editor_property('wave_index', parsed['wave_index'])
        struct.set_editor_property('effect', rule_effect_enum(parsed['effect']))
        if parsed['next_wave_index'] >= 0:
            struct.set_editor_property('next_wave_index', parsed['next_wave_index'])
        struct.set_editor_property('delay_seconds', parsed['delay_s'])
        struct.set_editor_property('once', parsed['once'])
        structs.append(struct)
    return structs


def apply_fields(asset, row):
    """Writes every URoomDefinition field of the room_combat_01 row."""
    asset.set_editor_property('room_id', str(row['room_id']))
    # TSoftObjectPtr properties take the LOADED object in editor Python (a
    # SoftObjectPath value is rejected); the soft path stored in the asset is
    # the object's fully-qualified path, so LoadSynchronous of the soft
    # reference can never be ambiguous.
    map_object_path = full_object_path(str(row['map_path']))
    map_world = unreal.load_asset(map_object_path)
    if map_world is None:
        raise RuntimeError('Could not load the map for the soft reference: ' + map_object_path)
    asset.set_editor_property('map_path', map_world)
    spawn = row['player_spawn_location_cm']
    asset.set_editor_property('player_spawn_location',
                              unreal.Vector(float(spawn['x']), float(spawn['y']), float(spawn['z'])))
    asset.set_editor_property('player_spawn_yaw_degrees', float(row['player_spawn_yaw_degrees']))
    center = row['bounds_center_cm']
    asset.set_editor_property('bounds_center',
                              unreal.Vector(float(center['x']), float(center['y']), float(center['z'])))
    size = row['bounds_size_cm']
    asset.set_editor_property('bounds_size',
                              unreal.Vector(float(size['x']), float(size['y']), float(size['z'])))
    asset.set_editor_property('reward_table_id', str(row['reward_table_id']))
    asset.set_editor_property('waves', wave_structs(row))
    asset.set_editor_property('rules', rule_structs(row.get('parsed_rules', [])))


def vectors_close(a, b):
    return (abs(a.x - b.x) <= FLOAT_TOLERANCE and abs(a.y - b.y) <= FLOAT_TOLERANCE
            and abs(a.z - b.z) <= FLOAT_TOLERANCE)


def asset_matches(asset, row):
    """Field-by-field comparison of a loaded asset against the JSON row."""
    problems = []
    if str(asset.get_editor_property('room_id')) != str(row['room_id']):
        problems.append('room_id: asset ' + str(asset.get_editor_property('room_id'))
                        + ' != json ' + str(row['room_id']))
    asset_map = soft_path_string(asset.get_editor_property('map_path'))
    if asset_map != full_object_path(str(row['map_path'])):
        problems.append('map_path: asset ' + asset_map + ' != json ' + full_object_path(str(row['map_path'])))
    spawn = row['player_spawn_location_cm']
    if not vectors_close(asset.get_editor_property('player_spawn_location'),
                         unreal.Vector(float(spawn['x']), float(spawn['y']), float(spawn['z']))):
        problems.append('player_spawn_location mismatch')
    if abs(float(asset.get_editor_property('player_spawn_yaw_degrees'))
           - float(row['player_spawn_yaw_degrees'])) > FLOAT_TOLERANCE:
        problems.append('player_spawn_yaw_degrees mismatch')
    center = row['bounds_center_cm']
    if not vectors_close(asset.get_editor_property('bounds_center'),
                         unreal.Vector(float(center['x']), float(center['y']), float(center['z']))):
        problems.append('bounds_center mismatch')
    size = row['bounds_size_cm']
    if not vectors_close(asset.get_editor_property('bounds_size'),
                         unreal.Vector(float(size['x']), float(size['y']), float(size['z']))):
        problems.append('bounds_size mismatch')
    if str(asset.get_editor_property('reward_table_id')) != str(row['reward_table_id']):
        problems.append('reward_table_id mismatch')
    asset_waves = asset.get_editor_property('waves')
    json_waves = row['waves']
    if len(asset_waves) != len(json_waves):
        problems.append('waves: asset holds ' + str(len(asset_waves)) + ' waves, json holds '
                        + str(len(json_waves)))
    else:
        for wave_index, (asset_wave, json_wave) in enumerate(zip(asset_waves, json_waves)):
            if str(asset_wave.get_editor_property('enemy_id')) != str(json_wave['enemy_id']):
                problems.append('wave ' + str(wave_index) + ' enemy_id mismatch')
            if int(asset_wave.get_editor_property('count')) != int(json_wave['count']):
                problems.append('wave ' + str(wave_index) + ' count mismatch')
            asset_locations = asset_wave.get_editor_property('spawn_locations')
            json_locations = json_wave['spawn_locations_cm']
            if len(asset_locations) != len(json_locations):
                problems.append('wave ' + str(wave_index) + ' spawn_locations count mismatch')
            else:
                for loc_index, (asset_location, json_location) in enumerate(zip(asset_locations, json_locations)):
                    if not vectors_close(asset_location, unreal.Vector(
                            float(json_location['x']), float(json_location['y']), float(json_location['z']))):
                        problems.append('wave ' + str(wave_index) + ' spawn_locations['
                                        + str(loc_index) + '] mismatch')
    asset_rules = asset.get_editor_property('rules')
    json_rules = row.get('parsed_rules', [])
    if len(asset_rules) != len(json_rules):
        problems.append('rules: asset holds ' + str(len(asset_rules)) + ' rules, json holds '
                        + str(len(json_rules)))
    else:
        for rule_index, (asset_rule, json_rule) in enumerate(zip(asset_rules, json_rules)):
            if str(asset_rule.get_editor_property('rule_id')) != str(json_rule['rule_id']):
                problems.append('rule ' + str(rule_index) + ' rule_id mismatch')
            if rule_event_json_name(asset_rule.get_editor_property('event')) != json_rule['event']:
                problems.append('rule ' + str(rule_index) + ' event mismatch')
            if rule_effect_json_name(asset_rule.get_editor_property('effect')) != json_rule['effect']:
                problems.append('rule ' + str(rule_index) + ' effect mismatch')
            asset_region = str(asset_rule.get_editor_property('region_id'))
            if asset_region == 'None':
                asset_region = ''
            if asset_region != str(json_rule['region_id']):
                problems.append('rule ' + str(rule_index) + ' region_id mismatch')
            if int(asset_rule.get_editor_property('wave_index')) != int(json_rule['wave_index']):
                problems.append('rule ' + str(rule_index) + ' wave_index mismatch')
            if int(asset_rule.get_editor_property('next_wave_index')) != int(json_rule['next_wave_index']):
                problems.append('rule ' + str(rule_index) + ' next_wave_index mismatch')
            if abs(float(asset_rule.get_editor_property('delay_seconds')) - float(json_rule['delay_s'])) > FLOAT_TOLERANCE:
                problems.append('rule ' + str(rule_index) + ' delay_s mismatch')
            if bool(asset_rule.get_editor_property('once')) != bool(json_rule['once']):
                problems.append('rule ' + str(rule_index) + ' once mismatch')
    return problems


def find_triggers():
    found = []
    for actor in actors.get_all_level_actors():
        if actor.get_class().get_name() == 'RoomTrigger':
            found.append(actor)
    return found


def count_class(class_name):
    return len([actor for actor in actors.get_all_level_actors()
                if actor.get_class().get_name() == class_name])


def write_report(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding='utf-8')


def run_generate():
    row = validate_document(json.loads(Path(source_json_path()).read_text(encoding='utf-8')),
                            source_json_path())

    # Duplicate-asset guard: an auto-suffixed copy (DA_room_combat_01_1) would
    # be a second source of truth; refuse to run next to one.
    listing = unreal.EditorAssetLibrary.list_assets(ROOMS_PATH, recursive=False) if \
        unreal.EditorAssetLibrary.does_directory_exist(ROOMS_PATH) else []
    duplicates = [str(item) for item in listing
                  if Path(str(item)).name.startswith(COMBAT_ASSET_NAME + '_')]
    if duplicates:
        raise RuntimeError('Duplicate room-definition assets already exist: ' + ', '.join(duplicates))

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = False
    if unreal.EditorAssetLibrary.does_asset_exist(COMBAT_ASSET_PATH):
        asset = unreal.load_asset(COMBAT_ASSET_PATH)
        if asset is None:
            raise RuntimeError('Existing asset exists on disk but could not be loaded: ' + COMBAT_ASSET_PATH)
    else:
        asset = asset_tools.create_asset(COMBAT_ASSET_NAME, ROOMS_PATH, room_class(),
                                         unreal.DataAssetFactory())
        if asset is None:
            raise RuntimeError('create_asset returned null for ' + COMBAT_ASSET_PATH)
        created = True
    apply_fields(asset, row)
    if not unreal.EditorAssetLibrary.save_asset(COMBAT_ASSET_PATH):
        raise RuntimeError('save_asset failed for ' + COMBAT_ASSET_PATH)

    # Wire the production entry: exactly one ARoomTrigger on the combat map,
    # its room_definition_asset pointing at the generated definition. The
    # class-default FallbackWaves stay untouched for the transient-fallback
    # test fixtures; the asset simply wins wherever one is assigned.
    if not levels.load_level(COMBAT_MAP):
        raise RuntimeError('Could not load ' + COMBAT_MAP)
    triggers = find_triggers()
    if len(triggers) != 1:
        raise RuntimeError('Expected exactly one RoomTrigger on ' + COMBAT_MAP + ', found '
                           + str(len(triggers)) + '.')
    # TSoftObjectPtr takes the loaded object (a SoftObjectPath value is
    # rejected by the editor Python API); the serialized soft reference is
    # the asset's fully-qualified path.
    triggers[0].set_editor_property('room_definition_asset', asset)
    if count_class('RoomExit') != 1:
        raise RuntimeError('Expected exactly one RoomExit on ' + COMBAT_MAP + ', found '
                           + str(count_class('RoomExit')) + '.')
    if not levels.save_current_level():
        raise RuntimeError('Could not save ' + COMBAT_MAP)

    write_report(GENERATE_REPORT, {
        'mode': 'generate',
        'source': str(source_json_path()),
        'asset': COMBAT_ASSET_PATH,
        'created': created,
        'updated': not created,
        'trigger_wired': COMBAT_ASSET_OBJECT_PATH,
        'room_id': str(row['room_id']),
        'wave_counts': [int(wave['count']) for wave in row['waves']],
        'success': True,
    })
    unreal.log('UEMMO_ROOM_DEFINITION_ASSETS_GENERATED asset=' + COMBAT_ASSET_PATH
               + ' created=' + str(created) + ' trigger_wired=' + COMBAT_ASSET_OBJECT_PATH)


def run_verify():
    row = validate_document(json.loads(Path(source_json_path()).read_text(encoding='utf-8')),
                            source_json_path())
    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    # Duplicate guard in the fresh process as well.
    listing = unreal.EditorAssetLibrary.list_assets(ROOMS_PATH, recursive=False)
    duplicates = [str(item) for item in listing
                  if Path(str(item)).name.startswith(COMBAT_ASSET_NAME + '_')]
    record('no_duplicate_room_definition_assets', not duplicates, 'duplicates=' + str(duplicates))

    asset = unreal.load_asset(COMBAT_ASSET_PATH)
    record('asset_loads_from_disk', asset is not None, 'path=' + COMBAT_ASSET_PATH)
    field_problems = asset_matches(asset, row) if asset is not None else ['asset did not load']
    record('asset_fields_match_json', not field_problems, 'problems=' + str(field_problems))

    if not levels.load_level(COMBAT_MAP):
        raise RuntimeError('Verify: could not load ' + COMBAT_MAP)
    triggers = find_triggers()
    record('combat_map_exactly_one_room_trigger', len(triggers) == 1, 'count=' + str(len(triggers)))
    record('combat_map_exactly_one_room_exit', count_class('RoomExit') == 1,
           'count=' + str(count_class('RoomExit')))
    trigger_reference = ''
    if len(triggers) == 1:
        trigger_reference = soft_path_string(triggers[0].get_editor_property('room_definition_asset'))
        record('trigger_references_generated_asset', trigger_reference == COMBAT_ASSET_OBJECT_PATH,
               'reference=' + trigger_reference)

    if not levels.load_level(BASE_MAP):
        raise RuntimeError('Verify: could not load ' + BASE_MAP)
    record('training_map_has_no_room_trigger', count_class('RoomTrigger') == 0,
           'count=' + str(count_class('RoomTrigger')))
    record('training_map_has_no_room_exit', count_class('RoomExit') == 0,
           'count=' + str(count_class('RoomExit')))

    report = {
        'mode': 'verify',
        'asset': COMBAT_ASSET_PATH,
        'trigger_reference': trigger_reference,
        'checks': checks,
        'success': all(check['passed'] for check in checks),
    }
    write_report(VERIFY_REPORT, report)
    if not report['success']:
        failed = [check['check'] for check in checks if not check['passed']]
        raise RuntimeError('Room definition verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_ROOM_DEFINITION_ASSETS_VERIFIED ' + str(VERIFY_REPORT))


if is_verify_mode():
    run_verify()
else:
    run_generate()
