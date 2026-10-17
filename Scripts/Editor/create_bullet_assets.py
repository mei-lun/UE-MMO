"""M5-061: save bullet material and current combat definitions; UE editor only."""
import json
from pathlib import Path
import unreal

root = Path(unreal.Paths.project_dir())
verify = "-UEMMOBulletVerify" in unreal.SystemLibrary.get_command_line()
target = "/Game/UEMMO/Materials/M_Bullet"
assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
report = {"success": False, "verify": verify}
try:
    material = unreal.load_asset(target) if assets.does_asset_exist(target) else None
    if not verify:
        if material is None:
            material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                "M_Bullet", "/Game/UEMMO/Materials", unreal.Material, unreal.MaterialFactoryNew())
        unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
        color = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
        color.set_editor_property("constant", unreal.LinearColor(4.0, 2.4, 0.4, 1.0))
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        unreal.MaterialEditingLibrary.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not assets.save_loaded_asset(material):
            raise RuntimeError("bullet material did not save")
        errors, created, updated, revision = unreal.CombatDataAssetBuilderLibrary.build_from_source_directory("Data/CombatSystem")
        if errors:
            raise RuntimeError(errors)
        report.update(created=int(created), updated=int(updated), config_revision=revision)
    if material is None or material.get_editor_property("shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        raise RuntimeError("saved bullet material missing or not unlit")
    for weapon_id in ("weapon_hitscan_sample", "weapon_burst_sample", "weapon_pellet_sample"):
        weapon = unreal.load_asset("/Game/UEMMO/Combat/Data/DA_Weapon_" + weapon_id)
        if weapon is None or int(weapon.get_editor_property("fire_mode")) != 2:
            raise RuntimeError("weapon was not saved as a projectile: " + weapon_id)
        if str(weapon.get_editor_property("projectile_id")) != "bullet_linear":
            raise RuntimeError("weapon bullet binding did not survive reload: " + weapon_id)
    report["success"] = True
except Exception as error:
    report["error"] = str(error)
    raise
finally:
    output = root / "Artifacts" / "Tasks" / "M5-061" / "assets"
    output.mkdir(parents=True, exist_ok=True)
    (output / ("reload.json" if verify else "generate.json")).write_text(json.dumps(report, indent=2), encoding="utf-8")
