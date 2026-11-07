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
    # M8-019A: a previously-failed import can leave an empty texture whose
    # source data replace_existing never repopulates; delete and re-import.
    tex_path = DEST + '/T/' + name
    if ASSETS.does_asset_exist(tex_path):
        ASSETS.delete_asset(tex_path)
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
    """M8-019A: build the role material by DUPLICATING the proven fighter
    material and retargeting its PageTex node texture. The from-scratch
    graph build (identical node dump!) produced MIDs that rendered nothing
    in-game; the duplicated material carries the fighter's proven shader
    lineage. The UVRect parameter contract is inherited unchanged."""
    name = 'M_%sVisual' % ROLE.capitalize()
    mat_path = DEST + '/M/' + name
    if ASSETS.does_asset_exist(mat_path):
        ASSETS.delete_asset(mat_path)
    fighter_path = ('/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/'
                    'Materials/M_DNFMFighterVisual.M_DNFMFighterVisual')
    mat = unreal.EditorAssetLibrary.duplicate_asset(fighter_path, mat_path)
    if mat is None:
        raise RuntimeError('material duplication failed: ' + fighter_path)
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(mat):
        cls = expr.get_class().get_name()
        if cls == 'MaterialExpressionTextureSampleParameter2D':
            expr.set_editor_property('texture', textures[0])
    unreal.MaterialEditingLibrary.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    REPORT['material'] = {'name': name, 'path': mat_path,
                          'method': 'duplicate fighter + retarget PageTex',
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


def save_report():
    ART.mkdir(parents=True, exist_ok=True)
    (ART / ('ue-%s-%s.json' % (MODE, ROLE))).write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')


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
    if MODE != 'import' or not REPORT['success']:
        ART.mkdir(parents=True, exist_ok=True)
        save_report()
        unreal.log('UEMMO M8-017 %s %s done (no keepalive): report %s' % (MODE, ROLE, ART / ('ue-%s-%s.json' % (MODE, ROLE))))

if MODE == 'import' and REPORT['success']:
    # M8-019A: BC7 compression runs ASYNC after import. The first import
    # saved textures immediately, capturing EMPTY compressed mips - every
    # MID sampling them rendered nothing in-game (the editor renders from
    # source data and looked fine, which masked this). Idle ~300 ticks to
    # let the compression queue finish, then RE-SAVE all imported assets.
    state = {'frame': 0}

    def resave_tick(_dt):
        try:
            state['frame'] += 1
            if state['frame'] < 300:
                return
            for row in REPORT['textures']:
                tex = unreal.load_asset(DEST + '/T/' + row['name'])
                if tex is not None:
                    ASSETS.save_loaded_asset(tex, only_if_is_dirty=False)
            mat = unreal.load_asset(DEST + '/M/M_%sVisual' % ROLE.capitalize())
            if mat is not None:
                ASSETS.save_loaded_asset(mat, only_if_is_dirty=False)
            da_path = DEST + '/' + SET_NAME
            if ASSETS.does_asset_exist(da_path):
                ASSETS.save_loaded_asset(unreal.load_asset(da_path), only_if_is_dirty=False)
            unreal.log('UEMMO M8-017 %s: post-compression resave done' % ROLE)
            REPORT['post_compression_resave'] = True
            REPORT['success'] = True
            save_report()
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        except Exception:
            import traceback as tb2
            REPORT['errors'].append(tb2.format_exc())
            save_report()
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    handle = unreal.register_slate_post_tick_callback(resave_tick)
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    globals()['_resave_keepalive'] = (handle,)
