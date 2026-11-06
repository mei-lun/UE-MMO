"""M8-010: bamboo map scene-pixel -> UE world calibration (pure, offline).

Task card: Docs/Tasks/M8-010.md. Input data:
Data/DNFMReplica/verify/map_bamboo.json (calibration_inputs / ground /
tile_layers / objects_by_layer / walkability_fields).

Zero UE writes, zero DNFM writes. Deterministic: every number below is a pure
function of the constants and the input JSON.

Axis mapping decision (code evidence, see bamboo_calibration.json "evidence"):
  scene x-right -> UE +X (no mirror; camera right = +X at fixed yaw -90)
  scene y-down  -> UE +Y (depth lane; larger y = nearer the viewer;
                   S key -> +Y in PlanarMovement.h, camera sits on +Y side)
  scene z       -> UE +Z (elevation, up-positive; preview projects
                   canvasY = y - z, so larger z draws higher on screen)
  scale s = 1.7615 cm/px (M6-022 mandate; exact calibration 192/109 = 1.761468)

Anchor: scene pixel (0, 0, 0) maps to UE (0, 0, 0). PROVISIONAL: the DNFM
controller origin (originX/originY) is unrecovered (map_bamboo.json gap G5),
so only coordinate DIFFERENCES are meaningful until M8-012 pins the world
anchor. The anchor is isolated in px_to_world so it can be swapped in one place.

Two tile-placement readings are carried (see unknowns U3):
  primary ("P", task card / M8-005 JSON): tile top y = floorHeight - 80 - originY
          = 40 (y-down), tile canvas y in [40, 640], floor line y = 120.
  variant ("Q", generator code as written): tile top y = 80 - floorHeight = -40
          (render_dnf_map_preview.py:33, q6_compose_layers.py:40), tile canvas
          y in [-40, 560], floor line y = tile_top + 80 = 40.
Objects and spawn points are identical in both readings (they do not move);
only the tile band / floor line / passage-grid rows shift by 80 px = 140.92 cm.
"""

from __future__ import annotations

import json
import math
import os
import sys
from datetime import datetime

# ---------------------------------------------------------------------------
# Mandated constants (Docs/Tasks/M8-010.md + established M8 facts)
# ---------------------------------------------------------------------------

SCALE_CM_PER_PX = 1.7615                 # M6-022 mandate (fighter calibration)
M6_022_EXACT_SCALE = 192.0 / 109.0       # 1.761468... (M6-022 report line 23)

FLOOR_HEIGHT_PX = 120                    # STTileScript field, both unique tiles
TILE_W_PX = 224
TILE_H_PX = 600
TILE_COUNT = 4                           # base_tile_instance_count
GROUND_X_MIN_PX = 0
GROUND_X_MAX_PX = TILE_W_PX * TILE_COUNT  # 896

TILE_TOP_PRIMARY_PX = FLOOR_HEIGHT_PX - 80        # 40  (reading P, y-down)
TILE_TOP_PREVIEW_VARIANT_PX = 80 - FLOOR_HEIGHT_PX  # -40 (reading Q, generator)
FLOOR_LINE_OFFSET_PX = 80                # tile top hangs 80 px from floor line

GRID_ROWS = 30
GRID_COLS = 14
CELL_W_PX = TILE_W_PX // GRID_COLS       # 16  (geometric inference, U5)
CELL_H_PX = TILE_H_PX // GRID_ROWS       # 20  (geometric inference, U5)

GRID_VALUE_ROWS_0_9 = 2                  # rows 0-9 all 2 (both tiles identical)
GRID_VALUE_ROWS_10_29 = 0                # rows 10-29 all 0

DEFAULT_SCENE_PATH = os.path.abspath(os.path.join(
    os.path.dirname(__file__), '..', '..', 'Data', 'DNFMReplica', 'verify',
    'map_bamboo.json'))

BACKDROP_LAYERS = (1, 2, 4)              # DISTANTBACK / MIDDLEBACK / CLOSEBACK
GROUND_ADJACENT_LAYERS = (5, 6, 7)       # BOTTOM / NORMAL / CONTACT


# ---------------------------------------------------------------------------
# Pure conversion functions
# ---------------------------------------------------------------------------

def px_to_world(x_px: float, y_px: float, z_px: float = 0.0):
    """Scene pixel (x right, y down, z elevation) -> UE (X, Y, Z) cm.

    X_cm = s * x_px   (scene x-right -> UE +X, no mirror)
    Y_cm = s * y_px   (scene y-down -> UE +Y: depth toward the viewer)
    Z_cm = s * z_px   (scene z elevation, up-positive -> UE +Z; z=0 floor plane)

    Provisional anchor: scene (0,0,0) -> UE (0,0,0); controller origin
    unrecovered (unknown U1), only differences are meaningful.
    """
    return (SCALE_CM_PER_PX * x_px, SCALE_CM_PER_PX * y_px, SCALE_CM_PER_PX * z_px)


def world_to_px(x_cm: float, y_cm: float, z_cm: float = 0.0):
    """Exact inverse of px_to_world (same provisional anchor)."""
    return (x_cm / SCALE_CM_PER_PX, y_cm / SCALE_CM_PER_PX, z_cm / SCALE_CM_PER_PX)


