# -*- coding: utf-8 -*-
"""Fix the DNFArt material usage flags (the M7-visual package defect).

The M6-008 Interchange import left every DNFArt base material without the
bUsedWithSkeletalMesh usage flag, so in a packaged runtime the whole hero and
goblin meshes fall back to the default grey material (LogMaterial: "missing
usage flag SkeletalMesh! Default Material will be used in game"). Usage flags
live on BASE materials only - instances inherit them - so this script walks
every material under /Game/UEMMO/ArtValidation/DNFArt, resolves each instance
to its base, sets the flag on every base that lacks it, saves, then re-loads
and re-checks in a second pass.

Run inside the editor (Scripts/PrepareTrainingRoom.ps1 invocation pattern):
  UnrealEditor-Cmd.exe UEMMO.uproject -run=pythonscript -script=<this file>
Writes Artifacts/dnf-material-usage-report.json with {"success": bool}.
"""
import json
import os

import unreal

ROOT = "/Game/UEMMO/ArtValidation/DNFArt"
REPORT_PATH = os.path.join(
    unreal.Paths.project_dir(), "Artifacts", "dnf-material-usage-report.json")

USAGE_FLAG = "used_with_skeletal_mesh"


def list_dnf_art_assets():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    return registry.get_assets_by_path(ROOT, recursive=True)


def resolve_base_material(asset_data):
    """Return the base unreal.Material behind an asset (itself or parent chain)."""
    obj = unreal.load_asset(str(asset_data.package_name))
    if isinstance(obj, unreal.Material):
        return obj
    if isinstance(obj, unreal.MaterialInstanceConstant):
        parent = obj.get_editor_property("parent")
        hops = 0
        while parent is not None and isinstance(parent, unreal.MaterialInstanceConstant) and hops < 8:
            parent = parent.get_editor_property("parent")
            hops += 1
        if isinstance(parent, unreal.Material):
            return parent
    return None


def main():
    assets = list_dnf_art_assets()
    bases = {}
    instance_count = 0
    for asset in assets:
        base = resolve_base_material(asset)
        if base is None:
            continue
        base_path = base.get_path_name()
        bases[base_path] = base
        if isinstance(unreal.load_asset(str(asset.package_name)), unreal.MaterialInstanceConstant):
            instance_count += 1

    fixed = []
    already_ok = []
    for base_path in sorted(bases):
        material = bases[base_path]
        if not material.get_editor_property(USAGE_FLAG):
            material.set_editor_property(USAGE_FLAG, True)
            if not unreal.EditorAssetLibrary.save_loaded_asset(material):
                unreal.log_error("DNFMATFIX: failed to save %s" % base_path)
            fixed.append(base_path)
        else:
            already_ok.append(base_path)

    # Second pass verification: fresh loads must all carry the flag.
    verify_failures = []
    for base_path in sorted(bases):
        reloaded = unreal.load_asset(base_path)
        if not isinstance(reloaded, unreal.Material) or not reloaded.get_editor_property(USAGE_FLAG):
            verify_failures.append(base_path)

    report = {
        "success": len(verify_failures) == 0,
        "root": ROOT,
        "scanned_assets": len(assets),
        "material_instances": instance_count,
        "base_materials": len(bases),
        "fixed_bases": fixed,
        "already_ok_bases": already_ok,
        "verify_failures": verify_failures,
    }
    os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=1)
    unreal.log_warning("DNFMATFIX: scanned=%d instances=%d bases=%d fixed=%d verify_failures=%d"
                       % (len(assets), instance_count, len(bases), len(fixed), len(verify_failures)))


main()
