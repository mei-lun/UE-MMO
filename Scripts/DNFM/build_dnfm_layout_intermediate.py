#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M6-011: DNFM guild-scene layout audit -> UE placement intermediate data.

Reads the layout audit JSON exported from DNFM 130.5.8.0 prefab bundles and
produces per-prefab UE placement records plus a validation report.

Pure standard library; no numpy; deterministic output (sorted keys/lists,
no timestamps inside generated files). Re-runnable: outputs are overwritten.

Coordinate convention (derived, cited in validation.md):
  Unity numeric units are interpreted as meters and converted to UE
  centimeters; Unity (x,y,z) -> Blender (x,-z,y) at mesh build time, FBX
  export/import carries Blender-native Z-up axes into UE (see derivation in
  the emitted validation.md). Net point map: UE = (x, z, y) * 100 of Unity.
  Instance matrices are base-changed on the whole 4x4:
      M_UE = S4 * M_unity * S4^-1,  S4: UE_x=U_x, UE_y=U_z, UE_z=U_y, x100.

Run (Windows, `py` launcher; the `python` shim is blocked on this machine):
    py -I Scripts/DNFM/build_dnfm_layout_intermediate.py
"""

import argparse
import csv
import json
import math
import os
import sys

# ---------------------------------------------------------------------------
# Fixed task paths (overridable via CLI for reuse, defaults per M6-011 card).
# ---------------------------------------------------------------------------
DEFAULT_AUDIT_DIR = r"D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\reports\map_layout_audit"
DEFAULT_CATALOG = (
    r"D:\Github-Poj\UE-MMO\SourceAssets\Unpacked\DNFM130_5_8\handoff\asset_catalog.csv"
)
DEFAULT_OUT_DIR = (
    r"D:\Github-Poj\UE-MMO\Artifacts\Tasks\M6-011\2026-11-04T0215_0800\intermediate"
)

# bundle file stem -> target root name (None = every root in the bundle)
SOURCES = [
    ("00505_b9ae13b06f1f09.json", ["GuildagitGroundDummy"]),
    ("00160_0b4c26b37a5218.json", ["BG_Planet_Prepab"]),
    ("00502_5143dacdc67c8d.json", None),  # all 140 decoration prefab roots
]

# Documented convention references (quoted verbatim into validation.md).
CONVENTION_CITES = [
    "Docs/DNF-Research/05-DNFM静态资源UE适配.md:22  "
    "「原数值单位按 1 Unity 单位 = 1 米转换到 UE 厘米，Unity (x,y,z) → Blender (x,-z,y)，"
    "FBX 再负责引擎坐标约定转换」",
    "Scripts/blender_dnfm_static.py:11  def vec(v):return (v[0],-v[2],v[1])",
    "Scripts/blender_dnfm_static.py:13  scene.unit_settings.scale_length=1.0 (1 Blender unit = 1 m)",
    "Scripts/blender_dnfm_static.py:67  axis_forward='-Y',axis_up='Z',apply_scale_options='FBX_SCALE_NONE'",
    "Scripts/Editor/import_dnfm_static.py:128  imp.convert_scene=True;imp.convert_scene_unit=True",
    "Scripts/Editor/import_dnfm_static.py:141  'Unity numeric units interpreted as meters; FBX imported centimeters.'",
    "Scripts/Editor/import_dnfm_static.py:158-159  verify: UE bounds extent == unity extent * 100 (sorted per axis)",
    "Engine UE_5.8 Source/Editor/UnrealEd/Private/Fbx/FbxMainImport.cpp:1525-1560  "
    "UnrealImportAxis(Up=Z, Front=-Y, RightHanded); ConvertScene; unit convert to cm",
    "Engine UE_5.8 Source/Editor/UnrealEd/Private/Factories/FbxStaticMeshImportData.cpp:18  "
    "bTransformVertexToAbsolute = true (node global transform baked into static mesh vertices)",
    "Engine UE_5.8 Source/Editor/UnrealEd/Private/Fbx/FbxMainImport.cpp:2060  ComputeTotalMatrix applies GlobalTransform",
    "Engine UE_5.8 Source/Editor/UnrealEd/Private/Fbx/FbxUtilsImport.cpp:63-71  ConvertPos: (x, -y, z) RHS->LHS flip",
    "Engine UE_5.8 Source/Runtime/Core/Private/Math/UnrealMath.cpp:636-679  FQuat::Rotator() formulas (replicated below)",
]

# ---------------------------------------------------------------------------
# Small matrix helpers (row-major 4x4, column-vector convention, T*R*S,
# matching the audit's matrix_convention field).
# ---------------------------------------------------------------------------


def mat4_identity():
    return [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]


def mat4_mult(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat4_inverse(m):
    # General 4x4 inverse via Gauss-Jordan with partial pivoting.
    n = 4
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(n)] for i, row in enumerate(m)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            raise ValueError("singular 4x4 matrix")
        a[col], a[piv] = a[piv], a[col]
        p = a[col][col]
        a[col] = [v / p for v in a[col]]
        for r in range(n):
            if r != col and a[r][col] != 0.0:
                f = a[r][col]
                a[r] = [a[r][k] - f * a[col][k] for k in range(2 * n)]
    return [row[n:] for row in a]


def det3(m):
    return (
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
        - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])
    )


# Axis-swap + unit-scale base-change matrix S4 (column-vector convention).
# UE_x = 100*U_x ; UE_y = 100*U_z ; UE_z = 100*U_y  (meters -> cm included).
# Derivation is quoted in the emitted validation.md.
S4 = [
    [100.0, 0.0, 0.0, 0.0],
    [0.0, 0.0, 100.0, 0.0],
    [0.0, 100.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
]
S4_INV = mat4_inverse(S4)
# Permutation-only view (for AABB extents and scale permuting).
Q3 = [[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 1.0, 0.0]]


def base_change(m):
    """Full 4x4 base change S4 * m * S4^-1 (basis vectors and translation)."""
    return mat4_mult(mat4_mult(S4, m), S4_INV)


def transform_aabb_unity_to_ue(aabb_min, aabb_max):
    """Map an axis-aligned Unity-space AABB to UE cm.

    The net axis map is a pure Y/Z permutation (no sign flips, no axis
    mixing), so the image of an axis-aligned box is an axis-aligned box:
    extents are permuted and scaled by 100.
    """
    xmin, ymin, zmin = aabb_min
    xmax, ymax, zmax = aabb_max
    return {
        "min": [100.0 * xmin, 100.0 * zmin, 100.0 * ymin],
        "max": [100.0 * xmax, 100.0 * zmax, 100.0 * ymax],
    }


# ---------------------------------------------------------------------------
# UE transform decomposition (mirrors UE conventions).
# ---------------------------------------------------------------------------

SINGULARITY_THRESHOLD = 0.4999995
RAD_TO_DEG = 180.0 / math.pi
ORTHONORMAL_TOL = 1.0e-3
UNIFORM_REL_TOL = 1.0e-4


def quat_from_matrix(m):
    """Shepperd's method; m is a proper rotation 3x3 (unit length columns)."""
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    if w < 0.0:  # canonical sign (same rotation, deterministic output)
        x, y, z, w = -x, -y, -z, -w
    return x, y, z, w


