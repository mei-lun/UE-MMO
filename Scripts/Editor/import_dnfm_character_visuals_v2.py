"""M8-017: import swordman/gunner DNFM visual sets into UE editor assets.

Parameterized by UEMMO_DNFM_VISUAL_ROLE (swordman|gunner). Per role, in
/Game/UEMMO/DNFM/Characters/<Role>:
  - T_<Role>_page<N>   one UTexture2D per manifest page (Nearest/Clamp/no
    mips when available/TC_EditorIcon/sRGB - the M6-022 contract)
  - M_<Role>Visual     Unlit + TwoSided + Masked material with the frozen
    UVRect parameter contract (PageTex/UVRectMin/UVRectSize, V flipped in
    the graph - M6-022)
  - DA_<Role>VisualSet UDNFMCharacterVisualSet built through the production
    C++ parser BuildManifestJson (same entry the fighter used).

MODE=verify re-opens the assets and asserts counts against the manifest.
Run via Scripts/RunDNFMCharacterVisualImportV2.ps1 (single-writer + run id).
Evidence: Artifacts/Tasks/M8-017/ue-<mode>-<role>.json
"""

import hashlib
import json
import os
import traceback
from pathlib import Path

import unreal

DEST_ROOT = '/Game/UEMMO/DNFM/Characters'
ROOT = Path(unreal.Paths.project_dir()).resolve()
DATA = ROOT / 'Data' / 'DNFMReplica' / 'characters'
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-017'

ROLE = os.environ.get('UEMMO_DNFM_VISUAL_ROLE', '')
ROLE_KIND = 'characters' if ROLE in ('swordman', 'gunner') else ('monsters' if ROLE in ('goblinthrower', 'taubeast') else '')
if not ROLE_KIND:
    raise ValueError('UEMMO_DNFM_VISUAL_ROLE must be swordman, gunner, goblinthrower or taubeast, got %r' % ROLE)
MODE = os.environ.get('UEMMO_DNFM_VISUAL_MODE', 'import')
RUN_ID = os.environ.get('UEMMO_DNFM_VISUAL_RUN_ID', '')

ROLE_DIR = ROOT / 'Data' / 'DNFMReplica' / ROLE_KIND / ROLE
MANIFEST_PATH = ROLE_DIR / 'manifest.json'
DEST = ('/Game/UEMMO/DNFM/Characters/' if ROLE_KIND == 'characters' else '/Game/UEMMO/DNFM/Monsters/') + ROLE.capitalize()
SET_NAME = 'DA_%sVisualSet' % ROLE.capitalize()

TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
ASSETS = unreal.EditorAssetLibrary

REPORT = {'success': False, 'mode': MODE, 'role': ROLE, 'run_id': RUN_ID,
          'textures': [], 'material': None, 'data_asset': None, 'limitations': [], 'errors': []}


def enum_member(enum_cls, *names):
    for name in names:
        member = getattr(enum_cls, name, None)
        if member is not None:
            return member
    raise AttributeError('no %s member among %s' % (enum_cls, names))


FILTER_NEAREST = enum_member(unreal.TextureFilter, 'TF_NEAREST', 'TF_Nearest')
MIP_NOMIPMAPS = getattr(unreal.TextureMipGenSettings, 'TMGS_NOMIPMAPS', None)
COMPRESSION_EDITOR_ICON = enum_member(unreal.TextureCompressionSettings, 'TC_EDITOR_ICON', 'TC_EDITORICON', 'TC_EditorIcon')


def node(mat, cls, **props):
    n = unreal.MaterialEditingLibrary.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(a, output, b, input_name):
    if not unreal.MaterialEditingLibrary.connect_material_expressions(a, output, b, input_name):
        raise RuntimeError('connect failed: %s -> %s/%s' % (a.get_class().get_name(), b.get_class().get_name(), input_name))


def output(a, out, prop):
    if not unreal.MaterialEditingLibrary.connect_material_property(a, out, prop):
        raise RuntimeError('connect property failed: %s -> %s' % (a.get_class().get_name(), prop))


def load(path):
    obj = unreal.load_asset(path)
    if obj is None:
        raise RuntimeError('missing asset ' + path)
    return obj


def import_texture(index, page):
    png = ROLE_DIR / page['file']
    disk_sha = hashlib.sha256(png.read_bytes()).hexdigest()
    if disk_sha != page['sha256']:
        raise RuntimeError('page png hash drift: %s' % page['file'])
    name = 'T_%s_page%d' % (ROLE, index)
    task = unreal.AssetImportTask()
    task.filename = str(png)
    task.destination_path = DEST + '/T'
    task.destination_name = name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = load(DEST + '/T/' + name)
    tex.set_editor_property('filter', FILTER_NEAREST)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    if MIP_NOMIPMAPS is None:
        REPORT['limitations'].append('TMGS_NOMIPMAPS unavailable; mips left default for ' + name)
    else:
        tex.set_editor_property('mip_gen_settings', MIP_NOMIPMAPS)
    tex.set_editor_property('srgb', True)
    tex.set_editor_property('compression_settings', COMPRESSION_EDITOR_ICON)
    tex.set_editor_property('never_stream', True)
    ASSETS.save_loaded_asset(tex)
    REPORT['textures'].append({'name': name, 'source_png': str(png),
                               'source_sha256': disk_sha,
                               'size': list(page['size']),
                               'cells_on_page': page.get('cells_on_page')})
    return tex


