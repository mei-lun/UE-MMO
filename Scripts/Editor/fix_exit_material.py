"""M8-043 fix: rebuild M_ExitPortal from scratch with the proven Frac pulse pattern.

Time->Sine material graphs cook-fail in this project (recorded in M8-030: "首版
Sine 图 cook 失败后改用 Frac 变体"); the failed compile renders as the engine
default checker. The old asset is deleted and recreated cleanly, then the
volcano exit's material reference is re-established and the map saved.
"""
import unreal

MAT_PATH = '/Game/UEMMO/Rooms/M_ExitPortal'
MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
lib = unreal.MaterialEditingLibrary

if unreal.EditorAssetLibrary.does_asset_exist(MAT_PATH):
    if not unreal.EditorAssetLibrary.delete_asset(MAT_PATH):
        raise RuntimeError('cannot delete the broken M_ExitPortal')

tools = unreal.AssetToolsHelpers.get_asset_tools()
mat = tools.create_asset('M_ExitPortal', '/Game/UEMMO/Rooms', unreal.Material,
                         unreal.MaterialFactoryNew())
if mat is None:
    raise RuntimeError('cannot create M_ExitPortal')

# base color: dark volcanic stone
dark = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -600, -200)
dark.set_editor_property('constant', unreal.LinearColor(0.05, 0.045, 0.045, 1.0))
lib.connect_material_property(dark, '', unreal.MaterialProperty.MP_BASE_COLOR)

# emissive: Color param x (deep red .. bright orange pulse, Time*0.5 -> Frac -> Lerp)
tint = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, 260)
tint.set_editor_property('parameter_name', 'Color')
tint.set_editor_property('default_value', unreal.LinearColor(1.0, 0.35, 0.1, 1.0))

time_e = lib.create_material_expression(mat, unreal.MaterialExpressionTime, -700, 40)
speed = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -560, 140)
speed.set_editor_property('r', 0.5)
mul = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -420, 40)
lib.connect_material_expressions(time_e, '', mul, 'A')
lib.connect_material_expressions(speed, '', mul, 'B')
frac = lib.create_material_expression(mat, unreal.MaterialExpressionFrac, -280, 40)
lib.connect_material_expressions(mul, '', frac, '')

lo = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -280, 260)
lo.set_editor_property('constant', unreal.LinearColor(0.22, 0.03, 0.015, 1.0))
hi = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -280, -40)
hi.set_editor_property('constant', unreal.LinearColor(1.0, 0.5, 0.12, 1.0))
lerp = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -120, 60)
lib.connect_material_expressions(frac, '', lerp, 'Alpha')
lib.connect_material_expressions(lo, '', lerp, 'A')
lib.connect_material_expressions(hi, '', lerp, 'B')

pulse = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, 40, 160)
lib.connect_material_expressions(lerp, '', pulse, 'A')
lib.connect_material_expressions(tint, '', pulse, 'B')
lib.connect_material_property(pulse, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)

metal = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 420)
metal.set_editor_property('r', 0.1)
lib.connect_material_property(metal, '', unreal.MaterialProperty.MP_METALLIC)
rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 520)
rough.set_editor_property('r', 0.65)
lib.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)

lib.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)

# re-establish the exit reference in the map
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)
fixed = []
for a in ACTORS.get_all_level_actors():
    if a.get_class().get_name() == 'RoomExit':
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, mat)
        fixed.append(a.get_actor_label())
if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-043/fix-material-result.txt', 'w') as fh:
    fh.write('rebuilt=True exits_reassigned=%s\n' % fixed)