def normalize_axis(angle):
    """FRotator::NormalizeAxis -> [0, 360)."""
    a = math.fmod(angle, 360.0)
    if a < 0.0:
        a += 360.0
    return a


def quat_to_rotator(x, y, z, w):
    """Exact replication of UE FQuat::Rotator() (UnrealMath.cpp:636-679).

    Returns (pitch, yaw, roll) in degrees; singular branch yaw normalized
    to [0,360) exactly like UE.
    """
    sing = z * x - w * y
    yaw_y = 2.0 * (w * z + x * y)
    yaw_x = 1.0 - 2.0 * (y * y + z * z)
    if sing < -SINGULARITY_THRESHOLD:
        pitch = -90.0
        yaw = normalize_axis(-2.0 * math.atan2(x, w) * RAD_TO_DEG)
        roll = 0.0
    elif sing > SINGULARITY_THRESHOLD:
        pitch = 90.0
        yaw = normalize_axis(2.0 * math.atan2(x, w) * RAD_TO_DEG)
        roll = 0.0
    else:
        pitch = math.asin(max(-1.0, min(1.0, 2.0 * sing))) * RAD_TO_DEG
        yaw = math.atan2(yaw_y, yaw_x) * RAD_TO_DEG
        roll = math.atan2(
            -2.0 * (w * x + y * z),
            1.0 - 2.0 * (x * x + y * y),
        ) * RAD_TO_DEG
    return pitch, yaw, roll


def decompose_ue_matrix(m4):
    """Decompose a UE placement matrix into loc/rot/scale with flags.

    Scale is extracted from column lengths (valid for R*S compositions).
    Negative determinant (mirrored instance) is folded into a single
    negative scale axis (the largest axis) so the rotation stays proper.
    Orthonormality deviation flags shear (nested non-uniform scale).
    """
    t = [m4[0][3], m4[1][3], m4[2][3]]
    l3 = [row[:3] for row in m4[:3]]
    col_norms = [
        math.sqrt(l3[0][i] ** 2 + l3[1][i] ** 2 + l3[2][i] ** 2) for i in range(3)
    ]
    sx, sy, sz = col_norms
    det = det3(l3)
    mirrored = det < 0.0
    mirror_axis = None
    if mirrored:
        mirror_axis = int(max(range(3), key=lambda i: col_norms[i]))
        col_norms[mirror_axis] = -col_norms[mirror_axis]
    inv_s = [1.0 / v if v != 0.0 else 0.0 for v in col_norms]
    # Note: with the negative scale the column of R0 already flips sign
    # (R0 = L * diag(1/s)); R0 * diag(s) reconstructs L exactly and R0 is
    # proper (det +1). Do NOT negate the column again.
    r0 = [
        [l3[r][c] * inv_s[c] for c in range(3)]
        for r in range(3)
    ]
    # orthonormality deviation of R0 (R0^T * R0 - I)
    rt = [[r0[0][j] * r0[0][k] + r0[1][j] * r0[1][k] + r0[2][j] * r0[2][k] for k in range(3)] for j in range(3)]
    dev = 0.0
    for j in range(3):
        for k in range(3):
            target = 1.0 if j == k else 0.0
            dev = max(dev, abs(rt[j][k] - target))
    qx, qy, qz, qw = quat_from_matrix(r0)
    pitch, yaw, roll = quat_to_rotator(qx, qy, qz, qw)
    scale = [col_norms[0], col_norms[1], col_norms[2]]
    smax = max(abs(v) for v in scale)
    smin = min(abs(v) for v in scale)
    uniform = (smax - smin) <= UNIFORM_REL_TOL * max(1.0, smax)
    # reconstruction error: R0 * diag(scale) vs original linear part
    recon = 0.0
    for r in range(3):
        for c in range(3):
            recon = max(recon, abs(r0[r][c] * scale[c] - l3[r][c]))
    return {
        "location_cm": [t[0], t[1], t[2]],
        "rotation_quaternion_xyzw": [qx, qy, qz, qw],
        "rotator_pitch_yaw_roll_deg": [pitch, yaw, roll],
        "scale": scale,
        "uniform_scale": uniform,
        "determinant": det,
        "mirrored": mirrored,
        "mirror_axis_negated": ("xyz"[mirror_axis] if mirror_axis is not None else None),
        "orthonormality_max_deviation": dev,
        "shear_suspect": dev > ORTHONORMAL_TOL,
        "reconstruction_max_error": recon,
    }


# ---------------------------------------------------------------------------
# Input loading
# ---------------------------------------------------------------------------