def build_material(textures):
    name = 'M_%sVisual' % ROLE.capitalize()
    mat_path = DEST + '/M/' + name
    if ASSETS.does_asset_exist(mat_path):
        ASSETS.delete_asset(mat_path)
    mat = TOOLS.create_asset(name, DEST + '/M', unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property('two_sided', True)
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_MASKED)
    uv0 = node(mat, unreal.MaterialExpressionTextureCoordinate, u_tiling=1.0, v_tiling=1.0)
    mask_u = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
    connect(uv0, '', mask_u, '')
    mask_v = node(mat, unreal.MaterialExpressionComponentMask, r=False, g=True, b=False, a=False)
    connect(uv0, '', mask_v, '')
    v_flip = node(mat, unreal.MaterialExpressionOneMinus)
    connect(mask_v, '', v_flip, '')
    quad_uv = node(mat, unreal.MaterialExpressionAppendVector)
    connect(mask_u, '', quad_uv, 'A')
    connect(v_flip, '', quad_uv, 'B')
    uv_size = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='UVRectSize',
                   default_value=unreal.LinearColor(1.0, 1.0, 0.0, 0.0))
    uv_min = node(mat, unreal.MaterialExpressionVectorParameter, parameter_name='UVRectMin',
                  default_value=unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    mask_size = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
    connect(uv_size, '', mask_size, '')
    mask_min = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
    connect(uv_min, '', mask_min, '')
    scaled = node(mat, unreal.MaterialExpressionMultiply)
    connect(quad_uv, '', scaled, 'A')
    connect(mask_size, '', scaled, 'B')
    offset = node(mat, unreal.MaterialExpressionAdd)
    connect(scaled, '', offset, 'A')
    connect(mask_min, '', offset, 'B')
    sample = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name='PageTex',
                  texture=textures[0])
    connect(offset, '', sample, 'UVs')
    output(sample, 'RGB', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    output(sample, 'A', unreal.MaterialProperty.MP_OPACITY_MASK)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    REPORT['material'] = {'name': name, 'path': mat_path,
                          'uv_formula': 'pageUV = UVRectMin + vec(u, 1-v) * UVRectSize'}
    return mat


def build_data_asset(textures, manifest_text, manifest):
    actions = manifest['actions']
    pages = manifest['pages']
    path = DEST + '/' + SET_NAME
    asset = None
    if ASSETS.does_asset_exist(path):
        existing = load(path)
        if existing.get_class().get_name() == 'DNFMCharacterVisualSet':
            asset = existing
        elif not ASSETS.delete_asset(path):
            raise RuntimeError('stale non-DNFM data asset could not be deleted: ' + path)
    if asset is None:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class', unreal.DNFMCharacterVisualSet)
        asset = TOOLS.create_asset(SET_NAME, DEST, unreal.DNFMCharacterVisualSet, factory)
        if asset is None:
            raise RuntimeError('data asset creation failed: ' + path)
    error_text = unreal.DNFMCharacterVisualSet.build_manifest_into_set(manifest_text, textures, asset) or ''
    if error_text:
        raise RuntimeError('BuildManifestJson refused the manifest: %s' % error_text)
    frame_count = sum(len(a['frames']) for a in actions)
    assert len(asset.get_editor_property('actions')) == len(actions), 'actions count mismatch'
    assert len(asset.get_editor_property('page_textures')) == len(pages), 'page textures mismatch'
    ASSETS.set_metadata_tag(asset, 'DNFM.Source', 'DNFM130_5_8_202 %s composite (M8-016 manifest)' % ROLE)
    ASSETS.set_metadata_tag(asset, 'DNFM.ManifestSha256',
                            hashlib.sha256(manifest_text.encode('utf-8')).hexdigest())
    ASSETS.set_metadata_tag(asset, 'DNFM.Calibration', json.dumps({
        'cm_per_px': manifest.get('px_to_cm'),
        'rule': 'pivot of each frame is the foot anchor (M8-016 pipeline)'}))
    ASSETS.save_loaded_asset(asset)
    REPORT['data_asset'] = {'asset': SET_NAME, 'path': path,
                            'character_id': manifest.get('character'),
                            'actions': len(actions), 'frames': frame_count,
                            'pages': len(pages), 'saved': True}


def run_import():
    manifest_text = MANIFEST_PATH.read_text(encoding='utf-8')
    manifest = json.loads(manifest_text)
    textures = [import_texture(index, page) for index, page in enumerate(manifest['pages'])]
    build_material(textures)
    build_data_asset(textures, manifest_text, manifest)


def run_verify():
    manifest = json.loads(MANIFEST_PATH.read_text(encoding='utf-8'))
    textures = [load(DEST + '/T/T_%s_page%d' % (ROLE, index))
                for index in range(len(manifest['pages']))]
    asset = load(DEST + '/' + SET_NAME)
    assert len(asset.get_editor_property('actions')) == len(manifest['actions']), 'actions mismatch'
    assert len(asset.get_editor_property('page_textures')) == len(manifest['pages']), 'pages mismatch'
    assert asset.get_editor_property('character_id') == manifest.get('character'), 'character mismatch'
    mat = load(DEST + '/M/M_%sVisual' % ROLE.capitalize())
    REPORT['data_asset'] = {'verified': True, 'actions': len(manifest['actions']),
                            'pages': len(manifest['pages'])}
    REPORT['material'] = {'verified': True, 'path': DEST + '/M/M_%sVisual' % ROLE.capitalize()}


try:
    if MODE == 'import':
        run_import()
        REPORT['success'] = True
    elif MODE == 'verify':
        run_verify()
        REPORT['success'] = True
    else:
        raise ValueError(MODE)
except Exception:
    REPORT['errors'].append(traceback.format_exc())
    raise
finally:
    ART.mkdir(parents=True, exist_ok=True)
    (ART / ('ue-%s-%s.json' % (MODE, ROLE))).write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
    unreal.log('UEMMO M8-017 %s %s done: report %s' % (MODE, ROLE, ART / ('ue-%s-%s.json' % (MODE, ROLE))))