def tile_top_y_px(variant: str = "P") -> int:
    """Tile image top edge in scene y-down pixels under a placement reading."""
    if variant == "P":
        return TILE_TOP_PRIMARY_PX
    if variant == "Q":
        return TILE_TOP_PREVIEW_VARIANT_PX
    raise ValueError(f"unknown variant {variant!r} (expected 'P' or 'Q')")


def tile_placement(index: int, origin=(0, 0), variant: str = "P"):
    """Native tile placement x=224*index+originX; y per reading (origin 0,0).

    Evidence: map_bamboo.json tile_native_placement (MapViewManager.LoadBaseTile
    native method 242386); reading P applies the formula in y-down space,
    reading Q negates it into the preview canvas (80 - floorHeight).
    """
    if not 0 <= index < TILE_COUNT:
        raise ValueError(f"tile index {index} outside 0..{TILE_COUNT - 1}")
    x = TILE_W_PX * index + origin[0]
    if variant == "P":
        y = FLOOR_HEIGHT_PX - 80 - origin[1]
    elif variant == "Q":
        y = 80 - FLOOR_HEIGHT_PX - origin[1]
    else:
        raise ValueError(f"unknown variant {variant!r}")
    return (x, y)


def ground_band(variant: str = "P") -> dict:
    """Ground tile band: scene px rect and its UE cm box (floor plane Z=0)."""
    top = tile_top_y_px(variant)
    x0 = px_to_world(GROUND_X_MIN_PX, 0.0)[0]
    x1 = px_to_world(GROUND_X_MAX_PX, 0.0)[0]
    _x, y0, _z = px_to_world(0.0, top)
    _x, y1, _z = px_to_world(0.0, top + TILE_H_PX)
    return {
        "variant": variant,
        "tile_top_y_px": top,
        "px": {"x": [GROUND_X_MIN_PX, GROUND_X_MAX_PX],
               "y": [top, top + TILE_H_PX]},
        "world_cm": {"x": [x0, x1], "y": [y0, y1], "z": [0.0, 0.0]},
    }


def floor_line(variant: str = "P") -> dict:
    """Floor reference line: the native placement hangs the tile top
    FLOOR_LINE_OFFSET_PX above the floorHeight line, so the walk plane meets
    scene y = floorHeight (reading P) or y = tile_top + 80 (reading Q).
    The plane itself is UE Z = 0; this is a Y (depth) coordinate, not a height.
    """
    y_px = tile_top_y_px(variant) + FLOOR_LINE_OFFSET_PX
    x, y, z = px_to_world(0.0, y_px, 0.0)
    return {"variant": variant, "y_px": y_px, "world_cm": {"x": x, "y": y, "z": z}}


def row_value(row_top: int) -> int:
    """Passage value of a grid row counted from the tile top (both tiles identical)."""
    if not 0 <= row_top < GRID_ROWS:
        raise ValueError(f"row {row_top} outside 0..{GRID_ROWS - 1}")
    return GRID_VALUE_ROWS_0_9 if row_top <= 9 else GRID_VALUE_ROWS_10_29


def spawn_row_top(y_px: float, variant: str = "P") -> int:
    """Grid row (counted from tile top) containing scene y."""
    return int(math.floor((y_px - tile_top_y_px(variant)) / CELL_H_PX))