def load_catalog(path):
    """asset_catalog.csv -> dict keyed by catalog 'id' (SM_..._hash names)."""
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    catalog = {}
    dupes = []
    for row in rows:
        key = (row.get("id") or "").strip()
        if not key:
            continue
        if key in catalog:
            dupes.append(key)
        catalog[key] = {
            "ue_object_path": (row.get("ue_object_path") or "").strip(),
            "project_uasset": (row.get("project_uasset") or "").strip(),
            "category": (row.get("category") or "").strip(),
        }
    return catalog, rows, dupes


def load_audit(audit_dir, bundle_file):
    with open(os.path.join(audit_dir, bundle_file), "r", encoding="utf-8") as f:
        return json.load(f)


def is_mapped_instance(inst):
    refs = inst.get("mesh_refs") or []
    return bool(refs) and refs[0].get("ue_mesh_id")


def collect_root_data(root, data, catalog):
    """Build the intermediate record for one audit root."""
    root_key = root["root_transform"]
    insts = [i for i in data["renderer_instances"] if i.get("root_transform_key") == root_key]
    mesh_renderers = [i for i in insts if i.get("renderer_type") == "MeshRenderer"]
    psrs = [i for i in insts if i.get("renderer_type") != "MeshRenderer"]
    psr_mesh_slots = sum(len(i.get("mesh_refs") or []) for i in psrs)
    psr_slots_mapped = sum(
        1 for i in psrs for ref in (i.get("mesh_refs") or []) if ref.get("ue_mesh_id")
    )

    anomalies = []
    mapped = []
    unmapped = []
    for inst in mesh_renderers:
        matrix = inst.get("root_relative_matrix")
        if matrix is None:
            anomalies.append(
                {"hierarchy_path": inst.get("hierarchy_path"), "issue": "missing root_relative_matrix"}
            )
            continue
        if len(matrix) != 4 or any(len(r) != 4 for r in matrix):
            anomalies.append(
                {"hierarchy_path": inst.get("hierarchy_path"), "issue": "root_relative_matrix is not 4x4"}
            )
            continue
        ue_m = base_change(matrix)
        ue = decompose_ue_matrix(ue_m)
        refs = inst.get("mesh_refs") or []
        ref0 = refs[0] if refs else {}
        mesh_id = ref0.get("ue_mesh_id")
        entry = {
            "renderer_key": inst.get("renderer_key"),
            "hierarchy_path": inst.get("hierarchy_path"),
            "gameobject_name": inst.get("gameobject_name"),
            "active_in_stored_hierarchy": inst.get("active_in_stored_hierarchy"),
            "renderer_enabled": inst.get("renderer_enabled"),
            "unity_root_relative_matrix": matrix,
            "ue_transform": ue,
        }
        aabb = ref0.get("root_relative_aabb_from_mesh_bounds")
        if aabb:
            entry["ue_bounds_estimate_cm"] = transform_aabb_unity_to_ue(aabb["min"], aabb["max"])
        if mesh_id:
            cat = catalog.get(mesh_id)
            entry.update(
                {
                    "ue_mesh_id": mesh_id,
                    "ue_object_path": cat["ue_object_path"] if cat else None,
                    "project_uasset": cat["project_uasset"] if cat else None,
                    "catalog_found": cat is not None,
                    "mesh_source_name": ref0.get("source_name"),
                    "mesh_source_key": ref0.get("source_key"),
                    "materials": [
                        {
                            "slot": m.get("slot"),
                            "ue_material_id": m.get("ue_material_id"),
                            "missing_texture_slots": sorted(
                                k
                                for k, v in (m.get("missing_textures") or {}).items()
                                if isinstance(v, dict) and v.get("status") == "missing"
                            ),
                        }
                        for m in sorted(inst.get("materials") or [], key=lambda m: m.get("slot", 0))
                    ],
                }
            )
            mapped.append(entry)
        else:
            entry.update(
                {
                    "ue_mesh_id": None,
                    "mesh_source_key": ref0.get("source_key"),
                    "mesh_source_name": ref0.get("source_name"),
                    "unresolved_reason": "mesh ref has no ue_mesh_id (missing source dependency)",
                }
            )
            unmapped.append(entry)

    mapped.sort(key=lambda e: (e["hierarchy_path"] or "", e["renderer_key"] or ""))
    unmapped.sort(key=lambda e: (e["hierarchy_path"] or "", e["renderer_key"] or ""))
    distinct_meshes = sorted({e["ue_mesh_id"] for e in mapped})
    catalog_missing = sorted({e["ue_mesh_id"] for e in mapped if not e["catalog_found"]})
    non_uniform = [e for e in mapped if not e["ue_transform"]["uniform_scale"]]
    mirrored = [e for e in mapped if e["ue_transform"]["mirrored"]]
    shear = [e for e in mapped if e["ue_transform"]["shear_suspect"]]
    negative_scale = [
        e for e in mapped if any(s < 0.0 for s in e["ue_transform"]["scale"])
    ]

    # Missing texture references (reported as-is; not patched or fabricated).
    slots_total = 0
    slots_with_missing = 0
    missing_inputs = set()
    missing_source_cabs = set()
    for inst in mesh_renderers:
        for m in inst.get("materials") or []:
            slots_total += 1
            mt = m.get("missing_textures") or {}
            bad = {
                k: v
                for k, v in mt.items()
                if isinstance(v, dict) and v.get("status") == "missing"
            }
            if bad:
                slots_with_missing += 1
                missing_inputs.update(bad.keys())
                for v in bad.values():
                    sk = v.get("source_key") or ""
                    if ":" in sk:
                        missing_source_cabs.add(sk.split(":", 1)[0])

    bounds_min, bounds_max = None, None
    have_bounds = False
    for e in mapped:
        b = e.get("ue_bounds_estimate_cm")
        if not b:
            continue
        have_bounds = True
        if bounds_min is None:
            bounds_min = list(b["min"])
            bounds_max = list(b["max"])
        else:
            for k in range(3):
                bounds_min[k] = min(bounds_min[k], b["min"][k])
                bounds_max[k] = max(bounds_max[k], b["max"][k])

    active_count = sum(1 for e in mapped if e["active_in_stored_hierarchy"] and e["renderer_enabled"])

    stats = {
        "mesh_renderer_total": len(mesh_renderers),
        "mesh_renderer_mapped": len(mapped),
        "mesh_renderer_unmapped": len(unmapped),
        "distinct_ue_mesh_ids": len(distinct_meshes),
        "catalog_missing_mesh_ids": len(catalog_missing),
        "non_uniform_scale_instances": len(non_uniform),
        "mirrored_instances": len(mirrored),
        "negative_scale_instances": len(negative_scale),
        "shear_suspect_instances": len(shear),
        "active_and_enabled_mapped": active_count,
        "material_slots_total": slots_total,
        "material_slots_with_missing_textures": slots_with_missing,
        "distinct_missing_texture_inputs": sorted(missing_inputs),
        "distinct_missing_source_cabs": sorted(missing_source_cabs),
        "psr_instance_count": len(psrs),
        "psr_mesh_slots_total": psr_mesh_slots,
        "psr_mesh_slots_mapped": psr_slots_mapped,
        "max_reconstruction_error": max(
            (e["ue_transform"]["reconstruction_max_error"] for e in mapped), default=0.0
        ),
        "max_orthonormality_deviation": max(
            (e["ue_transform"]["orthonormality_max_deviation"] for e in mapped), default=0.0
        ),
    }

    record = {
        "source_bundle": os.path.basename(root.get("_source_file", "")) or root.get("_source_file", ""),
        "root_name": root.get("name"),
        "root_transform_key": root_key,
        "root_gameobject_key": root.get("root_gameobject"),
        "original_paths": root.get("original_paths"),
        "node_count": root.get("node_count"),
        "audit_root_record": {
            "renderer_counts": root.get("renderer_counts"),
            "mesh_instance_count": root.get("mesh_instance_count"),
            "mesh_instances_mapped_to_ue_manifest": root.get("mesh_instances_mapped_to_ue_manifest"),
            "active_mesh_instances": root.get("active_mesh_instances"),
            "unresolved_mesh_keys": root.get("unresolved_mesh_keys"),
            "resolved_subset_root_space_aabb": root.get("resolved_subset_root_space_aabb"),
        },
        "instance_stats": stats,
        "instances": mapped,
        "unmapped_instances": unmapped,
        "anomalies": anomalies,
        "ue_bounds_union_cm": (
            {"min": bounds_min, "max": bounds_max} if have_bounds else None
        ),
    }
    extras = {
        "distinct_ue_mesh_ids": distinct_meshes,
        "catalog_missing_mesh_ids": catalog_missing,
        "non_uniform": non_uniform,
        "mirrored": mirrored,
        "shear": shear,
    }
    return record, extras


