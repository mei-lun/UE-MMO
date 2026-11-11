"""M8-041: give the volcano walk floor a proper dark material.

The M8-040 VolcanoWalkFloor kept the engine plane's WorldGridMaterial (the
grey checker the user flagged as "map content wrong"). Build a plain dark
volcanic-rock material (no texture dependency) and assign it. Idempotent.
"""
import unreal

MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
MAT_PATH = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoFloorBase'
FLOOR_LABEL = 'VolcanoWalkFloor'

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MEL = unreal.MaterialEditingLibrary

if not ASSETS.does_asset_exist(MAT_PATH):
    mat = ASSETS.duplicate_asset('/Engine/BasicShapes/BasicShapeMaterial', MAT_PATH)
    if mat is None:
        raise RuntimeError('material dup failed')
    # Rebuild the graph as a flat dark volcanic colour (no texture sample).
    for expr in list(MEL.get_material_expressions(mat)):
        MEL.delete_material_expression(mat, expr)
    color = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, -200)
    color.set_editor_property('constant', unreal.LinearColor(0.16, 0.11, 0.10, 1.0))
    MEL.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -400, 0)
    rough.set_editor_property('r', 0.95)
    MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    ASSETS.save_loaded_asset(mat)
    print('UEMMO_VOLCANO_FLOOR: material created')
else:
    mat = unreal.load_asset(MAT_PATH)
    print('UEMMO_VOLCANO_FLOOR: material exists')

if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)
assigned = 0
for a in ACTORS.get_all_level_actors():
    if a.get_actor_label() != FLOOR_LABEL:
        continue
    smc = a.get_editor_property('static_mesh_component')
    if smc is not None:
        smc.set_material(0, mat)
        assigned += 1
if assigned != 1:
    raise RuntimeError('expected exactly one walk floor, assigned %d' % assigned)
if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
print('UEMMO_VOLCANO_FLOOR: assigned and saved')
