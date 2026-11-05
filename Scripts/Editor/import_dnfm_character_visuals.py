"""M6-022: import the DNFM fighter composite page into UE editor assets.

Source of truth: SourceAssets/Unpacked/DNFM130_5_8_202/Characters/Fighter/composite/
(manifest.json from Scripts/DNFM/prepare_fighter_visual_set.py + fighter_page_0.png).

MODE=import builds, in /Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter:
  - T_DNFMFighter_page<N>   one UTexture2D per manifest page: Nearest/Clamp/no
    mipmaps (plan section 5.2), sRGB color, TC_EditorIcon compression (BC7 keeps
    the hard alpha edges the Masked material cuts on; TC_Masks would collapse
    the RGB to mask channels), never streamed.
  - M_DNFMFighterVisual     Unlit + TwoSided + BLEND_MASKED material with the
    frozen parameter contract PageTex / UVRectMin / UVRectSize and the linear
    mapping pageUV = UVRectMin + quadUV * UVRectSize (V y-down from the page
    top - the M6-015 screenshot-verified orientation chain). The Masked-vs-
    Translucent choice is made from an in-process alpha histogram: Masked
    unless the soft-alpha (1..254) share of visible pixels reaches 0.5%.
  - DA_DNFMFighterVisualSet the UDNFMCharacterVisualSet data asset, filled by
    UDNFMCharacterVisualSet::BuildFromManifestJson (the validated C++ parser -
    the import re-parses nothing in Python).

MODE=verify re-loads everything in a FRESH editor process and asserts sizes,
sampling settings, material graph/parameters, every manifest frame row against
the data asset, and the pawn CDO DefaultVisualSet soft binding. Writes
ue-import.json / ue-verify.json (with the launch run_id) into
Artifacts/Tasks/M6-022 for the task evidence trail.
"""

import hashlib
import json
import os
import struct
import traceback
import zlib
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
COMPOSITE_DIR = ROOT / 'SourceAssets/Unpacked/DNFM130_5_8_202/Characters/Fighter/composite'
ART = ROOT / 'Artifacts/Tasks/M6-022'
ART.mkdir(parents=True, exist_ok=True)
MODE = os.environ.get('UEMMO_DNFM_VISUAL_MODE', 'import')
RUN_ID = os.environ.get('UEMMO_DNFM_VISUAL_RUN_ID', '')

DEST = '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter'
MATERIAL_NAME = 'M_DNFMFighterVisual'
SET_NAME = 'DA_DNFMFighterVisualSet'
MANIFEST_PATH = COMPOSITE_DIR / 'manifest.json'
MANIFEST_TEXT = MANIFEST_PATH.read_text(encoding='utf-8-sig')
MANIFEST = json.loads(MANIFEST_TEXT)
if MANIFEST.get('generator') != 'Scripts/DNFM/prepare_fighter_visual_set.py':
    raise RuntimeError('Unexpected manifest generator: %s' % MANIFEST.get('generator'))

PAGES = MANIFEST['pages']
ACTIONS = MANIFEST['actions']
TEXTURE_NAMES = ['T_DNFMFighter_page%d' % index for index in range(len(PAGES))]

# Calibration inputs (plan section 5.1): standing head-top pixels come from the
# rest clip's frame 0 (canvas y-down: the pivot IS the foot anchor, the canvas
# top is the head top); the target height is the pawn capsule (half-height 96).
CALIB_TARGET_CM = 192.0
REST_CLIP = next(a for a in ACTIONS if a['action_id'] == 'rest')
CALIB_HEAD_TOP_PX = float(REST_CLIP['frames'][0]['pivot_px'][1])

SOFT_ALPHA_DECISION_SHARE = 0.005  # Masked below this soft share of visible pixels

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def enum_member(enum_cls, *names):
    """UE python upper-cases UENUM members (TA_Clamp -> TA_CLAMP); resolve by
    trying the given spellings and then a case-insensitive scan."""
    for name in names:
        if hasattr(enum_cls, name):
            return getattr(enum_cls, name)
    wanted = {name.lower() for name in names}
    for attr in dir(enum_cls):
        if attr.lower() in wanted:
            return getattr(enum_cls, attr)
    raise AttributeError('%s has none of %s (has: %s)' % (enum_cls.__name__, names,
                                                          [a for a in dir(enum_cls) if a.startswith('T')][:12]))


FILTER_NEAREST = enum_member(unreal.TextureFilter, 'TF_NEAREST', 'TF_Nearest')
MIP_NOMIPMAPS = None
try:
    MIP_NOMIPMAPS = enum_member(unreal.TextureMipGenSettings, 'TMGS_NO_MIPMAPS', 'TMGS_NOMIPMAPS', 'TMGS_NoMipmaps')