def expected_mr_mapped(root, psr_slots_mapped):
    """Audit-claimed mapped MeshRenderer count (audit mapping includes PSR slots)."""
    total = root.get("mesh_instances_mapped_to_ue_manifest") or 0
    return total - psr_slots_mapped


# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------


def build(audit_dir, catalog_path, out_dir):
    catalog, catalog_rows, catalog_dupes = load_catalog(catalog_path)
    os.makedirs(out_dir, exist_ok=True)

    built = []  # (record, extras, root_audit, bundle_file)
    anomalies_global = []

    for bundle_file, root_names in SOURCES:
        data = load_audit(audit_dir, bundle_file)
        roots = data["roots"]
        targets = [r for r in roots if root_names is None or r["name"] in root_names]
        if root_names is not None:
            found = {r["name"] for r in targets}
            for want in root_names:
                if want not in found:
                    anomalies_global.append(
                        {"bundle": bundle_file, "issue": "root not found: %s" % want}
                    )
        for root in targets:
            root["_source_file"] = bundle_file
            record, extras = collect_root_data(root, data, catalog)
            built.append((record, extras, root, bundle_file))

    written = []
    # Named single-root outputs first.
    merged_00502 = None
    for record, extras, root, bundle_file in built:
        if bundle_file.startswith("00502"):
            if merged_00502 is None:
                merged_00502 = {
                    "source_bundle": bundle_file,
                    "kind": "decoration_prefab_set",
                    "prefab_count": 0,
                    "prefabs": [],
                }
            prefab_entry = dict(record)
            prefab_entry.pop("source_bundle", None)
            merged_00502["prefabs"].append(prefab_entry)
            merged_00502["prefab_count"] += 1
        else:
            path = os.path.join(out_dir, "%s.json" % record["root_name"])
            with open(path, "w", encoding="utf-8") as f:
                json.dump(record, f, ensure_ascii=False, indent=1)
            written.append(path)
    if merged_00502 is not None:
        merged_00502["prefabs"].sort(key=lambda p: p["root_name"] or "")
        path = os.path.join(out_dir, "00502_decorations_index.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(merged_00502, f, ensure_ascii=False, indent=1)
        written.append(path)

    report = build_validation_md(built, catalog_rows, catalog_dupes, anomalies_global, out_dir)
    report_path = os.path.join(out_dir, "validation.md")
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(report)
    written.append(report_path)
    return written


def fmt(v, nd=3):
    return ("%.*f" % (nd, v)).rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def fmt_vec(v, nd=3):
    return "(" + ", ".join(fmt(x, nd) for x in v) + ")"


def build_validation_md(built, catalog_rows, catalog_dupes, anomalies_global, out_dir):
    lines = []
    add = lines.append
    add("# M6-011 DNFM 公会场景布局 → UE 放置中间数据 校验报告")
    add("")
    add("生成脚本：`Scripts/DNFM/build_dnfm_layout_intermediate.py`（纯标准库，确定性输出，未启动 UE，未修改任何既有文件）。")
    add("输入为只读审计数据与 `asset_catalog.csv`；本报告与中间 JSON 全部数值可由脚本重复生成。")
    add("")

    # --- convention ---
    add("## 1. 坐标与单位约定（推导依据）")
    add("")
    add("### 1.1 文档与脚本依据")
    add("")
    for c in CONVENTION_CITES:
        add("- %s" % c)
    add("")
    add("### 1.2 实测锚点（对导入链路的二进制证据）")
    add("")
    add("- 实测 `SourceAssets/Unpacked/DNFM130_5_8/fbx/SM_Stone_Middle_017_2dd7e3c225.fbx`（FBX 7.4 二进制，直接解析）：")
    add("  - `GlobalSettings`: `UpAxis=2(+) [Z上]`, `FrontAxis=1(+1) [Y前]`, `CoordAxis=0(-1)`, `UnitScaleFactor=1.0`，")
    add("    即 Blender 导出（`axis_forward='-Y', axis_up='Z'`）把 Blender 原生 Z-up 右手系原样写入文件标签。")
    add("  - `Vertices` 与 `geometry/SM_Stone_Middle_017_2dd7e3c225.json` 的 Unity 顶点逐分量对比：")
    add("    FBX 顶点 = `(U_x, -U_z, U_y)`（即 `vec()` 的结果），几何未再旋转。")
    add("  - Model 节点 `Lcl Rotation=(0,0,180)`, `Lcl Scaling=(100,100,100)`：导出器把 Blender 前向 -Y 对齐到 FBX 前向 +Y 的 180°Z 补偿和 米→厘米 的 x100 都放在节点变换里。")
    add("- UE 5.8 导入（本机引擎源码）：导入目标轴系为 `(Up=Z, Front=-Y, RightHanded)`（`FbxMainImport.cpp:1525-1530`），与文件标签（Z-up、Y+ 前）不一致 → `ConvertScene` 对一级节点加 180°Z 预旋转，与导出器的 180°Z 相互抵消；")
    add("  `bTransformVertexToAbsolute=true`（`FbxStaticMeshImportData.cpp:18`）使节点全局变换（含 x100）烘进静态网格顶点；随后 `ConvertPos` 做 `(x,-y,z)` 手性翻转（`FbxUtilsImport.cpp:63-71`）。")
    add("")
    add("### 1.3 净映射（本脚本实现）")
    add("")
    add("对网格顶点逐步合成：`UE = ConvertPos(S(100) · vec(U)) = (U_x, U_z, U_y) × 100`，")
    add("即 Unity (x,y,z) → UE (x, z, y)·100：**UE X=Unity X，UE Y(纵深)=Unity Z，UE Z(高度)=Unity Y，单位米→厘米**。")
    add("")
    add("实例放置矩阵按整块 4x4 基变换（基向量与平移一致变换，不是只把平移乘 100）：")
    add("")
    add("```")
    add("S4 = [[100,0,0,0],[0,0,100,0],[0,100,0,0],[0,0,0,1]]   # 列向量约定")
    add("M_UE = S4 · M_unity · S4^-1")
    add("```")
    add("")
    add("- 位置：`UE_cm = 100 · (t_x, t_z, t_y)`")
    add("- 旋转：`R_UE = Q · R_U · Q^-1`（Q 为 Y↔Z 置换），输出四元数 + UE Rotator（按 `FQuat::Rotator()` 公式逐行复刻）")
    add("- 缩放：`(sx, sz, sy)`（缩放是无量纲的；x100 已在网格顶点内）")
    add("- 基向量经 Y↔Z 置换行列式为 -1：Unity 左手系 → UE 左手系经由一个反演映射，网格三角形绕序由 FBX 导入器统一翻转；")
    add("  实例矩阵本身 `det<0`（镜像几何）无法用 UE StaticMesh 正常表示，单独列出（见 §6）。")
    add("")
    add("审计侧约定佐证：audit JSON `matrix_convention` = 「4x4 row-major JSON, column-vector multiplication. T*R*S; ...」；")
    add("`root_relative_matrix` 排除根节点自身局部变换（README:37），适合直接作为 UE 内 prefab 根 Actor 下的相对摆放。")
    add("")

    # --- per-root comparison ---
    add("## 2. 实例数与审计声明对照")
    add("")
    add("| Root | 来源 | 审计声明 (MeshRenderer总数/可映射) | 本脚本 MR总数/可映射/不可映射 | 不同网格数 | 备注 |")
    add("|---|---|---|---|---|---|")

    claims = {
        "GuildagitGroundDummy": "980 / 972，95 个不同网格 (README:11)",
        "BG_Planet_Prepab": "32 / 32 (README:12)",
        "00502 全部装饰根": "156 / 156，152 个不同网格 (README:13)",
    }
    rows_00502 = [b for b in built if b[3].startswith("00502")]
    for record, extras, root, bundle_file in built:
        name = record["root_name"]
        st = record["instance_stats"]
        if bundle_file.startswith("00502"):
            claim = "-（bundle 级声明见合计行）"
        elif name in claims:
            claim = claims[name]
        else:
            claim = "-"
        expected = expected_mr_mapped(root, st["psr_mesh_slots_mapped"])
        note = []
        if st["mesh_renderer_total"] != (root.get("renderer_counts") or {}).get("MeshRenderer"):
            note.append("MR总数与 renderer_counts 不一致")
        if st["mesh_renderer_mapped"] != expected:
            note.append(
                "可映射数(%d) != 审计映射数(%d)-PSR槽(%d)" % (st["mesh_renderer_mapped"], expected, st["psr_mesh_slots_mapped"])
            )
        add(
            "| %s | %s | %s | %d / %d / %d | %d | %s |"
            % (name, bundle_file[:5], claim, st["mesh_renderer_total"], st["mesh_renderer_mapped"], st["mesh_renderer_unmapped"], st["distinct_ue_mesh_ids"], "；".join(note) or "一致")
        )
    st00502 = aggregate_stats(rows_00502)
    add(
        "| 00502 合计（140 个根） | 00502 | 156 / 156，152 网格 | %d / %d / %d | %d | %s |"
        % (
            st00502["mr_total"],
            st00502["mapped"],
            st00502["unmapped"],
            len(st00502["distinct_meshes"]),
            "一致" if (st00502["mr_total"], st00502["mapped"]) == (156, 156) else "与声明不符",
        )
    )
    add("")
    add("审计的 `mesh_instances_mapped_to_ue_manifest` 同时计入 ParticleSystemRenderer 的已知网格槽位；")
    add("上表可映射数已扣除 PSR 槽位后与逐实例统计对照。PSR 忽略数量见 §5。")
    add("")

    # --- catalog cross-check ---
    add("## 3. ue_mesh_id 与 asset_catalog.csv 交叉核对")
    add("")
    catalog_by_id = set()
    for row in catalog_rows:
        if row.get("id"):
            catalog_by_id.add(row["id"].strip())
    total_refs = 0
    missing = set()
    distinct_meshes_all = set()
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        total_refs += st["mesh_renderer_mapped"]
        distinct_meshes_all.update(extras["distinct_ue_mesh_ids"])
        missing.update(extras["catalog_missing_mesh_ids"])
    add("- 资产目录行数（含表头外的数据行）：%d；id 重复：%d" % (len(catalog_rows), len(catalog_dupes)))
    add("- 本批可映射 MeshRenderer 实例总数：%d，涉及不同 ue_mesh_id：%d" % (total_refs, len(distinct_meshes_all)))
    add("- 在 asset_catalog.csv 中找不到的 ue_mesh_id 数量：**%d**（预期 0）%s" % (len(missing), "—— 通过" if not missing else "—— 异常: " + ", ".join(sorted(missing))))
    for record, extras, root, bundle_file in built:
        if extras["catalog_missing_mesh_ids"]:
            add("  - %s: %s" % (record["root_name"], ", ".join(extras["catalog_missing_mesh_ids"])))
    add("- `ue_object_path`/`project_uasset` 由目录按 id 解析；解析失败的实例在 JSON 中 `catalog_found=false`、路径字段为 null。")
    add("")
    add("### 3.1 缺图材质（如实登记，不伪造补齐）")
    add("")
    add("审计 JSON 的 `materials[].missing_textures` 记录每个材质槽缺失的纹理引用；本脚本原样登记，不做任何补齐或猜测。")
    add("")
    def missing_line(label, st):
        return "- %s：材质槽 %d，其中有缺失纹理引用的槽 %d；缺失纹理输入：%s；缺失来源 CAB：%s" % (
            label,
            st["material_slots_total"],
            st["material_slots_with_missing_textures"],
            ", ".join("`%s`" % s for s in st["distinct_missing_texture_inputs"]) or "无",
            ", ".join("`%s`" % s for s in st["distinct_missing_source_cabs"]) or "无",
        )

    agg = {"material_slots_total": 0, "material_slots_with_missing_textures": 0,
           "distinct_missing_texture_inputs": set(), "distinct_missing_source_cabs": set()}
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        if st["material_slots_total"] == 0:
            continue
        if bundle_file.startswith("00502"):
            agg["material_slots_total"] += st["material_slots_total"]
            agg["material_slots_with_missing_textures"] += st["material_slots_with_missing_textures"]
            agg["distinct_missing_texture_inputs"].update(st["distinct_missing_texture_inputs"])
            agg["distinct_missing_source_cabs"].update(st["distinct_missing_source_cabs"])
            continue
        add(missing_line(record["root_name"], st))
    if agg["material_slots_total"]:
        agg["distinct_missing_texture_inputs"] = sorted(agg["distinct_missing_texture_inputs"])
        agg["distinct_missing_source_cabs"] = sorted(agg["distinct_missing_source_cabs"])
        add(missing_line("00502（140 个装饰 prefab 合计）", agg))
    add("")
    add("逐实例明细见 JSON `instances[].materials[].missing_texture_slots`（该槽缺失的纹理输入名列表）。")
    add("")

    # --- non-uniform / shear ---
    add("## 4. 非均匀缩放与病态矩阵")
    add("")
    total_non_uniform = 0
    total_mirrored = 0
    total_shear = 0
    total_neg = 0
    max_recon = 0.0
    max_dev = 0.0
    samples_non_uniform = []
    samples_shear = []
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        total_non_uniform += st["non_uniform_scale_instances"]
        total_mirrored += st["mirrored_instances"]
        total_shear += st["shear_suspect_instances"]
        total_neg += st["negative_scale_instances"]
        max_recon = max(max_recon, st["max_reconstruction_error"])
        max_dev = max(max_dev, st["max_orthonormality_deviation"])
        for e in extras["non_uniform"][:5]:
            if len(samples_non_uniform) < 10:
                samples_non_uniform.append((record["root_name"], e))
        for e in extras["shear"][:5]:
            if len(samples_shear) < 10:
                samples_shear.append((record["root_name"], e))
    add("- 非均匀缩放（UE 侧任两轴缩放差超过相对容差 1e-4）实例数：**%d**；负缩放分量实例数：%d。" % (total_non_uniform, total_neg))
    add("- 旋转正交性最大偏差（R0ᵀR0−I 元素绝对值最大值）：%.3e；分解重构最大误差（R·S 对比原线性部分）：%.3e。" % (max_dev, max_recon))
    add("- 正交性偏差 > 1e-3（疑似剪切，Unity 层级里非均匀缩放与旋转嵌套所致）实例数：**%d**。" % total_shear)
    if samples_shear:
        add("- 剪切疑似样例（关卡侧应优先用层级或直接使用 4x4 矩阵，不要强行拆分 loc/rot/scale）：")
        add("")
        add("  | Root | hierarchy_path | UE scale | 正交偏差 |")
        add("  |---|---|---|---|")
        for rn, e in samples_shear[:10]:
            add(
                "  | %s | `%s` | %s | %.3e |"
                % (rn, e["hierarchy_path"], fmt_vec(e["ue_transform"]["scale"]), e["ue_transform"]["orthonormality_max_deviation"])
            )
    add("")
    if samples_non_uniform:
        add("- 非均匀缩放样例（关卡侧可用按分量缩放或层级放置）：")
        add("")
        add("  | Root | hierarchy_path | UE scale (x,y,z) | uniform |")
        add("  |---|---|---|---|")
        for rn, e in samples_non_uniform[:10]:
            add(
                "  | %s | `%s` | %s | %s |"
                % (rn, e["hierarchy_path"], fmt_vec(e["ue_transform"]["scale"]), e["ue_transform"]["uniform_scale"])
            )
    add("")

    # --- PSR ---
    add("## 5. ParticleSystemRenderer（按需求忽略，仅计数）")
    add("")
    psr_total = 0
    psr_slots = 0
    psr_slots_mapped = 0
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        psr_total += st["psr_instance_count"]
        psr_slots += st["psr_mesh_slots_total"]
        psr_slots_mapped += st["psr_mesh_slots_mapped"]
    add("- 本批 PSR 实例总数：%d；PSR 网格槽位总数：%d，其中有 ue_mesh_id 的槽位：%d。" % (psr_total, psr_slots, psr_slots_mapped))
    add("- 全部未写入实例数据（按用户文档要求 PSR 需要粒子系统语义，不作为静态物体摆放）；数量仅在此登记。")
    add("")
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        if st["psr_instance_count"]:
            add("  - %s：PSR 实例 %d，网格槽 %d（可映射 %d）" % (record["root_name"], st["psr_instance_count"], st["psr_mesh_slots_total"], st["psr_mesh_slots_mapped"]))
    add("")

    # --- mirror ---
    add("## 6. 负行列式（镜像）实例")
    add("")
    add("- det<0 的实例总数：%d。UE StaticMesh 不支持镜像缩放（负 scale 会翻转法线/绕序，通常需资产侧镜像或翻面处理）。" % total_mirrored)
    if total_mirrored:
        add("- 脚本策略：把镜像折叠为「单轴负缩放 + 正常旋转」（选绝对值最大的缩放轴取负），JSON 中 `mirrored=true` 且记录 `mirror_axis_negated`；")
        add("  关卡侧拿到这类实例时不要直接把负 scale 写进 StaticMeshActor，先做资产镜像或接受翻面显示。样例：")
        add("")
        add("  | Root | hierarchy_path | det | 折叠到负缩放的轴 | scale |")
        add("  |---|---|---|---|---|")
        shown = 0
        for record, extras, root, bundle_file in built:
            for e in extras["mirrored"]:
                if shown >= 10:
                    break
                ut = e["ue_transform"]
                add(
                    "  | %s | `%s` | %.4f | %s | %s |"
                    % (record["root_name"], e["hierarchy_path"], ut["determinant"], ut["mirror_axis_negated"], fmt_vec(ut["scale"]))
                )
                shown += 1
        add("")
    else:
        add("- 本批未发现镜像实例。")
        add("")

    # --- bounds ---
    add("## 7. UE 包围盒粗估（厘米，帮助关卡搭建）")
    add("")
    add("由每个可映射实例 `mesh_refs[].root_relative_aabb_from_mesh_bounds`（Unity 根相对、已含实例变换的网格包围盒）")
    add("经 Y↔Z 置换并 ×100 后取并集。仅含 MeshRenderer 可映射实例（不含 PSR 槽位、不含不可映射实例），")
    add("也不代表运行时完整可走范围（README:38 同样声明其非完整边界）。")
    add("")
    add("| Root | UE AABB min (cm) | UE AABB max (cm) | 尺寸 (cm) |")
    add("|---|---|---|---|")
    union_min = None
    union_max = None
    for record, extras, root, bundle_file in built:
        b = record["ue_bounds_union_cm"]
        if not b:
            add("| %s | - | - | 无可映射包围盒 |" % record["root_name"])
            continue
        size = [b["max"][k] - b["min"][k] for k in range(3)]
        add("| %s | %s | %s | %s |" % (record["root_name"], fmt_vec(b["min"], 1), fmt_vec(b["max"], 1), fmt_vec(size, 1)))
        if union_min is None:
            union_min = list(b["min"])
            union_max = list(b["max"])
        else:
            for k in range(3):
                union_min[k] = min(union_min[k], b["min"][k])
                union_max[k] = max(union_max[k], b["max"][k])
    if union_min is not None and len(built) > 1:
        add("| 合计并集 | %s | %s | %s |" % (fmt_vec(union_min, 1), fmt_vec(union_max, 1), fmt_vec([union_max[k] - union_min[k] for k in range(3)], 1)))
    add("")
    add("审计 Unity 根空间 AABB（README:38-39，含 PSR 槽位）经同一映射后的对照：")
    add("")
    add("| Root | 审计 Unity AABB → UE 映射 min (cm) | max (cm) |")
    add("|---|---|---|")
    for record, extras, root, bundle_file in built:
        ra = (record["audit_root_record"] or {}).get("resolved_subset_root_space_aabb")
        if not ra:
            continue
        ue = transform_aabb_unity_to_ue(ra["min"], ra["max"])
        add("| %s | %s | %s |" % (record["root_name"], fmt_vec(ue["min"], 1), fmt_vec(ue["max"], 1)))
    add("")
    add("两者差异仅来自 PSR 槽位与不可映射实例的包围盒是否并入。")
    add("")
    add("注意：00502 的装饰 prefab 在源数据里对网格节点带大倍率缩放（多数为 250×，个别更大，")
    add("如 GA_maingate 408.9×、GA_fishing_deco_lv1 内 apple_001 312.9×），对应 UE 包围盒因此达数米至数百米；")
    add("这是审计 JSON 中保存的原始层级缩放，不是本脚本换算引入的误差。")
    add("")

    # --- sample matrix ---
    add("## 8. 矩阵抽样（含 README 已验证例子）")
    add("")
    sample_path = "GuildagitGroundDummy/trees1/GA_tree_01 (3)/Mesh/GA_tree_01"
    found_sample = None
    for record, extras, root, bundle_file in built:
        for e in record["instances"] + record["unmapped_instances"]:
            if e["hierarchy_path"] == sample_path:
                found_sample = (record["root_name"], e)
                break
        if found_sample:
            break
    if found_sample:
        rn, e = found_sample
        ut = e["ue_transform"]
        add("README:24-30 已验证例 `GuildagitGroundDummy/trees1/GA_tree_01 (3)/Mesh/GA_tree_01`：")
        add("")
        add("- ue_mesh_id：`%s`（材质槽：`%s`）" % (e.get("ue_mesh_id"), ", ".join(m["ue_material_id"] or "null" for m in e.get("materials", []))))
        add("- Unity root_relative_matrix：")
        add("")
        for row in e["unity_root_relative_matrix"]:
            add("  [%s]" % ", ".join("%.10g" % v for v in row))
        add("")
        add("- Unity 根相对平移：%s；本脚本换算 UE 位置：%s cm（=100·(x,z,y)）" % (fmt_vec([row[3] for row in e["unity_root_relative_matrix"][:3]]), fmt_vec(ut["location_cm"], 2)))
        add("- UE 四元数 (x,y,z,w)：%s" % fmt_vec(ut["rotation_quaternion_xyzw"], 6))
        add("- UE Rotator (pitch,yaw,roll)：%s 度" % fmt_vec(ut["rotator_pitch_yaw_roll_deg"], 4))
        add("- UE 缩放：%s（uniform=%s）" % (fmt_vec(ut["scale"], 5), ut["uniform_scale"]))
        add("")
    else:
        add("警告：未找到样例实例 `%s`。" % sample_path)
        add("")
    add("另取 2 个不同 Root 的实例展示（前几个可映射实例）：")
    add("")
    add("| Root | hierarchy_path | Unity 位置 (m) | UE 位置 (cm) | UE Rotator (P,Y,R)° | UE scale | det |")
    add("|---|---|---|---|---|---|---|")
    shown = 0
    for record, extras, root, bundle_file in built:
        for e in record["instances"][:2]:
            ut = e["ue_transform"]
            unity_t = [row[3] for row in e["unity_root_relative_matrix"][:3]]
            add(
                "| %s | `%s` | %s | %s | %s | %s | %.4f |"
                % (
                    record["root_name"],
                    e["hierarchy_path"],
                    fmt_vec(unity_t),
                    fmt_vec(ut["location_cm"], 2),
                    fmt_vec(ut["rotator_pitch_yaw_roll_deg"], 2),
                    fmt_vec(ut["scale"], 4),
                    ut["determinant"],
                )
            )
            shown += 1
    add("")

    # --- unmapped ---
    add("## 9. 不可映射实例（保留原 mesh key）")
    add("")
    total_unmapped = 0
    for record, extras, root, bundle_file in built:
        st = record["instance_stats"]
        total_unmapped += st["mesh_renderer_unmapped"]
        if st["mesh_renderer_unmapped"]:
            add("- %s：%d 个，mesh key 见 JSON `unmapped_instances[].mesh_source_key`：" % (record["root_name"], st["mesh_renderer_unmapped"]))
            for e in record["unmapped_instances"]:
                add("  - `%s` @ `%s`（renderer_enabled=%s）" % (e.get("mesh_source_key"), e["hierarchy_path"], e.get("renderer_enabled")))
    if not total_unmapped:
        add("- 本批无不可映射实例。")
    else:
        add("- 合计：%d 个。这些位置已给出 UE 变换，可作占位标记，但对应网格尚未导入。" % total_unmapped)
    add("")

    # --- anomalies ---
    add("## 10. 其他异常与限制")
    add("")
    if anomalies_global:
        for a in anomalies_global:
            add("- %s" % json.dumps(a, ensure_ascii=False))
    field_missing = 0
    for record, extras, root, bundle_file in built:
        field_missing += len(record["anomalies"])
    add("- 缺失字段/非 4x4 矩阵的实例数：%d。" % field_missing)
    add("- 审计 `validation.json` 声明父引用一致、矩阵有限、四元数范数误差 <2e-7（success=true）；本脚本未复算该审计，只信任并消费其输出。")
    add("- 本批只覆盖 00505/GuildagitGroundDummy、00160/BG_Planet_Prepab、00502 全部根；00160 另一个 FBX 导入根 `BG_Planet`（30 MeshRenderer）按 README:18 未并入。")
    add("- 00486、00504 及 00505 其余 5 个根不在本任务交付范围（其网格依赖大多缺失，见 README:14-16）。")
    add("- Rotator 输出复刻 UE `FQuat::Rotator()`（UnrealMath.cpp:636-679，双精度公式同 float 版），奇异分支 yaw 归一化到 [0,360)，与 UE 一致。")
    add("- 非均匀缩放+旋转嵌套的实例若疑似剪切（§4），关卡侧建议整块 4x4 重建层级或按分量核对，不要只取 loc/rot/scale。")
    add("")
    return "\n".join(lines)


def aggregate_stats(records):
    mr_total = 0
    mapped = 0
    unmapped = 0
    distinct = set()
    for record, _extras, _root, _b in records:
        st = record["instance_stats"]
        mr_total += st["mesh_renderer_total"]
        mapped += st["mesh_renderer_mapped"]
        unmapped += st["mesh_renderer_unmapped"]
        distinct.update(_extras["distinct_ue_mesh_ids"])
    return {"mr_total": mr_total, "mapped": mapped, "unmapped": unmapped, "distinct_meshes": distinct}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--audit-dir", default=DEFAULT_AUDIT_DIR)
    ap.add_argument("--catalog", default=DEFAULT_CATALOG)
    ap.add_argument("--out", default=DEFAULT_OUT_DIR)
    args = ap.parse_args(argv)
    written = build(args.audit_dir, args.catalog, args.out)
    print("written %d files:" % len(written))
    for p in written:
        print("  %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
