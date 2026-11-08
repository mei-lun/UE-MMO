"""M8-024: complete the guild scene's placeholder materials.

The M6-011 intermediates marked 78 materials' _MainTex missing (the source
cab was unresolved); M8-024 extracted the 35 missing textures from
prefabs_3d_guildagit_textures.npk and marked them resolved_m8_024 in the
manifest. This script imports those textures under Imported/DNFM130_5_8/
Textures and REBUILDS each affected material in place with the same
build_material logic the M6-009 import used (the meshes reference the
material assets and need no re-import). Verify mode re-checks the metadata.
"""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
STAGE = ROOT / 'SourceAssets' / 'Unpacked' / 'DNFM130_5_8'
MANIFEST = STAGE / 'manifest.json'
GUILD_TEX_DIR = STAGE / 'guild_textures'
DEST = '/Game/UEMMO/Imported/DNFM130_5_8'
TARGET_SLOTS = ('_MainTex', '_BaseMap', '_BaseColorMap', '_DiffuseTex')
MODE = 'verify' if 'UEMMOGuildMaterialVerify' in unreal.SystemLibrary.get_command_line() else 'generate'

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary
REPORT = {'mode': MODE, 'materials': [], 'textures': [], 'errors': [], 'success': False}


def load(path):
    obj = unreal.load_asset(path)
    if not obj:
        raise RuntimeError('Missing asset ' + path)
    return obj


def path(kind, name):
    return DEST + '/' + kind + '/' + name


def node(mat, cls, **props):
    n = MEL.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(a, output, b, input):
    if not MEL.connect_material_expressions(a, output, b, input):
        raise RuntimeError('Material edge failed ' + input)


def output(a, out, prop):
    if not MEL.connect_material_property(a, out, prop):
        raise RuntimeError('Material output failed ' + str(prop))


def scalar(mat, value):
    return node(mat, unreal.MaterialExpressionConstant, r=float(value))


def multiply(mat, a, ao, b, bo):
    n = node(mat, unreal.MaterialExpressionMultiply)
    connect(a, ao, n, 'A')
    connect(b, bo, n, 'B')
    return n


def texture_node(mat, entry, normal=False):
    n = node(mat, unreal.MaterialExpressionTextureSample, texture=load(path('Textures', entry['id'])))
    if normal:
        n.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    scale = entry.get('scale', {'x': 1, 'y': 1})
    offset = entry.get('offset', {'x': 0, 'y': 0})
    uv = node(mat, unreal.MaterialExpressionTextureCoordinate, u_tiling=scale['x'], v_tiling=scale['y'])
    offset_ue = {'x': offset['x'], 'y': 1 - scale['y'] - offset['y']}
    if offset_ue['x'] or offset_ue['y']:
        add = node(mat, unreal.MaterialExpressionAdd)
        off = node(mat, unreal.MaterialExpressionConstant2Vector, r=offset_ue['x'], g=offset_ue['y'])
        connect(uv, '', add, 'A')
        connect(off, '', add, 'B')
        uv = add
    connect(uv, '', n, 'UVs')
    return n