except AttributeError:
    pass  # recorded as a limitation; mip_gen_settings stays default
COMPRESSION_EDITOR_ICON = enum_member(unreal.TextureCompressionSettings, 'TC_EDITOR_ICON', 'TC_EDITORICON', 'TC_EditorIcon')


def tex_size(tex):
    """UTexture2D size probe. blueprint_get_built_texture_size is the honest
    5.8.3 surface (the built/serialized size - the plain size getters read the
    lazy streaming platform data, which reports a 32x32 placeholder in a fresh
    process before the texture builds)."""
    try:
        built = tex.blueprint_get_built_texture_size()
        return [int(built.x), int(built.y)]
    except AttributeError:
        pass
    try:
        return [int(tex.blueprint_get_size_x()), int(tex.blueprint_get_size_y())]
    except AttributeError:
        pass
    try:
        return [int(tex.get_editor_property('size_x')), int(tex.get_editor_property('size_y'))]
    except Exception:
        return None

REPORT = {
    'success': False,
    'mode': MODE,
    'run_id': RUN_ID,
    'dest': DEST,
    'manifest': {'path': str(MANIFEST_PATH), 'sha256': hashlib.sha256(MANIFEST_TEXT.encode('utf-8')).hexdigest(),
                 'character': MANIFEST.get('character'), 'outfit': MANIFEST.get('outfit'),
                 'actions': len(ACTIONS), 'frames': sum(len(a['frames']) for a in ACTIONS), 'pages': len(PAGES)},
    'alpha_histogram': None,
    'textures': [],
    'material': None,
    'data_asset': None,
    'binding': None,
    'calibration': None,
    'errors': [],
    'limitations': [],
}


def save_report():
    (ART / ('ue-%s.json' % MODE)).write_text(json.dumps(REPORT, ensure_ascii=True, indent=2), encoding='utf-8')


def load(path):
    obj = unreal.load_asset(path)
    if not obj:
        raise RuntimeError('Missing asset ' + path)
    return obj


def node(mat, cls, **props):
    n = MEL.create_material_expression(mat, cls)
    for k, v in props.items():
        n.set_editor_property(k, v)
    return n


def connect(a, output, b, input_name):
    if not MEL.connect_material_expressions(a, output, b, input_name):
        raise RuntimeError('Material edge failed ' + input_name)


def output(a, out, prop):
    if not MEL.connect_material_property(a, out, prop):
        raise RuntimeError('Material output failed %s' % prop)


# ---------------------------------------------------------------------------
# Alpha histogram (no PIL inside the editor python; ImageWrapper first, then a
# minimal pure-python PNG decoder for 8-bit RGB/RGBA/palette images).
# ---------------------------------------------------------------------------

def _png_reconstruct(idat, w, h, bpp):
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    pos = 0
    prev = bytearray(stride)
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def _alpha_histogram_png_decoder(png_path):
    data = png_path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise RuntimeError('not a PNG')
    pos, idat, w, h, depth, color_type, trns = 8, [], 0, 0, 8, 6, b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, depth, color_type = struct.unpack('>IIBB', chunk[:10])
        elif typ == b'IDAT':
            idat.append(chunk)
        elif typ == b'tRNS':
            trns = chunk
        elif typ == b'IEND':
            break
    if depth != 8 or color_type not in (2, 3, 6):
        raise RuntimeError('unsupported PNG depth/color type %s/%s' % (depth, color_type))
    total = w * h
    if color_type == 2:
        return {'fully_transparent': 0, 'fully_opaque': total, 'soft_1_254': 0, 'total_pixels': total}
    pixels = _png_reconstruct(idat, w, h, {3: 1, 6: 4}[color_type])
    if color_type == 6:
        alpha = bytes(pixels[3::4])
    else:
        alpha = bytes(pixels)
        if trns:
            table = bytes(max(0, min(255, trns[i])) if i < len(trns) else 255 for i in range(256))
            alpha = bytes(table[i] for i in alpha)
        else:
            alpha = b'\xff' * total
    fully_transparent = alpha.count(0)
    fully_opaque = alpha.count(255)
    return {'fully_transparent': fully_transparent, 'fully_opaque': fully_opaque,
            'soft_1_254': total - fully_transparent - fully_opaque, 'total_pixels': total}


