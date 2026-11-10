"""M8-036: import the skill-FX flipbook pages into /Game/UEMMO/Combat/FX.

Eight strip pages from Data/DNFMReplica/characters/fighter/fx (built by
build_skill_fx.py) become nearest-filtered textures plus one flipbook
material each - a duplicate of M_ThrowStoneVisual with the PageTex swapped
(the UVRect contract pageUV = UVRectMin + vec(u, 1-v) * UVRectSize is
inherited unchanged, one strip cell per frame, V flipped in the material).

Verify mode (UEMMOSkillFxVerify on the command line) re-checks the assets on
disk instead of importing.
"""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
SRC = ROOT / 'Data' / 'DNFMReplica' / 'characters' / 'fighter' / 'fx'
MANIFEST = SRC / 'skill_fx_manifest.json'
DEST = '/Game/UEMMO/Combat/FX'
PARENT_MATERIAL = '/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_ThrowStoneVisual.M_ThrowStoneVisual'
MODE = 'verify' if 'UEMMOSkillFxVerify' in unreal.SystemLibrary.get_command_line() else 'import'
REPORT = {'success': False, 'mode': MODE, 'errors': [], 'materials': []}

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def import_texture(name, png_path):
    tex_path = DEST + '/T_' + name
    if ASSETS.does_asset_exist(tex_path):
        ASSETS.delete_asset(tex_path)
    task = unreal.AssetImportTask()
    task.filename = str(png_path)
    task.destination_path = DEST
    task.destination_name = 'T_' + name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = unreal.load_asset(tex_path)
    if tex is None:
        raise RuntimeError('texture import failed: ' + tex_path)
    tex.set_editor_property('filter', unreal.TextureFilter.TF_NEAREST)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('srgb', True)
    tex.set_editor_property('never_stream', True)
    ASSETS.save_loaded_asset(tex)
    return tex


def build_material(name, tex):
    mat_path = DEST + '/M_' + name
    if ASSETS.does_asset_exist(mat_path):
        ASSETS.delete_asset(mat_path)
    parent = unreal.load_asset(PARENT_MATERIAL)
    if parent is None:
        raise RuntimeError('parent material missing: ' + PARENT_MATERIAL)
    mat = ASSETS.duplicate_asset(PARENT_MATERIAL, mat_path)
    if mat is None:
        raise RuntimeError('material dup failed for ' + mat_path)
    swapped = False
    for expr in MEL.get_material_expressions(mat):
        if expr.get_class().get_name() == 'MaterialExpressionTextureSampleParameter2D':
            expr.set_editor_property('texture', tex)
            swapped = True
    if not swapped:
        raise RuntimeError('no TextureSampleParameter2D found in the duplicated material')
    MEL.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    return mat


def run_import():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if not ASSETS.does_directory_exist(DEST):
        ASSETS.make_directory(DEST)
    for page in manifest['pages']:
        name = 'FX_' + page['page']
        tex = import_texture(name, SRC / page['png'])
        mat = build_material(name, tex)
        if mat is None:
            raise RuntimeError('material missing after build: ' + name)
        REPORT['materials'].append({'page': page['page'], 'material': DEST + '/M_' + name,
                                    'texture': DEST + '/T_' + name,
                                    'frames': page['frames'], 'frame_ms': page['frame_ms']})
        print('UEMMO_SKILL_FX: %s -> %d frames @ %dms' % (page['page'], page['frames'], page['frame_ms']))
    REPORT['success'] = True


def run_verify():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    missing = []
    for page in manifest['pages']:
        for kind in ('T_', 'M_'):
            path = DEST + '/' + kind + 'FX_' + page['page']
            if not ASSETS.does_asset_exist(path):
                missing.append(path)
    REPORT['verify'] = {'checked': len(manifest['pages']) * 2, 'missing': missing,
                        'success': not missing}
    if missing:
        raise RuntimeError('verify: missing assets: %s' % missing)
    print('UEMMO_SKILL_FX_VERIFY: %d pages ok' % len(manifest['pages']))


try:
    run_verify() if MODE == 'verify' else run_import()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
    raise
finally:
    (ROOT / 'Artifacts' / 'Logs' / 'skill-fx-import.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1), encoding='utf-8')
    unreal.log('UEMMO M8-036 skill fx %s done' % MODE)
