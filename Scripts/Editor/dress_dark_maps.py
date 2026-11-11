"""M8-030: dress the volcano and the main menu against the black zones.

The camera's frame shows whatever the side view reaches: the far back (the
backdrop), the middle depth band between the old arena floor and the walk
band, and the ground in front of the walk band. Both maps are training-arena
duplicates whose checker floor and open sides read as "black edges / missing
content". This script lays ONE consistent volcanic-rock composition on both:

- a dark ground cover over the middle depth zone and the front strip (the
  checker floor disappears under it),
- a tall far backdrop wall filling the top of the frame,
- two dynamic lava-glow strips framing the walk band (the material animates
  itself through a Time->Sine emissive pulse - no tick driver needed).

Idempotent by label; verifies actor counts after a fresh load.
"""
import unreal

VOLCANO = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
MENU = '/Game/UEMMO/Maps/L_MainMenu'
MAT_ROCK = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoRockDark'
MAT_LAVA = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoLavaPulse'
BACK_Y = -580.0  # just in front of each map's FoundationBackdrop (y -620)

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MEL = unreal.MaterialEditingLibrary


def build_materials():
    if not ASSETS.does_asset_exist(MAT_ROCK):
        rock = ASSETS.duplicate_asset('/Engine/BasicShapes/BasicShapeMaterial', MAT_ROCK)
        for expr in list(MEL.get_material_expressions(rock)):
            MEL.delete_material_expression(rock, expr)
        color = MEL.create_material_expression(rock, unreal.MaterialExpressionConstant3Vector, -400, -200)
        color.set_editor_property('constant', unreal.LinearColor(0.075, 0.055, 0.055, 1.0))
        MEL.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
        rough = MEL.create_material_expression(rock, unreal.MaterialExpressionConstant, -400, 40)
        rough.set_editor_property('r', 1.0)
        MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
        rock.set_editor_property('two_sided', True)
        MEL.recompile_material(rock)
        ASSETS.save_loaded_asset(rock)
    if ASSETS.does_asset_exist(MAT_LAVA):
        ASSETS.delete_asset(MAT_LAVA)
    if True:
        lava = ASSETS.duplicate_asset('/Engine/BasicShapes/BasicShapeMaterial', MAT_LAVA)
        for expr in list(MEL.get_material_expressions(lava)):
            MEL.delete_material_expression(lava, expr)
        # The dynamic sweep: Time -> x0.5 -> Frac -> the glow lerp (a 2s
        # linear sweep; the Frac/Multiply nodes cook cleanly where the Sine
        # graph failed the SM5/SM6 cook).
        time_expr = MEL.create_material_expression(lava, unreal.MaterialExpressionTime, -700, 0)
        speed = MEL.create_material_expression(lava, unreal.MaterialExpressionConstant, -560, 120)
        speed.set_editor_property('r', 0.5)
        mul = MEL.create_material_expression(lava, unreal.MaterialExpressionMultiply, -560, 0)
        MEL.connect_material_expressions(time_expr, '', mul, 'A')
        MEL.connect_material_expressions(speed, '', mul, 'B')
        frac = MEL.create_material_expression(lava, unreal.MaterialExpressionFrac, -420, 0)
        MEL.connect_material_expressions(mul, '', frac, '')
        lo = MEL.create_material_expression(lava, unreal.MaterialExpressionConstant3Vector, -420, 240)
        lo.set_editor_property('constant', unreal.LinearColor(0.55, 0.12, 0.02, 1.0))
        hi = MEL.create_material_expression(lava, unreal.MaterialExpressionConstant3Vector, -420, -160)
        hi.set_editor_property('constant', unreal.LinearColor(1.0, 0.38, 0.06, 1.0))
        lerp = MEL.create_material_expression(lava, unreal.MaterialExpressionLinearInterpolate, -240, 40)
        MEL.connect_material_expressions(frac, '', lerp, 'Alpha')
        MEL.connect_material_expressions(lo, '', lerp, 'A')
        MEL.connect_material_expressions(hi, '', lerp, 'B')
        MEL.connect_material_property(lerp, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        base = MEL.create_material_expression(lava, unreal.MaterialExpressionConstant3Vector, -240, 320)
        base.set_editor_property('constant', unreal.LinearColor(0.2, 0.05, 0.02, 1.0))
        MEL.connect_material_property(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
        lava.set_editor_property('two_sided', True)
        MEL.recompile_material(lava)
        ASSETS.save_loaded_asset(lava)


def ensure_quad(label, loc, scale, material, rot_pitch=0.0):
    found = [a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label]
    if found:
        return found[0], False
    quad = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc, unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    if quad is None:
        raise RuntimeError('spawn failed: ' + label)
    quad.set_actor_label(label)
    smc = quad.get_editor_property('static_mesh_component')
    smc.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Plane'))
    quad.set_actor_scale3d(scale)
    if rot_pitch != 0.0:
        quad.set_actor_rotation(unreal.Rotator(pitch=rot_pitch, yaw=0.0, roll=0.0), False)
    smc.set_material(0, material)
    smc.set_collision_profile_name('NoCollision')
    smc.set_cast_shadow(False)
    return quad, True


def ensure_cube(label, loc, scale, material):
    found = [a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label]
    if found:
        return found[0], False
    cube = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc,
        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    if cube is None:
        raise RuntimeError('spawn failed: ' + label)
    cube.set_actor_label(label)
    smc = cube.get_editor_property('static_mesh_component')
    smc.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
    cube.set_actor_scale3d(scale)
    smc.set_material(0, material)
    smc.set_collision_profile_name('NoCollision')
    smc.set_cast_shadow(False)
    return cube, True


def dress(map_path, y0, y1, x_center, x_span):
    """y0/y1: the walk band's back/front edge (the playable depth)."""
    if not LEVELS.load_level(map_path):
        raise RuntimeError('cannot load ' + map_path)
    rock = unreal.load_asset(MAT_ROCK)
    lava = unreal.load_asset(MAT_LAVA)
    created = 0
    # 1. the middle-zone ground cover: the back wall line -> the band's back edge.
    mid_center_y = (BACK_Y + y0) / 2.0
    mid_depth = y0 - BACK_Y
    _, c = ensure_quad('VolcanoDress_GroundMid',
        unreal.Vector(x=x_center, y=mid_center_y, z=2.0),
        unreal.Vector(x=x_span / 100.0, y=mid_depth / 100.0, z=1.0), rock)
    created += c
    # 2. the front ground cover: the band's front edge -> 700cm further.
    _, c = ensure_quad('VolcanoDress_GroundFront',
        unreal.Vector(x=x_center, y=y1 + 350.0, z=2.0),
        unreal.Vector(x=x_span / 100.0, y=7.0, z=1.0), rock)
    created += c
    # 3. the far backdrop wall: a 400cm-thick cube slab (no rotation - the
    # pitched-plane variant rendered edge-on), 2000cm tall, filling the top
    # of the frame behind the play area.
    for stale in [a for a in ACTORS.get_all_level_actors()
                  if a.get_actor_label() == 'VolcanoDress_BackWall']:
        ACTORS.destroy_actor(stale)
    _, c = ensure_cube('VolcanoDress_BackWall',
        unreal.Vector(x=x_center, y=BACK_Y, z=700.0),
        unreal.Vector(x=x_span / 100.0, y=4.0, z=20.0), rock)
    created += c
    # 4. the dynamic lava strips framing the walk band.
    for i, strip_y in enumerate((y0 + 15.0, y1 - 15.0)):
        _, c = ensure_quad('VolcanoDress_LavaStrip%d' % i,
            unreal.Vector(x=x_center, y=strip_y, z=25.0),
            unreal.Vector(x=x_span / 100.0, y=1.2, z=1.0), lava)
        created += c
    # M8-030: the training-arena leftovers (the blue boundary walls and the
    # depth guide lines) are visual pollution on the dressed maps - remove
    # them (the high BoundsCollision walls keep the actual play bounds).
    removed = 0
    for a in list(ACTORS.get_all_level_actors()):
        lbl = a.get_actor_label()
        if lbl.startswith('Boundary_') or lbl.startswith('DepthGuide_'):
            ACTORS.destroy_actor(a)
            removed += 1
    if not LEVELS.save_current_level():
        raise RuntimeError('save failed for ' + map_path)
    LEVELS.load_level(map_path)
    dressed = sum(1 for a in ACTORS.get_all_level_actors()
                  if a.get_actor_label().startswith('VolcanoDress_'))
    print('UEMMO_M8_030: %s dressed (+%d, verified %d on reload)' % (map_path, created, dressed))
    if dressed < 5:
        raise RuntimeError('dressing did not persist for ' + map_path)


build_materials()
dress(VOLCANO, y0=900.0, y1=1300.0, x_center=604.0, x_span=3700.0)
dress(MENU, y0=-500.0, y1=520.0, x_center=0.0, x_span=2450.0)
print('UEMMO_M8_030: done')