def alpha_histogram(png_path):
    """Soft-alpha share evidence for the Masked-vs-Translucent decision."""
    try:
        wrapper = unreal.ImageWrapperModule.get_image_wrapper(unreal.ImageFormat.PNG)
        wrapper.set_compressed_data(png_path.read_bytes())
        w, h = wrapper.get_width(), wrapper.get_height()
        raw = bytes(wrapper.get_raw_data(unreal.ImagePixelDataFormat.RGBA8))
        if len(raw) != w * h * 4:
            raise RuntimeError('raw RGBA size %d != %d' % (len(raw), w * h * 4))
        alpha = raw[3::4]
        result = {'fully_transparent': alpha.count(0), 'fully_opaque': alpha.count(255),
                  'total_pixels': w * h}
        result['soft_1_254'] = w * h - result['fully_transparent'] - result['fully_opaque']
        route = 'unreal.ImageWrapper'
    except Exception:
        result = _alpha_histogram_png_decoder(png_path)
        route = 'embedded_png_decoder'
    visible = result['fully_opaque'] + result['soft_1_254']
    result['visible_pixels'] = visible
    result['soft_fraction_of_visible'] = (result['soft_1_254'] / visible) if visible else 0.0
    result['route'] = route
    result['decision_rule'] = ('Masked while soft(1..254) share of visible pixels < %.1f%%; '
                               'else Translucent + depth test' % (SOFT_ALPHA_DECISION_SHARE * 100))
    result['decision'] = 'MASKED' if result['soft_fraction_of_visible'] < SOFT_ALPHA_DECISION_SHARE else 'TRANSLUCENT'
    return result


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------

def import_texture(index, page):
    png = COMPOSITE_DIR / page['file']
    disk_sha = hashlib.sha256(png.read_bytes()).hexdigest()
    if disk_sha != page['sha256']:
        raise RuntimeError('page png hash drift: %s' % page['file'])
    name = TEXTURE_NAMES[index]
    task = unreal.AssetImportTask()
    task.filename = str(png)
    task.destination_path = DEST + '/Textures'
    task.destination_name = name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = load(DEST + '/Textures/' + name)
    # Plan section 5.2 sampling: Nearest keeps the pixel-art cells crisp at the
    # fixed side-view zoom, Clamp kills edge wrap bleed, no mipmaps stop
    # cross-cell blending (the M6-014 precedent).
    tex.set_editor_property('filter', FILTER_NEAREST)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    if MIP_NOMIPMAPS is None:
        REPORT['limitations'].append('TextureMipGenSettings.TMGS_NOMIPMAPS unavailable in this build; '
                                     'mip_gen_settings left at default (cross-cell mip bleed possible)')
    else:
        tex.set_editor_property('mip_gen_settings', MIP_NOMIPMAPS)
    # sRGB: the page is displayed color (unlit emissive), not a data map - the
    # same choice the M6-014 flipbook atlas shipped with.
    tex.set_editor_property('srgb', True)
    # Hard alpha edges for Masked: TC_EditorIcon (BC7 on desktop) preserves the
    # alpha steps; TC_Masks would collapse RGB to mask channels and TC_Default
    # (BC1/BC3) blurs alpha more.
    tex.set_editor_property('compression_settings', COMPRESSION_EDITOR_ICON)
    # No mips to stream; the one character page stays resident (2048^2 BC7 is
    # a few MB - fine for this prototype).
    tex.set_editor_property('never_stream', True)
    ASSETS.save_loaded_asset(tex)
    size = tex_size(tex)
    expected = list(page['size'])
    if size is None:
        REPORT['limitations'].append('texture size probe unavailable in this build; size not asserted for ' + name)
    elif size != expected:
        raise RuntimeError('%s imported as %s, expected %s' % (name, size, expected))
    row = {'page': index, 'texture': name, 'path': DEST + '/Textures/' + name,
           'source_png': str(png), 'source_sha256': disk_sha, 'size': size,
           'filter': 'TF_Nearest', 'address_x': 'TA_CLAMP', 'address_y': 'TA_CLAMP',
           'mip_gen_settings': 'TMGS_NoMipmaps', 'srgb': True,
           'compression_settings': 'TC_EditorIcon', 'never_stream': True,
           'compression_reason': 'BC7 (TC_EditorIcon) keeps hard alpha edges for Masked; TC_Masks drops RGB',
           'srgb_reason': 'displayed color on the unlit emissive surface (M6-014 precedent)'}
    REPORT['textures'].append(row)
    return tex


EXPECTED_MATERIAL_NODES = 12  # TexCoord, MaskU, MaskV, OneMinus, Append, 2x VectorParameter, 2x MaskRG, Multiply, Add, PageTex


