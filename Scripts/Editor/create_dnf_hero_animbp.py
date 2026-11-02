# -*- coding: utf-8 -*-
"""Create ABP_DNFHero (the DNF fighter's AnimBP) from the ArtValidation clips.

The M7-visual integration drove hero_skm through AnimationSingleNode with
AnimationData mutated at runtime: a constructor-set single node never plays in
the packaged game, runtime AnimationData writes do not restart the node, and
montages cannot render through UAnimSingleNodeInstance at all - the player
froze on one pose (the 2026-11-03 playtest report). The mannequin AnimBP
cannot drive the DNF skeleton, so this script builds the fighter its own
AnimBP on hero_skm_Skeleton: BS_DNFHero_Locomotion (Speed x Direction,
idle + run_x samples) and ABP_DNFHero (UPrototypeAnimInstance parent) whose
AnimGraph is assembled by the native UPrototypeAnimInstance::
EditorBuildLocomotionGraph (blendspace + jump/fall/land + the DefaultSlot the
MNT_* montages route through), exactly like ABP_Prototype.

Modes via the UEMMODnfHeroAnimBPMode env var (generate default, verify).
Writes Artifacts/dnf-animbp-report.json with {"success": bool}.
"""
import json
import os

import unreal

HERO_DIR = "/Game/UEMMO/ArtValidation/DNFArt/Hero"
BS_PATH = HERO_DIR + "/BS_DNFHero_Locomotion"
ANIMBP_PATH = HERO_DIR + "/ABP_DNFHero"
SKELETON_PATH = HERO_DIR + "/hero_skm_Skeleton"
IDLE_PATH = HERO_DIR + "/hero_idle_Anim"
RUN_PATH = HERO_DIR + "/hero_run_x_Anim"
JUMP_PATH = HERO_DIR + "/hero_jump_start_Anim"
FALL_PATH = HERO_DIR + "/hero_jump_fall_Anim"
LAND_PATH = HERO_DIR + "/hero_jump_land_Anim"
REPORT_PATH = os.path.join(
    unreal.Paths.project_dir(), "Artifacts", "dnf-animbp-report.json")

MODE = os.environ.get("UEMMODnfHeroAnimBPMode", "generate").lower()


def load(path):
    return unreal.load_asset(path)


def load_required(path):
    obj = load(path)
    if obj is None:
        raise RuntimeError("missing asset: %s" % path)
    return obj


def write_report(payload):
    with open(REPORT_PATH, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1)


def graph_census(bp):
    # UEdGraph::Nodes is protected in UE 5.8 (create_locomotion_assets.py
    # precedent): count nodes through UBlueprint.get_nodes_of_class instead.
    census = {}
    for key, class_name in (("blend_space_players", "AnimGraphNode_BlendSpacePlayer"),
                            ("slot_nodes", "AnimGraphNode_Slot")):
        node_class = getattr(unreal, class_name, None)
        if node_class is not None and hasattr(bp, "get_nodes_of_class"):
            census[key] = len(bp.get_nodes_of_class(node_class))
    return census