def spawn_cell(y_px: float, x_px: float, variant: str = "P") -> dict:
    """Grid cell (row from top, row from bottom, col, value) for a scene point.

    Column mapping assumes the grid covers each tile canvas (16 px cells over
    224 px, geometric inference U5); tile index = x // 224.
    """
    r_top = spawn_row_top(y_px, variant)
    tile_index = int(x_px // TILE_W_PX)
    col = int((x_px - tile_index * TILE_W_PX) // CELL_W_PX)
    return {
        "tile_index": tile_index,
        "row_from_top": r_top,
        "row_from_bottom": GRID_ROWS - 1 - r_top,
        "col": col,
        "value": row_value(r_top),
    }


def walkable_y_range_px(hypothesis: str, variant: str = "P",
                        row_order: str = "top"):
    """Walkable scene-y range [min, max) in pixels for one hypothesis.

    hypothesis: 'A' = value 0 walkable / value 2 blocks,
                'B' = value 2 walkable / value 0 blocks.
    row_order:  'top' = grid row 0 at the tile top edge,
                'bottom' = grid row 0 at the tile bottom edge (unknown U4).
    The value pattern is row-uniform, so walkability is a pure Y band and X
    spans the whole ground strip.
    """
    if hypothesis not in ("A", "B"):
        raise ValueError(f"unknown hypothesis {hypothesis!r}")
    top = tile_top_y_px(variant)
    if row_order == "top":
        # rows 0-9: y in [top, top+200); rows 10-29: y in [top+200, top+600)
        lo2, hi2 = top, top + 10 * CELL_H_PX          # value 2 band
        lo0, hi0 = top + 10 * CELL_H_PX, top + GRID_ROWS * CELL_H_PX  # value 0 band
    elif row_order == "bottom":
        # rows 0-9 from bottom = top rows 20-29; rows 10-29 from bottom = top rows 0-19
        lo2, hi2 = top + 20 * CELL_H_PX, top + GRID_ROWS * CELL_H_PX
        lo0, hi0 = top, top + 20 * CELL_H_PX
    else:
        raise ValueError(f"unknown row_order {row_order!r}")
    return (lo0, hi0) if hypothesis == "A" else (lo2, hi2)


def walkability_band(hypothesis: str, variant: str = "P",
                     row_order: str = "top") -> dict:
    """Full world-space walkable band for one hypothesis / reading / row order."""
    lo_px, hi_px = walkable_y_range_px(hypothesis, variant, row_order)
    _x, y_lo, _z = px_to_world(0.0, lo_px)
    _x, y_hi, _z = px_to_world(0.0, hi_px)
    x_lo, x_hi = px_to_world(GROUND_X_MIN_PX, 0.0)[0], px_to_world(GROUND_X_MAX_PX, 0.0)[0]
    return {
        "hypothesis": hypothesis,
        "tile_placement_variant": variant,
        "grid_row_order": row_order,
        "walkable": {
            "x_cm": [x_lo, x_hi],
            "y_cm": [y_lo, y_hi],
            "z_cm": 0.0,
            "y_px_range": [lo_px, hi_px],
        },
        "blocked": _blocked_band(hypothesis, variant, row_order),
    }


def _blocked_band(hypothesis: str, variant: str, row_order: str) -> dict:
    """The complementary Y band(s) inside the ground strip (blocked for walking)."""
    top = tile_top_y_px(variant)
    lo_px, hi_px = walkable_y_range_px(hypothesis, variant, row_order)
    bands = []
    if lo_px > top:
        _x, y0, _z = px_to_world(0.0, top)
        _x, y1, _z = px_to_world(0.0, lo_px)
        bands.append({"y_cm": [y0, y1], "y_px_range": [top, lo_px]})
    if hi_px < top + TILE_H_PX:
        _x, y0, _z = px_to_world(0.0, hi_px)
        _x, y1, _z = px_to_world(0.0, top + TILE_H_PX)
        bands.append({"y_cm": [y0, y1], "y_px_range": [hi_px, top + TILE_H_PX]})
    return {"y_cm_bands": bands,
            "x_cm": [px_to_world(GROUND_X_MIN_PX, 0.0)[0],
                     px_to_world(GROUND_X_MAX_PX, 0.0)[0]]}


def spawn_standable_counts(scene: dict, hypothesis: str, variant: str,
                           row_order: str) -> int:
    """How many of the 8 monster+rival spawn points fall in walkable cells."""
    count = 0
    points = ([tuple(p) for p in scene["calibration_inputs"]["monster_spawn_points"]]
              + [tuple(p) for p in scene["calibration_inputs"]["rival_spawn_points"]])
    for x, y, _z in points:
        if spawn_cell(y, x, variant)["row_from_top"] in _walkable_rows(hypothesis, row_order):
            count += 1
    return count


def _walkable_rows(hypothesis: str, row_order: str):
    """Grid rows (counted from tile top) that are walkable under a hypothesis."""
    if row_order == "top":
        rows_top = range(10, 30) if hypothesis == "A" else range(0, 10)
    else:
        rows_top = range(0, 20) if hypothesis == "A" else range(20, 30)
    return set(rows_top)


# ---------------------------------------------------------------------------
# Scene loading and prediction tables
# ---------------------------------------------------------------------------

def load_scene(path: str = DEFAULT_SCENE_PATH) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def _layer_group(draw_layer: int) -> str:
    if draw_layer in BACKDROP_LAYERS:
        return "backdrop"
    if draw_layer in GROUND_ADJACENT_LAYERS:
        return "ground_adjacent"
    return "other"


def object_world_predictions(scene: dict) -> list:
    """One entry per animation object (50): anchor + image-foot predictions.

    anchor_px = STAssignAnimation position x,y,z (raw scene pixels).
    foot_px   = composed first-frame canvas rect: bottom-center x
                (top_left[0] + w/2) and bottom edge y (top_left[1] + h) --
                the mandated foot approximation (map_bamboo.json
                calibration_inputs.foot_point_definition).
    World values use px_to_world (y-down -> +Y, z=0 -> floor plane Z=0).
    Backdrop layers (1/2/4) get the same flat-floor projection baseline; their
    in-world placement as billboards is M8-012's decision (unknown U8).
    """
    foot_by_index = {entry["index"]: entry
                     for entry in scene["calibration_inputs"]["object_foot_points"]}
    predictions = []
    for layer_key in sorted(scene["objects_by_layer"], key=int):
        for obj in scene["objects_by_layer"][layer_key]["objects"]:
            index = obj["index"]
            frame = obj["first_frame"]
            top_left = frame["top_left"]
            size = frame["image_size_scaled"]
            foot_x = top_left[0] + size[0] / 2.0
            foot_y = top_left[1] + size[1]
            pos_x, pos_y, pos_z = obj["position_xyz"]
            ax, ay, az = px_to_world(pos_x, pos_y, pos_z)
            fx, fy, fz = px_to_world(foot_x, foot_y, pos_z)
            recorded = foot_by_index[index]
            predictions.append({
                "index": index,
                "draw_layer": obj["draw_layer"],
                "layer_group": _layer_group(obj["draw_layer"]),
                "script_block": obj["script_block"],
                "anchor_px": {"x": pos_x, "y": pos_y, "z": pos_z},
                "frame_top_left_px": {"x": top_left[0], "y": top_left[1]},
                "frame_size_px": {"w": size[0], "h": size[1]},
                "foot_px": {"x": foot_x, "y": foot_y},
                "world_anchor_cm": {"x": ax, "y": ay, "z": az},
                "world_foot_cm": {"x": fx, "y": fy, "z": fz},
                "recorded_bottom_y": recorded["first_frame_image_bottom_y"],
            })
    predictions.sort(key=lambda entry: entry["index"])
    return predictions


def _spawn_entries(points, source: str) -> list:
    entries = []
    for x, y, z in points:
        wx, wy, wz = px_to_world(x, y, z)
        cell_p = spawn_cell(y, x, "P")
        cell_q = spawn_cell(y, x, "Q")
        entries.append({
            "source": source,
            "px": {"x": x, "y": y, "z": z},
            "world_cm": {"x": wx, "y": wy, "z": wz},
            "grid_cell_variant_P": cell_p,
            "grid_cell_variant_Q": cell_q,
        })
    return entries


def spawn_world_predictions(scene: dict) -> dict:
    """Monster / rival / passive spawn points -> UE cm + grid cells (both readings).

    Spawn points carry no animation frame, so their canvas-equivalent vertical
    is y - z (render_dnf_map_preview.py:56); all bamboo z = 0.
    """
    ci = scene["calibration_inputs"]
    return {
        "monster_spawn_predictions": _spawn_entries(
            [tuple(p) for p in ci["monster_spawn_points"]], "STAssignMonster"),
        "rival_spawn_predictions": _spawn_entries(
            [tuple(p) for p in ci["rival_spawn_points"]], "STRivalMapSpawnData"),
        "passive_object_predictions": _spawn_entries(
            [tuple(p) for p in ci["passive_object_points"]], "STAssignPassiveObject"),
    }


# ---------------------------------------------------------------------------
# Calibration document assembly
# ---------------------------------------------------------------------------

def _r(value, digits: int = 6):
    return round(value, digits)


def _round_tree(node):
    if isinstance(node, float):
        return _r(node)
    if isinstance(node, dict):
        return {key: _round_tree(val) for key, val in node.items()}
    if isinstance(node, list):
        return [_round_tree(item) for item in node]
    return node


def build_calibration_document(scene: dict, generated_at: str) -> dict:
    """Assemble the full calibration document (pure function of inputs + timestamp)."""
    hypotheses = []
    for hypothesis in ("A", "B"):
        variants = []
        for variant in ("P", "Q"):
            for row_order in ("top", "bottom"):
                band = walkability_band(hypothesis, variant, row_order)
                band["spawn_points_standable_of_8"] = spawn_standable_counts(
                    scene, hypothesis, variant, row_order)
                variants.append(band)
        primary = variants[0]
        hypotheses.append({
            "id": hypothesis,
            "meaning": ("value 0 walkable, value 2 blocks" if hypothesis == "A"
                        else "value 2 walkable, value 0 blocks"),
            "primary_band": primary,
            "variants": variants,
        })

    ground = {
        "primary": ground_band("P"),
        "preview_variant": ground_band("Q"),
        "floor_line": {"primary": floor_line("P"), "preview_variant": floor_line("Q")},
        "floor_z_cm": 0.0,
        "z_derivation": (
            "The tile's 600 px image height is projected depth-face (canvas y), "
            "not UE height: the preview projects canvasY = y - z, monsters and "
            "interactables sit at z=0 across y=201..261 with no vertical spread, "
            "and the walk plane is flat. Walk plane Z = 0 cm; scene z (all 0 in "
            "this map) maps to UE Z = s * z. Tile band world Y span "
            "[70.46, 1127.36] cm (primary reading); floor line Y = 211.38 cm."),
        "tile_placement_formula": {
            "native_evidence": "x=224*index+originX; unityY=floorHeight-80-originY "
                               "(MapViewManager.LoadBaseTile method 242386)",
            "origin": [0, 0],
            "origin_status": "controller origin unrecovered (gap G5); offline preview used (0,0)",
            "primary_tile_tops_px": [tile_placement(i, variant="P")[1] for i in range(TILE_COUNT)],
            "preview_variant_tile_tops_px": [tile_placement(i, variant="Q")[1] for i in range(TILE_COUNT)],
        },
        "cell_size": {"w_px": CELL_W_PX, "h_px": CELL_H_PX,
                      "w_cm": _r(CELL_W_PX * SCALE_CM_PER_PX),
                      "h_cm": _r(CELL_H_PX * SCALE_CM_PER_PX),
                      "status": "geometric inference 224/14, 600/30; not native (gap G2)"},
    }

    axis_mapping = {
        "scene_x_right_to": "UE +X",
        "scene_y_down_to": "UE +Y",
        "scene_z_to": "UE +Z",
        "mirror": "none for map placement (facing mirror is sprite-only)",
        "decision_evidence": [
            "Source/UEMMO/Character/SideViewCameraComponent.h:101-102,158-168 - fixed rig "
            "yaw=-90 pitch=-18; yaw -90 => camera forward=(0,-1,0), camera right=+X, "
            "spring arm puts the camera on the +Y side looking toward -Y, so larger "
            "world Y is nearer the viewer and lower on the ground plane.",
            "Source/UEMMO/Character/PlanarMovement.h:6-7,18-19,41-52 - axis convention "
            "X horizontal / Y depth / Z vertical; AxisY 'S positive, W negative'; "
            "ComputePlanarVelocity returns FVector(AxisX, AxisY, 0).",
            "Source/UEMMO/PrototypeCharacter.cpp:185-193 - the planar vector is fed to "
            "AddMovementInput as a world-space direction, so S drives world +Y.",
            "Source/UEMMO/Character/DNFMCharacterVisualComponent.cpp:39-51,470-476,500-514 - "
            "canvas x -> camera Right * Facing, canvas y-down -> -Up (image up = world up); "
            "camera basis from the fixed rig (EAxis::Y right, EAxis::Z up); "
            "yaw 0 faces right. Screen vertical belongs to Z/Up, depth to Y.",
            "DNFM preview projection canvasY = mapY - mapZ "
            "(render_dnf_map_preview.py:56): larger z draws higher on screen => "
            "z is up-positive elevation.",
        ],
        "corollary": ("scene y-down maps to +Y, NOT -Y: mapping it to -Y would invert "
                      "the W/S depth keys relative to the camera side and flip which "
                      "end of the ground band is near the viewer."),
    }

    scale = {
        "cm_per_px": SCALE_CM_PER_PX,
        "source": "M6-022 fighter calibration, mandated by Docs/Tasks/M8-010.md",
        "m6_022_exact": _r(M6_022_EXACT_SCALE, 9),
        "m6_022_exact_expression": "192 cm / 109 px",
        "delta_vs_exact_cm_per_px": _r(SCALE_CM_PER_PX - M6_022_EXACT_SCALE, 9),
        "map_width_cm": _r(GROUND_X_MAX_PX * SCALE_CM_PER_PX),
        "tile_w_cm": _r(TILE_W_PX * SCALE_CM_PER_PX),
        "tile_h_px_as_depth_note": ("tile 600 px height is projected depth face, "
                                    "NOT 1056.9 cm of UE height (see ground_band.z_derivation)"),
    }

    assertions = [
        {"id": "A1", "kind": "axis_mapping_depth_keys",
         "action": "Stand on a monster spawn, press S then W.",
         "expect": "S moves the character down-screen toward the viewer with UE Y "
                   "increasing (dY > 0); W moves up-screen with Y decreasing (dY < 0); "
                   "dX = 0 for both.",
         "numeric": {"s_key_delta_y_cm": "+", "w_key_delta_y_cm": "-"},
         "rejects": "scene y-down -> -Y mapping"},
        {"id": "A2", "kind": "scale_reference",
         "action": "Compare fighter height with one ground tile width.",
         "expect": "tile width 224 px = 394.576 cm ~= 2.055 x fighter 192 cm; "
                   "ground strip 896 px = 1578.304 cm = 4 tiles.",
         "numeric": {"tile_width_cm": 394.576, "ground_strip_cm": 1578.304}},
        {"id": "A3", "kind": "ground_plane",
         "action": "Walk the walkable band; watch the capsule bottom.",
         "expect": "Capsule bottom rests at Z=0 across the band; ground texture is a "
                   "camera-facing/depth projection, no steps. Floor line (primary "
                   "reading) at Y=211.38 cm; tile band Y in [70.46, 1127.36] cm.",
         "numeric": {"floor_z_cm": 0.0, "floor_line_y_cm_primary": 211.38,
                     "tile_band_y_cm_primary": [70.46, 1127.36]}},
        {"id": "A4", "kind": "walkability_hypothesis_adjudication",
         "action": "From monster spawn 3 (px 622,260 -> world 1095.653, 457.99, 0) walk "
                   "W (-Y) and S (+Y) under the PRIMARY reading (tile top 40, rows from top).",
         "expect_hypothesis_A": "movement blocked near Y=422.76 cm (row 10 boundary); "
                                "S to Y<=1127.36 cm allowed.",
         "expect_hypothesis_B": "spawn area itself blocked (row 11 = value 0 = wall); "
                                "walkable only Y in [70.46, 422.76) cm.",
         "numeric": {"hypothesis_A_walkable_y_cm": [422.76, 1127.36],
                     "hypothesis_B_walkable_y_cm": [70.46, 422.76]}},
        {"id": "A5", "kind": "spawn_standability",
         "action": "Check whether each of the 4 monster + 4 rival spawn points is "
                   "standable (primary reading, rows from top).",
         "expect": "hypothesis A: exactly 2 of 8 standable (monsters at px y=260,261); "
                   "hypothesis B: exactly 6 of 8 standable (both y=201 monsters + all rivals).",
         "numeric": {"hypothesis_A_standable": 2, "hypothesis_B_standable": 6}},
        {"id": "A6", "kind": "x_bounds",
         "action": "Walk A/D to the map edges from any walkable cell.",
         "expect": "movement stops at X<0 and X>=1578.304 cm (tile ground ends; "
                   "x<0 / x>896 px content is backdrop/fog, not floor).",
         "numeric": {"min_x_cm": 0.0, "max_x_cm": 1578.304}},
        {"id": "A7", "kind": "tile_reading_adjudication",
         "action": "Compare the character's feet with the ground texture at a spawn point.",
         "expect": "if feet meet the ground texture at floor line Y=211.38 cm the "
                   "primary reading (P, tile top y=40) holds; if the texture is "
                   "140.92 cm (80 px) off, the preview-generator reading (Q, tile top "
                   "y=-40, floor line Y=70.46) holds.",
         "numeric": {"floor_line_y_cm_P": 211.38, "floor_line_y_cm_Q": 70.46,
                     "delta_cm": 140.92}},
        {"id": "A8", "kind": "variant_cross_check",
         "action": "Count standable spawn points per reading/row-order (see "
                   "walkability_hypotheses[].variants[].spawn_points_standable_of_8).",
         "expect": "3 of 4 reading x row-order combos put ALL 8 spawn points in value-0 "
                   "cells (A walkable); only P+top splits them (monsters 2/0, rivals 2). "
                   "A map where nothing is walkable (B outside P+top) is implausible.",
         "numeric": {"P_top": {"A": 2, "B": 6}, "P_bottom": {"A": 8, "B": 0},
                     "Q_top": {"A": 8, "B": 0}, "Q_bottom": {"A": 8, "B": 0}}},
    ]

    evidence = [
        {"claim": "scale s=1.7615 cm/px (exact 192/109=1.761468)",
         "refs": ["Docs/Tasks/Reports/M6-022.md:23 (109px/192cm/s=1.761468)",
                  "Source/UEMMO/PrototypeCharacter.cpp:2529-2533 (SetWorldScaleFromStandingHeight)"]},
        {"claim": "camera fixed rig yaw=-90 pitch=-18; camera right=+X; camera on +Y side",
         "refs": ["Source/UEMMO/Character/SideViewCameraComponent.h:101-102",
                  "Source/UEMMO/Character/SideViewCameraComponent.h:158-168"]},
        {"claim": "depth input S->+Y, W->-Y, applied as world XY direction",
         "refs": ["Source/UEMMO/Character/PlanarMovement.h:18-19",
                  "Source/UEMMO/Character/PlanarMovement.h:41-52",
                  "Source/UEMMO/PrototypeCharacter.cpp:185-193"]},
        {"claim": "canvas x -> Right*Facing, canvas y-down -> -Up, single mirror, foot anchor",
         "refs": ["Source/UEMMO/Character/DNFMCharacterVisualComponent.cpp:39-51",
                  "Source/UEMMO/Character/DNFMCharacterVisualComponent.cpp:470-476",
                  "Source/UEMMO/Character/DNFMCharacterVisualComponent.cpp:500-514"]},
        {"claim": "tile placement native formula + origin unrecovered",
         "refs": ["Data/DNFMReplica/verify/map_bamboo.json:813 (tile_native_placement)",
                  "D:/Github-Poj/DNFM/DNFM_export_130.5.8.0/tools/render_dnf_map_preview.py:77 "
                  "(tile_placement_evidence, method 242386)"]},
        {"claim": "generator code places tiles at y=80-floorHeight (= -40), objects at y-z",
         "refs": ["D:/Github-Poj/DNFM/DNFM_export_130.5.8.0/tools/render_dnf_map_preview.py:33",
                  "D:/Github-Poj/DNFM/DNFM_export_130.5.8.0/tools/render_dnf_map_preview.py:56",
                  "Artifacts/Tasks/M8-005/2026-11-07T0030+0800/scripts/q6_compose_layers.py:40"]},
        {"claim": "official preview bounds [-239,-284,2842,560] imply tiles at canvas y [-40,560]",
         "refs": ["restored/maps/previews/map_013_00595.json (bounds; tile bottom -40+600=560)"]},
        {"claim": "task-card reading tile top y=40, tile canvas [40,640]",
         "refs": ["Data/DNFMReplica/verify/map_bamboo.json:1308 (ground_strip_y_top_pixels=40)",
                  "Data/DNFMReplica/verify/map_bamboo.json:1309-1313 (tile_canvas_y_range_inferred)"]},
        {"claim": "passage grid 30x14, rows 0-9=2, rows 10-29=0, both tiles identical",
         "refs": ["Data/DNFMReplica/verify/map_bamboo.json:3668-3681 (tile_passage_grid_field)",
                  "Artifacts/Tasks/M8-005/2026-11-07T0030+0800/db_queries/q4_grids.json "
                  "(both 30x14 grids verified)"]},
        {"claim": "ground strip x [0,896] px, tile PNG 224x600, floor_height=120",
         "refs": ["Data/DNFMReplica/verify/map_bamboo.json:1304-1315 (ground)",
                  "Data/DNFMReplica/verify/map_bamboo.json:3469-3589 (tile_layers)"]},
    ]

    unknowns = [
        {"id": "U1", "topic": "controller_origin",
         "detail": "DNFM controller origin (originX/originY) unrecovered (map_bamboo.json "
                   "gap G5); the anchor scene(0,0,0)->UE(0,0,0) is provisional. Only "
                   "coordinate differences are meaningful until M8-012 pins the anchor.",
         "handling": "anchor isolated in px_to_world; all bands/tables report differences "
                     "that survive any translation"},
        {"id": "U2", "topic": "passage_value_semantics",
         "detail": "0/2 meanings unrecovered (gap G2).",
         "handling": "both hypotheses A (0 walkable) and B (2 walkable) carried with "
                     "full numeric bands and spawn cross-checks"},
        {"id": "U3", "topic": "tile_y_top_reading",
         "detail": "Task card / M8-005 JSON record tile top y = floorHeight-80-originY = 40 "
                   "(y-down), but the offline preview generator code computes "
                   "y = 80-floorHeight = -40 (render_dnf_map_preview.py:33, "
                   "q6_compose_layers.py:40); official preview bounds maxy=560 = -40+600 "
                   "match the code. The two readings shift the tile band and floor line "
                   "by 80 px = 140.92 cm relative to objects/spawns.",
         "handling": "primary = P (task card mandate, tile top 40, floor line 120); "
                     "variant Q carried in full; M8-012 adjudicates (assertion A7)"},
        {"id": "U4", "topic": "grid_row_order",
         "detail": "Whether grid row 0 counts from the tile top or bottom edge is not "
                   "native-evidenced.",
         "handling": "row_order 'top'/'bottom' both carried per variant"},
        {"id": "U5", "topic": "grid_cell_size",
         "detail": "16x20 px cell size is geometric inference (224/14, 600/30), not native "
                   "(gap G2).",
         "handling": "used for row/col mapping only; flagged in every derived cell"},
        {"id": "U6", "topic": "scene_z_semantics",
         "detail": "z = up-positive elevation is inferred from the y-z projection; every "
                   "bamboo z is 0, so the sign is untestable offline.",
         "handling": "Z_cm = s*z recorded as convention; assertion A3 tests the flat floor"},
        {"id": "U7", "topic": "floor_line_semantics",
         "detail": "'floor line = floorHeight = tile top + 80' derives from the placement "
                   "formula; whether the 80 px offset hangs the image above or below the "
                   "floor line is not independently evidenced.",
         "handling": "carried inside the P/Q readings; assertion A7 decides"},
        {"id": "U8", "topic": "backdrop_foot_points",
         "detail": "For backdrop layers (1/2/4) the canvas foot y conflates depth and "
                   "height; the flat-floor projection baseline is not their final world "
                   "placement.",
         "handling": "table marks layer_group=backdrop; M8-012 places billboards by layer"},
        {"id": "U9", "topic": "scale_rounding",
         "detail": "Mandated s=1.7615 vs M6-022 exact 192/109=1.761468; delta 3.21e-05 "
                   "cm/px (~0.006 cm over the 896 px map).",
         "handling": "mandated value used; delta recorded"},
        {"id": "U10", "topic": "native_axis_names",
         "detail": "The decompiled 'unityY' naming (method 242386) does not fully recover "
                   "the original runtime's axis senses; this calibration anchors to UE "
                   "conventions and this project's code, not to DNFM runtime internals.",
         "handling": "documented; does not affect the UE-side predictions"},
    ]

    document = {
        "assertions": assertions,
        "axis_mapping": axis_mapping,
        "evidence": evidence,
        "generated_at": generated_at,
        "ground_band": ground,
        "inputs": {
            "scene_json": "Data/DNFMReplica/verify/map_bamboo.json",
            "scene_payload_sha256": scene["identity"]["payload_sha256"],
            "maps_hash": scene["identity"]["maps_hash"],
            "task_card": "Docs/Tasks/M8-010.md",
        },
        "object_world_predictions": object_world_predictions(scene),
        "scale": scale,
        "spawn_world_predictions": spawn_world_predictions(scene),
        "unknowns": unknowns,
        "walkability_hypotheses": hypotheses,
    }
    return _round_tree(document)


# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------

def _close(a: float, b: float) -> bool:
    return math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-9)


def _vec_close(a, b) -> bool:
    return all(_close(x, y) for x, y in zip(a, b))


def run_self_test(scene: dict) -> int:
    """Every assertion raises AssertionError on failure; returns the count run."""
    checks = 0

    # 1. scale provenance
    assert abs(SCALE_CM_PER_PX - M6_022_EXACT_SCALE) < 1e-4
    checks += 1

    # 2. px_to_world basics + inverse round trip
    assert _vec_close(px_to_world(0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
    assert _vec_close(px_to_world(896.0, 640.0, 0.0), (1578.304, 1127.36, 0.0))
    assert _vec_close(px_to_world(659.0, 201.0, 0.0), (1160.8285, 354.0615, 0.0))
    assert _vec_close(world_to_px(*px_to_world(123.0, -45.5, 7.25)), (123.0, -45.5, 7.25))
    checks += 4

    # 3. ground formula reproduction, both readings
    assert [tile_placement(i, variant="P")[0] for i in range(4)] == [0, 224, 448, 672]
    assert all(tile_placement(i, variant="P")[1] == 40 for i in range(4))
    assert all(tile_placement(i, variant="Q")[1] == -40 for i in range(4))
    assert tile_placement(3, variant="P")[0] + TILE_W_PX == GROUND_X_MAX_PX == 896
    checks += 4

    # 4. ground band cm values
    band_p = ground_band("P")
    assert band_p["px"]["y"] == [40, 640]
    assert _vec_close(band_p["world_cm"]["x"], (0.0, 1578.304))
    assert _vec_close(band_p["world_cm"]["y"], (70.46, 1127.36))
    band_q = ground_band("Q")
    assert band_q["px"]["y"] == [-40, 560]
    assert _vec_close(band_q["world_cm"]["y"], (-70.46, 986.44))
    assert _close(floor_line("P")["world_cm"]["y"], 211.38)
    assert _close(floor_line("Q")["world_cm"]["y"], 70.46)
    checks += 7

    # 5. passage grid pattern + census against the scene JSON
    census = {0: 0, 2: 0}
    for row in range(GRID_ROWS):
        value = row_value(row)
        census[value] += GRID_COLS
        assert value == (2 if row <= 9 else 0)
    assert census == {0: 280, 2: 140}
    scene_census = scene["tile_layers"][0]["passage_value_census"]
    assert scene_census == {"0": 280, "2": 140}
    assert scene["tile_layers"][0]["passage_grid_rows"] == 30
    assert scene["tile_layers"][0]["passage_grid_cols"] == 14
    assert all(layer["passage_value_census"] == scene_census for layer in scene["tile_layers"])
    checks += 6

    # 6. walkability bands (all 8 combos), exact cm values
    expected = {
        ("A", "P", "top"): (422.76, 1127.36),
        ("A", "P", "bottom"): (70.46, 775.06),
        ("B", "P", "top"): (70.46, 422.76),
        ("B", "P", "bottom"): (775.06, 1127.36),
        ("A", "Q", "top"): (281.84, 986.44),
        ("A", "Q", "bottom"): (-70.46, 634.14),
        ("B", "Q", "top"): (-70.46, 281.84),
        ("B", "Q", "bottom"): (634.14, 986.44),
    }
    for (hyp, variant, order), (lo, hi) in expected.items():
        band = walkability_band(hyp, variant, order)
        assert _vec_close(band["walkable"]["y_cm"], (lo, hi)), (hyp, variant, order)
        assert _vec_close(band["walkable"]["x_cm"], (0.0, 1578.304))
        blocked_total = sum(hi_cm - lo_cm for band_cm in band["blocked"]["y_cm_bands"]
                            for lo_cm, hi_cm in [band_cm["y_cm"]])
        assert _close(blocked_total, 1056.9 - (hi - lo))  # 600 px = 1056.9 cm total
    checks += len(expected) * 3

    # 7. spawn cell mapping (primary reading)
    assert spawn_cell(201, 659, "P") == {"tile_index": 2, "row_from_top": 8,
                                         "row_from_bottom": 21, "col": 13, "value": 2}
    assert spawn_cell(260, 622, "P")["value"] == 0
    assert spawn_cell(201, 659, "Q")["row_from_top"] == 12
    assert spawn_cell(201, 659, "Q")["value"] == 0
    checks += 4

    # 8. spawn standability counts per combo
    counts = {(h, v, o): spawn_standable_counts(scene, h, v, o)
              for h in ("A", "B") for v in ("P", "Q") for o in ("top", "bottom")}
    assert counts == {("A", "P", "top"): 2, ("A", "P", "bottom"): 8,
                      ("A", "Q", "top"): 8, ("A", "Q", "bottom"): 8,
                      ("B", "P", "top"): 6, ("B", "P", "bottom"): 0,
                      ("B", "Q", "top"): 0, ("B", "Q", "bottom"): 0}, counts
    checks += 1

    # 9. object foot points: 50 objects, data integrity vs recorded bottom y
    predictions = object_world_predictions(scene)
    assert len(predictions) == 50, len(predictions)
    assert len({entry["index"] for entry in predictions}) == 50
    for entry in predictions:
        assert _close(entry["foot_px"]["y"], entry["recorded_bottom_y"]), entry["index"]
        assert _vec_close((entry["world_foot_cm"]["x"], entry["world_foot_cm"]["y"]),
                          px_to_world(entry["foot_px"]["x"], entry["foot_px"]["y"])[:2])
    layer_counts = {}
    for entry in predictions:
        layer_counts[entry["draw_layer"]] = layer_counts.get(entry["draw_layer"], 0) + 1
    assert layer_counts == {1: 3, 2: 8, 4: 1, 5: 13, 6: 20, 7: 5}
    checks += 4

    # 10. spawn predictions: 4 monsters + 4 rivals + 18 passives, world values
    spawns = spawn_world_predictions(scene)
    assert len(spawns["monster_spawn_predictions"]) == 4
    assert len(spawns["rival_spawn_predictions"]) == 4
    assert len(spawns["passive_object_predictions"]) == 18
    first = spawns["monster_spawn_predictions"][0]
    assert first["px"] == {"x": 659, "y": 201, "z": 0}
    assert _vec_close((first["world_cm"]["x"], first["world_cm"]["y"]),
                      (1160.8285, 354.0615))
    checks += 4

    # 11. document assembly: key set + deterministic serialization
    document = build_calibration_document(scene, "2026-11-07T02:30:00+08:00")
    required_keys = {"assertions", "axis_mapping", "evidence", "generated_at",
                     "ground_band", "inputs", "object_world_predictions", "scale",
                     "spawn_world_predictions", "unknowns", "walkability_hypotheses"}
    assert required_keys <= set(document)
    assert len(document["assertions"]) == 8
    assert len(document["unknowns"]) == 10
    assert len(document["walkability_hypotheses"]) == 2
    a_bytes = json.dumps(document, sort_keys=True, ensure_ascii=False, indent=2)
    b_bytes = json.dumps(build_calibration_document(scene, "2026-11-07T02:30:00+08:00"),
                         sort_keys=True, ensure_ascii=False, indent=2)
    assert a_bytes == b_bytes
    checks += 4

    return checks


def main(argv) -> int:
    emit_path = None
    args = list(argv[1:])
    if "--emit" in args:
        emit_path = args[args.index("--emit") + 1]
        args = [a for i, a in enumerate(args) if not (a == "--emit" or i == args.index("--emit") + 1)]
    scene = load_scene(args[0] if args else DEFAULT_SCENE_PATH)
    count = run_self_test(scene)
    print(f"M8-010 bamboo_px_to_world self-test: {count} checks passed")
    if emit_path:
        document = build_calibration_document(
            scene, datetime.now().astimezone().isoformat(timespec="seconds"))
        with open(emit_path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(document, handle, sort_keys=True, ensure_ascii=False, indent=2)
            handle.write("\n")
        print(f"wrote {emit_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