def _build_material_graph(mat, textures, blend):
    """Build the exact graph; returns the sample node (raises on any edge
    failure or compile error)."""
    uv0 = node(mat, unreal.MaterialExpressionTextureCoordinate, u_tiling=1.0, v_tiling=1.0)
    # V-FLIP (M6-022 screenshot finding): the engine Plane's UVs run v=0 at
    # local -Y (the world-DOWN side of the camera-facing quad), while the
    # component's page rects are y-down from the page top. Sampling min.y at
    # the world bottom renders the sprite upside down (the first rendered
    # screenshot proved it). The graph therefore samples (u, 1-v): the page's
    # top row lands on quad local +Y = camera Up, per plan section 5.2.
    mask_u = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
    connect(uv0, '', mask_u, '')
    mask_v = node(mat, unreal.MaterialExpressionComponentMask, r=False, g=True, b=False, a=False)
    connect(uv0, '', mask_v, '')
    v_flip = node(mat, unreal.MaterialExpressionOneMinus)
    connect(mask_v, '', v_flip, '')
    quad_uv = node(mat, unreal.MaterialExpressionAppendVector)
    connect(mask_u, '', quad_uv, 'A')
    connect(v_flip, '', quad_uv, 'B')
    # VectorParameter (not Vector2: absent from this build's python surface) -
    # its R/G channels carry the UV values, exactly what the component's
    # SetVectorParameterValue(FLinearColor) writes (M6-021 contract). A
    # ComponentMask(R,G) keeps the arithmetic float2: a raw float4 parameter
    # output against the float2 TexCoord fails the SM6 compile with
    # "Arithmetic between types float2 and float3 are undefined".
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
    if blend == unreal.BlendMode.BLEND_MASKED:
        output(sample, 'A', unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        output(sample, 'A', unreal.MaterialProperty.MP_OPACITY)
    return sample


def build_material(textures):
    blend = unreal.BlendMode.BLEND_MASKED
    if REPORT['alpha_histogram']['decision'] != 'MASKED':
        blend = unreal.BlendMode.BLEND_TRANSLUCENT
    mp = DEST + '/Materials/' + MATERIAL_NAME
    mat = None
    if ASSETS.does_asset_exist(mp):
        mat = load(mp)
        # 5.8.3 quirk: on a reused asset the cleanup can leave stale orphan
        # nodes behind (observed: two survivors of the previous graph). Sweep
        # repeatedly; any survivor means the asset is rebuilt from scratch.
        for _ in range(3):
            MEL.delete_all_material_expressions(mat)
            if MEL.get_num_material_expressions(mat) == 0:
                break
        if MEL.get_num_material_expressions(mat) > 0:
            if not ASSETS.delete_asset(mp):
                raise RuntimeError('stale material could not be deleted: ' + mp)
            mat = None
    if mat is None:
        mat = TOOLS.create_asset(MATERIAL_NAME, DEST + '/Materials', unreal.Material,
                                 unreal.MaterialFactoryNew())
    # 5.8.3 quirk: on a reused material asset the expression cleanup can leave
    # stale survivors that only surface in the node count after the new graph
    # is built. If the exact count misses, the asset is deleted and the graph
    # is built once more from scratch.
    for attempt in range(2):
        MEL.delete_all_material_expressions(mat)
        if MEL.get_num_material_expressions(mat) != 0:
            if attempt == 0 and ASSETS.delete_asset(mp):
                mat = TOOLS.create_asset(MATERIAL_NAME, DEST + '/Materials', unreal.Material,
                                         unreal.MaterialFactoryNew())
                continue
            raise RuntimeError('material graph not empty before build: %d nodes'
                               % MEL.get_num_material_expressions(mat))
        mat.set_editor_property('blend_mode', blend)
        mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
        mat.set_editor_property('two_sided', True)
        mat.set_editor_property('opacity_mask_clip_value', 0.5)
        _build_material_graph(mat, textures, blend)
        if MEL.get_num_material_expressions(mat) == EXPECTED_MATERIAL_NODES:
            break
        if attempt == 0 and ASSETS.delete_asset(mp):
            mat = TOOLS.create_asset(MATERIAL_NAME, DEST + '/Materials', unreal.Material,
                                     unreal.MaterialFactoryNew())
            continue
        raise RuntimeError('material graph has %d nodes, expected %d'
                           % (MEL.get_num_material_expressions(mat), EXPECTED_MATERIAL_NODES))
    MEL.recompile_material(mat)
    compile_errors = MEL.recompile_material(mat)
    if compile_errors:
        raise RuntimeError('material failed to compile: %s' % (compile_errors,))
    ASSETS.set_metadata_tag(mat, 'DNFM.Source', 'DNFM130_5_8_202 fighter composite page (M6-020 manifest)')
    ASSETS.set_metadata_tag(mat, 'DNFM.MaterialContract', json.dumps({
        'page_uv_formula': 'pageUV = UVRectMin + vec(u, 1-v) * UVRectSize '
                           '(V flipped in the graph)',
        'v_direction': ('the engine Plane quad runs quadUV.y=0 on the world-DOWN side; the graph flips V so the '
                        'page top row renders at quad local +Y = camera Up (M6-022 first-render screenshot finding; '
                        'the M6-015 acceptance never asserted vertical orientation)'),
        'params': ['PageTex', 'UVRectMin', 'UVRectSize'],
        'blend': 'BLEND_MASKED' if blend == unreal.BlendMode.BLEND_MASKED else 'BLEND_TRANSLUCENT',
        'opacity_mask_clip_value': 0.5,
        'alpha_decision': REPORT['alpha_histogram'],
    }))
    ASSETS.save_loaded_asset(mat)
    REPORT['material'] = {'material': MATERIAL_NAME, 'path': mp,
                          'blend': 'BLEND_MASKED' if blend == unreal.BlendMode.BLEND_MASKED else 'BLEND_TRANSLUCENT',
                          'shading_model': 'MSM_UNLIT', 'two_sided': True, 'opacity_mask_clip_value': 0.5,
                          'page_texture': textures[0].get_name(),
                          'uv_formula': 'pageUV = UVRectMin + vec(u, 1-v) * UVRectSize (graph V-flip)',
                          'blend_reason': REPORT['alpha_histogram']['decision'] + ': soft-alpha share of visible pixels '
                                          + '%.4f%% < %.1f%% threshold' % (
                                              REPORT['alpha_histogram']['soft_fraction_of_visible'] * 100,
                                              SOFT_ALPHA_DECISION_SHARE * 100)}
    return mat


def build_data_asset(textures):
    path = DEST + '/' + SET_NAME
    asset = None
    if ASSETS.does_asset_exist(path):
        existing = load(path)
        if existing.get_class().get_name() == 'DNFMCharacterVisualSet':
            asset = existing  # re-populate in place (ParseManifestInto resets first)
        elif not ASSETS.delete_asset(path):
            raise RuntimeError('stale non-DNFM data asset could not be deleted: ' + path)
    if asset is None:
        factory = unreal.DataAssetFactory()
        try:
            factory.set_editor_property('data_asset_class', unreal.DNFMCharacterVisualSet)
        except Exception as exc:
            raise RuntimeError('DataAssetFactory has no data_asset_class property: %s' % exc)
        asset = TOOLS.create_asset(SET_NAME, DEST, unreal.DNFMCharacterVisualSet, factory)
        if asset is None:
            raise RuntimeError('data asset creation failed: ' + path)
    if asset.get_class().get_name() != 'DNFMCharacterVisualSet':
        raise RuntimeError('data asset class mismatch: %s' % asset.get_class().get_name())
    # The scripting entry returns the refusal reason as the FString return
    # value (empty == success; see the C++ comment for why the error is not
    # an out param or a bool on this python-binding path).
    error_text = unreal.DNFMCharacterVisualSet.build_manifest_into_set(MANIFEST_TEXT, textures, asset) or ''
    if error_text:
        raise RuntimeError('BuildFromManifestJson refused the manifest: %s' % error_text)
    # Post-build sanity against the manifest shape (the C++ parser validated
    # the contract; these asserts pin the populated asset).
    readback = {
        'character_id': repr(asset.get_editor_property('character_id')),
        'outfit_id': repr(asset.get_editor_property('outfit_id')),
        'generator': repr(asset.get_editor_property('generator')),
        'actions': len(asset.get_editor_property('actions')),
        'page_textures': len(asset.get_editor_property('page_textures')),
        'object_name': str(asset.get_name()),
        'outer': str(asset.get_outer()),
    }
    assert asset.get_editor_property('character_id') == MANIFEST['character'], ('character_id', readback)
    assert asset.get_editor_property('outfit_id') == MANIFEST['outfit'], ('outfit_id', readback)
    assert len(asset.get_editor_property('actions')) == len(ACTIONS), ('actions count', readback)
    assert len(asset.get_editor_property('page_textures')) == len(PAGES), ('page textures', readback)
    ASSETS.set_metadata_tag(asset, 'DNFM.Source', 'DNFM130_5_8_202 fighter composite (M6-020 manifest)')
    ASSETS.set_metadata_tag(asset, 'DNFM.ManifestSha256',
                            hashlib.sha256(MANIFEST_TEXT.encode('utf-8')).hexdigest())
    ASSETS.set_metadata_tag(asset, 'DNFM.Calibration', json.dumps({
        'rest_head_top_px': CALIB_HEAD_TOP_PX, 'target_standing_height_cm': CALIB_TARGET_CM,
        'cm_per_pixel': CALIB_TARGET_CM / CALIB_HEAD_TOP_PX,
        'rule': 'pivot of rest frame 0 is the foot; canvas top is the head top'}))
    saved = ASSETS.save_loaded_asset(asset)
    if not saved:
        raise RuntimeError('data asset save failed: ' + path)
    REPORT['data_asset'] = {'asset': SET_NAME, 'path': path, 'class': 'DNFMCharacterVisualSet',
                            'character_id': MANIFEST['character'], 'outfit_id': MANIFEST['outfit'],
                            'actions': len(ACTIONS), 'frames': sum(len(a['frames']) for a in ACTIONS),
                            'pages': len(PAGES), 'page_texture_0': str(asset.get_editor_property('page_textures')[0]),
                            'build_from_manifest_json': 'accepted; error_text=%r' % error_text,
                            'saved': True}
    return asset


def import_all():
    REPORT['alpha_histogram'] = alpha_histogram(COMPOSITE_DIR / PAGES[0]['file'])
    textures = [import_texture(index, page) for index, page in enumerate(PAGES)]
    build_material(textures)
    build_data_asset(textures)


# ---------------------------------------------------------------------------
# Verify (fresh editor process)
# ---------------------------------------------------------------------------

def check_texture(page, index):
    name = TEXTURE_NAMES[index]
    path = DEST + '/Textures/' + name
    tex = load(path)
    assert tex.get_editor_property('filter') == FILTER_NEAREST, (path, 'filter')
    assert tex.get_editor_property('address_x') == unreal.TextureAddress.TA_CLAMP, (path, 'address_x')
    assert tex.get_editor_property('address_y') == unreal.TextureAddress.TA_CLAMP, (path, 'address_y')
    if MIP_NOMIPMAPS is not None:
        assert tex.get_editor_property('mip_gen_settings') == MIP_NOMIPMAPS, (path, 'mip_gen_settings')
    assert tex.get_editor_property('srgb') is True, (path, 'srgb')
    assert tex.get_editor_property('compression_settings') == COMPRESSION_EDITOR_ICON, \
        (path, 'compression_settings')
    assert tex.get_editor_property('never_stream') is True, (path, 'never_stream')
    size = tex_size(tex)
    if size is not None:
        assert size == list(page['size']), (path, 'size', size, page['size'])
    else:
        if 'texture_size_probe_unavailable' not in REPORT['limitations']:
            REPORT['limitations'].append('texture size probe unavailable in this build; size not asserted in verify')
    disk_sha = hashlib.sha256((COMPOSITE_DIR / page['file']).read_bytes()).hexdigest()
    assert disk_sha == page['sha256'], ('source png hash drift', page['file'])
    return {'page': index, 'texture': name, 'path': path, 'size': size, 'filter': 'TF_Nearest',
            'address': 'TA_CLAMP', 'mip_gen_settings': 'TMGS_NoMipmaps', 'srgb': True,
            'compression_settings': 'TC_EditorIcon', 'never_stream': True, 'source_sha256': disk_sha}


def find_expressions(mat, cls, prop_name=None, prop_value=None):
    """Graph enumeration via MaterialEditingLibrary (UMaterial.expressions is
    a protected property in this build; get_material_expressions is the
    working surface)."""
    found = []
    try:
        candidates = list(MEL.get_material_expressions(mat))
    except Exception as exc:
        if 'expressions_unreadable' not in REPORT['limitations']:
            REPORT['limitations'].append('material graph enumeration unavailable in this session type: %r' % exc)
        return found
    for e in candidates:
        if not isinstance(e, cls):
            continue
        if prop_name is not None and e.get_editor_property(prop_name) != prop_value:
            continue
        found.append(e)
    return found


def check_material():
    mp = DEST + '/Materials/' + MATERIAL_NAME
    mat = load(mp)
    assert mat.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_MASKED, (mp, 'blend_mode')
    assert mat.get_editor_property('shading_model') == unreal.MaterialShadingModel.MSM_UNLIT, (mp, 'shading_model')
    assert mat.get_editor_property('two_sided') is True, (mp, 'two_sided')
    assert abs(mat.get_editor_property('opacity_mask_clip_value') - 0.5) < 1e-6, (mp, 'clip')
    samples = find_expressions(mat, unreal.MaterialExpressionTextureSampleParameter2D, 'parameter_name', 'PageTex')
    assert len(samples) == 1, (mp, 'PageTex sample parameter')
    # The exact graph contract: one node of each kind, nothing else.
    node_types = [type(e).__name__ for e in MEL.get_material_expressions(mat)]
    assert len(node_types) == EXPECTED_MATERIAL_NODES, (mp, 'node count', node_types)
    assert node_types.count('MaterialExpressionTextureCoordinate') == 1, (mp, node_types)
    assert node_types.count('MaterialExpressionComponentMask') == 4, (mp, node_types)  # U, V, RG(min), RG(size)
    assert node_types.count('MaterialExpressionOneMinus') == 1, (mp, node_types)  # the V flip
    assert node_types.count('MaterialExpressionAppendVector') == 1, (mp, node_types)
    assert node_types.count('MaterialExpressionVectorParameter') == 2, (mp, node_types)
    assert node_types.count('MaterialExpressionMultiply') == 1, (mp, node_types)
    assert node_types.count('MaterialExpressionAdd') == 1, (mp, node_types)
    texture = samples[0].get_editor_property('texture')
    assert texture is not None and texture.get_name() == TEXTURE_NAMES[0], (mp, 'PageTex texture ref')
    v2_params = {}
    for pname in ('UVRectMin', 'UVRectSize'):
        nodes = find_expressions(mat, unreal.MaterialExpressionVectorParameter, 'parameter_name', pname)
        if not nodes:
            nodes = find_expressions(mat, unreal.MaterialExpressionVector2Parameter, 'parameter_name', pname)
        assert len(nodes) == 1, (mp, pname, 'vector parameter')
        v2_params[pname] = nodes[0]
    emissive = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_OPACITY_MASK)
    for prop_name, node_obj in (('emissive', emissive), ('opacity_mask', opacity)):
        assert isinstance(node_obj, unreal.MaterialExpressionTextureSampleParameter2D), (mp, prop_name, type(node_obj))
    contract = ASSETS.get_metadata_tag(mat, 'DNFM.MaterialContract')
    assert contract and 'UVRectMin' in json.loads(contract)['page_uv_formula'] \
        and '1-v' in json.loads(contract)['page_uv_formula'], (mp, 'DNFM.MaterialContract')
    dep_opts = unreal.AssetRegistryDependencyOptions(include_soft_package_references=False,
                                                     include_hard_package_references=True,
                                                     include_searchable_names=False,
                                                     include_soft_management_references=False,
                                                     include_hard_management_references=False)
    refs = [str(p).rsplit('/', 1)[-1] for p in
            unreal.AssetRegistryHelpers.get_asset_registry().get_dependencies(mp, dep_opts)]
    assert TEXTURE_NAMES[0] in refs, (mp, 'hard texture dependency missing', refs)
    return {'material': MATERIAL_NAME, 'path': mp, 'blend': 'BLEND_MASKED', 'shading_model': 'MSM_UNLIT',
            'two_sided': True, 'opacity_mask_clip_value': 0.5, 'page_tex_parameter': 'PageTex',
            'page_texture_ref': texture.get_name(), 'uv_min_parameter': 'UVRectMin',
            'uv_size_parameter': 'UVRectSize', 'emissive_and_opacity_mask_from_page_sample': True,
            'metadata_contract': True, 'hard_dependencies': refs}


