"""M8-011: import the DNFM bamboo P02 segment into UE editor assets.

Builds (MODE=import, in /Game/UEMMO/DNFM/Bamboo and /Game/UEMMO/DNFM/Maps):
  - T_/M_ one UTexture2D + UMaterial per placed PNG (2 tiles + 50 object
    first frames): Nearest/Clamp/NoMipmaps/TC_EditorIcon/sRGB, Unlit +
    TwoSided + Masked-or-Translucent (in-process alpha histogram, M6-022
    rule). Object quads are vertical (roll=90) with the M6-022 (u, 1-v)
    flip; tile quads are horizontal (ground plane) sampled directly (the
    engine Plane runs v=0 at local -Y: for the unrotated plane that is the
    world-far side, matching the PNG top row = tile far edge; for the
    roll=90 quad it is the world-DOWN side, hence the flip).
  - L_DNFM_Bamboo01: 4 ground tile instances + 50 layered object quads
    placed from Data/DNFMReplica/bamboo_placement.json via
    Data/DNFMReplica/bamboo_calibration.json (s=1.7615 cm/px; scene
    x->+X, y-down->+Y, z->+Z; M8-010). A hidden collision slab keeps the
    pawn on Z=0 (walk-band walls are M8-012). PlayerStart sits at the
    center of walkability hypothesis A (0=walkable) for the configured
    tile variant (UEMMO_DNFM_BAMBOO_TILE_VARIANT=P|Q).
  - MODE=verify re-opens the saved map and asserts actor counts and
    transforms against a recomputation from the same JSON inputs.

Run via Scripts/RunDNFMBambooImport.ps1 (single-writer + run-id guards).
Evidence: Artifacts/Tasks/M8-011/ue-<mode>.json
"""

import json
import os
import struct
import traceback
import zlib
from pathlib import Path

import unreal

DEST = '/Game/UEMMO/DNFM/Bamboo'
MAP = DEST + '/Maps/L_DNFM_Bamboo01'
LABEL_PREFIX = 'DNFM_Bamboo_'

ROOT = Path(unreal.Paths.project_dir()).resolve()
DATA = ROOT / 'Data' / 'DNFMReplica'
CALIB_PATH = DATA / 'bamboo_calibration.json'
PLACEMENT_PATH = DATA / 'bamboo_placement.json'
ASSETS_DIR = DATA / 'bamboo_assets'
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-011'

# M8-013: fog objects (BossMap_fog) use the baked two-frame cycler path.
FOG_INDICES = (46, 47, 48, 49)

MODE = os.environ.get('UEMMO_DNFM_BAMBOO_MODE', 'import')
RUN_ID = os.environ.get('UEMMO_DNFM_BAMBOO_RUN_ID', '')
VARIANT = os.environ.get('UEMMO_DNFM_BAMBOO_TILE_VARIANT', 'P')
if VARIANT not in ('P', 'Q'):
    raise ValueError('UEMMO_DNFM_BAMBOO_TILE_VARIANT must be P or Q, got %r' % VARIANT)

TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
ASSETS = unreal.EditorAssetLibrary

REPORT = {'success': False, 'mode': MODE, 'run_id': RUN_ID, 'tile_variant': VARIANT,
          'textures': [], 'materials': [], 'actors': {}, 'player_start': None,
          'floor_slab': None, 'limitations': [], 'errors': []}


def enum_member(enum_cls, *names):
    """UE python upper-cases UENUM members (TA_Clamp -> TA_CLAMP); resolve by
    trying the verbatim name first and then the upper-case form."""
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


def _png_reconstruct(idat, w, h, bpp):
    stride = w * bpp
    prev = bytearray(stride)
    out = bytearray(h * stride)
    pos = 0
    for row in range(h):
        ftype = idat[pos]
        pos += 1
        line = bytearray(idat[pos:pos + stride])
        pos += stride
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ftype != 0:
            raise ValueError('unsupported PNG filter %d' % ftype)
        out[row * stride:(row + 1) * stride] = line
        prev = line
    return out


