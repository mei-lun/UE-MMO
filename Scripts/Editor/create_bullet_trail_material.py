"""Create/reload the M5-062 optical trail material inside the UE editor."""
import json
from pathlib import Path
import unreal

root = Path(unreal.Paths.project_dir())
verify = "-UEMMOTrailVerify" in unreal.SystemLibrary.get_command_line()
assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
path = "/Game/UEMMO/Materials/M_BulletTrail"
report = {"success": False, "verify": verify, "material": path}
try:
    material = unreal.load_asset(path) if assets.does_asset_exist(path) else None
    if not verify:
        if material is None:
            material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                "M_BulletTrail", "/Game/UEMMO/Materials", unreal.Material, unreal.MaterialFactoryNew())
        unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
        color = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
        color.set_editor_property("constant", unreal.LinearColor(10.0, 5.0, 1.0, 1.0))
        opacity = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionScalarParameter)
        opacity.set_editor_property("parameter_name", "TrailOpacity")
        opacity.set_editor_property("default_value", 0.85)
        unreal.MaterialEditingLibrary.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        unreal.MaterialEditingLibrary.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not assets.save_loaded_asset(material):
            raise RuntimeError("trail material save failed")
    if material is None:
        raise RuntimeError("trail material missing")
    if material.get_editor_property("shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        raise RuntimeError("trail material is not unlit")
    if material.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_ADDITIVE:
        raise RuntimeError("trail material is not additive")
    parameters = [str(p) for p in unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)]
    if "TrailOpacity" not in parameters:
        raise RuntimeError("fade parameter did not survive save")
    report["success"] = True
except Exception as error:
    report["error"] = str(error)
    raise
finally:
    out = root / "Artifacts" / "Tasks" / "M5-062" / "assets"
    out.mkdir(parents=True, exist_ok=True)
    (out / ("reload.json" if verify else "generate.json")).write_text(json.dumps(report, indent=2), encoding="utf-8")