def int_pair(value):
    """FIntPoint arrives as IntPoint (x/y) or a 2-sequence depending on build."""
    if hasattr(value, 'x'):
        return [int(value.x), int(value.y)]
    return [int(value[0]), int(value[1])]


def check_data_asset():
    path = DEST + '/' + SET_NAME
    asset = load(path)
    assert asset.get_class().get_name() == 'DNFMCharacterVisualSet', (path, 'class')
    assert asset.get_editor_property('character_id') == MANIFEST['character'], (path, 'character_id')
    assert asset.get_editor_property('outfit_id') == MANIFEST['outfit'], (path, 'outfit_id')
    assert asset.get_editor_property('generator') == MANIFEST['generator'], (path, 'generator')
    clips = asset.get_editor_property('actions')
    assert len(clips) == len(ACTIONS), (path, 'actions count', len(clips))
    frames_checked = 0
    mismatches = []
    for manifest_action, clip in zip(ACTIONS, clips):
        if str(clip.get_editor_property('action_id')) != manifest_action['action_id']:
            mismatches.append([manifest_action['action_id'], 'action_id'])
            continue
        if bool(clip.get_editor_property('loop')) != bool(manifest_action['loop']):
            mismatches.append([manifest_action['action_id'], 'loop'])
        frames = clip.get_editor_property('frames')
        if len(frames) != len(manifest_action['frames']):
            mismatches.append([manifest_action['action_id'], 'frame count'])
            continue
        for manifest_frame, frame in zip(manifest_action['frames'], frames):
            if int(frame.get_editor_property('duration_ms')) != int(manifest_frame['duration_ms']):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'duration_ms'])
            if int(frame.get_editor_property('page_index')) != int(manifest_frame['page']):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'page'])
            if int_pair(frame.get_editor_property('uv_rect_pos_px')) != list(manifest_frame['uv_rect_px'][:2]):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'uv_rect_pos_px'])
            if int_pair(frame.get_editor_property('uv_rect_size_px')) != list(manifest_frame['uv_rect_px'][2:]):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'uv_rect_size_px'])
            if int_pair(frame.get_editor_property('canvas_size_px')) != list(manifest_frame['canvas_size_px']):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'canvas_size_px'])
            if int_pair(frame.get_editor_property('pivot_px')) != list(manifest_frame['pivot_px']):
                mismatches.append([manifest_action['action_id'], manifest_frame['index'], 'pivot_px'])
            frames_checked += 1
    assert not mismatches, ('frame contract mismatches', mismatches[:10])
    page_textures = asset.get_editor_property('page_textures')
    page_paths = [str(p) for p in page_textures]
    assert all(TEXTURE_NAMES[i] in page_paths[i] for i in range(len(PAGES))), ('page soft refs', page_paths)
    page_sizes = [int_pair(s) for s in asset.get_editor_property('page_sizes_px')]
    assert page_sizes == [list(p['size']) for p in PAGES], ('page sizes', page_sizes)
    pivot = int_pair(clips[0].get_editor_property('frames')[0].get_editor_property('pivot_px'))
    assert clips[0].get_editor_property('action_id') == 'rest' and pivot[1] == int(CALIB_HEAD_TOP_PX), \
        ('rest frame 0 head-top px', pivot)
    return {'asset': SET_NAME, 'path': path, 'actions': len(ACTIONS), 'frames_checked': frames_checked,
            'contract_mismatches': 0, 'page_soft_refs': page_paths, 'page_sizes_px': page_sizes,
            'rest_head_top_px': pivot[1]}


