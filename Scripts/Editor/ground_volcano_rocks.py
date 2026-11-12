"""M8-043: ground the 44 volcano OBJ planes, thicken the floors, restyle the exit visual.

Run via UnrealEditor-Cmd -run=pythonscript. Saves L_DNFM_Volcano01.
Steps (per Docs/Audit-2026-11-12.md M8-043):
  1. Every VOL_OBJ_* plane: shift so its bounds bottom edge sits at z=-30.
  2. VOL_OBJ_10..13 (CONTACT, inside the walk band): NoCollision on their mesh.
  3. VolcanoWalkFloor / VolcanoDress_GroundMid / GroundFront: Plane -> 20cm Cube,
     top face kept at the current height.
  4. VolcanoRoomExit_01: ExitVisual material -> M_ExitPortal (creates it if missing;
     keeps the runtime "Color" tint contract of ARoomExit).
"""
import sys

import unreal

MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
GROUND_TARGET_Z_MIN = -30.0   # planes settle 30cm into the ground (no gap flicker)
CUBE_PATH = '/Engine/BasicShapes/Cube.Cube'
FLOOR_THICKNESS_CM = 20.0
EXIT_MAT_PATH = '/Game/UEMMO/Rooms/M_ExitPortal'
CONTACT_IDS = ('VOL_OBJ_10_', 'VOL_OBJ_11_', 'VOL_OBJ_12_', 'VOL_OBJ_13_')

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def move_bottom_to(actor, target_z_min):
    origin, extent = actor.get_actor_bounds(False)
    z_min = origin.z - extent.z
    delta = target_z_min - z_min
    loc = actor.get_actor_location()
    actor.set_actor_location(unreal.Vector(loc.x, loc.y, loc.z + delta),
                             sweep=False, teleport=True)
    return delta


def make_exit_material():
    mat = unreal.load_asset(EXIT_MAT_PATH)
    if mat is not None:
        return mat, False
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    package = '/Game/UEMMO/Rooms/M_ExitPortal'
    mat = asset_tools.create_asset('M_ExitPortal', '/Game/UEMMO/Rooms',
                                   unreal.Material, factory)
    if mat is None:
        raise RuntimeError('cannot create M_ExitPortal')
    lib = unreal.MaterialEditingLibrary

    dark = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -600, 0)
    dark.set_editor_property('constant', unreal.LinearColor(0.05, 0.045, 0.045, 1.0))
    lib.connect_material_property(dark, 'BaseColor', unreal.MaterialProperty.MP_BASE_COLOR)

    # Color vector param: ARoomExit.ApplyExitState writes red/green here at runtime.
    # NOTE: pulse uses the Time->Frac pattern — Time->Sine graphs cook-fail in this
    # project (M8-030); the fix path is Scripts/Editor/fix_exit_material.py.
    tint = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, 200)
    tint.set_editor_property('parameter_name', 'Color')
    tint.set_editor_property('default_value', unreal.LinearColor(1.0, 0.35, 0.1, 1.0))

    time_e = lib.create_material_expression(mat, unreal.MaterialExpressionTime, -850, 420)
    speed = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -700, 520)
    speed.set_editor_property('r', 0.5)
    mul0 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -560, 420)
    lib.connect_material_expressions(time_e, '', mul0, 'A')
    lib.connect_material_expressions(speed, '', mul0, 'B')
    frac = lib.create_material_expression(mat, unreal.MaterialExpressionFrac, -420, 420)
    lib.connect_material_expressions(mul0, '', frac, '')
    lo = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -420, 560)
    lo.set_editor_property('constant', unreal.LinearColor(0.22, 0.03, 0.015, 1.0))
    hi = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -420, 260)
    hi.set_editor_property('constant', unreal.LinearColor(1.0, 0.5, 0.12, 1.0))
    lerp = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -260, 420)
    lib.connect_material_expressions(frac, '', lerp, 'Alpha')
    lib.connect_material_expressions(lo, '', lerp, 'A')
    lib.connect_material_expressions(hi, '', lerp, 'B')
    mul = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -100, 340)
    lib.connect_material_expressions(lerp, '', mul, 'A')
    lib.connect_material_expressions(tint, '', mul, 'B')
    lib.connect_material_property(mul, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    metal = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 700)
    metal.set_editor_property('r', 0.1)
    lib.connect_material_property(metal, '', unreal.MaterialProperty.MP_METALLIC)
    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 800)
    rough.set_editor_property('r', 0.65)
    lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat, True


def main():
    if not LEVELS.load_level(MAP):
        raise RuntimeError('cannot load ' + MAP)
    actors = ACTORS.get_all_level_actors()

    moved = []
    for a in actors:
        label = a.get_actor_label()
        if label.startswith('VOL_OBJ_'):
            delta = move_bottom_to(a, GROUND_TARGET_Z_MIN)
            if label.startswith(CONTACT_IDS):
                for c in a.get_components_by_class(unreal.StaticMeshComponent):
                    c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
            moved.append((label, round(delta, 1)))

    cube = unreal.load_asset(CUBE_PATH)
    thickened = []
    for a in actors:
        label = a.get_actor_label()
        if label not in ('VolcanoWalkFloor', 'VolcanoDress_GroundMid', 'VolcanoDress_GroundFront'):
            continue
        comp = a.get_components_by_class(unreal.StaticMeshComponent)[0]
        origin, extent = a.get_actor_bounds(False)
        top_z = origin.z + extent.z
        old_scale = a.get_actor_scale3d()
        new_scale = unreal.Vector(old_scale.x, old_scale.y, FLOOR_THICKNESS_CM / 100.0)
        loc = a.get_actor_location()
        comp.set_static_mesh(cube)
        a.set_actor_scale3d(new_scale)
        a.set_actor_location(unreal.Vector(loc.x, loc.y, top_z - FLOOR_THICKNESS_CM / 2.0),
                             sweep=False, teleport=True)
        thickened.append((label, round(top_z, 1)))

    mat, created = make_exit_material()
    exits = [a for a in ACTORS.get_all_level_actors()
             if a.get_class().get_name() == 'RoomExit']
    exit_done = []
    for a in exits:
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, mat)
        exit_done.append(a.get_actor_label())

    if not LEVELS.save_current_level():
        raise RuntimeError('save failed')
    print('UEMMO_VOL_GROUND: moved=%d %s' % (len(moved), moved[:6]))
    print('UEMMO_VOL_GROUND: contact_nocollision=4 thickened=%s exit_mat=%s(created=%s) exits=%s'
          % (thickened, EXIT_MAT_PATH, created, exit_done))
    sys.stdout.flush()


main()
