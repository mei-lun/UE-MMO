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
  {mode, source_dir, success, created, updated, config_revision, errors,
   vehicles}

M5-038 vehicle segment: when the source directory's manifest.json registers
the vehicles / vehicle_seats / vehicle_mounts tables, this script additionally
generates one data asset per vehicle row under
/Game/UEMMO/CombatSystem/Vehicles (DA_Vehicle_<vehicle_id>,
DA_VehicleSeat_<seat_id>, DA_VehicleMount_<mount_id> - one asset per source
row of each table, mirroring the catalog's per-table id-keyed lookup shape).
The vehicle rows are validated in this script BEFORE the six-table builder
runs (structural schema, value bounds mirroring Vehicles/VehicleTypes.h,
cross-table ids: seat/mount -> vehicle, mount -> weapons.json weapon, one
seat per vehicle), so an illegal vehicle source refuses the whole generation
before any asset is written and never partially overwrites legal assets. The
vehicle assets carry the same ConfigRevision the builder stamped on the
six-table assets of this run (one generation, one provenance stamp).

Optional environment overrides:
  UEMMOCombatDataSourceDir - source directory. Default Data/CombatSystem
                             (project-relative paths resolve against the
                             project directory).
  UEMMOCombatReportDir     - extra directory that receives a report copy.

Modes:
  generate (default)      - six-table assets via the C++ builder plus the
                            vehicle segment; writes
                            combat-system-assets-report.json.
  verify                  - UEMMOCombatVehicleAssetsVerify env var or the
                            -UEMMOCombatVehicleAssetsVerify command-line
                            token: a FRESH process reloads the saved vehicle
                            assets from disk and compares every field against
                            the same JSON rows plus the revision recorded by
                            the last generate run; writes
                            combat-system-assets-verify.json. No asset is
                            written in this mode.
  system test room        - UEMMO_SYSTEM_TEST_ROOM env var: the M5-018A map
                            build/verify mode replaces generation entirely
                            (one editor process, one write surface).

Failure contract: everything is validated before the first write, so any
parse/validation/save failure leaves the existing assets untouched and
reports success=false with the error text naming the offending table, row and
field. The script raises after writing the report so the UE process exits
nonzero. The top-level 'errors' field stays the C++ builder's error text; a
vehicle-segment failure is named inside the 'vehicles' section (the top-level
success flag is the AND of both segments).

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import math
import os
import re
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
DEFAULT_SOURCE_DIR = 'Data/CombatSystem'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-system-assets-report.json'
VEHICLE_VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-system-assets-verify.json'

# M5-038: the vehicle segment's target folder and asset naming (one asset per
# source row, keyed by the row id - the catalog's per-table lookup shape).
VEHICLE_ASSET_DIR = '/Game/UEMMO/CombatSystem/Vehicles'
VEHICLE_ASSET_PREFIXES = {
    'vehicles': 'DA_Vehicle_',
    'vehicle_seats': 'DA_VehicleSeat_',
    'vehicle_mounts': 'DA_VehicleMount_',
}

# M5-038: mirrors of the Vehicles/VehicleTypes.h validator bounds so the
# editor-side validation refuses exactly what the C++ parser refuses.
MAX_VEHICLE_HEALTH = 100000.0
MAX_VEHICLE_DEFENSE = 10000.0
MAX_VEHICLE_SPEED_CM_S = 3000.0
MAX_VEHICLE_ACCELERATION_CM_S2 = 10000.0
MAX_VEHICLE_CAMERA_DISTANCE_CM = 2000.0
MAX_VEHICLE_CAMERA_HEIGHT_CM = 2000.0
MAX_VEHICLE_CAMERA_PITCH_DEG = 89.0
MAX_TURRET_PITCH_DEG = 89.0
MAX_VEHICLE_BODY_OFFSET_CM = 1000.0

VEHICLE_TABLE_FIELDS = ('vehicle_id', 'drive_type', 'max_health', 'defense',
                        'max_speed_cm_s', 'acceleration_cm_s2', 'mesh_path',
                        'camera_distance_cm', 'camera_height_cm', 'camera_pitch_deg')
SEAT_TABLE_FIELDS = ('seat_id', 'vehicle_id', 'seat_role', 'seat_location_cm', 'exit_offset_cm')
MOUNT_TABLE_FIELDS = ('mount_id', 'vehicle_id', 'turret_weapon_id', 'location_cm',
                      'pitch_min_deg', 'pitch_max_deg')

ID_PATTERN = re.compile(r'^[a-z0-9_]{1,64}$')
FLOAT_TOLERANCE = 1e-6


def source_dir():
    override = os.environ.get('UEMMOCombatDataSourceDir')
    if override:
        return override
    return DEFAULT_SOURCE_DIR


def source_dir_path():
    """The source directory as an absolute Path (the vehicle segment reads
    the JSON files directly; the C++ builder resolves the same directory)."""
    override = source_dir()
    path = Path(override)
    if not path.is_absolute():
        path = ROOT / path
    return path


def write_report(report):
    GENERATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(report, indent=2)
    GENERATE_REPORT.write_text(payload, encoding='utf-8')
    extra = os.environ.get('UEMMOCombatReportDir')
    if extra:
        extra_dir = Path(extra)
        extra_dir.mkdir(parents=True, exist_ok=True)
        (extra_dir / GENERATE_REPORT.name).write_text(payload, encoding='utf-8')


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_vehicle_verify_mode():
    """M5-038 fresh-process reload verification of the saved vehicle assets."""
    if os.environ.get('UEMMOCombatVehicleAssetsVerify'):
        return True
    return '-UEMMOCombatVehicleAssetsVerify' in command_line()


# ---------------------------------------------------------------------------
# M5-038: vehicle source validation (validate-first; mirrors the strict
# CombatDataTableParser row schemas and the VehicleTypes.h value bounds so
# this pre-filter never refuses a config the C++ loader would accept)
# ---------------------------------------------------------------------------

def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def number_or_fail(row, field, label, problems):
    value = row.get(field)
    if not is_number(value) or not math.isfinite(float(value)):
        problems.append('FAIL ' + label + " field '" + field + "': required finite number.")
        return None
    return float(value)


def bounded(row, field, label, problems, maximum, strict_minimum=False):
    """Mirror of the C++ value bounds: a non-negative (or strictly positive)
    finite number inside maximum. The refusal names the field."""
    value = number_or_fail(row, field, label, problems)
    if value is None:
        return None
    if strict_minimum and value <= 0.0:
        problems.append('FAIL ' + label + " field '" + field + "': must be greater than 0.")
        return None
    if not strict_minimum and value < 0.0:
        problems.append('FAIL ' + label + " field '" + field + "': must not be negative.")
        return None
    if value > maximum:
        problems.append('FAIL ' + label + " field '" + field + "': must stay within "
                        + ('(0..' if strict_minimum else '0..') + str(maximum)
                        + ' (got ' + str(value) + ').')
        return None
    return value


def signed_bounded(row, field, label, problems, maximum):
    value = number_or_fail(row, field, label, problems)
    if value is None:
        return None
    if abs(value) > maximum:
        problems.append('FAIL ' + label + " field '" + field + "': must stay within +/-"
                        + str(maximum) + ' (got ' + str(value) + ').')
        return None
    return value


def read_vector(row, field, label, problems, max_abs_per_axis):
    value = row.get(field)
    if not isinstance(value, dict):
        problems.append('FAIL ' + label + " field '" + field + "': expected an {x, y, z} object.")
        return None
    result = []
    for axis in ('x', 'y', 'z'):
        axis_value = value.get(axis)
        if not is_number(axis_value) or not math.isfinite(float(axis_value)):
            problems.append('FAIL ' + label + " field '" + field + "." + axis
                            + "': required finite number.")
            return None
        if abs(float(axis_value)) > max_abs_per_axis:
            problems.append('FAIL ' + label + " field '" + field + "." + axis
                            + "': must stay within +/-" + str(max_abs_per_axis) + ' cm.')
            return None
        result.append(float(axis_value))
    return result


def check_id(row, field, label, problems, taken=None):
    value = row.get(field)
    if not isinstance(value, str) or not ID_PATTERN.match(value):
        problems.append('FAIL ' + label + " field '" + field
                        + "': must be 1..64 characters of a-z, 0-9 and _.")
        return None
    if taken is not None and value in taken:
        problems.append('FAIL ' + label + " field '" + field + "': duplicate id '" + value + "'.")
        return None
    return value


def read_table_document(path, expected_table):
    """Reads one source table file; raises with a named error when the
    document shell (schema_version, table name, rows array) is broken."""
    if not path.exists():
        raise RuntimeError('FAIL ' + path.name + ': the required table file is missing.')
    try:
        doc = json.loads(path.read_text(encoding='utf-8'))
    except Exception as error:  # noqa: BLE001 - re-raised with the file name
        raise RuntimeError('FAIL ' + path.name + ': the file is not valid JSON (' + str(error) + ').')
    if not isinstance(doc, dict):
        raise RuntimeError('FAIL ' + path.name + ': the document is not a JSON object.')
    if doc.get('schema_version') != 1:
        raise RuntimeError('FAIL ' + path.name + ": field 'schema_version': expected 1 (got "
                           + str(doc.get('schema_version')) + ').')
    if doc.get('table') != expected_table:
        raise RuntimeError('FAIL ' + path.name + ": field 'table': expected '" + expected_table
                           + "' (got " + str(doc.get('table')) + ').')
    rows = doc.get('rows')
    if not isinstance(rows, list):
        raise RuntimeError('FAIL ' + path.name + ": field 'rows': required array.")
    return rows


def read_manifest_registered_tables(src):
    """Returns the manifest's required_tables list, an empty list when the
    directory carries no manifest, or None when the manifest is broken (the
    C++ loader refuses the whole directory and owns the authoritative error)."""
    manifest_path = src / 'manifest.json'
    if not manifest_path.exists():
        return []
    try:
        doc = json.loads(manifest_path.read_text(encoding='utf-8'))
        required = doc.get('required_tables')
        if not isinstance(required, list):
            return None
        return required
    except Exception:  # noqa: BLE001 - the loader reports the broken manifest
        return None


def load_vehicle_rows():
    """Validates the vehicle source tables and returns their rows, or None
    when the source directory does not enable the vehicle segment.

    The segment runs only when manifest.json registers all three vehicle
    tables (a missing or broken manifest defers to the C++ loader, which
    refuses the directory with the authoritative error). Every structural
    and value problem raises before the six-table builder runs, so nothing
    is written for an illegal source.
    """
    src = source_dir_path()
    registered = read_manifest_registered_tables(src)
    if registered is None:
        return None
    needed = ('vehicles', 'vehicle_seats', 'vehicle_mounts')
    if not all(name in registered for name in needed):
        return None

    problems = []

    vehicles = []
    vehicle_ids = set()
    for index, row in enumerate(read_table_document(src / 'vehicles.json', 'vehicles')):
        label = 'vehicles[' + str(index) + ']'
        if not isinstance(row, dict):
            problems.append('FAIL ' + label + ': entry is not a JSON object.')
            continue
        for field in row:
            if field not in VEHICLE_TABLE_FIELDS:
                problems.append('FAIL ' + label + " field '" + str(field)
                                + "': unknown vehicle field.")
        vehicle_id = check_id(row, 'vehicle_id', label, problems, vehicle_ids)
        if vehicle_id is not None:
            vehicle_ids.add(vehicle_id)
            label = "vehicle '" + vehicle_id + "'"
        drive_type = row.get('drive_type')
        if not isinstance(drive_type, str) or drive_type.lower() != 'ground':
            problems.append('FAIL ' + label + " field 'drive_type': only 'ground' is implemented"
                            " (aircraft and boat drive types are not supported; got "
                            + str(drive_type) + ').')
        bounded(row, 'max_health', label, problems, MAX_VEHICLE_HEALTH, strict_minimum=True)
        bounded(row, 'defense', label, problems, MAX_VEHICLE_DEFENSE)
        bounded(row, 'max_speed_cm_s', label, problems, MAX_VEHICLE_SPEED_CM_S, strict_minimum=True)
        bounded(row, 'acceleration_cm_s2', label, problems, MAX_VEHICLE_ACCELERATION_CM_S2,
                strict_minimum=True)
        mesh_path = row.get('mesh_path')
        # Mirror of the C++ gate: presence is the source-table rule (a
        # non-empty string); the mesh asset itself is not required to exist.
        if not isinstance(mesh_path, str) or not mesh_path:
            problems.append('FAIL ' + label + " field 'mesh_path': must be a non-empty soft object path"
                            " (the hull mesh reference).")
        bounded(row, 'camera_distance_cm', label, problems, MAX_VEHICLE_CAMERA_DISTANCE_CM,
                strict_minimum=True)
        signed_bounded(row, 'camera_height_cm', label, problems, MAX_VEHICLE_CAMERA_HEIGHT_CM)
        signed_bounded(row, 'camera_pitch_deg', label, problems, MAX_VEHICLE_CAMERA_PITCH_DEG)
        vehicles.append(row)

    seats = []
    seat_ids = set()
    seat_vehicle_ids = set()
    for index, row in enumerate(read_table_document(src / 'vehicle_seats.json', 'vehicle_seats')):
        label = 'vehicle_seats[' + str(index) + ']'
        if not isinstance(row, dict):
            problems.append('FAIL ' + label + ': entry is not a JSON object.')
            continue
        for field in row:
            if field not in SEAT_TABLE_FIELDS:
                problems.append('FAIL ' + label + " field '" + str(field)
                                + "': unknown seat field.")
        seat_id = check_id(row, 'seat_id', label, problems, seat_ids)
        if seat_id is not None:
            seat_ids.add(seat_id)
            label = "seat '" + seat_id + "'"
        vehicle_id = row.get('vehicle_id')
        if not isinstance(vehicle_id, str) or vehicle_id not in vehicle_ids:
            problems.append('FAIL ' + label + " field 'vehicle_id': '" + str(vehicle_id)
                            + "' does not resolve into the vehicles table.")
        elif vehicle_id in seat_vehicle_ids:
            problems.append('FAIL ' + label + " field 'vehicle_id': vehicle '" + vehicle_id
                            + "' already carries a seat (one seat per vehicle).")
        else:
            seat_vehicle_ids.add(vehicle_id)
        seat_role = row.get('seat_role')
        if not isinstance(seat_role, str) or seat_role.lower() != 'driver':
            problems.append('FAIL ' + label + " field 'seat_role': only 'driver' is implemented"
                            " (gunner and passenger roles of multi-seat layouts are not supported; got "
                            + str(seat_role) + ').')
        read_vector(row, 'seat_location_cm', label, problems, MAX_VEHICLE_BODY_OFFSET_CM)
        read_vector(row, 'exit_offset_cm', label, problems, MAX_VEHICLE_BODY_OFFSET_CM)
        seats.append(row)

    mounts = []
    mount_ids = set()
    weapon_ids = set()
    weapons_rows = read_table_document(src / 'weapons.json', 'weapons')
    for weapon_row in weapons_rows:
        if isinstance(weapon_row, dict) and isinstance(weapon_row.get('weapon_id'), str):
            weapon_ids.add(weapon_row['weapon_id'])
    for index, row in enumerate(read_table_document(src / 'vehicle_mounts.json', 'vehicle_mounts')):
        label = 'vehicle_mounts[' + str(index) + ']'
        if not isinstance(row, dict):
            problems.append('FAIL ' + label + ': entry is not a JSON object.')
            continue
        for field in row:
            if field not in MOUNT_TABLE_FIELDS:
                problems.append('FAIL ' + label + " field '" + str(field)
                                + "': unknown mount field.")
        mount_id = check_id(row, 'mount_id', label, problems, mount_ids)
        if mount_id is not None:
            mount_ids.add(mount_id)
            label = "mount '" + mount_id + "'"
        vehicle_id = row.get('vehicle_id')
        if not isinstance(vehicle_id, str) or vehicle_id not in vehicle_ids:
            problems.append('FAIL ' + label + " field 'vehicle_id': '" + str(vehicle_id)
                            + "' does not resolve into the vehicles table.")
        turret_weapon_id = row.get('turret_weapon_id')
        if not isinstance(turret_weapon_id, str) or not turret_weapon_id:
            problems.append('FAIL ' + label + " field 'turret_weapon_id': a turret mount without a"
                            " weapon is refused.")
        elif turret_weapon_id not in weapon_ids:
            problems.append('FAIL ' + label + " field 'turret_weapon_id': '" + turret_weapon_id
                            + "' does not resolve into the weapons table.")
        read_vector(row, 'location_cm', label, problems, MAX_VEHICLE_BODY_OFFSET_CM)
        min_pitch = signed_bounded(row, 'pitch_min_deg', label, problems, MAX_TURRET_PITCH_DEG)
        max_pitch = signed_bounded(row, 'pitch_max_deg', label, problems, MAX_TURRET_PITCH_DEG)
        if min_pitch is not None and max_pitch is not None and min_pitch > max_pitch:
            problems.append('FAIL ' + label + " field 'pitch_min_deg': must not exceed pitch_max_deg (got "
                            + str(min_pitch) + ' > ' + str(max_pitch) + ').')
        mounts.append(row)

    if problems:
        raise RuntimeError('the vehicle source tables are illegal; nothing was generated.\n'
                           + '\n'.join(problems))
    return {'vehicles': vehicles, 'seats': seats, 'mounts': mounts}


# ---------------------------------------------------------------------------
# M5-038: vehicle asset generation (create or update in place, never duplicate)
# ---------------------------------------------------------------------------

def vehicle_asset_class(class_name):
    candidate = getattr(unreal, class_name, None)
    if candidate is not None:
        return candidate
    loaded = unreal.load_class(None, '/Script/UEMMO.' + class_name)
    if loaded is None:
        raise RuntimeError('UEMMO.' + class_name + ' not found; build the UEMMO module first.')
    return loaded


def resolve_or_create_asset(asset_tools, asset_path, class_name, created, updated):
    """Finds the asset (in memory or on disk) or creates it once; a foreign
    class occupying the object path is refused, never overwritten."""
    name = Path(asset_path).name
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        asset = unreal.load_asset(asset_path)
        if asset is None:
            raise RuntimeError('the asset exists on disk but could not be loaded: ' + asset_path)
        if asset.get_class().get_name() != class_name:
            raise RuntimeError('the object path ' + asset_path + ' is occupied by a foreign class ('
                               + asset.get_class().get_name() + ', expected ' + class_name + ').')
        updated.append(asset_path)
        return asset
    asset = asset_tools.create_asset(name, VEHICLE_ASSET_DIR, vehicle_asset_class(class_name),
                                     unreal.DataAssetFactory())
    if asset is None:
        raise RuntimeError('create_asset returned null for ' + asset_path)
    created.append(asset_path)
    return asset


def apply_vehicle_fields(asset, row, revision):
    asset.set_editor_property('vehicle_id', str(row['vehicle_id']))
    asset.set_editor_property('drive_type', 0)  # numeric EVehicleDriveType::Ground
    asset.set_editor_property('max_health', float(row['max_health']))
    asset.set_editor_property('defense', float(row['defense']))
    asset.set_editor_property('max_speed_cm_s', float(row['max_speed_cm_s']))
    asset.set_editor_property('acceleration_cm_s2', float(row['acceleration_cm_s2']))
    asset.set_editor_property('mesh_path', str(row['mesh_path']))
    asset.set_editor_property('camera_distance_cm', float(row['camera_distance_cm']))
    asset.set_editor_property('camera_height_cm', float(row['camera_height_cm']))
    asset.set_editor_property('camera_pitch_deg', float(row['camera_pitch_deg']))
    asset.set_editor_property('config_revision', revision)


def apply_seat_fields(asset, row, revision):
    asset.set_editor_property('seat_id', str(row['seat_id']))
    asset.set_editor_property('vehicle_id', str(row['vehicle_id']))
    asset.set_editor_property('seat_role', 0)  # numeric EVehicleSeatRole::Driver
    seat_location = row['seat_location_cm']
    asset.set_editor_property('seat_location_cm',
                              unreal.Vector(float(seat_location['x']), float(seat_location['y']),
                                            float(seat_location['z'])))
    exit_offset = row['exit_offset_cm']
    asset.set_editor_property('exit_offset_cm',
                              unreal.Vector(float(exit_offset['x']), float(exit_offset['y']),
                                            float(exit_offset['z'])))
    asset.set_editor_property('config_revision', revision)


def apply_mount_fields(asset, row, revision):
    asset.set_editor_property('mount_id', str(row['mount_id']))
    asset.set_editor_property('vehicle_id', str(row['vehicle_id']))
    asset.set_editor_property('turret_weapon_id', str(row['turret_weapon_id']))
    location = row['location_cm']
    asset.set_editor_property('location_cm',
                              unreal.Vector(float(location['x']), float(location['y']),
                                            float(location['z'])))
    asset.set_editor_property('min_pitch_degrees', float(row['pitch_min_deg']))
    asset.set_editor_property('max_pitch_degrees', float(row['pitch_max_deg']))
    asset.set_editor_property('config_revision', revision)


def guard_against_duplicates(expected_names):
    """An auto-suffixed copy (DA_Vehicle_x_1) would be a second source of
    truth; refuse to run next to one (the M5-070 room-asset pattern)."""
    if not unreal.EditorAssetLibrary.does_directory_exist(VEHICLE_ASSET_DIR):
        return
    listing = unreal.EditorAssetLibrary.list_assets(VEHICLE_ASSET_DIR, recursive=False)
    for item in listing:
        item_name = Path(str(item)).name
        for expected in expected_names:
            if item_name.startswith(expected + '_'):
                raise RuntimeError('duplicate vehicle assets already exist: ' + item_name)


def build_vehicle_assets(rows, config_revision):
    """Creates or updates one asset per vehicle/seat/mount row and saves each
    package. Returns (created_paths, updated_paths)."""
    expected_names = []
    for row in rows['vehicles']:
        expected_names.append(VEHICLE_ASSET_PREFIXES['vehicles'] + row['vehicle_id'])
    for row in rows['seats']:
        expected_names.append(VEHICLE_ASSET_PREFIXES['vehicle_seats'] + row['seat_id'])
    for row in rows['mounts']:
        expected_names.append(VEHICLE_ASSET_PREFIXES['vehicle_mounts'] + row['mount_id'])
    guard_against_duplicates(expected_names)

    if not unreal.EditorAssetLibrary.does_directory_exist(VEHICLE_ASSET_DIR):
        if not unreal.EditorAssetLibrary.make_directory(VEHICLE_ASSET_DIR):
            raise RuntimeError('could not create the vehicle asset directory ' + VEHICLE_ASSET_DIR)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = []
    updated = []

    def generate(asset_name, class_name, apply_fields, row):
        asset_path = VEHICLE_ASSET_DIR + '/' + asset_name
        asset = resolve_or_create_asset(asset_tools, asset_path, class_name, created, updated)
        apply_fields(asset, row, config_revision)
        if not unreal.EditorAssetLibrary.save_asset(asset_path):
            raise RuntimeError('save_asset failed for ' + asset_path)

    for row in rows['vehicles']:
        generate(VEHICLE_ASSET_PREFIXES['vehicles'] + row['vehicle_id'],
                 'CombatVehicleAsset', apply_vehicle_fields, row)
    for row in rows['seats']:
        generate(VEHICLE_ASSET_PREFIXES['vehicle_seats'] + row['seat_id'],
                 'CombatVehicleSeatAsset', apply_seat_fields, row)
    for row in rows['mounts']:
        generate(VEHICLE_ASSET_PREFIXES['vehicle_mounts'] + row['mount_id'],
                 'CombatVehicleMountAsset', apply_mount_fields, row)
    return created, updated


# ---------------------------------------------------------------------------
# M5-038: fresh-process vehicle asset verification (no writes in this mode)
# ---------------------------------------------------------------------------

def text_value(value):
    return str(value)


def vectors_close(a, b):
    return (abs(a.x - b.x) <= FLOAT_TOLERANCE and abs(a.y - b.y) <= FLOAT_TOLERANCE
            and abs(a.z - b.z) <= FLOAT_TOLERANCE)


def verify_vehicle_assets():
    """Reloads the saved vehicle assets from disk and compares every field
    against the same JSON rows plus the revision the generate run stamped."""
    rows = load_vehicle_rows()
    if rows is None:
        raise RuntimeError('vehicle asset verify: the source directory does not register the'
                           ' vehicle tables in manifest.json.')
    if not GENERATE_REPORT.exists():
        raise RuntimeError('vehicle asset verify: no generate report at '
                           + str(GENERATE_REPORT) + '; run the generate mode first.')
    generate_report = json.loads(GENERATE_REPORT.read_text(encoding='utf-8'))
    expected_revision = str(generate_report.get('config_revision') or '')
    if not expected_revision:
        raise RuntimeError('vehicle asset verify: the generate report carries no config_revision.')

    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    seen_revisions = set()
    plan = []
    for row in rows['vehicles']:
        plan.append((VEHICLE_ASSET_PREFIXES['vehicles'] + row['vehicle_id'],
                     'CombatVehicleAsset', row, 'vehicle'))
    for row in rows['seats']:
        plan.append((VEHICLE_ASSET_PREFIXES['vehicle_seats'] + row['seat_id'],
                     'CombatVehicleSeatAsset', row, 'seat'))
    for row in rows['mounts']:
        plan.append((VEHICLE_ASSET_PREFIXES['vehicle_mounts'] + row['mount_id'],
                     'CombatVehicleMountAsset', row, 'mount'))

    for asset_name, class_name, row, kind in plan:
        asset_path = VEHICLE_ASSET_DIR + '/' + asset_name
        exists = unreal.EditorAssetLibrary.does_asset_exist(asset_path)
        record(asset_name + '_exists_on_disk', exists, 'path=' + asset_path)
        if not exists:
            continue
        asset = unreal.load_asset(asset_path)
        record(asset_name + '_loads', asset is not None, 'path=' + asset_path)
        if asset is None:
            continue
        record(asset_name + '_class', asset.get_class().get_name() == class_name,
               'class=' + asset.get_class().get_name())
        field_problems = []
        if kind == 'vehicle':
            if text_value(asset.get_editor_property('vehicle_id')) != str(row['vehicle_id']):
                field_problems.append('vehicle_id')
            if int(asset.get_editor_property('drive_type')) != 0:
                field_problems.append('drive_type')
            for prop, field in (('max_health', 'max_health'), ('defense', 'defense'),
                                ('max_speed_cm_s', 'max_speed_cm_s'),
                                ('acceleration_cm_s2', 'acceleration_cm_s2'),
                                ('camera_distance_cm', 'camera_distance_cm'),
                                ('camera_height_cm', 'camera_height_cm'),
                                ('camera_pitch_deg', 'camera_pitch_deg')):
                if abs(float(asset.get_editor_property(prop)) - float(row[field])) > FLOAT_TOLERANCE:
                    field_problems.append(field)
            if text_value(asset.get_editor_property('mesh_path')) != str(row['mesh_path']):
                field_problems.append('mesh_path')
        elif kind == 'seat':
            if text_value(asset.get_editor_property('seat_id')) != str(row['seat_id']):
                field_problems.append('seat_id')
            if text_value(asset.get_editor_property('vehicle_id')) != str(row['vehicle_id']):
                field_problems.append('vehicle_id')
            if int(asset.get_editor_property('seat_role')) != 0:
                field_problems.append('seat_role')
            if not vectors_close(asset.get_editor_property('seat_location_cm'),
                                 unreal.Vector(float(row['seat_location_cm']['x']),
                                               float(row['seat_location_cm']['y']),
                                               float(row['seat_location_cm']['z']))):
                field_problems.append('seat_location_cm')
            if not vectors_close(asset.get_editor_property('exit_offset_cm'),
                                 unreal.Vector(float(row['exit_offset_cm']['x']),
                                               float(row['exit_offset_cm']['y']),
                                               float(row['exit_offset_cm']['z']))):
                field_problems.append('exit_offset_cm')
        else:
            if text_value(asset.get_editor_property('mount_id')) != str(row['mount_id']):
                field_problems.append('mount_id')
            if text_value(asset.get_editor_property('vehicle_id')) != str(row['vehicle_id']):
                field_problems.append('vehicle_id')
            if text_value(asset.get_editor_property('turret_weapon_id')) != str(row['turret_weapon_id']):
                field_problems.append('turret_weapon_id')
            if not vectors_close(asset.get_editor_property('location_cm'),
                                 unreal.Vector(float(row['location_cm']['x']),
                                               float(row['location_cm']['y']),
                                               float(row['location_cm']['z']))):
                field_problems.append('location_cm')
            if abs(float(asset.get_editor_property('min_pitch_degrees'))
                   - float(row['pitch_min_deg'])) > FLOAT_TOLERANCE:
                field_problems.append('pitch_min_deg')
            if abs(float(asset.get_editor_property('max_pitch_degrees'))
                   - float(row['pitch_max_deg'])) > FLOAT_TOLERANCE:
                field_problems.append('pitch_max_deg')
        asset_revision = text_value(asset.get_editor_property('config_revision'))
        seen_revisions.add(asset_revision)
        if not asset_revision:
            field_problems.append('config_revision_empty')
        elif asset_revision != expected_revision:
            field_problems.append('config_revision ' + asset_revision + ' != generate run '
                                  + expected_revision)
        record(asset_name + '_fields_match_json', not field_problems, 'problems=' + str(field_problems))

    record('all_vehicle_assets_share_one_config_revision', len(seen_revisions) <= 1,
           'revisions=' + str(sorted(seen_revisions)))

    report = {
        'mode': 'verify',
        'asset_dir': VEHICLE_ASSET_DIR,
        'config_revision': expected_revision,
        'checks': checks,
        'success': all(check['passed'] for check in checks),
    }
    VEHICLE_VERIFY_REPORT.parent.mkdir(parents=True, exist_ok=True)
    VEHICLE_VERIFY_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not report['success']:
        failed = [check['check'] for check in checks if not check['passed']]
        raise RuntimeError('vehicle asset verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_M5_038_VEHICLE_ASSETS_VERIFIED ' + str(VEHICLE_VERIFY_REPORT))


# ---------------------------------------------------------------------------
# M5-018A: the system test room map mode (unchanged)
# ---------------------------------------------------------------------------

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
            # M5-018C: the fake-target previews must stay removed (the room
            # shows only the config-spawned targets).
            previews = [a for a in actor_sub.get_all_level_actors()
                        if a.get_actor_label().startswith('ResourcePreview')]
            if previews:
                raise RuntimeError('the room still holds ' + str(len(previews))
                                   + ' ResourcePreview props; rerun the build mode')
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
            # M5-018C: the copied arena carries the M0 resource preview props -
            # a static Quinn dummy that no combat query can target, which the
            # M5-H01 playtest read as a broken test-room target. The room keeps
            # the arena shell (floor/walls/PlayerStart) but sheds the
            # fake-target previews so only the config targets stand there.
            previews = [a for a in actor_sub.get_all_level_actors()
                        if a.get_actor_label().startswith('ResourcePreview')]
            for preview in previews:
                actor_sub.destroy_actor(preview)
            report['removed_resource_previews'] = len(previews)
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


# ---------------------------------------------------------------------------
# Generation entry
# ---------------------------------------------------------------------------

def main():
    # Validate-first: the vehicle segment refuses an illegal vehicle source
    # BEFORE the six-table builder runs, so no asset is ever written for a
    # broken directory. A directory that does not register the vehicle tables
    # keeps the pure M5-008 behavior (rows=None skips the segment). Every
    # failure path writes the report exactly once, then raises so the UE
    # process exits nonzero.
    report = {
        'mode': 'generate',
        'source_dir': source_dir(),
        'success': False,
        'created': 0,
        'updated': 0,
        'config_revision': '',
        'errors': '',
        'vehicles': None,
    }

    try:
        vehicle_rows = load_vehicle_rows()
    except Exception as error:  # noqa: BLE001 - reported then raised
        report['vehicles'] = {
            'mode': 'generate',
            'asset_dir': VEHICLE_ASSET_DIR,
            'skipped': '',
            'created': [],
            'updated': [],
            'config_revision': '',
            'success': False,
            'errors': str(error),
        }
        write_report(report)
        unreal.log_error('UEMMO_M5_038 vehicle source validation failed; no asset was touched: '
                         + str(error))
        raise

    # The UFUNCTION returns the error text (empty on success); the out
    # parameters come back with it as one tuple:
    # (errors, created, updated, config_revision).
    errors, created, updated, revision = (
        unreal.CombatDataAssetBuilderLibrary.build_from_source_directory(source_dir()))
    six_success = not (errors or '').strip()
    report['success'] = six_success
    report['created'] = int(created)
    report['updated'] = int(updated)
    report['config_revision'] = revision or ''
    report['errors'] = errors or ''
    if not six_success:
        report['vehicles'] = {'mode': 'generate', 'skipped': 'six_table_generation_failed'}
        write_report(report)
        unreal.log_error(
            'combat system asset generation failed; no asset was touched: ' + (errors or 'unknown error'))
        raise RuntimeError('combat system asset generation failed')
    unreal.log('combat system asset generation succeeded: created=%d updated=%d revision=%s'
               % (int(created), int(updated), revision or ''))

    # The vehicle segment shares the run's provenance stamp.
    if vehicle_rows is None:
        report['vehicles'] = {
            'mode': 'generate',
            'asset_dir': VEHICLE_ASSET_DIR,
            'skipped': 'vehicles_not_registered_in_manifest',
            'created': [],
            'updated': [],
            'config_revision': revision or '',
            'success': True,
            'errors': '',
        }
        report['success'] = True
        write_report(report)
        return
    try:
        vehicle_created, vehicle_updated = build_vehicle_assets(vehicle_rows, revision or '')
    except Exception as error:  # noqa: BLE001 - reported then raised
        report['vehicles'] = {
            'mode': 'generate',
            'asset_dir': VEHICLE_ASSET_DIR,
            'skipped': '',
            'created': [],
            'updated': [],
            'config_revision': revision or '',
            'success': False,
            'errors': str(error),
        }
        report['success'] = False
        write_report(report)
        unreal.log_error('UEMMO_M5_038 vehicle asset generation failed: ' + str(error))
        raise
    report['vehicles'] = {
        'mode': 'generate',
        'asset_dir': VEHICLE_ASSET_DIR,
        'skipped': '',
        'created': vehicle_created,
        'updated': vehicle_updated,
        'config_revision': revision or '',
        'success': True,
        'errors': '',
    }
    report['success'] = True
    write_report(report)
    unreal.log('UEMMO_M5_038_VEHICLE_ASSETS_GENERATED created=%d updated=%d dir=%s'
               % (len(vehicle_created), len(vehicle_updated), VEHICLE_ASSET_DIR))


if os.environ.get('UEMMO_SYSTEM_TEST_ROOM'):
    # M5-018A: the map build/verify mode replaces the table generation for
    # this invocation (one editor process, one write surface).
    build_system_test_room_map()
elif is_vehicle_verify_mode():
    # M5-038: the fresh-process reload verification replaces generation for
    # this invocation (no asset is written in this mode).
    verify_vehicle_assets()
else:
    main()