def generate():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    assets = unreal.EditorAssetLibrary
    skeleton = load_required(SKELETON_PATH)

    created_bs = not assets.does_asset_exist(BS_PATH)
    if created_bs:
        factory = unreal.BlendSpaceFactoryNew()
        factory.set_editor_property("target_skeleton", skeleton)
        bs = tools.create_asset("BS_DNFHero_Locomotion", HERO_DIR, unreal.BlendSpace, factory)
    else:
        bs = load(BS_PATH)
    if bs is None:
        raise RuntimeError("BlendSpace asset could not be created or loaded.")

    samples = []
    for anim_path, speed, direction in ((IDLE_PATH, 0.0, 0.0), (RUN_PATH, 420.0, 0.0)):
        sample = unreal.BlendSample()
        sample.set_editor_property("animation", load_required(anim_path))
        sample.set_editor_property("sample_value", unreal.Vector(speed, direction, 0.0))
        samples.append(sample)
    bs.set_editor_property("sample_data", samples)
    assets.save_loaded_asset(bs, only_if_is_dirty=False)

    created_abp = not assets.does_asset_exist(ANIMBP_PATH)
    if created_abp:
        factory = unreal.AnimBlueprintFactory()
        factory.set_editor_property("target_skeleton", skeleton)
        native_parent = unreal.load_object(None, "/Script/UEMMO.PrototypeAnimInstance")
        if native_parent is None:
            raise RuntimeError("Native class UPrototypeAnimInstance not found; build the project first.")
        factory.set_editor_property("parent_class", native_parent)
        bp = tools.create_asset("ABP_DNFHero", HERO_DIR, unreal.AnimBlueprint, factory)
    else:
        bp = load(ANIMBP_PATH)
    if bp is None:
        raise RuntimeError("AnimBP asset could not be created or loaded.")

    built = unreal.PrototypeAnimInstance.editor_build_locomotion_graph(
        bp, bs, load_required(JUMP_PATH), load_required(FALL_PATH), load_required(LAND_PATH))
    compiled = unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    status = None
    try:
        status = str(bp.get_editor_property("status"))
    except Exception:
        status = "unreadable"
    # The builder fixes the BlendSpace axis ranges (Speed max 420) and
    # resamples its runtime data on the loaded object - save the BlendSpace
    # again AFTER the build or the samples sit outside the still-default
    # axis bounds and the asset loads invalid (create_locomotion_assets.py
    # carries the same post-build save for exactly this reason).
    assets.save_loaded_asset(bs, only_if_is_dirty=False)
    assets.save_loaded_asset(bp, only_if_is_dirty=False)

    report = {
        "mode": "generate",
        "created_blendspace": created_bs,
        "created_animbp": created_abp,
        "graph_build": bool(built),
        "compiled": bool(compiled),
        "status": status,
        "graph": graph_census(bp),
    }
    report["success"] = bool(built) and bool(compiled) and "BS_Error" not in (status or "")
    write_report(report)
    if not report["success"]:
        raise RuntimeError("DNF hero AnimBP build incomplete; inspect dnf-animbp-report.json")
    unreal.log_warning("UEMMO_DNF_ANIMBP_CREATE_SUCCESS")


def verify():
    assets = unreal.EditorAssetLibrary
    bs = load(BS_PATH)
    bp = load(ANIMBP_PATH)
    checks = {
        "blendspace loads as BlendSpace": isinstance(bs, unreal.BlendSpace),
        "animbp loads as AnimBlueprint": isinstance(bp, unreal.AnimBlueprint),
    }
    if isinstance(bs, unreal.BlendSpace):
        samples = bs.get_editor_property("sample_data")
        checks["blendspace has idle and run samples (2)"] = len(samples) == 2
        # The M3-028 lesson: ValidateSampleData alone ships an asset that
        # samples nothing at runtime - the built runtime data must be present.
        checks["blendspace runtime data resampled"] = (
            bs.get_editor_property("grid_samples").num() > 0)
    census = graph_census(bp) if isinstance(bp, unreal.AnimBlueprint) else {}
    checks["graph routes one blendspace player"] = census.get("blend_space_players", 0) == 1
    checks["graph routes the montage slot"] = census.get("slot_nodes", 0) == 1
    compiled = unreal.BlueprintEditorLibrary.compile_blueprint(bp) if isinstance(bp, unreal.AnimBlueprint) else False
    checks["recompile succeeds"] = bool(compiled)
    assets.save_loaded_asset(bp, only_if_is_dirty=False)

    report = {"mode": "verify", "checks": checks, "graph": census}
    report["success"] = all(bool(v) for v in checks.values())
    write_report(report)
    if not report["success"]:
        raise RuntimeError("DNF hero AnimBP verify failed; inspect dnf-animbp-report.json")
    unreal.log_warning("UEMMO_DNF_ANIMBP_VERIFY_SUCCESS")


if MODE == "verify":
    verify()
else:
    generate()
