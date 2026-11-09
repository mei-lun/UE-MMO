"""M8-028: import the volcano map from Data/DNFMReplica/volcano_assets.

Adapted from the M8-011 bamboo import (import_dnfm_bamboo_map.py). Key
differences: the volcano has NO tile grid (the ground is object-based per
the map_volcano.json audit), so the tile placement is skipped entirely.
The 44 objects are placed as textured vertical quads at their calibrated
world positions (the volcano_calibration.json s=1.7615, the depth
y-down→+Y convention from M8-028 part 1).
"""
import json
import math
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
DATA = ROOT / 'Data' / 'DNFMReplica'
ASSETS_DIR = DATA / 'volcano_assets'
CALIB_PATH = DATA / 'volcano_calibration.json'
PLACEMENT_PATH = DATA / 'volcano_placement.json'
DEST = '/Game/UEMMO/DNFM/Volcano'
MAP = DEST + '/Maps/L_DNFM_Volcano01'
LABEL_PREFIX = 'VOL_'
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-028'
MODE = 'verify' if 'UEMMOVolcanoMapVerify' in unreal.SystemLibrary.get_command_line() else 'import'
REPORT = {'success': False, 'mode': MODE, 'errors': [], 'objects': []}

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def load(path):
    obj = unreal.load_asset(path)
    if not obj:
        raise RuntimeError('Missing asset ' + path)
    return obj


def import_texture(name, png_path, size_px):
    tex_path = DEST + '/T/' + name
    if ASSETS.does_asset_exist(tex_path):
        ASSETS.delete_asset(tex_path)
    task = unreal.AssetImportTask()
    task.filename = str(png_path)
    task.destination_path = DEST + '/T'
    task.destination_name = name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = load(tex_path)
    tex.set_editor_property('filter', unreal.TextureFilter.TF_NEAREST)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('srgb', True)
    tex.set_editor_property('never_stream', True)
    ASSETS.save_loaded_asset(tex)
    return tex


def build_material(name, tex):
    mat_path = DEST + '/M/' + name
    if ASSETS.does_asset_exist(mat_path):
        ASSETS.delete_asset(mat_path)
    fighter = '/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_GoblinthrowerVisual.M_GoblinthrowerVisual'
    mat = ASSETS.duplicate_asset(fighter, mat_path)
    if mat is None:
        raise RuntimeError('material dup failed: ' + fighter)
    for expr in MEL.get_material_expressions(mat):
        if expr.get_class().get_name() == 'MaterialExpressionTextureSampleParameter2D':
            expr.set_editor_property('texture', tex)
    MEL.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    return mat


# The side-view quad rotation (the same 108° about X the goblin/player use).
QUAT_ROT = unreal.Quat(x=0.809016995, y=0.0, z=0.0, w=0.587785252)


def run_import():
    calib = json.loads(CALIB_PATH.read_text(encoding='utf-8'))
    place = json.loads(PLACEMENT_PATH.read_text(encoding='utf-8'))
    s = calib['scale']['cm_per_px']
    REPORT['cm_per_px'] = s

    if not ASSETS.does_directory_exist(DEST):
        ASSETS.make_directory(DEST)
    for sub in ('/T', '/M', '/Maps'):
        if not ASSETS.does_directory_exist(DEST + sub):
            ASSETS.make_directory(DEST + sub)

    # Create the map (duplicate the training arena as the base).
    if not ASSETS.does_asset_exist(MAP):
        base = '/Game/UEMMO/Maps/L_TrainingArena'
        if not ASSETS.does_asset_exist(base):
            raise RuntimeError('base map missing: ' + base)
        if not ASSETS.duplicate_asset(base, MAP):
            raise RuntimeError('map dup failed')
        print('UEMMO_VOLCANO: map created')

    if not LEVELS.load_level(MAP):
        raise RuntimeError('cannot load ' + MAP)

    # Remove the training dummies (the M3-026 precedent).
    for a in list(ACTORS.get_all_level_actors()):
        if a.get_class().get_name() == 'TrainingEnemy':
            ACTORS.destroy_actor(a)

    # Remove previous volcano objects (idempotent re-run).
    for a in list(ACTORS.get_all_level_actors()):
        label = a.get_actor_label()
        if label.startswith(LABEL_PREFIX):
            ACTORS.destroy_actor(a)

    # Place the 44 objects as textured quads.
    plane_mesh = load('/Engine/BasicShapes/Plane')
    for o in place['objects']:
        w = o.get('world')
        if not w:
            continue
        idx = o['index']
        layer = o['draw_layer_name']
        fr = (o.get('frames') or [{}])[0]
        png = fr.get('image_file')
        if not png:
            continue
        png_path = ASSETS_DIR / png
        if not png_path.is_file():
            continue
        name = 'OBJ_%02d_%s' % (idx, layer)
        tex = import_texture('T_' + name, png_path, [fr.get('png_size_px', [64, 64])[0], 64])
        mat = build_material('M_' + name, tex)

        actor = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor,
            unreal.Vector(x=w['x_cm'], y=w['foot_y_cm'], z=w['height_cm'] / 2.0),
            unreal.Rotator(pitch=-18.0, yaw=-90.0, roll=90.0))
        if actor is None:
            print('UEMMO_VOLCANO: spawn failed for %s' % name)
            continue
        actor.set_actor_label(LABEL_PREFIX + name)
        smc = actor.get_component_by_class(unreal.StaticMeshComponent)
        if smc:
            smc.set_static_mesh(plane_mesh)
            smc.set_world_scale3d(unreal.Vector(w['width_cm'] / 100.0, w['height_cm'] / 100.0, 1.0))
            smc.set_material(0, mat)
            smc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        REPORT['objects'].append({'name': name, 'layer': layer, 'world': w})
        print('UEMMO_VOLCANO: placed %s at (%.0f, %.0f)' % (name, w['x_cm'], w['foot_y_cm']))

    if not LEVELS.save_current_level():
        raise RuntimeError('save failed')
    REPORT['success'] = True


def run_verify():
    if not LEVELS.load_level(MAP):
        raise RuntimeError('verify: cannot load ' + MAP)
    count = 0
    for a in ACTORS.get_all_level_actors():
        if a.get_actor_label().startswith(LABEL_PREFIX):
            count += 1
    REPORT['verify'] = {'volcano_objects': count, 'success': count > 0}
    if count == 0:
        raise RuntimeError('verify: no volcano objects found')
    print('UEMMO_VOLCANO_VERIFY: %d objects' % count)


try:
    run_verify() if MODE == 'verify' else run_import()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
    raise
finally:
    ART.mkdir(parents=True, exist_ok=True)
    (ART / ('ue-volcano-map-%s.json' % MODE)).write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1), encoding='utf-8')
    unreal.log('UEMMO M8-028 volcano map %s done' % MODE)