def check_binding():
    binding = {'expected': DEST + '/' + SET_NAME + '.' + SET_NAME}
    cdo = None
    try:
        candidate = unreal.PrototypeCharacter.static_class().get_default_object()
        if candidate is not None and type(candidate).__name__ != 'Class':
            cdo = candidate
    except Exception:
        pass
    if cdo is None:
        # 5.8.3: Class.get_default_object may hand back the class wrapper;
        # the CDO package path is the direct route.
        cdo = unreal.load_object(None, '/Script/UEMMO.Default__PrototypeCharacter')
    component = cdo.get_editor_property('dnfm_visual')
    soft = component.get_editor_property('default_visual_set')
    binding['cdo_default_visual_set'] = str(soft)
    assert binding['expected'] in binding['cdo_default_visual_set'], (binding['expected'], binding['cdo_default_visual_set'])
    binding['bound'] = True
    return binding


def verify():
    REPORT['alpha_histogram'] = alpha_histogram(COMPOSITE_DIR / PAGES[0]['file'])
    REPORT['textures'] = [check_texture(page, index) for index, page in enumerate(PAGES)]
    REPORT['material'] = check_material()
    REPORT['data_asset'] = check_data_asset()
    REPORT['binding'] = check_binding()
    REPORT['calibration'] = {
        'standing_head_top_px': CALIB_HEAD_TOP_PX,
        'target_standing_height_cm': CALIB_TARGET_CM,
        'cm_per_pixel': CALIB_TARGET_CM / CALIB_HEAD_TOP_PX,
        'rule': 'SetWorldScaleFromStandingHeight(headTopPx=rest frame 0 pivot y, 192 cm capsule height)',
    }


try:
    if MODE == 'import':
        import_all()
    elif MODE == 'verify':
        verify()
    else:
        raise ValueError(MODE)
    REPORT['success'] = True
    save_report()
    unreal.log('UEMMO M6-022 %s OK: report %s' % (MODE, ART / ('ue-%s.json' % MODE)))
except Exception:
    REPORT['errors'].append(traceback.format_exc())
    save_report()
    raise
