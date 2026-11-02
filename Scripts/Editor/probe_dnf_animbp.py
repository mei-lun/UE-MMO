# -*- coding: utf-8 -*-
"""Fresh-process verification of ABP_DNFHero + BS_DNFHero_Locomotion (the
M3-028 lesson: the BlendSpace must carry built runtime data or it samples
nothing at runtime). Writes Artifacts/dnf-animbp-verify.json."""
import json
import os

import unreal

HERO_DIR = "/Game/UEMMO/ArtValidation/DNFArt/Hero"
REPORT_PATH = os.path.join(
    unreal.Paths.project_dir(), "Artifacts", "dnf-animbp-verify.json")

bs = unreal.load_asset(HERO_DIR + "/BS_DNFHero_Locomotion")
bp = unreal.load_asset(HERO_DIR + "/ABP_DNFHero")

checks = {}
checks["blendspace loads"] = isinstance(bs, unreal.BlendSpace)
checks["animbp loads"] = isinstance(bp, unreal.AnimBlueprint)
if isinstance(bs, unreal.BlendSpace):
    samples = bs.get_editor_property("sample_data")
    checks["two samples (idle, run)"] = len(samples) == 2
    # The axis ranges are fixed by the native builder (Speed max 420); a
    # sample outside them loads invalid (the "越界" LoadError) - read back
    # the run sample's stored value to prove the range took effect.
    if len(samples) == 2:
        run_value = samples[1].get_editor_property("sample_value")
        checks["run sample stored at speed 420"] = abs(run_value.x - 420.0) < 0.01

census = {}
if isinstance(bp, unreal.AnimBlueprint):
    for key, class_name in (("blend_space_players", "AnimGraphNode_BlendSpacePlayer"),
                            ("slot_nodes", "AnimGraphNode_Slot")):
        node_class = getattr(unreal, class_name, None)
        census[key] = len(bp.get_nodes_of_class(node_class)) if node_class else -1
    compiled = unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    checks["recompile succeeds"] = bool(compiled)
checks.update(census)

report = {"success": all(bool(v) for v in checks.values()), "checks": checks}
with open(REPORT_PATH, "w", encoding="utf-8") as handle:
    json.dump(report, handle, indent=1)
unreal.log_warning("DNFANIMBP VERIFY: %s" % json.dumps(checks))
