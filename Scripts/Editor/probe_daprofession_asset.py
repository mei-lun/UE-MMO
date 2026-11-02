# -*- coding: utf-8 -*-
"""Probe: dump the DA_ProfessionConfig asset's embedded skill rows (the
packaged publish source) to settle where the runtime AttackPending=true came
from. Writes Artifacts/daprofession-rows.json."""
import json
import os

import unreal

asset = unreal.load_asset("/Game/UEMMO/Professions/DA_ProfessionConfig")
rows = []
if asset is None:
    unreal.log_error("PROBE: DA_ProfessionConfig failed to load")
else:
    for row in asset.get_editor_property("skills"):
        rows.append({
            "skill_id": str(row.skill_id),
            "profession_id": str(row.profession_id),
            "attack_id": str(row.attack_id),
            "attack_pending": bool(row.attack_pending),
            "placeholder": bool(row.placeholder),
        })

report = {"asset_loaded": asset is not None, "skill_rows": rows}
out = os.path.join(unreal.Paths.project_dir(), "Artifacts", "daprofession-rows.json")
with open(out, "w", encoding="utf-8") as handle:
    json.dump(report, handle, indent=1)
unreal.log_warning("PROBE: wrote %d skill rows" % len(rows))
