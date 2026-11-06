#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M8-018: offline monster visual-set compositor (DNFM P04-C, offline part).

Builds the paged RGBA atlas + manifest for DNFM monsters from their M8-007 /
M8-008 verify JSONs, with the same manifest schema (manifest_version 2) as the
M8-016 character pipeline (Scripts/DNFM/prepare_character_visual_set.py, which
this script imports for its verified primitives and never modifies):
AtlasLibrary (mode=ro sqlite, y-flipped crops, reference following, palette
decode), AnimationStore (script_index -> animations sha cross-check),
pack_pages (row-major shelf, <= 2048 pages), build_preview (strip with pivot
baseline) and the per-frame manifest field names.

Monster differences vs the character path (all recorded in the manifest):
  * single layer: a monster has no outfit layers; each animation frame
    references exactly one image (the body atlas for pose actions, a class
    effect overlay img for pure-effect actions such as taubeast
    charge/warning) or nothing at all.
  * frames come from the monster verify JSON (M8-007/M8-008), which is itself
    cross-checked frame-by-frame against a fresh read of the restored
    animation DB (delay/position/scale/rotation/color/flip/image refs).
  * transforms: monsters carry genuine per-frame uniform scale (charge 2.0,
    warning 1.7) and per-frame color_rgba tint (warning red 255,0,0,255);
    both are baked into the cell and recorded per frame.  Rotation (overturn
    179.91 deg) follows the M8-016 technique.
  * anchor convention: per-frame negated position.  The frame position places
    the layer canvas so canvas point p lands at position-space p+position
    (identical to M8-016); the feet/action anchor is therefore the canvas
    point (-position).  Transforms apply about that per-frame anchor and the
    anchor always lands exactly at position-space (0,0) = pivot_px.  Basis:
    the goblin body frames share position [-91,-152] (anchor at canvas
    (91,152), ground contact +8 px below the anchor) while the overturn frame
    shifts position to [-95,-97]; a fixed canvas anchor would bury the flipped
    body ~127 px below the anchor, while the per-frame anchor keeps the
    overturned body at +17 px ground contact, in line with the standing
    frames.  (Taubeast behaves the same: body [-320,-500], overturn
    [-320,-228].)
  * non-drawing logic frames (taubeast: 6 frames with image_path=null,
    classified non_drawing_logic_frame by M8-008) get explicit manifest
    entries with draw_frame=false + reason + evidence and do NOT occupy any
    atlas page: a 1x1/5x5 placeholder cell would consume packing space while
    carrying zero pixels, and the engine timeline only needs the duration,
    which the manifest entry keeps.  They stay in their action's frame
    sequence so the timeline order is complete.

Determinism: same inputs + same arguments -> byte-identical outputs (fixed
assertion seed, no timestamps or machine-specific data in any artifact).