def alpha_histogram(png_path):
    """Soft-alpha share of visible pixels (M6-022 Masked-vs-Translucent rule),
    decoded in-process from the RGBA PNG (stdlib only)."""
    data = Path(png_path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a png: %s' % png_path)
    pos = 8
    w = h = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', chunk[:10])
            if depth != 8 or color != 6:
                raise ValueError('expected 8-bit RGBA png, got depth=%d color=%d' % (depth, color))
        elif ctype == b'IDAT':
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    px = _png_reconstruct(raw, w, h, 4)
    visible = soft = 0
    for i in range(3, len(px), 4):
        a = px[i]
        if a:
            visible += 1
            if a < 255:
                soft += 1
    return {'visible': visible, 'soft': soft,
            'soft_share': (soft / visible) if visible else 0.0}


def load(path):
    obj = unreal.load_asset(path)
    if obj is None:
        raise RuntimeError('missing asset ' + path)
    return obj


def bamboo_png_size(path):
    """Baked fog PNG dimensions (stdlib PNG header read)."""
    data = Path(path).read_bytes()
    w, h = struct.unpack('>II', data[16:24])
    return [int(w), int(h)]


def tex_size(tex):
    try:
        return [tex.get_editor_property('source').get_editor_property('size_x'),
                tex.get_editor_property('source').get_editor_property('size_y')]
    except Exception:
        return None


def import_texture(name, rel_png, expected_size):
    png = Path(rel_png)
    if not png.is_absolute():
        png = ASSETS_DIR / rel_png
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
    size = tex_size(tex)
    if size is not None and size != expected_size:
        raise RuntimeError('%s imported as %s, expected %s' % (name, size, expected_size))
    return tex


def build_material(name, tex, blend, v_flip):
    mat_path = DEST + '/M/' + name
    if ASSETS.does_asset_exist(mat_path):
        # Rebuild must not stack expression nodes across runs.
        ASSETS.delete_asset(mat_path)
    mat = TOOLS.create_asset(name, DEST + '/M', unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property('two_sided', True)
    mat.set_editor_property('blend_mode', blend)
    uv0 = node(mat, unreal.MaterialExpressionTextureCoordinate, u_tiling=1.0, v_tiling=1.0)
    if v_flip:
        # M6-022 finding: engine Plane v=0 sits on the world-DOWN side of the
        # camera-facing vertical quad; PNG rows are y-down from the top, so
        # sample (u, 1-v) to land the PNG top row on world UP.
        mask_u = node(mat, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
        connect(uv0, '', mask_u, '')
        mask_v = node(mat, unreal.MaterialExpressionComponentMask, r=False, g=True, b=False, a=False)
        connect(uv0, '', mask_v, '')
        v_flip_node = node(mat, unreal.MaterialExpressionOneMinus)
        connect(mask_v, '', v_flip_node, '')
        uv = node(mat, unreal.MaterialExpressionAppendVector)
        connect(mask_u, '', uv, 'A')
        connect(v_flip_node, '', uv, 'B')
    else:
        # Horizontal ground quad (no actor rotation): quad v=0 at local -Y =
        # world -Y = far side; PNG top row = tile far edge (v=0). Direct.
        uv = uv0
    sample = node(mat, unreal.MaterialExpressionTextureSample, texture=tex)
    connect(uv, '', sample, 'UVs')
    output(sample, 'RGB', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if blend == unreal.BlendMode.BLEND_MASKED:
        output(sample, 'A', unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        output(sample, 'A', unreal.MaterialProperty.MP_OPACITY)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
    return mat


def spawn_quad(actors, label, mesh, mat, center, size_cm, rotation):
    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor,
        unreal.Vector(x=center[0], y=center[1], z=center[2]),
        unreal.Rotator(pitch=rotation.get('pitch', 0.0),
                       yaw=rotation.get('yaw', 0.0),
                       roll=rotation.get('roll', 0.0)))
    actor.set_actor_label(label)
    smc = actor.static_mesh_component
    smc.set_static_mesh(mesh)
    smc.set_material(0, mat)
    # Picture quads are display-only: the BasicShapes/Plane ships blocking
    # collision, which would turn every sprite into an invisible wall across
    # the walk band (M8-012 walk evidence). Only FloorSlab and the walls
    # collide. The string profile avoids the build's python enum surface.
    smc.set_collision_profile_name('NoCollision')
    actor.set_actor_scale3d(unreal.Vector(x=size_cm[0] / 100.0, y=size_cm[1] / 100.0, z=1.0))
    actor.set_folder_path('DNFM_Bamboo')
    return actor


def expected_transforms(calib, place):
    s = calib['scale']['cm_per_px']
    expected = {}
    for inst in place['tiles']['instances']:
        y_top = inst['y_top_px'] if VARIANT == 'P' else inst['y_top_screen_ydown_px']
        key = 'tile_' + inst['tile_hash64']
        expected[LABEL_PREFIX + 'Tile%02d_' % inst['placement_index'] + inst['tile_hash64'][:8]] = {
            'center': ((inst['x_px'] + 112.0) * s, (y_top + 300.0) * s, 0.0),
            'size_cm': (224.0 * s, 600.0 * s),
            'scale': (2.24 * s, 6.0 * s, 1.0), 'rot': {'pitch': 0.0, 'yaw': 0.0, 'roll': 0.0},
            'material_key': key}
    for obj in place['objects']:
        obj_index = obj.get('index', 0)
        if obj_index in FOG_INDICES:
            # Fog objects place two mutually-exclusive frame quads with the
            # same transform plus a cycler; verified separately below.
            ff = obj['first_frame']
            w_px, h_px = ff['png_size_px']
            tlx, _tly = ff['top_left_px']
            foot_y = ff['foot_point_px']['y']
            for frame_idx in (0, 1):
                expected[LABEL_PREFIX + 'Fog%02d_F%d' % (obj_index, frame_idx)] = {
                    'center': ((tlx + w_px / 2.0) * s, foot_y * s, (h_px * s) / 2.0),
                    'size_cm': (w_px * s, h_px * s),
                    'scale': (w_px * s / 100.0, h_px * s / 100.0, 1.0),
                    'rot': {'pitch': 0.0, 'yaw': 0.0, 'roll': 90.0}}
            continue
        ff = obj['first_frame']
        w_px, h_px = ff['png_size_px']
        tlx, _tly = ff['top_left_px']
        foot_y = ff['foot_point_px']['y']
        base = Path(ff['image_file']).stem
        key = 'L%d_%02d_%s' % (obj['draw_layer'], obj_index, base.split('_')[-1])
        expected[LABEL_PREFIX + 'L%d_%02d' % (obj['draw_layer'], obj_index)] = {
            'center': ((tlx + w_px / 2.0) * s, foot_y * s, (h_px * s) / 2.0),
            'size_cm': (w_px * s, h_px * s),
            'scale': (w_px * s / 100.0, h_px * s / 100.0, 1.0),
            'rot': {'pitch': 0.0, 'yaw': 0.0, 'roll': 90.0},
            'material_key': key}
    return expected


def spawn_expected(actors, expected, plane_mesh, material_of):
    counts = {'tile': 0, 'object': 0}
    for label, spec in expected.items():
        if 'Fog' in label:
            continue  # fog quads are placed by the cycler branch (M8-013)
        mat = material_of(spec['material_key'])
        actor = spawn_quad(actors, label, plane_mesh, mat, spec['center'], spec['size_cm'], spec['rot'])
        counts['tile' if 'Tile' in label else 'object'] += 1
    return counts


def build():
    calib = json.loads(CALIB_PATH.read_text(encoding='utf-8'))
    place = json.loads(PLACEMENT_PATH.read_text(encoding='utf-8'))
    s = calib['scale']['cm_per_px']
    REPORT['cm_per_px'] = s

    plane_mesh = load('/Engine/BasicShapes/Plane')
    cube_mesh = load('/Engine/BasicShapes/Cube')

    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    if ASSETS.does_asset_exist(MAP):
        if not levels.load_level(MAP):
            raise RuntimeError('cannot load existing ' + MAP)
        for a in list(actors.get_all_level_actors()):
            if a.get_actor_label().startswith(LABEL_PREFIX):
                actors.destroy_actor(a)
    else:
        if not levels.new_level(MAP):
            raise RuntimeError('cannot create ' + MAP)

    # --- resolve variant tile top + hypothesis-A walkable band
    inst0 = place['tiles']['instances'][0]
    y_top_px = inst0['y_top_px'] if VARIANT == 'P' else inst0['y_top_screen_ydown_px']
    hyp_a = next((h for h in calib['walkability_hypotheses'] if h.get('id') == 'A'), None)
    if hyp_a is None:
        raise RuntimeError('walkability hypothesis A missing from calibration json')
    candidates = [hyp_a['primary_band']] + hyp_a.get('variants', [])
    band = next((c for c in candidates
                 if c.get('tile_placement_variant') == VARIANT and 'walkable' in c), None)
    if band is None:
        raise RuntimeError('no hypothesis-A band for tile variant %s' % VARIANT)
    y_walk = band['walkable']['y_cm']
    REPORT['walkable_band_cm'] = y_walk

    materials = {}

    def material_of(name):
        return materials[name]

    # --- ground tiles (horizontal quads on the ground plane, no v-flip);
    # import/material built ONCE per unique tile hash - rebuilding the same
    # asset name per instance would delete the material already assigned to
    # earlier instances (they would fall back to the default gray material).
    tile_done = set()
    for inst in place['tiles']['instances']:
        name = 'tile_' + inst['tile_hash64']
        if name in tile_done:
            continue
        tile_done.add(name)
        tex = import_texture('T_' + name, inst['image_file'], [224, 600])
        hist = alpha_histogram(ASSETS_DIR / inst['image_file'])
        blend = unreal.BlendMode.BLEND_MASKED if hist['soft_share'] < 0.005 else unreal.BlendMode.BLEND_TRANSLUCENT
        materials[name] = build_material('M_' + name, tex, blend, v_flip=False)
        REPORT['textures'].append({'name': 'T_' + name, 'soft_share': round(hist['soft_share'], 6), 'blend': str(blend)})

    # --- object quads (vertical, roll=90, 1-v flip; bottom on ground Z=0 at
    # the foot line depth). Fog objects (M8-013) take the baked two-frame
    # path instead: flip+tint are pre-baked into their PNGs, both frames are
    # placed as mutually-exclusive quads, and an ADNFMPropFrameCycler toggles
    # visibility on the source delays.
    fog_timing = json.loads((DATA / 'bamboo_fog' / 'fog_timing.json').read_text(encoding='utf-8'))
    fog_by_index = {entry['index']: entry for entry in fog_timing['objects']}
    fog_cycles = []
    for obj in place['objects']:
        ff = obj['first_frame']
        w_px, h_px = ff['png_size_px']
        obj_index = obj.get('index', 0)
        fog = fog_by_index.get(obj_index)
        if fog is not None and obj['draw_layer'] == fog['draw_layer']:
            fog_entries = []
            for frame in fog['frames']:
                name = 'fog_%02d_f%d' % (obj_index, frame['frame'])
                baked_png = DATA / 'bamboo_fog' / frame['baked_png']
                tex = import_texture('T_' + name, str(baked_png),
                                     list(bamboo_png_size(baked_png)))
                hist = alpha_histogram(baked_png)
                blend = unreal.BlendMode.BLEND_MASKED if hist['soft_share'] < 0.005 else unreal.BlendMode.BLEND_TRANSLUCENT
                materials[name] = build_material('M_' + name, tex, blend, v_flip=True)
                REPORT['textures'].append({'name': 'T_' + name, 'soft_share': round(hist['soft_share'], 6), 'blend': str(blend)})
                tlx, _tly = ff['top_left_px']
                foot_y = ff['foot_point_px']['y']
                quad = spawn_quad(actors, LABEL_PREFIX + 'Fog%02d_F%d' % (obj_index, frame['frame']),
                                  plane_mesh, materials[name],
                                  ((tlx + w_px / 2.0) * s, foot_y * s, (h_px * s) / 2.0),
                                  (w_px * s, h_px * s), {'pitch': 0.0, 'yaw': 0.0, 'roll': 90.0})
                fog_entries.append((quad, frame['delay_ms']))
            cycler_cls = unreal.load_class(None, '/Script/UEMMO.DNFMPropFrameCycler')
            if cycler_cls is None:
                raise RuntimeError('DNFMPropFrameCycler class not found in module UEMMO')
            cycler = actors.spawn_actor_from_class(cycler_cls,
                unreal.Vector(x=(ff['top_left_px'][0] + w_px / 2.0) * s,
                              y=ff['foot_point_px']['y'] * s, z=(h_px * s) / 2.0),
                unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
            cycler.set_actor_label(LABEL_PREFIX + 'FogCycler%02d' % obj_index)
            cycler.set_editor_property('frame_a_actor', fog_entries[0][0])
            cycler.set_editor_property('frame_b_actor', fog_entries[1][0])
            cycler.set_editor_property('frame_a_delay_seconds', fog_entries[0][1] / 1000.0)
            cycler.set_editor_property('frame_b_delay_seconds', fog_entries[1][1] / 1000.0)
            cycler.set_folder_path('DNFM_Bamboo')
            fog_cycles.append({'index': obj_index,
                               'delays_seconds': [fog_entries[0][1] / 1000.0, fog_entries[1][1] / 1000.0]})
            continue
        base = Path(ff['image_file']).stem
        name = 'L%d_%02d_%s' % (obj['draw_layer'], obj_index, base.split('_')[-1])
        tex = import_texture('T_' + name, ff['image_file'], [int(w_px), int(h_px)])
        hist = alpha_histogram(ASSETS_DIR / ff['image_file'])
        blend = unreal.BlendMode.BLEND_MASKED if hist['soft_share'] < 0.005 else unreal.BlendMode.BLEND_TRANSLUCENT
        materials[name] = build_material('M_' + name, tex, blend, v_flip=True)
        REPORT['textures'].append({'name': 'T_' + name, 'soft_share': round(hist['soft_share'], 6), 'blend': str(blend)})

    expected = expected_transforms(calib, place)
    counts = spawn_expected(actors, expected, plane_mesh, material_of)
    counts['fog_quad'] = 2 * len(FOG_INDICES)
    counts['fog_cycler'] = len(fog_cycles)

    # --- hidden collision slab (top face exactly at Z=0): keeps the pawn on
    # the ground.
    slab = actors.spawn_actor_from_class(unreal.StaticMeshActor,
        unreal.Vector(x=789.15, y=530.0, z=-50.0), unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    slab.set_actor_label(LABEL_PREFIX + 'FloorSlab')
    slab.static_mesh_component.set_static_mesh(cube_mesh)
    slab.set_actor_scale3d(unreal.Vector(x=22.0, y=18.0, z=1.0))
    slab.set_actor_hidden_in_game(True)
    slab.set_folder_path('DNFM_Bamboo')
    REPORT['floor_slab'] = {'center': [789.15, 530.0, -50.0], 'scale': [22.0, 18.0, 1.0],
                            'note': 'cube 100cm; top face Z=0; hidden in game'}

    # --- M8-012 walk-band walls (hypothesis A, this tile variant): the
    # blocked strip and map borders become solid; the in-engine walk test
    # then verifies the band rather than asserting an open void.
    x_min, x_max = band['walkable']['x_cm']
    y_far, y_near = y_walk[0], y_walk[1]
    wall_specs = [
        ('WallFar', (789.15, y_far - 10.0, 200.0), (17.0, 0.2, 4.0)),
        ('WallNear', (789.15, y_near + 10.0, 200.0), (17.0, 0.2, 4.0)),
        ('WallLeft', (x_min - 10.0, 670.0, 200.0), (0.2, 12.0, 4.0)),
        ('WallRight', (x_max + 10.0, 670.0, 200.0), (0.2, 12.0, 4.0)),
    ]
    for wname, center, scale in wall_specs:
        wall = actors.spawn_actor_from_class(unreal.StaticMeshActor,
            unreal.Vector(x=center[0], y=center[1], z=center[2]),
            unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
        wall.set_actor_label(LABEL_PREFIX + wname)
        wall.static_mesh_component.set_static_mesh(cube_mesh)
        wall.set_actor_scale3d(unreal.Vector(x=scale[0], y=scale[1], z=scale[2]))
        wall.set_actor_hidden_in_game(True)
        wall.set_folder_path('DNFM_Bamboo')
    REPORT['walk_walls'] = [{'name': n, 'center': c, 'scale': sc} for n, c, sc in wall_specs]
    REPORT['walk_band_note'] = 'hypothesis A (value 0 walkable); variant %s' % VARIANT

    # --- M8-014 exit marker at the east edge gap (no door art exists in the
    # source set - M8-005 gaps; a translucent indicator column marks the exit
    # as a PC adaptation, never presented as original art).
    marker = actors.spawn_actor_from_class(unreal.StaticMeshActor,
        unreal.Vector(x=1560.0, y=(y_walk[0] + y_walk[1]) / 2.0, z=150.0),
        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    marker.set_actor_label(LABEL_PREFIX + 'ExitMarker')
    marker.static_mesh_component.set_static_mesh(cube_mesh)
    marker.set_actor_scale3d(unreal.Vector(x=0.4, y=6.0, z=3.0))
    marker_mat_path = DEST + '/M/M_ExitMarker'
    if ASSETS.does_asset_exist(marker_mat_path):
        ASSETS.delete_asset(marker_mat_path)
    marker_mat = TOOLS.create_asset('M_ExitMarker', DEST + '/M', unreal.Material, unreal.MaterialFactoryNew())
    marker_mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
    marker_mat.set_editor_property('two_sided', True)
    marker_mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    m_opacity = node(marker_mat, unreal.MaterialExpressionConstant, r=0.35)
    output(m_opacity, '', unreal.MaterialProperty.MP_OPACITY)
    m_color = node(marker_mat, unreal.MaterialExpressionConstant3Vector,
                   constant=unreal.LinearColor(r=0.2, g=1.0, b=0.35, a=1.0))
    output(m_color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(marker_mat)
    ASSETS.save_loaded_asset(marker_mat)
    marker.static_mesh_component.set_material(0, marker_mat)
    marker.set_folder_path('DNFM_Bamboo')
    REPORT['exit_marker'] = {'center': [1560.0, (y_walk[0] + y_walk[1]) / 2.0, 150.0],
                             'note': 'adaptation indicator (no source door art); walkable-to at the east gap'}

    # --- PlayerStart at hypothesis-A band center for this variant
    ps = actors.spawn_actor_from_class(unreal.PlayerStart,
        unreal.Vector(x=789.15, y=(y_walk[0] + y_walk[1]) / 2.0, z=100.0),
        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    ps.set_actor_label(LABEL_PREFIX + 'PlayerStart')
    REPORT['player_start'] = {'x_cm': 789.15, 'y_cm': (y_walk[0] + y_walk[1]) / 2.0,
                              'band_variant': VARIANT, 'hypothesis': 'A'}

    # --- lights (guild-map pattern; unlit map content ignores them, lit
    # fallback/3D content needs them)
    sun = actors.spawn_actor_from_class(unreal.DirectionalLight,
        unreal.Vector(x=0.0, y=0.0, z=3000.0), unreal.Rotator(pitch=-55.0, yaw=30.0, roll=0.0))
    sun.set_actor_label(LABEL_PREFIX + 'Sun')
    sky = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(x=0.0, y=0.0, z=2000.0))
    sky.set_actor_label(LABEL_PREFIX + 'Sky')
    sky.light_component.set_editor_property('intensity', 1.0)

    levels.save_current_level()
    counts['materials'] = len(materials)
    REPORT['actors'] = counts
    REPORT['map'] = MAP


def verify():
    calib = json.loads(CALIB_PATH.read_text(encoding='utf-8'))
    place = json.loads(PLACEMENT_PATH.read_text(encoding='utf-8'))
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level(MAP):
        raise RuntimeError('cannot load ' + MAP)
    expected = expected_transforms(calib, place)
    found = 0
    mism = []
    for a in actors.get_all_level_actors():
        label = a.get_actor_label()
        if label in expected:
            found += 1
            loc = a.get_actor_location()
            scl = a.get_actor_scale3d()
            spec = expected[label]
            delta = max(abs(loc.x - spec['center'][0]), abs(loc.y - spec['center'][1]),
                        abs(loc.z - spec['center'][2]))
            sdelta = max(abs(scl.x - spec['scale'][0]), abs(scl.y - spec['scale'][1]))
            if delta > 0.05 or sdelta > 0.001:
                mism.append({'label': label, 'delta_cm': delta, 'delta_scale': sdelta})
    if found != len(expected):
        raise RuntimeError('verify: found %d of %d expected quads' % (found, len(expected)))
    if mism:
        raise RuntimeError('verify: transform mismatches: %s' % mism[:5])
    cyclers = [a for a in actors.get_all_level_actors()
               if a.get_actor_label().startswith(LABEL_PREFIX + 'FogCycler')]
    if len(cyclers) != len(FOG_INDICES):
        raise RuntimeError('verify: expected %d fog cyclers, found %d' % (len(FOG_INDICES), len(cyclers)))
    for cycler in cyclers:
        if cycler.get_editor_property('frame_a_actor') is None or cycler.get_editor_property('frame_b_actor') is None:
            raise RuntimeError('verify: cycler %s missing frame references' % cycler.get_actor_label())
    REPORT['actors'] = {'verified_quads': found, 'expected': len(expected),
                        'fog_cyclers': len(cyclers)}


def capture_map():
    """MODE=capture: load the saved map and export SceneCapture2D shots from
    game-like fixed-camera poses (guild-map tick-settle mechanism)."""
    import math
    calib = json.loads(CALIB_PATH.read_text(encoding='utf-8'))
    place = json.loads(PLACEMENT_PATH.read_text(encoding='utf-8'))
    s = calib['scale']['cm_per_px']
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level(MAP):
        raise RuntimeError('cannot load ' + MAP)
    world = unreal.EditorLevelLibrary.get_editor_world()
    dest_dir = ART / 'capture'
    dest_dir.mkdir(parents=True, exist_ok=True)

    # band center from placement (tiles) as the mid-view target
    band_mid_y = None
    hyp_a = next(h for h in calib['walkability_hypotheses'] if h.get('id') == 'A')
    candidates = [hyp_a['primary_band']] + hyp_a.get('variants', [])
    band = next(c for c in candidates
                if c.get('tile_placement_variant') == VARIANT and 'walkable' in c)
    y_walk = band['walkable']['y_cm']
    band_mid_y = (y_walk[0] + y_walk[1]) / 2.0
    mid_x = 789.15
    pitch = -18.0
    pr = math.radians(-pitch)

    def pose(target_x, target_y, target_z, dist, fov):
        loc = unreal.Vector(x=target_x, y=target_y + dist * math.cos(pr), z=target_z + dist * math.sin(pr))
        rot = unreal.Rotator(pitch=pitch, yaw=-90.0, roll=0.0)
        return loc, rot, fov

    shots = [
        ('game-mid', pose(mid_x, band_mid_y, 100.0, 2200.0, 60.0)),
        ('game-entry', pose(951.0, band_mid_y - 300.0, 100.0, 2200.0, 60.0)),
        ('game-wide', pose(mid_x, band_mid_y, 100.0, 4600.0, 75.0)),
    ]
    # M8-015: append the comparison poses from the offline reference manifest
    compare = DATA / 'bamboo_compare' / 'compare_manifest.json'
    if compare.is_file():
        manifest = json.loads(compare.read_text(encoding='utf-8'))
        points = manifest.get('points', manifest if isinstance(manifest, list) else [])
        for point in points:
            near = point.get('poses', {}).get('near')
            if not near:
                continue
            loc = near.get('location_cm')
            rot = near.get('rotation')
            if not (loc and rot):
                continue
            shots.append(('compare-%s' % point.get('id', point.get('name', 'point')),
                          (unreal.Vector(x=loc['x'], y=loc['y'], z=loc['z']),
                           unreal.Rotator(pitch=rot['pitch'], yaw=rot['yaw'], roll=rot['roll']),
                           near.get('fov', 60.0))))
        REPORT['compare_points'] = len(points)

    ca = actors.spawn_actor_from_class(unreal.SceneCapture2D, shots[0][1][0], shots[0][1][1])
    ca.set_actor_label(LABEL_PREFIX + 'CaptureProbe')
    cap = ca.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(world, 1920, 1080,
        unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.06, 0.07, 0.1, 1.0))
    cap.set_editor_property('texture_target', rt)
    cap.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    cap.set_editor_property('capture_every_frame', True)
    cap.set_editor_property('fov_angle', shots[0][1][2])

    state = {'frame': 0, 'shot': 0}
    REPORT['previews'] = []

    def tick(_dt):
        try:
            state['frame'] += 1
            if state['frame'] < 180:
                return
            frame = (state['frame'] - 180) % 90
            if frame == 0 and state['shot'] < len(shots):
                name, (loc, rot, fov) = shots[state['shot']]
                ca.set_actor_location(loc, False, False)
                ca.set_actor_rotation(rot, False)
                cap.set_editor_property('fov_angle', fov)
                cap.capture_scene()
            if frame == 60 and state['shot'] < len(shots):
                name = shots[state['shot']][0]
                unreal.RenderingLibrary.export_render_target(world, rt, str(dest_dir), name + '.png')
                out = dest_dir / (name + '.png')
                if not (out.is_file() and out.stat().st_size > 1024):
                    raise RuntimeError('capture missing: %s' % out)
                REPORT['previews'].append({'name': name, 'file': str(out), 'bytes': out.stat().st_size})
                state['shot'] += 1
                if state['shot'] == len(shots):
                    REPORT['success'] = True
                    save_capture_report()
                    cap.set_editor_property('capture_every_frame', False)
                    cap.set_editor_property('texture_target', None)
                    actors.destroy_actor(ca)
                    unreal.RenderingLibrary.release_render_target2d(rt)
                    unreal.unregister_slate_post_tick_callback(handle)
                    unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        except Exception:
            REPORT['errors'].append(traceback.format_exc())
            save_capture_report()
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    handle = unreal.register_slate_post_tick_callback(tick)
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    globals()['_capture_keepalive'] = (ca, cap, rt, handle)


def save_capture_report():
    (ART / 'ue-capture.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')


try:
    if MODE == 'import':
        build()
        REPORT['success'] = True
    elif MODE == 'verify':
        verify()
        REPORT['success'] = True
    elif MODE == 'capture':
        # Report ownership moves to the tick callback; only write here on
        # setup failure.
        capture_map()
        if not REPORT['success'] and not REPORT['errors']:
            raise RuntimeError('capture did not finish (keepalive tick did not run to completion)')
    else:
        raise ValueError(MODE)
except Exception:
    REPORT['errors'].append(traceback.format_exc())
    if MODE != 'capture':
        raise
finally:
    if MODE != 'capture' or REPORT['success'] or REPORT['errors']:
        ART.mkdir(parents=True, exist_ok=True)
        (ART / ('ue-%s.json' % MODE)).write_text(
            json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
        unreal.log('UEMMO M8-011 %s done: report %s' % (MODE, ART / ('ue-%s.json' % MODE)))
