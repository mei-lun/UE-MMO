"""M8-019B: import the throwstone projectile texture and material.

Same contract as the proven fighter/goblin visual imports (M6-022/M8-019A):
Nearest/Clamp/sRGB/TC_EditorIcon/never_stream/TMGS_NOMIPMAPS texture, then a
DUPLICATE of the proven fighter material with its PageTex node retargeted to
the stone page (the same shader lineage; the UVRectMin/UVRectSize parameter
contract is inherited unchanged - the projectile drives the rect directly
from C++). The flipbook timing (4 frames x 30ms loop) lives in the projectile
class and mirrors Data/DNFMReplica/monsters/goblinthrower/throwstone_manifest.json.
"""
import json
from pathlib import Path

import unreal

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

ROOT = Path(unreal.Paths.project_dir()).resolve()
MANIFEST_PATH = ROOT / 'Data' / 'DNFMReplica' / 'monsters' / 'goblinthrower' / 'throwstone_manifest.json'
PAGE_PNG = ROOT / 'Data' / 'DNFMReplica' / 'monsters' / 'goblinthrower' / 'throwstone_page_0.png'
DEST = '/Game/UEMMO/DNFM/Monsters/Goblinthrower'
FIGHTER_MATERIAL = ('/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/'
                    'Materials/M_DNFMFighterVisual.M_DNFMFighterVisual')

TEXTURE_NAME = 'T_throwstone_page0'
MATERIAL_NAME = 'M_ThrowStoneVisual'

REPORT = {'success': False, 'errors': []}


def enum_member(enum_type, *names):
    for n in names:
        if hasattr(enum_type, n):
            return getattr(enum_type, n)
    raise RuntimeError('%s has none of %s' % (enum_type, names))


FILTER_NEAREST = enum_member(unreal.TextureFilter, 'TF_NEAREST')
MIP_NOMIPMAPS = getattr(unreal.TextureMipGenSettings, 'TMGS_NOMIPMAPS', None)
COMPRESSION_EDITOR_ICON = enum_member(unreal.TextureCompressionSettings, 'TC_EDITORICON', 'TC_EDITOR_ICON', 'TC_EditorIcon')


def load(path):
    obj = unreal.load_asset(path)
    if obj is None:
        raise RuntimeError('missing asset ' + path)
    return obj


def import_texture():
    disk_sha = hashlib.sha256(PAGE_PNG.read_bytes()).hexdigest()
    manifest = json.loads(MANIFEST_PATH.read_text(encoding='utf-8'))
    if manifest.get('page_sha256') != disk_sha:
        raise RuntimeError('throwstone page png sha drift vs manifest')
    tex_path = DEST + '/T/' + TEXTURE_NAME
    if ASSETS.does_asset_exist(tex_path):
        ASSETS.delete_asset(tex_path)
    task = unreal.AssetImportTask()
    task.filename = str(PAGE_PNG)
    task.destination_path = DEST + '/T'
    task.destination_name = TEXTURE_NAME
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = load(tex_path)
    tex.set_editor_property('filter', FILTER_NEAREST)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    if MIP_NOMIPMAPS is None:
        REPORT['limitations'] = REPORT.get('limitations', []) + ['TMGS_NOMIPMAPS unavailable; mips left default']
    else:
        tex.set_editor_property('mip_gen_settings', MIP_NOMIPMAPS)
    tex.set_editor_property('srgb', True)
    tex.set_editor_property('compression_settings', COMPRESSION_EDITOR_ICON)
    tex.set_editor_property('never_stream', True)
    ASSETS.save_loaded_asset(tex)
    REPORT['texture'] = {'path': tex_path, 'sha256': disk_sha,
                         'size': list(manifest['page_size_px'])}
    return tex


def build_material(tex):
    mat_path = DEST + '/M/' + MATERIAL_NAME
    if ASSETS.does_asset_exist(mat_path):
        ASSETS.delete_asset(mat_path)
    mat = ASSETS.duplicate_asset(FIGHTER_MATERIAL, mat_path)
    if mat is None:
        raise RuntimeError('material duplication failed: ' + FIGHTER_MATERIAL)
    retargeted = False
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(mat):
        if expr.get_class().get_name() == 'MaterialExpressionTextureSampleParameter2D':
            expr.set_editor_property('texture', tex)
            retargeted = True
    if not retargeted:
        raise RuntimeError('no TextureSampleParameter2D found on the duplicated material')
    unreal.MaterialEditingLibrary.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    REPORT['material'] = {'path': mat_path, 'method': 'duplicate fighter + retarget PageTex'}


def main():
    import hashlib
    globals()['hashlib'] = hashlib
    tex = import_texture()
    build_material(tex)
    REPORT['success'] = True


try:
    main()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
    raise
finally:
    art = ROOT / 'Artifacts' / 'Tasks' / 'M8-019B'
    art.mkdir(parents=True, exist_ok=True)
    (art / 'ue-throwstone-import.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
    unreal.log('UEMMO M8-019B throwstone import done: report %s' % (art / 'ue-throwstone-import.json'))