Standard library + Pillow (+ xxhash, via the imported module) only.  Run with
``py -I``.  Zero writes to the DNFM tree; all DNFM access is read-only
(sqlite mode=ro URIs).
"""

import argparse
import json
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_character_visual_set import (  # noqa: E402
    AtlasLibrary,
    AnimationStore,
    PipelineError,
    build_preview,
    make_layer,
    pack_pages,
    sha256_file,
)

from PIL import Image  # noqa: E402

GENERATOR = 'Scripts/DNFM/prepare_monster_visual_set.py'
MANIFEST_VERSION = 2
PIVOT_DEFINITION = '脚底/动作原点在 cell 内的位置（相对 uv_rect 左上，y 向下）'
DEFAULT_PAGE = 2048
DEFAULT_MARGIN = 2          # same transparent margin as the verified exporter
PREVIEW_SCALE = 2
REPO = Path(__file__).resolve().parents[2]
DEFAULT_DNFM_ROOT = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0')
DEFAULT_OUT_ROOT = REPO / 'Data/DNFMReplica/monsters'
SELF_TEST_SEED = 20261107
WHITE = [255, 255, 255, 255]
GROUND_CONTACT_TOLERANCE_PX = 60

MONSTER_KEYS = {
    'monster', 'task', 'verify_json', 'body', 'px_to_cm', 'anchor_convention',
    'actions', 'expected', 'excluded_sources', 'previews', 'notes',
}
BODY_KEYS = {'atlas_name', 'image_path', 'canvas', 'frame_count'}
ANCHOR_KEYS = {'kind', 'formula', 'basis', 'evidence'}
ACTION_KEYS = {'action_id', 'ani_key', 'group', 'verify_name'}
EXPECTED_KEYS = {'frame_records_total', 'direct_frames', 'no_draw_frames'}
EXCLUDED_KEYS = {'subject', 'reason'}

IMAGE_SUFFIX = '.img'


class PipelineError(RuntimeError):
    """Any unsupported or inconsistent input; never silently degraded."""


# ---------------------------------------------------------------------------
# Config (strict contract).
# ---------------------------------------------------------------------------

def load_config(path):
    with open(path, 'r', encoding='utf-8') as f:
        cfg = json.load(f)
    if not isinstance(cfg, dict):
        raise PipelineError('config root must be an object')
    unknown = set(cfg) - MONSTER_KEYS
    if unknown:
        raise PipelineError('config has unknown top-level keys %s (allowed: %s)'
                            % (sorted(unknown), sorted(MONSTER_KEYS)))
    for key in ('monster', 'task', 'verify_json', 'body', 'anchor_convention',
                'actions', 'expected'):
        if key not in cfg:
            raise PipelineError('config missing required key %r' % key)
    body = cfg['body']
    if set(body) - BODY_KEYS:
        raise PipelineError('config.body has unknown keys %s' % sorted(set(body) - BODY_KEYS))
    for key in ('atlas_name', 'image_path'):
        if not isinstance(body.get(key), str) or not body[key]:
            raise PipelineError('config.body needs non-empty %r' % key)
    anchor = cfg['anchor_convention']
    if set(anchor) - ANCHOR_KEYS:
        raise PipelineError('config.anchor_convention has unknown keys %s'
                            % sorted(set(anchor) - ANCHOR_KEYS))
    if anchor.get('kind') != 'per_frame_negated_position':
        raise PipelineError('unsupported anchor_convention.kind %r' % anchor.get('kind'))
    ids = [a.get('action_id') for a in cfg['actions']]
    if not ids or len(set(ids)) != len(ids) or not all(ids):
        raise PipelineError('config.actions must have unique non-empty action_id values')
    for action in cfg['actions']:
        if set(action) - ACTION_KEYS:
            raise PipelineError('config action %r has unknown keys %s'
                                % (action.get('action_id'), sorted(set(action) - ACTION_KEYS)))
        for key in ('ani_key', 'group', 'verify_name'):
            if not isinstance(action.get(key), str) or not action[key]:
                raise PipelineError('config action %r needs non-empty %r'
                                    % (action.get('action_id'), key))
    expected = cfg['expected']
    if set(expected) - EXPECTED_KEYS:
        raise PipelineError('config.expected has unknown keys %s' % sorted(set(expected) - EXPECTED_KEYS))
    for exc in cfg.get('excluded_sources') or []:
        if set(exc) - EXCLUDED_KEYS:
            raise PipelineError('config excluded_sources entry has unknown keys %s'
                                % sorted(set(exc) - EXCLUDED_KEYS))
    return cfg


def load_verify(cfg_path_value):
    path = Path(cfg_path_value)
    if not path.is_absolute():
        path = REPO / path
    if not path.is_file():
        raise PipelineError('verify JSON not found: %s' % path)
    with open(path, 'r', encoding='utf-8') as f:
        return json.load(f), path


# ---------------------------------------------------------------------------
# Atlas resolution: image path -> atlas (name stem, uniqueness-checked).
# ---------------------------------------------------------------------------

def resolve_atlas(lib, image_path, by_name_cache):
    """Map a frame image path ('Monster/Tau/taubeast.img') to its atlas.

    Rule: the atlas name is the image file stem; the name must resolve to
    exactly one atlas in atlas.sqlite (exact case first, then a unique
    case-insensitive match).  Nothing is guessed beyond that."""
    stem = Path(image_path.replace('\\', '/')).name
    if not stem.lower().endswith(IMAGE_SUFFIX):
        raise PipelineError('frame image %r does not end in %r' % (image_path, IMAGE_SUFFIX))
    stem = stem[:-len(IMAGE_SUFFIX)]
    if stem in by_name_cache:
        return by_name_cache[stem]
    obj = lib.atlas(stem)  # raises unless the name is unique/absent
    by_name_cache[stem] = obj
    return obj


def atlas_frame_indices(obj):
    return {fr['index'] for pg in obj['pages'] for fr in pg['frames']}


# ---------------------------------------------------------------------------
# Per-frame verify cross-check (fresh DB read vs M8-007/M8-008 verify JSON).
# ---------------------------------------------------------------------------

def cross_check_frame(action_id, fi, db_frame, verify_frame):
    if int(db_frame['delay_ms']) != int(verify_frame['delay_ms']):
        raise PipelineError('action %r frame %d delay_ms db %s != verify %s'
                            % (action_id, fi, db_frame['delay_ms'], verify_frame['delay_ms']))
    if list(map(int, db_frame['position'])) != list(map(int, verify_frame['position'])):
        raise PipelineError('action %r frame %d position db %s != verify %s'
                            % (action_id, fi, db_frame['position'], verify_frame['position']))
    db_scale = [float(v) for v in (db_frame.get('scale') or [1.0, 1.0])]
    vf_scale = [float(v) for v in (verify_frame.get('scale') or [1.0, 1.0])]
    if db_scale != vf_scale:
        raise PipelineError('action %r frame %d scale db %s != verify %s'
                            % (action_id, fi, db_scale, vf_scale))
    db_rot = float(db_frame.get('rotation_degrees') or 0.0)
    vf_rot = float(verify_frame.get('rotation_degrees') or 0.0)
    if abs(db_rot - vf_rot) > 1e-4:
        raise PipelineError('action %r frame %d rotation db %s != verify %s'
                            % (action_id, fi, db_rot, vf_rot))
    db_color = [int(v) for v in (db_frame.get('color_rgba') or WHITE)]
    vf_color = [int(v) for v in (verify_frame.get('color_rgba') or WHITE)]
    if db_color != vf_color:
        raise PipelineError('action %r frame %d color db %s != verify %s'
                            % (action_id, fi, db_color, vf_color))
    if int(db_frame.get('flip_copy_type') or 0) != int(verify_frame.get('flip_copy_type') or 0):
        raise PipelineError('action %r frame %d flip_copy_type db %s != verify %s'
                            % (action_id, fi, db_frame.get('flip_copy_type'),
                               verify_frame.get('flip_copy_type')))
    db_img, db_fi = db_frame.get('image_path'), db_frame.get('image_frame_index')
    # verify JSON shapes: taubeast records 'image': 'Path.img#N'; goblin records
    # the image inside the nested 'atlas' dict plus a top-level image_frame_index.
    vf_img = verify_frame.get('image')
    vf_fi = verify_frame.get('image_frame_index')
    if vf_img is None and 'atlas' in verify_frame:
        vf_img = (verify_frame['atlas'] or {}).get('image')
    if vf_img is not None and vf_fi is None and '#' in vf_img:
        pass  # path#N shape: index split below
    if vf_img is None:
        if db_img is not None:
            raise PipelineError('action %r frame %d image db %r != verify None'
                                % (action_id, fi, db_img))
    elif '#' in vf_img:
        v_path, v_idx = vf_img.rsplit('#', 1)
        if db_img is None or db_img.lower() != v_path.lower() or int(v_idx) != int(db_fi):
            raise PipelineError('action %r frame %d image db (%r, %s) != verify %r'
                                % (action_id, fi, db_img, db_fi, vf_img))
    else:
        if db_img is None or db_img.lower() != str(vf_img).lower() \
                or (vf_fi is not None and int(vf_fi) != int(db_fi)):
            raise PipelineError('action %r frame %d image db (%r, %s) != verify (%r, %s)'
                                % (action_id, fi, db_img, db_fi, vf_img, vf_fi))


# ---------------------------------------------------------------------------
# Frame composition (single layer; per-frame anchor = negated position).
# ---------------------------------------------------------------------------

def apply_tint(img, color):
    """Per-frame color_rgba multiply: c_out = c_src * color_c / 255 (integer)."""
    if list(color) == WHITE:
        return img
    channels = img.split()
    out = []
    for ch, m in zip(channels, color):
        out.append(ch.point(lambda v, m=m: v * m // 255))
    return Image.merge('RGBA', out)


def composite_monster_cell(plan, margin):
    """One RGBA cell for a direct frame (content already bbox-trimmed).

    Untransformed frames take the verified M8-016 placement verbatim
    (content bbox corner + position, transparent margin, pivot = anchor).
    Transformed frames mirror/scale/rotate the single layer about the
    per-frame anchor (-position), then place the anchor exactly at
    position-space (0,0)."""
    frame = plan['_anim_frame']
    content = plan['content']
    bbox = plan['meta'].get('content_bbox_in_canvas')
    pos_x, pos_y = int(frame['position'][0]), int(frame['position'][1])
    ax, ay = -pos_x, -pos_y
    sx, sy = [float(v) for v in (frame.get('scale') or [1.0, 1.0])]
    rot = float(frame.get('rotation_degrees') or 0.0)
    tint = [int(v) for v in (frame.get('color_rgba') or WHITE)]
    plan['_anchor_canvas_px'] = [ax, ay]

    if content is None:
        # Direct frame whose atlas content is fully transparent: keep a 1x1
        # cell at the frame position so the pivot stays put (M8-016 rule for
        # fully empty frames); flagged blank_content in the manifest.
        plan['cell'] = Image.new('RGBA', (1 + 2 * margin, 1 + 2 * margin), (0, 0, 0, 0))
        plan['pivot_px'] = [margin - pos_x, margin - pos_y]
        plan['bbox_px'] = [pos_x, pos_y, pos_x, pos_y]
        plan['cell_origin'] = [pos_x - margin, pos_y - margin]
        plan['transform_applied'] = None
        plan['blank_content'] = True
        return plan

    plan['blank_content'] = False
    transformed = (sx, sy) != (1.0, 1.0) or rot != 0.0
    if not transformed:
        x0, y0 = bbox[0] + pos_x, bbox[1] + pos_y
        cell_w, cell_h = content.width + 2 * margin, content.height + 2 * margin
        cell = Image.new('RGBA', (cell_w, cell_h), (0, 0, 0, 0))
        cell.alpha_composite(apply_tint(content, tint), (margin, margin))
        plan['cell'] = cell
        plan['pivot_px'] = [-x0 + margin, -y0 + margin]
        plan['bbox_px'] = [x0, y0, x0 + content.width, y0 + content.height]
        plan['cell_origin'] = [x0 - margin, y0 - margin]
        plan['transform_applied'] = None
        return plan

    # ---- transformed frame: mirror -> scale -> rotate about the anchor ----
    c = content
    bx0, by0, bx1, by1 = bbox
    rx, ry = ax - bx0, ay - by0
    mirror = {'x': sx < 0, 'y': sy < 0}
    if mirror['x']:
        w = c.width
        c = c.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        bx0, bx1 = 2 * ax - bx1 + 1, 2 * ax - bx0 + 1
        rx = w - 1 - rx
    if mirror['y']:
        h = c.height
        c = c.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        by0, by1 = 2 * ay - by1 + 1, 2 * ay - by0 + 1
        ry = h - 1 - ry
    asx, asy = abs(sx), abs(sy)
    scaled = (asx, asy) != (1.0, 1.0)
    if scaled:
        w, h = c.size
        nw = int(math.floor(w * asx + 0.5))
        nh = int(math.floor(h * asy + 0.5))
        if nw < 1 or nh < 1:
            raise PipelineError('scale %s collapses frame content to %dx%d'
                                % ((sx, sy), nw, nh))
        c = c.resize((nw, nh), Image.Resampling.NEAREST)
        rx = int(math.floor(rx * asx + 0.5))
        ry = int(math.floor(ry * asy + 0.5))
    if rot != 0.0:
        w, h = c.size
        half = max(rx, ry, w - rx, h - ry, w, h) + 1
        big = Image.new('RGBA', (2 * half, 2 * half), (0, 0, 0, 0))
        big.alpha_composite(c, (half - rx, half - ry))
        big = big.rotate(rot, resample=Image.Resampling.NEAREST, expand=True,
                         fillcolor=(0, 0, 0, 0))
        center = (big.width // 2, big.height // 2)
        tb = big.getbbox()
        if tb is None:
            raise PipelineError('transformed frame became fully transparent')
        c = big.crop(tb)
        rx, ry = center[0] - tb[0], center[1] - tb[1]
    c = apply_tint(c, tint)
    cell = Image.new('RGBA', (c.width + 2 * margin, c.height + 2 * margin), (0, 0, 0, 0))
    cell.alpha_composite(c, (margin, margin))
    plan['cell'] = cell
    plan['pivot_px'] = [rx + margin, ry + margin]
    plan['bbox_px'] = [-rx, -ry, -rx + c.width, -ry + c.height]
    plan['cell_origin'] = [-rx - margin, -ry - margin]
    plan['transform_applied'] = {
        'scale': [sx, sy], 'rotation_degrees': rot,
        'mirror': mirror,
        'order': 'mirror_then_scale_then_rotate',
        'anchor_canvas_px': [ax, ay],
        'anchor_rule': 'per-frame negated position (-position)',
        'scale_rule': ('content resized about the anchor, half-up rounding, NEAREST; '
                       'anchor offset rounds the same way so the anchor lands exactly '
                       'at the canvas anchor (outer edges may drift <= 1 px)'),
        'resample': 'nearest',
    }
    if list(tint) != WHITE:
        plan['transform_applied']['color_rgba'] = tint
        plan['transform_applied']['color_rule'] = 'c_out = c_src * color / 255 per channel'
    return plan


def frame_manifest_entry(plan):
    frame = plan['_anim_frame']
    layer_frames = {}
    if plan['content'] is not None or plan['meta'].get('atlas_name') is not None:
        meta = plan['meta']
        layer_frames[meta['atlas_name']] = {
            'atlas_id': meta['atlas_id'], 'frame_index': meta['frame_index'],
            'page': meta['page'], 'offset': meta['offset'],
            'content_bbox_px': plan['placement'],
        }
    events = []
    for tag in frame.get('audio_tags') or []:
        events.append({'type': 'audio', 'name': tag.get('tag')})
    for tag in frame.get('audio_loop_tags') or []:
        events.append({'type': 'audio_loop', 'name': tag.get('tag')})
    delay = int(frame['delay_ms'])
    tint = [int(v) for v in (frame.get('color_rgba') or WHITE)]
    out = {
        'index': plan['frame_index'],
        # M6-021 contract: zero-duration frames clamp to 1ms with the original
        # value recorded as source_delay_ms.
        'duration_ms': max(delay, 1),
        'page': plan['page'],
        'uv_rect_px': list(plan['uv_rect_px']),
        'canvas_size_px': list(plan['cell'].size),
        'pivot_px': list(plan['pivot_px']),
        'pivot_definition': PIVOT_DEFINITION,
        'draw_frame': True,
        'layer_frames': layer_frames,
        'flip_copy_type': int(frame.get('flip_copy_type') or 0),
        'scale': [float(v) for v in (frame.get('scale') or [1.0, 1.0])],
        'rotation_degrees': float(frame.get('rotation_degrees') or 0.0),
        'source_frame': {'image_path': frame.get('image_path'),
                         'image_pack_index': int(frame.get('image_pack_index') or 0),
                         'image_frame_index': (int(frame['image_frame_index'])
                                               if frame.get('image_frame_index') is not None else None)},
        'events': events,
    }
    if delay < 1:
        out['source_delay_ms'] = delay
    if list(tint) != WHITE:
        out['color_rgba'] = tint
    if plan['transform_applied'] is not None:
        out['transform_applied'] = plan['transform_applied']
    if plan.get('blank_content'):
        out['blank_content'] = True
    return out


def no_draw_manifest_entry(plan, unresolved):
    frame = plan['_anim_frame']
    events = []
    for tag in frame.get('audio_tags') or []:
        events.append({'type': 'audio', 'name': tag.get('tag')})
    for tag in frame.get('audio_loop_tags') or []:
        events.append({'type': 'audio_loop', 'name': tag.get('tag')})
    delay = int(frame['delay_ms'])
    out = {
        'index': plan['frame_index'],
        'duration_ms': max(delay, 1),
        'page': None,
        'uv_rect_px': None,
        'canvas_size_px': [0, 0],
        'pivot_px': None,
        'pivot_definition': PIVOT_DEFINITION,
        'draw_frame': False,
        'no_draw': {
            'classification': unresolved['classification'],
            'reason': ('image_path=null logic/timing frame; M8-008 verify classifies it as '
                       '%s; kept in the timeline, occupies no atlas page, no art fabricated'
                       % unresolved['classification']),
            'evidence': unresolved['evidence'],
        },
        'position': list(frame['position']),
        'layer_frames': {},
        'flip_copy_type': int(frame.get('flip_copy_type') or 0),
        'scale': [float(v) for v in (frame.get('scale') or [1.0, 1.0])],
        'rotation_degrees': float(frame.get('rotation_degrees') or 0.0),
        'source_frame': {'image_path': None,
                         'image_pack_index': int(frame.get('image_pack_index') or 0),
                         'image_frame_index': None},
        'events': events,
    }
    if delay < 1:
        out['source_delay_ms'] = delay
    return out


# ---------------------------------------------------------------------------
# Verify-JSON structure helpers.
# ---------------------------------------------------------------------------

def verify_animations_by_name(verify):
    out = {}
    for a in verify['animations']:
        name = a.get('name')
        if not name:
            raise PipelineError('verify animation row without name')
        if name in out:
            raise PipelineError('verify JSON has duplicate animation name %r' % name)
        out[name] = a
    return out


def unresolved_lookup(verify):
    """Map (animation stem, frame_index) -> unresolved_frames row."""
    out = {}
    for row in verify.get('unresolved_frames') or []:
        stem = Path(row['animation']).stem
        key = (stem, int(row['frame_index']))
        if key in out:
            raise PipelineError('verify unresolved_frames has duplicate %s' % (key,))
        out[key] = row
    return out


# ---------------------------------------------------------------------------
# Main pipeline.
# ---------------------------------------------------------------------------

def run(args):
    dnfm_root = Path(args.dnfm_root)
    restored_root = dnfm_root / 'downloaded_130.5.8.202'
    config_path = Path(args.config)
    if not config_path.is_file():
        raise PipelineError('config not found: %s' % config_path)
    cfg_sha = sha256_file(config_path)
    cfg = load_config(config_path)
    verify, verify_path = load_verify(cfg['verify_json'])
    verify_sha = sha256_file(verify_path)
    page_size = int(args.page_size)
    margin = int(args.margin)
    if page_size <= 0 or page_size > 2048:
        raise PipelineError('--page-size must be within (0, 2048] (engine limit); got %s' % page_size)
    out_dir = Path(args.out_dir) if args.out_dir else DEFAULT_OUT_ROOT / cfg['monster']
    monster = cfg['monster']

    if args.actions.strip().lower() == 'all':
        actions = list(cfg['actions'])
    else:
        wanted = [a.strip() for a in args.actions.split(',') if a.strip()]
        by_id = {a['action_id']: a for a in cfg['actions']}
        unknown = [w for w in wanted if w not in by_id]
        if unknown:
            raise PipelineError('--actions references unknown action ids: %s' % unknown)
        actions = [by_id[w] for w in wanted]

    lib = AtlasLibrary(restored_root)
    store = AnimationStore(restored_root)
    by_name_cache = {}
    vanim = verify_animations_by_name(verify)
    unresolved = unresolved_lookup(verify)

    # ---- body atlas checks -------------------------------------------------
    body_cfg = cfg['body']
    body_obj = resolve_atlas(lib, body_cfg['image_path'], by_name_cache)
    if body_obj['name'] != body_cfg['atlas_name']:
        raise PipelineError('body image %r resolves to atlas %r, config says %r'
                            % (body_cfg['image_path'], body_obj['name'], body_cfg['atlas_name']))
    body_indices = atlas_frame_indices(body_obj)
    if len(body_indices) != int(body_cfg['frame_count']):
        raise PipelineError('body atlas %r has %d frames but config says %s'
                            % (body_obj['name'], len(body_indices), body_cfg['frame_count']))

    # goblin: verify atlas_body0 carries per-frame canvas/offset/rect for the
    # body atlas -- cross-check every DB frame record it lists.
    atlas_body0_check = None
    if 'atlas_body0' in verify:
        rec = verify['atlas_body0']
        if rec.get('atlas_name') != body_obj['name']:
            raise PipelineError('verify atlas_body0 names %r, body atlas is %r'
                                % (rec.get('atlas_name'), body_obj['name']))
        if int(rec.get('atlas_id', -1)) != lib.atlas_id(body_obj['name']):
            raise PipelineError('verify atlas_body0 id %s != db id %s'
                                % (rec.get('atlas_id'), lib.atlas_id(body_obj['name'])))
        db_by_index = {fr['index']: fr for pg in body_obj['pages'] for fr in pg['frames']}
        mismatches = []
        checked = 0
        for key, vfr in (rec.get('frames') or {}).items():
            fi = int(key)
            dfr = db_by_index.get(fi)
            if dfr is None:
                mismatches.append({'frame': fi, 'reason': 'absent in db'})
                continue
            for field in ('canvas', 'offset'):
                if [int(v) for v in dfr[field]] != [int(v) for v in vfr[field]]:
                    mismatches.append({'frame': fi, 'field': field,
                                       'db': dfr[field], 'verify': vfr[field]})
            rect = dfr.get('rect_pixels', dfr['rect'])
            if [float(v) for v in rect] != [float(v) for v in vfr['rect']]:
                mismatches.append({'frame': fi, 'field': 'rect', 'db': rect, 'verify': vfr['rect']})
            checked += 1
        atlas_body0_check = {'pass': not mismatches, 'checked': checked, 'failures': mismatches}

    # taubeast: verify external_deps describes every effect overlay img used.
    external_deps_check = None
    if 'external_deps' in verify:
        rows = []
        for dep in verify['external_deps']:
            obj = resolve_atlas(lib, dep['image_key'], by_name_cache)
            indices = atlas_frame_indices(obj)
            used = sorted({int(i) for i in dep['frames_used']})
            missing = [i for i in used if i not in indices]
            pg = obj['pages'][0]
            size_ok = (list(dep.get('page_size') or []) in ([], [pg['width'], pg['height']]))
            rows.append({'image_key': dep['image_key'], 'atlas_name': obj['name'],
                         'atlas_id': lib.atlas_id(obj['name']), 'frames_used': used,
                         'missing_indices': missing, 'page_size_ok': size_ok,
                         'used_by': dep.get('used_by')})
        bad = [r for r in rows if r['missing_indices'] or not r['page_size_ok']]
        external_deps_check = {'pass': not bad, 'checked': len(rows), 'failures': bad}

    # ---- resolve all actions ----------------------------------------------
    cells_all = []
    no_draw_entries = []
    action_outputs = []
    for action in actions:
        vrec = vanim.get(action['verify_name'])
        if vrec is None:
            raise PipelineError('action %r references verify animation %r which does not exist'
                                % (action['action_id'], action['verify_name']))
        v_path = vrec.get('script_key') or vrec.get('path')
        if v_path and v_path.lower() != action['ani_key'].lower():
            raise PipelineError('action %r config ani_key %r != verify path %r'
                                % (action['action_id'], action['ani_key'], v_path))
        loaded = store.load(action['ani_key'])
        anim = loaded['anim']
        source = loaded['source']
        if vrec.get('frame_count') is not None and int(vrec['frame_count']) != len(anim['frames']):
            raise PipelineError('action %r verify frame_count %s != parsed %s'
                                % (action['action_id'], vrec['frame_count'], len(anim['frames'])))
        if vrec.get('duration_ms') is not None:
            expect = sum(int(f['delay_ms']) for f in anim['frames'])
            if int(vrec['duration_ms']) != int(expect):
                raise PipelineError('action %r verify duration_ms %s != parsed %s'
                                    % (action['action_id'], vrec['duration_ms'], expect))
        if vrec.get('loop') is not None and bool(vrec['loop']) != bool(anim['loop']):
            raise PipelineError('action %r verify loop %s != parsed %s'
                                % (action['action_id'], vrec['loop'], anim['loop']))
        plans = []
        action_unresolved = []
        direct_count = 0
        for fi, (db_frame, v_frame) in enumerate(zip(anim['frames'], vrec['frames'])):
            cross_check_frame(action['action_id'], fi, db_frame, v_frame)
            image_path = db_frame.get('image_path')
            if image_path is None:
                if (action['verify_name'], fi) not in unresolved:
                    raise PipelineError('action %r frame %d has no image but verify unresolved_frames '
                                        'does not classify it' % (action['action_id'], fi))
                row = unresolved[(action['verify_name'], fi)]
                if int(row['delay_ms']) != int(db_frame['delay_ms']):
                    raise PipelineError('action %r frame %d unresolved delay_ms %s != parsed %s'
                                        % (action['action_id'], fi, row['delay_ms'], db_frame['delay_ms']))
                plan = {'frame_index': fi, 'content': None, 'meta': {'atlas_name': None},
                        'placement': None, '_anim_frame': db_frame,
                        '_kind': 'no_draw', '_action_id': action['action_id'],
                        'cell': None, 'pivot_px': None, 'uv_rect_px': None,
                        'page': None, 'bbox_px': None, 'transform_applied': None}
                plans.append(plan)
                action_unresolved.append(fi)
                continue
            direct_count += 1
            obj = resolve_atlas(lib, image_path, by_name_cache)
            content, meta = lib.load_layer_content(obj['name'], int(db_frame['image_frame_index']))
            meta['atlas_name'] = obj['name']
            meta['atlas_id'] = lib.atlas_id(obj['name'])
            plan = make_layer(obj['name'], content, meta, db_frame['position'])
            plan['frame_index'] = fi
            plan['_anim_frame'] = db_frame
            plan['_kind'] = 'direct'
            plan['_action_id'] = action['action_id']
            composite_monster_cell(plan, margin)
            plans.append(plan)
            cells_all.append(plan)
        transformed = [p['frame_index'] for p in plans if p['transform_applied'] is not None]
        action_outputs.append({
            'action_id': action['action_id'],
            'group': action['group'],
            'source_ani': source['path'],
            'source_sha256': source['sha256'],
            'loop': bool(anim['loop']),
            'sorting_layer': anim.get('sorting_layer'),
            'draw_shadow': anim.get('draw_shadow'),
            'verify_name': action['verify_name'],
            'layer_mapping': 'monster_single_layer',
            'source': 'script_index',
            'transformed_frames': transformed,
            'no_draw_frame_indexes': action_unresolved,
            '_plans': plans,
            '_direct_count': direct_count,
        })

    pages = pack_pages(cells_all, page_size)

    # ---- assertions --------------------------------------------------------
    assertions = {}

    # (1) frame counts: per action vs verify, plus the verify-summary totals.
    # verify frame shapes: taubeast 'image': 'Path.img#N'; goblin image inside
    # the nested 'atlas' dict.
    def _has_image(v_frame):
        if v_frame.get('image') is not None:
            return True
        return bool((v_frame.get('atlas') or {}).get('image'))

    expected = cfg['expected']
    verify_direct = sum(1 for a in verify['animations'] for f in a['frames'] if _has_image(f))
    verify_no_draw = sum(1 for a in verify['animations'] for f in a['frames'] if not _has_image(f))
    verify_records = sum(len(a['frames']) for a in verify['animations'])
    totals = {
        'frame_records_total': {'expected': expected['frame_records_total'], 'verify': verify_records},
        'direct_frames': {'expected': expected['direct_frames'], 'verify': verify_direct,
                          'composite': len(cells_all)},
        'no_draw_frames': {'expected': expected['no_draw_frames'], 'verify': verify_no_draw,
                           'composite': len(no_draw_entries)},
    }
    counts_ok = (verify_records == int(expected['frame_records_total'])
                 and verify_direct == int(expected['direct_frames'])
                 and verify_no_draw == int(expected['no_draw_frames'])
                 and len(cells_all) == verify_direct)
    per_action = [{'action_id': o['action_id'], 'frames': len(o['_plans']),
                   'direct': o['_direct_count'],
                   'verify_frame_count': vanim[o['verify_name']]['frame_count']}
                  for o in action_outputs]
    bad = [r for r in per_action
           if r['verify_frame_count'] is not None and int(r['verify_frame_count']) != r['frames']]
    frame_mismatch = [r for r in per_action if r['direct'] + (r['frames'] - r['direct']) != r['frames']]
    assertions['frame_counts_match_verify'] = {
        'pass': counts_ok and not bad and not frame_mismatch,
        'expected_vs_verify': totals,
        'per_action_checked': len(per_action),
        'failures': bad,
    }

    # (2) no-draw frames must match verify unresolved_frames exactly.
    composite_no_draw = [{'action': o['verify_name'], 'frame_index': i}
                         for o in action_outputs for i in o['no_draw_frame_indexes']]
    unresolved_keys = sorted('%s#%d' % k for k in unresolved)
    composite_keys = sorted('%s#%d' % (r['action'], r['frame_index']) for r in composite_no_draw)
    assertions['no_draw_frames'] = {
        'pass': composite_keys == unresolved_keys,
        'verify_unresolved': unresolved_keys,
        'composite_no_draw': composite_keys,
        'policy': 'draw_frame=false manifest entries; no page/cell occupied',
    }

    # (3) atlas resolution / body + effect dependency checks.
    assertions['atlas_resolution'] = {
        'pass': ((atlas_body0_check or {'pass': True})['pass']
                 and (external_deps_check or {'pass': True})['pass']),
        'body_atlas': {'name': body_obj['name'], 'atlas_id': lib.atlas_id(body_obj['name']),
                       'frame_count': len(body_indices)},
        'atlas_body0_check': atlas_body0_check,
        'external_deps_check': external_deps_check,
        'atlases_used': sorted(by_name_cache),
    }

    # (4) ground-contact consistency: body-atlas frames must keep the sprite
    # near the anchor; overturn (transformed) bottoms must stay within the
    # untransformed body range +- tolerance.  Catches anchor/transform bugs
    # (a wrong convention buries the flipped body far below the anchor).
    body_bottoms = {}
    untrans_bottoms = []
    trans_bottoms = []
    for plan in cells_all:
        if plan['meta'].get('atlas_name') != body_obj['name']:
            continue
        bottom = plan['bbox_px'][3]
        body_bottoms.setdefault(plan['_action_id'], []).append(bottom)
        if plan['transform_applied'] is None:
            untrans_bottoms.append(bottom)
        else:
            trans_bottoms.append(bottom)
    lo = min(untrans_bottoms) - GROUND_CONTACT_TOLERANCE_PX if untrans_bottoms else None
    hi = max(untrans_bottoms) + GROUND_CONTACT_TOLERANCE_PX if untrans_bottoms else None
    trans_bad = [b for b in trans_bottoms if lo is not None and not (lo <= b <= hi)]
    assertions['body_ground_contact'] = {
        'pass': not trans_bad,
        'untransformed_body_bottoms_px': {
            'min': min(untrans_bottoms) if untrans_bottoms else None,
            'max': max(untrans_bottoms) if untrans_bottoms else None,
        },
        'transformed_body_bottoms_px': trans_bottoms,
        'tolerance_px': GROUND_CONTACT_TOLERANCE_PX,
        'failures': trans_bad,
        'note': 'position-space bottom y of body-atlas cells vs the feet anchor; '
                'overturn (rotated lying pose) must stay near the standing range',
    }

    # (5) transform/tint census: flip_copy_type must be 0 everywhere; record
    # where non-unit scale / non-white tint were baked in.
    flips = sorted({int(p['_anim_frame'].get('flip_copy_type') or 0) for p in cells_all})
    tinted = sorted({'%s#%d' % (p['_action_id'], p['frame_index'])
                     for p in cells_all
                     if [int(v) for v in (p['_anim_frame'].get('color_rgba') or WHITE)] != WHITE})
    scaled = sorted({'%s#%d' % (p['_action_id'], p['frame_index'])
                     for p in cells_all
                     if [float(v) for v in (p['_anim_frame'].get('scale') or [1.0, 1.0])] != [1.0, 1.0]})
    assertions['transform_census'] = {
        'pass': flips == [0],
        'flip_copy_types_seen': flips,
        'tinted_frames': tinted,
        'scaled_frames_count': len(scaled),
        'scaled_frames': scaled,
    }

    # (6) pages invariants.
    cells_on_pages = sum(len(p['placements']) for p in pages)
    assertions['pages'] = {
        'pass': cells_on_pages == len(cells_all),
        'page_count': len(pages),
        'page_size': page_size,
        'cells_total': len(cells_all),
        'cells_on_pages': cells_on_pages,
    }

    # (7) sampled pixel statistics (deterministic pseudo-random sample).
    rng = random.Random(SELF_TEST_SEED)
    indices = list(range(len(cells_all)))
    rng.shuffle(indices)
    sample = []
    for idx in indices[:min(60, len(cells_all))]:
        plan = cells_all[idx]
        hist = plan['cell'].getchannel('A').histogram()
        visible = sum(hist[1:])
        sample.append({'action_id': plan['_action_id'], 'frame': plan['frame_index'],
                       'visible_px': visible, 'cell': list(plan['cell'].size),
                       'blank_by_data': plan['content'] is None})
    non_empty = [s for s in sample if not s['blank_by_data']]
    pixel_failures = [s for s in non_empty if s['visible_px'] <= 0]
    assertions['sampled_pixel_stats'] = {
        'pass': not pixel_failures,
        'sampled': len(sample),
        'blank_by_data': sum(1 for s in sample if s['blank_by_data']),
        'min_visible_px_non_blank': min((s['visible_px'] for s in non_empty), default=None),
        'max_visible_px_non_blank': max((s['visible_px'] for s in non_empty), default=None),
        'failures': pixel_failures,
    }

    # ---- write outputs (deterministic) ------------------------------------
    previews_dir = out_dir / 'preview'
    out_dir.mkdir(parents=True, exist_ok=True)
    previews_dir.mkdir(parents=True, exist_ok=True)
    pages_manifest = []
    for page in pages:
        canvas = Image.new('RGBA', (page_size, page_size), (0, 0, 0, 0))
        for cell, x, y in page['placements']:
            canvas.alpha_composite(cell['cell'], (x, y))
        name = '%s_page_%d.png' % (monster, page['index'])
        canvas.save(out_dir / name, 'PNG')
        pages_manifest.append({'file': name, 'sha256': sha256_file(out_dir / name),
                               'size': [page_size, page_size],
                               'cells_on_page': len(page['placements'])})
    preview_files = []
    preview_actions = cfg.get('previews') or [a['action_id'] for a in cfg['actions']]
    for action_id in preview_actions:
        out = next((o for o in action_outputs if o['action_id'] == action_id), None)
        if out is None:
            raise PipelineError('config previews reference unknown action %r' % action_id)
        strip = [p for p in out['_plans'] if p['_kind'] == 'direct']
        name = 'preview_%s.png' % action_id
        build_preview(strip, PREVIEW_SCALE).save(previews_dir / name, 'PNG')
        preview_files.append({'file': 'preview/%s' % name,
                              'sha256': sha256_file(previews_dir / name),
                              'frames': len(strip),
                              'no_draw_frames_skipped': len(out['no_draw_frame_indexes'])})

    anchor = cfg['anchor_convention']
    manifest = {
        'generator': GENERATOR,
        'manifest_version': MANIFEST_VERSION,
        'character': monster,
        'entity_type': 'monster',
        'outfit': None,
        'mode': 'monster_single_layer',
        'source_root': str(dnfm_root),
        'coordinate_space': ('Position space, X right / Y down, origin at the feet/action anchor; '
                             'per-frame pivot_px is that anchor inside the cell (uv top-left relative, y down)'),
        'px_to_cm': cfg.get('px_to_cm'),
        'cell_margin_px': margin,
        'page_size_px': page_size,
        'anchor_convention': {
            'kind': anchor['kind'],
            'formula': anchor['formula'],
            'basis': anchor['basis'],
            'evidence': anchor.get('evidence'),
            'note': 'untransformed frames place the layer canvas at the frame position exactly '
                    'like M8-016; transforms apply about the per-frame anchor, which always '
                    'lands at position-space (0,0)',
        },
        'transform_semantics': {
            'transform_order': 'mirror_then_scale_then_rotate',
            'rotation_direction': ('positive degrees rotate counterclockwise in y-down image space '
                                   '(PIL rotate(+angle), anchor at image center)'),
            'resample': 'nearest',
            'scale_rule': ('content resized about the anchor, half-up rounding, NEAREST; the anchor '
                           'offset rounds identically so the anchor lands exactly on the canvas '
                           'anchor; outer edges may drift <= 1 px from the ideal float position'),
            'color_rule': ('per-frame color_rgba multiplies RGBA channels (c_out = c_src*color/255); '
                           'baked into the cell and recorded per frame'),
        },
        'layer_rule': {
            'draw_order_bottom_to_top': ['body'],
            'single_layer': True,
            'note': ('monsters have no outfit layers; each frame references exactly one image '
                     '(body atlas or class effect overlay). taubeast charge/warning actions are '
                     'pure effect overlays (verify external_deps): their cells carry the effect '
                     'art at the authored frame scale; sorting_layer below the body (5 vs 10) is '
                     'recorded per action for runtime draw ordering'),
        },
        'excluded_sources': cfg.get('excluded_sources') or [],
        'pages': pages_manifest,
        'actions': [{
            'action_id': o['action_id'],
            'group': o['group'],
            'source_ani': o['source_ani'],
            'source_sha256': o['source_sha256'],
            'loop': o['loop'],
            'sorting_layer': o['sorting_layer'],
            'draw_shadow': o['draw_shadow'],
            'layers': [body_cfg['atlas_name']] if o['_direct_count'] else [],
            'draw_order': ['body'],
            'layer_mapping': o['layer_mapping'],
            'source': o['source'],
            'transformed_frames': o['transformed_frames'],
            'no_draw_frame_indexes': o['no_draw_frame_indexes'],
            'frames': [frame_manifest_entry(p) if p['_kind'] == 'direct'
                       else no_draw_manifest_entry(p, unresolved[(o['verify_name'], p['frame_index'])])
                       for p in o['_plans']],
        } for o in action_outputs],
        'missing': [],
        'verification': {
            'mode': 'monster_single_layer',
            'assertions': assertions,
        },
        'inputs': {
            'config_file': config_path.name,
            'config_sha256': cfg_sha,
            'verify_json': str(verify_path),
            'verify_sha256': verify_sha,
        },
        'determinism_note': 'Same inputs and arguments reproduce byte-identical outputs.',
    }
    manifest_path = out_dir / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')

    failed = [k for k, v in assertions.items() if not v.get('pass')]
    print(json.dumps({
        'monster': monster,
        'actions': [{'action_id': o['action_id'], 'group': o['group'],
                     'frames': len(o['_plans']), 'direct': o['_direct_count'],
                     'no_draw': len(o['no_draw_frame_indexes']),
                     'transformed': len(o['transformed_frames'])} for o in action_outputs],
        'frames_total': len(cells_all),
        'no_draw_total': sum(len(o['no_draw_frame_indexes']) for o in action_outputs),
        'pages': len(pages_manifest),
        'page_size': page_size,
        'previews': preview_files,
        'assertions_passed': not failed,
        'assertions_failed': failed,
        'out_dir': str(out_dir),
        'manifest_sha256': sha256_file(manifest_path),
    }, ensure_ascii=False))
    return 1 if failed else 0


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description='M8-018 monster visual-set compositor')
    ap.add_argument('--config', required=True,
                    help='monster pipeline config JSON (Data/DNFMReplica/monsters/<name>/...)')
    ap.add_argument('--out-dir', default=None,
                    help='defaults to Data/DNFMReplica/monsters/<monster>')
    ap.add_argument('--actions', default='all', help="comma-separated action ids, or 'all'")
    ap.add_argument('--dnfm-root', default=str(DEFAULT_DNFM_ROOT))
    ap.add_argument('--page-size', type=int, default=DEFAULT_PAGE)
    ap.add_argument('--margin', type=int, default=DEFAULT_MARGIN)
    args = ap.parse_args()
    try:
        sys.exit(run(args))
    except PipelineError as exc:
        print(json.dumps({'error': str(exc)}), file=sys.stderr)
        sys.exit(2)