def build_material(row):
    name = row['id']
    mp = path('Materials', name)
    mat = load(mp) if ASSETS.does_asset_exist(mp) else TOOLS.create_asset(
        name, DEST + '/Materials', unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    shader = (row.get('shader') or '').lower()
    f = row.get('floats', {})
    cs = row.get('colors', {})
    ts = row.get('textures', {})
    missing_base = any(k in row.get('missing_textures', {}) for k in TARGET_SLOTS)
    standard = shader in ['standard', 'autodesk interactive'] or missing_base
    unlit = not standard
    additive = 'additive' in shader or f.get('_DstBlend') == 1
    masked = f.get('_Mode') == 1 or row.get('tags', {}).get('RenderType') == 'TransparentCutout'
    translucent = f.get('_Mode') in [2, 3] or row.get('render_queue', 0) >= 3000 or 'transparent' in shader or 'alpha blend' in shader or (f.get('_SrcBlend') == 5 and f.get('_DstBlend') == 10)
    blend = unreal.BlendMode.BLEND_ADDITIVE if additive else unreal.BlendMode.BLEND_MASKED if masked else unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_OPAQUE
    if missing_base:
        blend = unreal.BlendMode.BLEND_OPAQUE
    mat.set_editor_property('blend_mode', blend)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT if unlit else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('two_sided', unlit or f.get('_Cull') == 0)
    mat.set_editor_property('opacity_mask_clip_value', float(f.get('_Cutoff', 0.333)))
    color = cs.get('_Color', cs.get('_BaseColor', {'r': 1, 'g': 1, 'b': 1, 'a': 1}))
    if missing_base:
        color = {'r': 0.38, 'g': 0.42, 'b': 0.5, 'a': 1}
    tint = node(mat, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(color['r'], color['g'], color['b'], 1))
    base = next((ts[k] for k in TARGET_SLOTS if k in ts), None)
    rgb = tint
    alpha = scalar(mat, color.get('a', 1))
    if base:
        tex = texture_node(mat, base)
        rgb = multiply(mat, tex, 'RGB', tint, '')
        alpha = multiply(mat, tex, 'A', alpha, '')
    if unlit:
        vc = node(mat, unreal.MaterialExpressionVertexColor)
        rgb = multiply(mat, rgb, '', vc, '')
        alpha = multiply(mat, alpha, '', vc, 'A')
    output(rgb, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR if unlit else unreal.MaterialProperty.MP_BASE_COLOR)
    if blend == unreal.BlendMode.BLEND_MASKED:
        output(alpha, '', unreal.MaterialProperty.MP_OPACITY_MASK)
    elif blend in [unreal.BlendMode.BLEND_TRANSLUCENT, unreal.BlendMode.BLEND_ADDITIVE]:
        output(alpha, '', unreal.MaterialProperty.MP_OPACITY)
    if not unlit:
        output(scalar(mat, 1 - f.get('_Glossiness', 0)), '', unreal.MaterialProperty.MP_ROUGHNESS)
        output(scalar(mat, f.get('_Metallic', 0)), '', unreal.MaterialProperty.MP_METALLIC)
    ASSETS.set_metadata_tag(mat, 'DNFM.SourceShader', row.get('shader', ''))
    ASSETS.set_metadata_tag(mat, 'DNFM.AppearanceStatus',
                            'MissingBaseTexture_NeutralPlaceholder' if missing_base else 'ApproximateUnityShader')
    ASSETS.save_loaded_asset(mat)


def import_texture(row):
    tex_path = path('Textures', row['id'])
    if ASSETS.does_asset_exist(tex_path):
        ASSETS.delete_asset(tex_path)
    task = unreal.AssetImportTask()
    task.filename = row['file']
    task.destination_path = DEST + '/Textures'
    task.destination_name = row['id']
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = load(tex_path)
    tex.set_editor_property('srgb', True)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_WRAP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_WRAP)
    ASSETS.save_loaded_asset(tex)
    REPORT['textures'].append({'id': row['id']})


def affected_rows(manifest):
    """The materials with at least one resolved_m8_024 base slot, patched in
    memory: the resolved texture enters the row's textures dict."""
    rows = []
    for row in manifest['materials']:
        missing = row.get('missing_textures') or {}
        resolved = {k: v for k, v in missing.items()
                    if v.get('status') == 'resolved_m8_024' and k in TARGET_SLOTS}
        if not resolved:
            continue
        for slot, ref in resolved.items():
            row.setdefault('textures', {})[slot] = {
                'id': ref['resolved_texture_id'],
                'scale': ref.get('scale', {'x': 1, 'y': 1}),
                'offset': ref.get('offset', {'x': 0, 'y': 0}),
            }
        remaining = {k: v for k, v in missing.items() if k not in resolved}
        if remaining:
            row['missing_textures'] = remaining
        else:
            row.pop('missing_textures', None)
        rows.append(row)
    return rows


def generate():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    texture_ids = {r['id'] for r in manifest['textures']
                   if r.get('source') == 'm8_024_guild_texture_completion'}
    tex_rows = {r['id']: r for r in manifest['textures']}
    for tid in sorted(texture_ids):
        import_texture(tex_rows[tid])
    rows = affected_rows(manifest)
    print('materials to rebuild:', len(rows))
    for i, row in enumerate(rows):
        build_material(row)
        if i % 10 == 0:
            unreal.log('[M8-024] rebuilt %d/%d' % (i + 1, len(rows)))
    REPORT['success'] = True


def verify():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    problems = []
    for row in affected_rows(manifest):
        mp = path('Materials', row['id'])
        if not ASSETS.does_asset_exist(mp):
            problems.append(row['id'] + ': missing')
            continue
        mat = load(mp)
        status = ''
        try:
            status = ASSETS.get_metadata_tag(mat, 'DNFM.AppearanceStatus')
        except Exception:
            pass
        if status == 'MissingBaseTexture_NeutralPlaceholder':
            problems.append(row['id'] + ': still placeholder')
    REPORT['problems'] = problems
    REPORT['success'] = not problems
    if problems:
        raise RuntimeError('guild material verify failed: %s' % problems[:8])
    unreal.log('UEMMO_M8_024_VERIFIED')


try:
    verify() if MODE == 'verify' else generate()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
    raise
finally:
    ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-024'
    ART.mkdir(parents=True, exist_ok=True)
    (ART / ('ue-guild-materials-%s.json' % MODE)).write_text(
        json.dumps(REPORT, ensure_ascii=True, indent=1), encoding='utf-8')
    unreal.log('UEMMO M8-024 guild materials %s done' % MODE)
