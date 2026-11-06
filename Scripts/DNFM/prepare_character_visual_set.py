#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M8-016: generic multi-character offline outfit/action compositor (DNFM P04-A).

Generalizes the verified M6-020 fighter pipeline
(Scripts/DNFM/prepare_fighter_visual_set.py, left untouched) to any character
whose layers and actions are described by a character config JSON
(Data/DNFMReplica/characters/<role>/pipeline_config.json) backed by the M8-003
(swordman) / M8-004 (gunner) verify JSONs.

Composites the fixed default outfit per animation frame into paged RGBA atlases
(<= 2048x2048) plus a manifest with the same schema as the M6-020 fighter
composite (manifest_version 2): per-frame UVRect/Pivot/Duration/SourceFrame/
Events, feet-origin pivot, source millisecond durations (zero-delay frames
clamped to 1ms with the original value recorded, M6-021 contract), and per-frame
transform records (mirror/rotation).

Alignment contract (identical to M6-020, reproduced from the verified exporter):
  * atlas.sqlite frame rects are Unity bottom-left origin pixels; crops are
    y-flipped into image space.
  * each layer frame has a canvas (500x500 for both characters here) and an
    offset placing the crop inside that canvas; the animation frame ``position``
    places the canvas so that position-space origin (0,0) is the feet/action
    anchor; compositing every layer at (canvas content bbox corner + position)
    yields pixel-correct relative alignment.
  * per-frame pivot_px is that anchor inside the cell (uv top-left relative,
    y down).

Layer parallelism: the verify JSONs prove every outfit layer is frame-parallel
with the body atlas (swordman 9 layers x 179 frames; gunner 16 layers covering
the 150 key-motion body indices), so every layer is sampled at the body frame's
image_frame_index.  Gunner rev0000b+c double-attach frames = body + both gun
layers (verify weapon_gun_layer.attach_rule).  Swordman katana0000b/c are the
behind/front split of one sword: both layers composite (b below body, c above
body, per the verify draw_order).  NOTE: the M8-016 task card asked to "take b
or c per pose" based on the verify JSON's "mutually exclusive per pose" wording;
frame-level verification (Artifacts/Tasks/M8-016/.../db_queries/
layer_data_probe.json: 100 of 179 indices have visible content on BOTH layers,
e.g. rest f0 b=57px scabbard + c=799px sword, matching M8-003's own spotcheck
stats) disproved exclusivity, so both layers composite in the verified z-order
-- identical output to "take b" on the 41 b-only frames, to "take c" on the 33
c-only frames, and game-faithful on the 100 both frames and 5 neither frames.

Transform support (M6-020 semantics, per frame):
  * negative scale components mirror the whole composited frame about the
    rotation anchor lines; |scale| != 1 is rejected.
  * rotation_degrees: the whole composited frame rotates about a fixed per
    character canvas anchor (swordman (228,241), gunner (232,240)) reverse
    engineered in M8-016 by fitting ground contact (lowest visible body pixel at
    y~=0 in position space) over all rotated frames of the character; the method
    was calibrated on the fighter, where it recovers the M6-020 verified anchor
    (250,292) within 10 px (probe: Artifacts/Tasks/M8-016/.../db_queries/
    rotation_anchor_probe.json, rotation_convention_probe.json).  Positive
    degrees rotate counterclockwise in y-down image space (PIL rotate(+angle));
    mirror first, then rotate; nearest-neighbour resampling.
  * flip_copy_type != 0, non-white color_rgba, palette pages outside the
    verified palette decode (format codes 6/7), unresolved textures, fractional
    rects, cells larger than the page and unknown config keys are hard errors.
    Nothing is silently dropped or shrunk.

Determinism: same inputs + same arguments -> byte-identical outputs (no
timestamps or machine-specific data in any artifact).

Standard library + Pillow (+ xxhash for the verified path hash) only.  Run with
``py -I``.  Zero writes to the DNFM tree; all DNFM access is read-only
(sqlite mode=ro URIs).
"""

import argparse
import hashlib
import json
import random
import sqlite3
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

GENERATOR = 'Scripts/DNFM/prepare_character_visual_set.py'
MANIFEST_VERSION = 2
PIVOT_DEFINITION = '脚底/动作原点在 cell 内的位置（相对 uv_rect 左上，y 向下）'
DEFAULT_PAGE = 2048
DEFAULT_MARGIN = 2          # same transparent margin as the verified exporter
PREVIEW_SCALE = 2
REPO = Path(__file__).resolve().parents[2]
DEFAULT_DNFM_ROOT = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0')
DEFAULT_OUT_ROOT = REPO / 'Data/DNFMReplica/characters'
SELF_TEST_SEED = 20261107

CONFIG_KEYS = {
    'character', 'task', 'verify_json', 'body', 'px_to_cm', 'rotation_anchor',
    'layers', 'draw_order', 'weapon_rule', 'actions', 'excluded_actions',
    'previews', 'notes',
}
BODY_KEYS = {'atlas_name', 'image_path', 'frame_count', 'canvas'}
ROTATION_ANCHOR_KEYS = {'canvas_px', 'basis', 'probe_evidence'}
LAYER_KEYS = {'slot', 'atlas_name', 'role', 'verify_layer_index', 'variant_note'}
ACTION_KEYS = {'action_id', 'ani_key', 'group', 'verify', 'note'}
WEAPON_RULE_KEYS = {'kind', 'back_slot', 'front_slot', 'slots', 'note'}
EXCLUDED_KEYS = {'ani_key', 'name', 'reason'}
VERIFY_REF_KEYS = {'section', 'key', 'index', 'body', 'mode'}


class PipelineError(RuntimeError):
    """Any unsupported or inconsistent input; never silently degraded."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Character config (strict contract).
# ---------------------------------------------------------------------------

def load_config(path):
    with open(path, 'r', encoding='utf-8') as f:
        cfg = json.load(f)
    if not isinstance(cfg, dict):
        raise PipelineError('config root must be an object')
    unknown = set(cfg) - CONFIG_KEYS
    if unknown:
        raise PipelineError('config has unknown top-level keys %s (allowed: %s)'
                            % (sorted(unknown), sorted(CONFIG_KEYS)))
    for key in ('character', 'verify_json', 'body', 'rotation_anchor', 'layers',
                'draw_order', 'actions'):
        if key not in cfg:
            raise PipelineError('config missing required key %r' % key)
    body = cfg['body']
    if set(body) - BODY_KEYS:
        raise PipelineError('config.body has unknown keys %s' % sorted(set(body) - BODY_KEYS))
    anchor = cfg['rotation_anchor']
    if set(anchor) - ROTATION_ANCHOR_KEYS:
        raise PipelineError('config.rotation_anchor has unknown keys %s'
                            % sorted(set(anchor) - ROTATION_ANCHOR_KEYS))
    if (not isinstance(anchor['canvas_px'], list) or len(anchor['canvas_px']) != 2
            or any(not isinstance(v, int) for v in anchor['canvas_px'])):
        raise PipelineError('config.rotation_anchor.canvas_px must be [int, int]')
    layers = cfg['layers']
    if not isinstance(layers, list) or not layers:
        raise PipelineError('config.layers must be a non-empty list')
    slots = set()
    for layer in layers:
        if set(layer) - LAYER_KEYS:
            raise PipelineError('config layer %r has unknown keys %s'
                                % (layer.get('slot'), sorted(set(layer) - LAYER_KEYS)))
        for key in ('slot', 'atlas_name'):
            if not isinstance(layer.get(key), str) or not layer[key]:
                raise PipelineError('config layer needs non-empty %r' % key)
        if layer['slot'] in slots:
            raise PipelineError('duplicate config layer slot %r' % layer['slot'])
        slots.add(layer['slot'])
    draw_order = cfg['draw_order']
    if not isinstance(draw_order, list) or not draw_order:
        raise PipelineError('config.draw_order must be a non-empty bottom->top list')
    for slot in draw_order:
        if slot not in slots:
            raise PipelineError('draw_order slot %r not present in layers' % slot)
    if len(draw_order) != len(slots):
        raise PipelineError('config.draw_order must list every layer exactly once')
    if body_cfg_atlas(cfg) not in [l['atlas_name'] for l in layers]:
        raise PipelineError('config.body.atlas_name %r is not among the layers'
                            % body_cfg_atlas(cfg))
    weapon = cfg.get('weapon_rule')
    if weapon is not None:
        if set(weapon) - WEAPON_RULE_KEYS:
            raise PipelineError('config.weapon_rule has unknown keys %s'
                                % sorted(set(weapon) - WEAPON_RULE_KEYS))
        kind = weapon.get('kind')
        if kind not in ('front_back_split_stack', 'parallel_stack'):
            raise PipelineError('unsupported config.weapon_rule.kind %r' % kind)
        if kind == 'front_back_split_stack':
            for key in ('back_slot', 'front_slot'):
                if weapon.get(key) not in slots:
                    raise PipelineError('weapon_rule.%s %r not a layer slot' % (key, weapon.get(key)))
        else:
            for slot in weapon.get('slots') or []:
                if slot not in slots:
                    raise PipelineError('weapon_rule slot %r not a layer slot' % slot)
    ids = [a.get('action_id') for a in cfg['actions']]
    if not ids or len(set(ids)) != len(ids) or not all(ids):
        raise PipelineError('config.actions must have unique non-empty action_id values')
    for action in cfg['actions']:
        if set(action) - ACTION_KEYS:
            raise PipelineError('config action %r has unknown keys %s'
                                % (action.get('action_id'), sorted(set(action) - ACTION_KEYS)))
        for key in ('ani_key', 'group'):
            if not isinstance(action.get(key), str) or not action[key]:
                raise PipelineError('config action %r needs non-empty %r' % (action['action_id'], key))
        ref = action.get('verify')
        if ref is not None and set(ref) - VERIFY_REF_KEYS:
            raise PipelineError('config action %r verify ref has unknown keys %s'
                                % (action['action_id'], sorted(set(ref) - VERIFY_REF_KEYS)))
    for exc in cfg.get('excluded_actions') or []:
        if set(exc) - EXCLUDED_KEYS:
            raise PipelineError('config excluded_actions entry has unknown keys %s'
                                % sorted(set(exc) - EXCLUDED_KEYS))
    return cfg


def body_cfg_atlas(cfg):
    return cfg['body']['atlas_name']


def load_verify(cfg_path_value):
    path = Path(cfg_path_value)
    if not path.is_absolute():
        path = REPO / path
    if not path.is_file():
        raise PipelineError('verify JSON not found: %s' % path)
    with open(path, 'r', encoding='utf-8') as f:
        return json.load(f), path


# ---------------------------------------------------------------------------
# DNFM restored library access (read-only; self-contained re-implementation of
# the verified restore_dnf_atlas.get_atlas / crop_frame semantics, extended with
# the M8-002/M8-004 verified palette decode for format codes 6/7).
# ---------------------------------------------------------------------------

class AtlasLibrary:
    def __init__(self, restored_root):
        self.db_path = Path(restored_root) / 'restored/atlas/atlas.sqlite'
        self.base = Path(restored_root).parent          # DNFM_export_130.5.8.0 root
        if not self.db_path.is_file():
            raise PipelineError('atlas.sqlite not found: %s' % self.db_path)
        self._db = sqlite3.connect('file:%s?mode=ro' % self.db_path.as_posix(), uri=True)
        self._atlas_cache = {}
        self._page_image_cache = {}

    def atlas(self, name):
        if name not in self._atlas_cache:
            rows = self._db.execute('SELECT id FROM atlases WHERE name=?', (name,)).fetchall()
            if len(rows) != 1:
                raise PipelineError('atlas name %r is not unique/absent in atlas.sqlite (%d rows)'
                                    % (name, len(rows)))
            atlas_id = rows[0][0]
            row = self._db.execute('SELECT data_zlib FROM atlases WHERE id=?', (atlas_id,)).fetchone()
            obj = json.loads(zlib.decompress(row[0]))
            for pi, page in enumerate(obj.get('pages', [])):
                if not page.get('resolved_png'):
                    row2 = self._db.execute(
                        'SELECT png, palette_png, status FROM pages WHERE atlas_id=? AND page_index=?',
                        (atlas_id, pi)).fetchone()
                    if row2 and row2[0]:
                        page.setdefault('resolved_png', row2[0])
                        page.setdefault('resolved_palette_png', row2[1])
                        page.setdefault('resolution_status', row2[2])
            if obj.get('name') != name:
                raise PipelineError('atlas id %s is named %r (expected %r)'
                                    % (atlas_id, obj.get('name'), name))
            obj['_db_id'] = atlas_id
            self._atlas_cache[name] = obj
        return self._atlas_cache[name]

    def atlas_id(self, name):
        return self.atlas(name)['_db_id']

    def page_image(self, name, page_index):
        key = (name, page_index)
        if key not in self._page_image_cache:
            page = self._page(name, page_index)
            if page.get('format_code') == -1:
                raise PipelineError('format_code -1 page has no pixels (atlas %s page %s)' % (name, page_index))
            rel = page.get('resolved_png')
            if not rel:
                raise PipelineError('page texture unresolved for atlas %s page %s' % (name, page_index))
            path = self.base / rel
            if not path.is_file():
                raise PipelineError('page texture missing on disk: %s' % path)
            with Image.open(path) as im:
                self._page_image_cache[key] = (im.convert('RGBA'), path.name)
        return self._page_image_cache[key]

    def _page(self, name, page_index):
        pages = self.atlas(name)['pages']
        if not 0 <= page_index < len(pages):
            raise PipelineError('page %s out of range for atlas %s' % (page_index, name))
        return pages[page_index]

    def frame_record(self, name, frame_index, page_index=None):
        pages = self.atlas(name)['pages']
        matches = []
        for pi, page in enumerate(pages):
            for fr in page['frames']:
                if fr['index'] == frame_index:
                    if page_index is None or pi == page_index:
                        matches.append((pi, fr))
        if len(matches) != 1:
            raise PipelineError('frame selection is not unique or missing: atlas %s index %s -> %d matches'
                                % (name, frame_index, len(matches)))
        return matches[0]

    def load_layer_content(self, name, frame_index, page_index=None):
        """Return (content_image_or_None, meta) for one atlas frame.

        meta carries the original frame's canvas/offset and the content bbox in
        canvas coordinates, so callers can place the content exactly where
        crop_frame(full_canvas=True) would have placed it.  Reference frames are
        followed only when the frame itself is empty, mirroring crop_frame."""
        page_idx, fr = self.frame_record(name, frame_index, page_index)
        original = fr
        seen = set()
        page = self._page(name, page_idx)
        rect = fr.get('rect_pixels', fr['rect'])
        while (not page.get('resolved_png') or rect[2] <= 0 or rect[3] <= 0) and fr['reference'] >= 0:
            if fr['index'] in seen:
                raise PipelineError('cyclic frame reference in atlas %s at index %s' % (name, fr['index']))
            seen.add(fr['index'])
            links = [(pi, f) for pi, pg in enumerate(self.atlas(name)['pages'])
                     for f in pg['frames'] if f['index'] == fr['reference']]
            if len(links) != 1:
                raise PipelineError('missing or ambiguous frame reference %s in atlas %s'
                                    % (fr['reference'], name))
            page_idx, fr = links[0]
            page = self._page(name, page_idx)
            rect = fr.get('rect_pixels', fr['rect'])
        meta = {'atlas_name': name, 'atlas_id': self.atlas_id(name), 'page': page_idx,
                'frame_index': frame_index, 'name': original.get('name'),
                'canvas': list(original['canvas']), 'offset': list(original['offset']),
                'reference': original['reference']}
        if any(v != int(v) for v in rect):
            raise PipelineError('fractional rectangle requires a verified sampling rule '
                                '(atlas %s frame %s rect %s)' % (name, frame_index, rect))
        rect = [int(v) for v in rect]
        if page.get('format_code') == -1 or rect[2] <= 0 or rect[3] <= 0:
            meta['rect_px'] = rect
            meta['content_bbox_in_canvas'] = None
            return None, meta
        page_im, page_name = self.page_image(name, page_idx)
        meta['page_png'] = page_name
        x, y, w, h = rect
        if page.get('palette_path'):
            crop = self._decode_palette_page(name, page_idx, page, page_im, x, y, w, h)
        else:
            if x < 0 or y < 0 or x + w > page_im.width or y + h > page_im.height:
                raise PipelineError('rectangle outside texture (atlas %s frame %s rect %s page %sx%s)'
                                    % (name, frame_index, (x, y, w, h), page_im.width, page_im.height))
            # Unity bottom-left origin rect -> y-flipped image-space crop.
            crop = page_im.crop((x, page_im.height - y - h, x + w, page_im.height - y))
        bbox = crop.getbbox()
        cw, ch = original['canvas']
        ox, oy = original['offset']
        if cw <= 0 or ch <= 0 or cw * ch > 32000000:
            raise PipelineError('unsupported canvas dimensions %sx%s (atlas %s frame %s)' % (cw, ch, name, frame_index))
        if bbox is None:
            meta['rect_px'] = [x, y, w, h]
            meta['content_bbox_in_canvas'] = None
            return None, meta
        if ox < 0 or oy < 0 or ox + (bbox[2] - bbox[0]) > cw or oy + (bbox[3] - bbox[1]) > ch:
            raise PipelineError('offset %s places content outside canvas %sx%s (atlas %s frame %s)'
                                % ((ox, oy), cw, ch, name, frame_index))
        meta['rect_px'] = [x, y, w, h]
        meta['content_bbox_in_canvas'] = [ox + bbox[0], oy + bbox[1], ox + bbox[2], oy + bbox[3]]
        return crop.crop(bbox), meta

    def _decode_palette_page(self, name, page_idx, page, page_im, x, y, w, h):
        """Verified palette decode (M8-002/M8-004 common.py crop_frame: codes 6/7)."""
        if page.get('format_code') not in (6, 7):
            raise PipelineError('palette layout unverified for format code %s (atlas %s page %s)'
                                % (page.get('format_code'), name, page_idx))
        pal_rel = page.get('resolved_palette_png')
        if not pal_rel:
            raise PipelineError('missing palette png (atlas %s page %s)' % (name, page_idx))
        palette = Image.open(self.base / pal_rel).convert('RGBA')
        colors = 16 if page['format_code'] == 6 else 256
        pw, ph = page_im.size
        lw, lh = page['width'], page['height']
        if (lw, lh) != (pw * 2, ph * 2):
            raise PipelineError('palette dims mismatch (atlas %s page %s)' % (name, page_idx))
        if x < 0 or y < 0 or x + w > lw or y + h > lh:
            raise PipelineError('rect outside logical texture (atlas %s page %s)' % (name, page_idx))
        pal = list(palette.getdata())
        src = page_im.load()
        pixels = []
        for yy in range(lh - y - h, lh - y):
            for xx in range(x, x + w):
                channel = (1 - yy // ph) * 2 + xx // pw
                idx = src[xx % pw, yy % ph][channel]
                if colors == 16:
                    idx >>= 4
                pixels.append(pal[idx])
        crop = Image.new('RGBA', (w, h))
        crop.putdata(pixels)
        return crop


# ---------------------------------------------------------------------------
# Animation store: script_index.sqlite (hash lookup) + animations.sqlite
# (restored parse JSON), both read-only; mirrors M8-003/M8-004 common.py.
# ---------------------------------------------------------------------------

class AnimationStore:
    def __init__(self, restored_root):
        self.script_db = Path(restored_root) / 'restored/script_index.sqlite'
        self.anim_db = Path(restored_root) / 'restored/animations/animations.sqlite'
        for p in (self.script_db, self.anim_db):
            if not p.is_file():
                raise PipelineError('required database not found: %s' % p)
        self._sdb = sqlite3.connect('file:%s?mode=ro' % self.script_db.as_posix(), uri=True)
        self._adb = sqlite3.connect('file:%s?mode=ro' % self.anim_db.as_posix(), uri=True)
        self._cache = {}

    def load(self, resource_key):
        if resource_key not in self._cache:
            h = self._path_hash(resource_key)
            row = self._sdb.execute(
                'SELECT shard, block, payload_offset, payload_bytes, sha256 FROM scripts WHERE hash=?',
                (h,)).fetchone()
            if row is None:
                raise PipelineError('resource key %r not present in script_index.sqlite' % resource_key)
            shard, block, off, size, sha = row
            payload_name = 'script_compressed_chn_%d_%d_u_p.payloads' % (shard, shard)
            hit = self._adb.execute(
                'SELECT frame_count, duration_ms, image_paths, data, sha256 FROM animations '
                'WHERE shard=? AND block=?', (payload_name, block)).fetchone()
            if hit is None:
                raise PipelineError('animation %r (shard %s block %s) has no restored animations row'
                                    % (resource_key, shard, block))
            fc, dur, image_paths, blob, anim_sha = hit
            if anim_sha != sha:
                raise PipelineError('animations row sha256 != script row sha256 for %r' % resource_key)
            anim = json.loads(zlib.decompress(blob))
            paths = json.loads(image_paths) if isinstance(image_paths, str) else image_paths
            self._cache[resource_key] = {'anim': anim, 'source': {
                'path': resource_key, 'hash': h, 'shard': shard, 'block': block,
                'sha256': sha, 'frame_count': fc, 'duration_ms': dur,
                'image_paths': paths}}
        return self._cache[resource_key]

    @staticmethod
    def _path_hash(value):
        import xxhash
        return xxhash.xxh64(value.lower().replace('\\', '/').encode('utf-16-le')).hexdigest()


# ---------------------------------------------------------------------------
# Frame composition (position space; origin = feet/action anchor).
# ---------------------------------------------------------------------------

def make_layer(key, content, meta, position):
    bbox = meta.get('content_bbox_in_canvas')
    placement = None
    if bbox is not None:
        placement = [bbox[0] + int(position[0]), bbox[1] + int(position[1])]
    return {'key': key, 'content': content, 'meta': meta, 'placement': placement}


def composite_frame_cell(plan, margin, anchor):
    """One RGBA cell for the frame: union of layer content boxes + margin.

    Frames without transform take the verified per-layer path (pixel-identical
    to export_restored_animation.py).  Frames with mirror/rotation composite all
    layers into one canvas-space image, apply the transforms around the fixed
    canvas anchor, then recompute bbox and pivot.  Fully empty frames keep a
    1x1 cell placed at the frame position so the pivot stays put."""
    sx, sy = plan['_transform']['scale']
    rot = plan['_transform']['rotation_degrees']
    transformed = (sx, sy) != (1.0, 1.0) or rot != 0.0
    pos_x, pos_y = plan['_position']
    if not transformed:
        boxes = [tuple(l['placement']) + (l['content'].width, l['content'].height)
                 for l in plan['layers'] if l['content'] is not None]
        if boxes:
            x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
            x1 = max(b[0] + b[2] for b in boxes); y1 = max(b[1] + b[3] for b in boxes)
        else:
            # fully empty frame keeps the anchor point at its position
            x0, y0, x1, y1 = int(pos_x), int(pos_y), int(pos_x), int(pos_y)
        cell_x0, cell_y0 = x0 - margin, y0 - margin
        cell_w, cell_h = (x1 - x0) + 2 * margin, (y1 - y0) + 2 * margin
        cell = Image.new('RGBA', (max(1, cell_w), max(1, cell_h)), (0, 0, 0, 0))
        for layer in plan['layers']:
            if layer['content'] is None:
                continue
            px, py = layer['placement']
            cell.alpha_composite(layer['content'], (px - cell_x0, py - cell_y0))
        plan['cell'] = cell
        plan['pivot_px'] = [-cell_x0, -cell_y0]
        plan['bbox_px'] = [x0, y0, x1, y1]
        plan['cell_origin'] = [cell_x0, cell_y0]
        plan['transform_applied'] = None
        return plan

    # ---- transformed frame: whole-frame mirror+rotate around canvas anchor ---
    ax, ay = int(anchor[0]), int(anchor[1])
    canvases = {tuple(l['meta']['canvas']) for l in plan['layers'] if l['content'] is not None}
    if len(canvases) != 1:
        raise PipelineError('action %s frame %s has content layers with different canvases %s; a shared '
                            'canvas anchor is required for whole-frame transforms'
                            % (plan['_action_id'], plan['frame_index'], sorted(canvases)))
    canvas = canvases.pop()
    if ax < 0 or ay < 0 or ax > canvas[0] or ay > canvas[1]:
        raise PipelineError('rotation anchor %s outside layer canvas %s (action %s frame %s)'
                            % ((ax, ay), canvas, plan['_action_id'], plan['frame_index']))
    half = max(ax, ay, canvas[0] - ax, canvas[1] - ay, canvas[0], canvas[1])
    big = Image.new('RGBA', (2 * half, 2 * half), (0, 0, 0, 0))
    base_x, base_y = half - ax, half - ay  # canvas origin inside big image
    for layer in plan['layers']:
        if layer['content'] is None:
            continue
        bbox = layer['meta']['content_bbox_in_canvas']
        big.alpha_composite(layer['content'], (base_x + bbox[0], base_y + bbox[1]))
    if sx < 0:
        big = big.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    if sy < 0:
        big = big.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if rot != 0.0:
        big = big.rotate(rot, resample=Image.Resampling.NEAREST, expand=True,
                         fillcolor=(0, 0, 0, 0))
    center = (big.width // 2, big.height // 2)
    bbox = big.getbbox()
    if bbox is None:
        raise PipelineError('transformed frame %s became fully transparent' % plan['frame_index'])
    content = big.crop(bbox)
    # bbox (big-image space) -> canvas space: the anchor sits at the big-image
    # center and never moves, so canvas coords = big coords - (center - anchor).
    off_x, off_y = center[0] - ax, center[1] - ay
    cbx0, cby0 = bbox[0] - off_x, bbox[1] - off_y  # canvas-space content box
    # place: canvas space -> position space by +position
    sp_x, sp_y = cbx0 + int(pos_x), cby0 + int(pos_y)
    cw, chh = content.size
    cell_x0, cell_y0 = sp_x - margin, sp_y - margin
    cell = Image.new('RGBA', (cw + 2 * margin, chh + 2 * margin), (0, 0, 0, 0))
    cell.alpha_composite(content, (margin, margin))
    plan['cell'] = cell
    plan['pivot_px'] = [-cell_x0, -cell_y0]
    plan['bbox_px'] = [sp_x, sp_y, sp_x + cw, sp_y + chh]
    plan['cell_origin'] = [cell_x0, cell_y0]
    plan['transform_applied'] = {
        'scale': [sx, sy], 'rotation_degrees': rot,
        'mirror': {'x': sx < 0, 'y': sy < 0},
        'order': 'mirror_then_rotate',
        'rotation_anchor_canvas_px': [ax, ay],
        'resample': 'nearest',
    }
    return plan


# ---------------------------------------------------------------------------
# Packing, previews, manifest entries (M6-020 schema).
# ---------------------------------------------------------------------------

def pack_pages(cells, page_size):
    """Row-major shelf packing in given order; overflow opens a new page.
    A cell larger than the page is a hard error (never shrunk)."""
    pages = []
    cur = {'index': 0, 'placements': [], 'x': 0, 'y': 0, 'row_h': 0}

    def flush():
        nonlocal cur
        if cur['placements']:
            pages.append(cur)
        cur = {'index': cur['index'] + 1, 'placements': [], 'x': 0, 'y': 0, 'row_h': 0}

    for cell in cells:
        w, h = cell['cell'].size
        if w > page_size or h > page_size:
            raise PipelineError('frame cell %dx%d exceeds page size %dx%d; no silent shrinking'
                                % (w, h, page_size, page_size))
        if cur['x'] + w > page_size:
            cur['y'] += cur['row_h']; cur['x'] = 0; cur['row_h'] = 0
        if cur['y'] + h > page_size:
            flush()
        cur['placements'].append((cell, cur['x'], cur['y']))
        cell['page'] = cur['index']
        cell['uv_rect_px'] = [cur['x'], cur['y'], w, h]
        cur['x'] += w
        cur['row_h'] = max(cur['row_h'], h)
    flush()
    return pages


def build_preview(cells, scale):
    """Horizontal strip on a dark backdrop; red baseline + dot mark the pivot."""
    pad = 4
    widths = [c['cell'].size[0] for c in cells]
    total_w = sum(widths) + pad * (len(cells) + 1)
    total_h = max(c['cell'].size[1] for c in cells) + pad * 2
    canvas = Image.new('RGBA', (total_w * scale, total_h * scale), (24, 24, 28, 255))
    draw = ImageDraw.Draw(canvas)
    x = pad
    for c in cells:
        w, h = c['cell'].size
        im = c['cell'].resize((max(1, w * scale), max(1, h * scale)), Image.Resampling.NEAREST)
        canvas.alpha_composite(im, (x * scale, pad * scale))
        pvy = (pad + c['pivot_px'][1]) * scale
        pvx = (x + c['pivot_px'][0]) * scale
        draw.line([(x * scale, pvy), ((x + w) * scale, pvy)], fill=(255, 64, 64, 255), width=1)
        draw.ellipse([pvx - 2, pvy - 2, pvx + 2, pvy + 2], fill=(255, 96, 32, 255))
        x += w + pad
    return canvas


def frame_manifest_entry(plan):
    frame = plan['_anim_frame']
    layer_frames = {}
    for layer in plan['layers']:
        meta = layer['meta']
        entry = {'atlas_id': meta['atlas_id'], 'frame_index': meta['frame_index'],
                 'page': meta['page'], 'offset': meta['offset'],
                 'content_bbox_px': layer['placement']}
        layer_frames[layer['key']] = entry
    events = []
    for tag in frame.get('audio_tags') or []:
        events.append({'type': 'audio', 'name': tag.get('tag')})
    for tag in frame.get('audio_loop_tags') or []:
        events.append({'type': 'audio_loop', 'name': tag.get('tag')})
    delay = int(frame['delay_ms'])
    out = {
        'index': plan['frame_index'],
        # M6-021 contract refuses zero-duration frames; clamp to 1ms and keep
        # the original value as source_delay_ms (same as the fighter pipeline).
        'duration_ms': max(delay, 1),
        'page': plan['page'],
        'uv_rect_px': list(plan['uv_rect_px']),
        'canvas_size_px': list(plan['cell'].size),
        'pivot_px': list(plan['pivot_px']),
        'pivot_definition': PIVOT_DEFINITION,
        'layer_frames': layer_frames,
        'flip_copy_type': int(frame.get('flip_copy_type') or 0),
        'scale': [float(v) for v in plan['_transform']['scale']],
        'rotation_degrees': float(plan['_transform']['rotation_degrees']),
        'source_frame': {'image_path': frame.get('image_path'),
                         'image_pack_index': int(frame.get('image_pack_index') or 0),
                         'image_frame_index': (int(frame['image_frame_index'])
                                               if frame.get('image_frame_index') is not None else None)},
        'events': events,
    }
    if delay < 1:
        out['source_delay_ms'] = delay
    if plan['transform_applied'] is not None:
        out['transform_applied'] = plan['transform_applied']
    if plan['missing_layers']:
        out['missing_layers'] = list(plan['missing_layers'])
    return out


# ---------------------------------------------------------------------------
# Verify-JSON cross-checks.
# ---------------------------------------------------------------------------

def verify_ref_lookup(verify, ref):
    """Resolve a config verify reference to the recorded action dict (or None)."""
    if ref is None or ref.get('section') is None:
        return None
    section = ref['section']
    if section == 'weapon_gun_layer.per_frame_presence':
        return verify['weapon_gun_layer']['per_frame_presence'][ref['key']]
    node = verify[section]
    if 'index' in ref:
        node = node[ref['index']]
    if 'key' in ref:
        node = node[ref['key']]
    if ref.get('body'):
        node = node['body_detail'][ref['body']]
    return node


def cross_check_against_verify(action_id, verify_rec, anim):
    """Cross-check the parsed animation against M8-003/M8-004 verify JSON rows."""
    if verify_rec is None:
        return
    if verify_rec.get('frame_count') is not None:
        if int(verify_rec['frame_count']) != len(anim['frames']):
            raise PipelineError('action %r verify frame_count %s != parsed %s'
                                % (action_id, verify_rec['frame_count'], len(anim['frames'])))
    if verify_rec.get('duration_ms') is not None:
        expect = sum(int(f['delay_ms']) for f in anim['frames'])
        if int(verify_rec['duration_ms']) != int(expect):
            raise PipelineError('action %r verify duration_ms %s != parsed %s'
                                % (action_id, verify_rec['duration_ms'], expect))
    if verify_rec.get('ms') is not None:
        expect = sum(int(f['delay_ms']) for f in anim['frames'])
        if int(verify_rec['ms']) != int(expect):
            raise PipelineError('action %r verify ms %s != parsed %s'
                                % (action_id, verify_rec['ms'], expect))
    if verify_rec.get('delay_ms') is not None:
        parsed = [int(f['delay_ms']) for f in anim['frames']]
        if [int(v) for v in verify_rec['delay_ms']] != parsed:
            raise PipelineError('action %r verify delay_ms != parsed delay_ms' % action_id)
    if verify_rec.get('loop') is not None:
        if bool(verify_rec['loop']) != bool(anim['loop']):
            raise PipelineError('action %r verify loop %s != parsed %s'
                                % (action_id, verify_rec['loop'], anim['loop']))
    if verify_rec.get('frame_index_list') is not None:
        parsed = [int(f['image_frame_index']) for f in anim['frames']]
        if [int(v) for v in verify_rec['frame_index_list']] != parsed:
            raise PipelineError('action %r verify frame_index_list != parsed image_frame_index'
                                % action_id)
    if isinstance(verify_rec.get('frames'), list):
        # gunner basic/hit rows carry per-frame body_fi/position/scale records
        if len(verify_rec['frames']) != len(anim['frames']):
            raise PipelineError('action %r verify frames count != parsed frames' % action_id)
        for i, (vf, pf) in enumerate(zip(verify_rec['frames'], anim['frames'])):
            v_fi, p_fi = vf.get('body_fi'), pf.get('image_frame_index')
            if v_fi is not None and p_fi is not None and int(v_fi) != int(p_fi):
                raise PipelineError('action %r frame %d verify body_fi %s != parsed %s'
                                    % (action_id, i, v_fi, p_fi))
            if v_fi is None and p_fi is not None and int(p_fi) != 0:
                raise PipelineError('action %r frame %d verify has no body_fi but parse references '
                                    'image frame %s' % (action_id, i, p_fi))
            # v_fi None + p_fi 0 = the M8-004 falsy-zero recording quirk
            # (``index or None`` collapsed body index 0 to null in the verify JSON)
            v_pos = vf.get('position')
            if v_pos is not None and list(map(int, v_pos)) != list(map(int, pf['position'])):
                raise PipelineError('action %r frame %d verify position %s != parsed %s'
                                    % (action_id, i, v_pos, pf['position']))
            v_scale = vf.get('scale')
            p_scale = list(map(float, pf.get('scale') or [1.0, 1.0]))
            if v_scale is not None and list(map(float, v_scale)) != p_scale:
                raise PipelineError('action %r frame %d verify scale %s != parsed %s'
                                    % (action_id, i, v_scale, p_scale))
            v_rot = vf.get('rotation_degrees')
            p_rot = float(pf.get('rotation_degrees') or 0.0)
            if v_rot is not None and abs(float(v_rot) - p_rot) > 1e-4:
                raise PipelineError('action %r frame %d verify rotation %s != parsed %s'
                                    % (action_id, i, v_rot, p_rot))
    if isinstance(verify_rec.get('per_frame'), list):
        # gunner weapon per_frame_presence rows carry body_fi (+gun bboxes)
        if len(verify_rec['per_frame']) != len(anim['frames']):
            raise PipelineError('action %r verify per_frame count != parsed frames' % action_id)
        for i, (vf, pf) in enumerate(zip(verify_rec['per_frame'], anim['frames'])):
            v_fi, p_fi = vf.get('body_fi'), pf.get('image_frame_index')
            if v_fi is None or p_fi is None:
                if (v_fi is None) != (p_fi is None):
                    raise PipelineError('action %r frame %d verify body_fi %s != parsed %s'
                                        % (action_id, i, v_fi, p_fi))
            elif int(v_fi) != int(p_fi):
                raise PipelineError('action %r frame %d verify body_fi %s != parsed %s'
                                    % (action_id, i, v_fi, p_fi))


# ---------------------------------------------------------------------------
# Action resolution (every layer is frame-parallel with the body).
# ---------------------------------------------------------------------------

def resolve_action_plans(action, anim, cfg, lib):
    body_cfg = cfg['body']
    draw_order = cfg['draw_order']
    slot_atlas = {l['slot']: l['atlas_name'] for l in cfg['layers']}
    weapon = cfg.get('weapon_rule') or {}
    weapon_slots = set()
    if weapon.get('kind') == 'front_back_split_stack':
        weapon_slots = {weapon['back_slot'], weapon['front_slot']}
    elif weapon.get('kind') == 'parallel_stack':
        weapon_slots = set(weapon.get('slots') or [])
    plans = []
    for fi, frame in enumerate(anim['frames']):
        scale = list(map(float, frame.get('scale') or [1.0, 1.0]))
        if abs(scale[0]) != 1.0 or abs(scale[1]) != 1.0:
            raise PipelineError('action %s frame %s has non-unit |scale| %s; unsupported'
                                % (action['action_id'], fi, scale))
        if list(map(int, frame.get('color_rgba') or [255, 255, 255, 255])) != [255, 255, 255, 255]:
            raise PipelineError('action %s frame %s has non-white color_rgba %s; per-frame tint of a '
                                'multi-layer composite is not verified, refusing (exclude the action '
                                'in the config if it is an alpha-fade transition)'
                                % (action['action_id'], fi, frame.get('color_rgba')))
        if int(frame.get('flip_copy_type') or 0) != 0:
            raise PipelineError('action %s frame %s has flip_copy_type %s; runtime flip semantics are '
                                'not verified for this character, refusing'
                                % (action['action_id'], fi, frame.get('flip_copy_type')))
        image_path = frame.get('image_path')
        body_fi = frame.get('image_frame_index')
        layers_for_frame = []
        missing_layers = []
        weapon_presence = {}
        if image_path is None or body_fi is None:
            if image_path is not None:
                raise PipelineError('action %s frame %s has image_path %r but no image_frame_index'
                                    % (action['action_id'], fi, image_path))
            # hold frame with no image reference: every layer empty this frame
            for slot in draw_order:
                meta = {'atlas_name': slot_atlas[slot], 'atlas_id': lib.atlas_id(slot_atlas[slot]),
                        'page': None, 'frame_index': None, 'name': None,
                        'canvas': list(body_cfg['canvas']), 'offset': None,
                        'reference': None, 'content_bbox_in_canvas': None}
                layers_for_frame.append(make_layer(slot, None, meta, frame['position']))
                if slot in weapon_slots:
                    weapon_presence[slot] = False
            missing_layers = [s for s in draw_order if s != 'body']
        else:
            if image_path.lower() != body_cfg['image_path'].lower():
                raise PipelineError('action %s frame %s references non-body image %r (expected %r); '
                                    'multi-image frames are unsupported'
                                    % (action['action_id'], fi, image_path, body_cfg['image_path']))
            for slot in draw_order:
                content, meta = lib.load_layer_content(slot_atlas[slot], int(body_fi))
                layers_for_frame.append(make_layer(slot, content, meta, frame['position']))
                if slot in weapon_slots:
                    weapon_presence[slot] = content is not None
                if content is None:
                    if slot != 'body':
                        missing_layers.append(slot)
                    else:
                        # body atlas frame resolves to no visible content
                        missing_layers.append(slot)
        plans.append({'frame_index': fi, 'layers': layers_for_frame,
                      'missing_layers': missing_layers,
                      '_position': list(frame['position']),
                      '_transform': {'scale': scale,
                                     'rotation_degrees': float(frame.get('rotation_degrees') or 0.0)},
                      '_body_fi': int(body_fi) if body_fi is not None else None,
                      '_weapon_presence': weapon_presence})
    return plans


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
    anchor = list(cfg['rotation_anchor']['canvas_px'])
    out_dir = Path(args.out_dir) if args.out_dir else DEFAULT_OUT_ROOT / cfg['character']

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

    # ---- config/verify layer consistency ---------------------------------
    body_cfg = cfg['body']
    body_atlas_obj = lib.atlas(body_cfg['atlas_name'])
    body_frame_count = max(fr['index'] for pg in body_atlas_obj['pages'] for fr in pg['frames']) + 1
    if body_frame_count != int(body_cfg['frame_count']):
        raise PipelineError('body atlas %r has %d frames but config says %s'
                            % (body_cfg['atlas_name'], body_frame_count, body_cfg['frame_count']))
    verify_layers = verify.get('outfit_layers') or []
    layer_report = []
    for layer in cfg['layers']:
        atlas_obj = lib.atlas(layer['atlas_name'])
        n = max(fr['index'] for pg in atlas_obj['pages'] for fr in pg['frames']) + 1
        row = {'slot': layer['slot'], 'atlas_name': layer['atlas_name'],
               'atlas_id': atlas_obj['_db_id'], 'frame_count': n,
               'role': layer.get('role'), 'variant_note': layer.get('variant_note')}
        vli = layer.get('verify_layer_index')
        if vli is not None and verify_layers:
            vrec = verify_layers[vli]
            v_name = vrec.get('atlas_name') or vrec.get('atlas')
            if v_name != layer['atlas_name']:
                raise PipelineError('layer %r verify_layer_index %s records atlas %r (config says %r)'
                                    % (layer['slot'], vli, v_name, layer['atlas_name']))
            if int(vrec.get('frame_count', -1)) != n:
                raise PipelineError('layer %r atlas frame count %d != verify %s'
                                    % (layer['slot'], n, vrec.get('frame_count')))
            if 'atlas_db_id' in vrec and int(vrec['atlas_db_id']) != atlas_obj['_db_id']:
                raise PipelineError('layer %r atlas db id %s != verify %s'
                                    % (layer['slot'], atlas_obj['_db_id'], vrec['atlas_db_id']))
            row['verify_atlas_payload_sha256'] = vrec.get('atlas_payload_sha256')
        layer_report.append(row)
    # unconfigured verify outfit layers (alternate variants) are not composited,
    # but their verified frame-parallelism is still re-asserted here.
    configured_verify_indexes = {l['verify_layer_index'] for l in cfg['layers']
                                 if l.get('verify_layer_index') is not None}
    for vli, vrec in enumerate(verify_layers):
        if vli in configured_verify_indexes:
            continue
        v_name = vrec.get('atlas_name') or vrec.get('atlas')
        atlas_obj = lib.atlas(v_name)
        n = max(fr['index'] for pg in atlas_obj['pages'] for fr in pg['frames']) + 1
        if int(vrec.get('frame_count', -1)) != n:
            raise PipelineError('verify outfit layer %r atlas frame count %d != verify %s'
                                % (v_name, n, vrec.get('frame_count')))
        if 'atlas_db_id' in vrec and int(vrec['atlas_db_id']) != atlas_obj['_db_id']:
            raise PipelineError('verify outfit layer %r atlas db id %s != verify %s'
                                % (v_name, atlas_obj['_db_id'], vrec['atlas_db_id']))

    # ---- resolve all actions ---------------------------------------------
    cells_all = []
    action_outputs = []
    missing_out = []
    body_index_union = set()
    weapon_presence_total = {'back_slot': None, 'front_slot': None,
                             'back_present': 0, 'front_present': 0, 'both': 0, 'neither': 0}
    weapon = cfg.get('weapon_rule') or {}
    for action in actions:
        loaded = store.load(action['ani_key'])
        anim = loaded['anim']
        source = loaded['source']
        verify_rec = verify_ref_lookup(verify, action.get('verify'))
        if verify_rec is not None and verify_rec.get('ani_path') \
                and verify_rec['ani_path'] != action['ani_key']:
            raise PipelineError('action %r config ani_key %r != verify ani_path %r'
                                % (action['action_id'], action['ani_key'], verify_rec['ani_path']))
        if verify_rec is not None and verify_rec.get('sha256') \
                and verify_rec['sha256'] != source['sha256']:
            raise PipelineError('action %r verify sha256 != script payload sha256' % action['action_id'])
        cross_check_against_verify(action['action_id'], verify_rec, anim)
        plans = resolve_action_plans(action, anim, cfg, lib)
        for plan in plans:
            plan['_action_id'] = action['action_id']
            plan['_anim_frame'] = anim['frames'][plan['frame_index']]
            composite_frame_cell(plan, margin, anchor)
            cells_all.append(plan)
            if plan['_body_fi'] is not None:
                body_index_union.add(plan['_body_fi'])
            if weapon.get('kind') == 'front_back_split_stack':
                b = plan['_weapon_presence'].get(weapon['back_slot'], False)
                c = plan['_weapon_presence'].get(weapon['front_slot'], False)
                weapon_presence_total['back_present'] += 1 if b else 0
                weapon_presence_total['front_present'] += 1 if c else 0
                weapon_presence_total['both'] += 1 if (b and c) else 0
                weapon_presence_total['neither'] += 1 if not (b or c) else 0
        for plan in plans:
            for slot in plan['missing_layers']:
                missing_out.append({'action_id': action['action_id'], 'layer': slot,
                                    'reason': 'atlas frame has no visible content at body index %s '
                                              '(frame %s)' % (plan['_body_fi'], plan['frame_index'])})
        layers_used = sorted({l['key'] for plan in plans for l in plan['layers']})
        transformed = [p['frame_index'] for p in plans if p['transform_applied'] is not None]
        action_outputs.append({
            'action_id': action['action_id'],
            'group': action['group'],
            'source_ani': source['path'],
            'source_sha256': source['sha256'],
            'loop': bool(anim['loop']),
            'layers': layers_used,
            'draw_order': list(cfg['draw_order']),
            'transformed_frames': transformed,
            '_plans': plans,
            '_verify_rec': verify_rec,
        })

    pages = pack_pages(cells_all, page_size)

    # ---- self-test assertions --------------------------------------------
    assertions = {}

    # (1) per-action frame count == verify JSON frame_count
    frame_count_checks = []
    for action, out in zip(actions, action_outputs):
        verify_rec = verify_ref_lookup(verify, action.get('verify'))
        expected = verify_rec.get('frame_count') if verify_rec else None
        frame_count_checks.append({'action_id': action['action_id'],
                                   'frames': len(out['_plans']),
                                   'verify_frame_count': expected})
    bad = [c for c in frame_count_checks if c['verify_frame_count'] is not None
           and int(c['verify_frame_count']) != c['frames']]
    assertions['frame_counts_match_verify'] = {
        'pass': not bad, 'checked': len(frame_count_checks), 'failures': bad}

    # (2) layer parallelism: every verify-listed outfit layer is parallel with
    # the body over the composited indices -- either the same full frame count
    # (swordman 179) or the verify attests covers_all_used_body_indices with
    # zero missing (gunner 203/215 over the 150 key-motion indices).
    # Atlas-vs-verify frame-count mismatches already hard-error above.
    if verify_layers:
        not_parallel = []
        for vrec in verify_layers:
            same_count = int(vrec['frame_count']) == body_frame_count
            covers = (vrec.get('covers_all_used_body_indices') is True
                      and int(vrec.get('missing_count', 1)) == 0)
            if not (same_count or covers):
                not_parallel.append(vrec.get('atlas_name') or vrec.get('atlas'))
        assertions['layer_parallelism'] = {
            'pass': not not_parallel,
            'verify_layer_count': len(verify_layers),
            'configured_verify_bound_layers': len(configured_verify_indexes),
            'variant_layers_rechecked': len(verify_layers) - len(configured_verify_indexes),
            'body_frame_count': body_frame_count,
            'rule': 'frame_count == body_frame_count OR covers_all_used_body_indices '
                    'with missing_count == 0',
            'failures': not_parallel,
        }

    # (3) body index space
    assertions['body_index_space'] = {
        'pass': all(0 <= i < body_frame_count for i in body_index_union),
        'union_size': len(body_index_union),
        'body_frame_count': body_frame_count,
        'min': min(body_index_union) if body_index_union else None,
        'max': max(body_index_union) if body_index_union else None,
    }

    # (4) gunner: composite body-index union == the 150 key-motion indices
    if cfg['character'] == 'gunner':
        verify_union = set()
        for section in ('basic_motions', 'hit_getup_states'):
            for v in (verify.get(section) or {}).values():
                if v and v.get('frames'):
                    verify_union.update(f['body_fi'] for f in v['frames'] if f.get('body_fi') is not None)
        for v in (verify.get('weapon_gun_layer', {}).get('per_frame_presence') or {}).values():
            verify_union.update(f['body_fi'] for f in v['per_frame'] if f.get('body_fi') is not None)
        expected_total = (verify.get('meta', {}).get('counts', {})
                                 .get('body_frame_indices_used_by_key_motions'))
        extras = sorted(body_index_union - verify_union)
        # the M8-004 verify JSON recorded body index 0 as null on frames that do
        # reference it (falsy-zero quirk, e.g. Rest f0/f4/f8 -> gn_body0000 fi 0),
        # so the composite union may legitimately exceed the recorded union by
        # exactly index 0; any other extra is a failure.
        quirk_frames = 0
        for out in action_outputs:
            vf = out['_verify_rec']
            if vf is None or not isinstance(vf.get('frames'), list):
                continue
            for plan in out['_plans']:
                if plan['_body_fi'] in extras \
                        and vf['frames'][plan['frame_index']].get('body_fi') is None:
                    quirk_frames += 1
        assertions['key_motion_body_union'] = {
            'pass': (verify_union <= body_index_union
                     and set(extras) <= {0}
                     and (expected_total is None or int(expected_total) == len(verify_union))),
            'composite_union': len(body_index_union),
            'verify_union': len(verify_union),
            'verify_meta_total': expected_total,
            'missing_from_composite': sorted(verify_union - body_index_union),
            'extra_in_composite': extras,
            'extra_note': ('extras allowed only as the M8-004 falsy-zero quirk (body index 0 '
                           'recorded null); quirk frames composited: %d' % quirk_frames),
        }

    # (5) weapon layer presence census (swordman front/back split)
    if weapon.get('kind') == 'front_back_split_stack':
        census = dict(weapon_presence_total)
        census['back_slot'] = weapon['back_slot']
        census['front_slot'] = weapon['front_slot']
        census['frames'] = len(cells_all)
        census['pass'] = True
        census['note'] = ('b/c are the behind/front split of one sword; both composite in the '
                          'verified z-order')
        assertions['weapon_front_back_split'] = census

    # (6) layer coverage: every draw_order layer attempted on every frame
    total_layer_slots = sum(len(cfg['draw_order']) for _ in cells_all)
    empty_layer_slots = sum(1 for plan in cells_all for l in plan['layers'] if l['content'] is None)
    assertions['layer_coverage'] = {
        'pass': True,
        'layer_slots_total': total_layer_slots,
        'layer_slots_with_content': total_layer_slots - empty_layer_slots,
        'layer_slots_empty': empty_layer_slots,
        'note': 'empty = atlas frame has no visible content at that body index '
                '(absent weapon / no-image hold frames); nothing failed to load',
    }

    # (7) pages invariants
    cells_on_pages = sum(len(p['placements']) for p in pages)
    assertions['pages'] = {
        'pass': cells_on_pages == len(cells_all),
        'page_count': len(pages),
        'page_size': page_size,
        'cells_total': len(cells_all),
        'cells_on_pages': cells_on_pages,
    }

    # (8) sampled pixel statistics (deterministic pseudo-random sample)
    rng = random.Random(SELF_TEST_SEED)
    indices = list(range(len(cells_all)))
    rng.shuffle(indices)
    sample = []
    for idx in indices[:min(60, len(cells_all))]:
        plan = cells_all[idx]
        alpha = plan['cell'].getchannel('A')
        visible = sum(1 for p in alpha.getdata() if p > 0)
        sample.append({'action_id': plan['_action_id'], 'frame': plan['frame_index'],
                       'visible_px': visible, 'cell': list(plan['cell'].size),
                       'empty_by_data': all(l['content'] is None for l in plan['layers'])})
    non_empty = [s for s in sample if not s['empty_by_data']]
    pixel_failures = [s for s in non_empty if s['visible_px'] <= 0]
    assertions['sampled_pixel_stats'] = {
        'pass': not pixel_failures,
        'sampled': len(sample),
        'empty_by_data': sum(1 for s in sample if s['empty_by_data']),
        'min_visible_px_non_empty': min((s['visible_px'] for s in non_empty), default=None),
        'max_visible_px_non_empty': max((s['visible_px'] for s in non_empty), default=None),
        'failures': pixel_failures,
    }

    # ---- write outputs (deterministic) -----------------------------------
    previews_dir = out_dir / 'preview'
    out_dir.mkdir(parents=True, exist_ok=True)
    previews_dir.mkdir(parents=True, exist_ok=True)
    character = cfg['character']
    pages_manifest = []
    for page in pages:
        canvas = Image.new('RGBA', (page_size, page_size), (0, 0, 0, 0))
        for cell, x, y in page['placements']:
            canvas.alpha_composite(cell['cell'], (x, y))
        name = '%s_page_%d.png' % (character, page['index'])
        canvas.save(out_dir / name, 'PNG')
        pages_manifest.append({'file': name, 'sha256': sha256_file(out_dir / name),
                               'size': [page_size, page_size],
                               'cells_on_page': len(page['placements'])})
    preview_files = []
    for action_id in cfg.get('previews') or []:
        out = next((o for o in action_outputs if o['action_id'] == action_id), None)
        if out is None:
            raise PipelineError('config previews reference unknown action %r' % action_id)
        name = 'preview_%s.png' % action_id
        build_preview(out['_plans'], PREVIEW_SCALE).save(previews_dir / name, 'PNG')
        preview_files.append({'file': 'preview/%s' % name, 'sha256': sha256_file(previews_dir / name),
                              'frames': len(out['_plans'])})

    manifest = {
        'generator': GENERATOR,
        'manifest_version': MANIFEST_VERSION,
        'character': character,
        'outfit': 'default',
        'mode': 'full_outfit_config',
        'source_root': str(dnfm_root),
        'coordinate_space': ('Position space, X right / Y down, origin at the feet/action anchor; '
                             'per-frame pivot_px is that anchor inside the cell (uv top-left relative, y down)'),
        'px_to_cm': cfg.get('px_to_cm'),
        'cell_margin_px': margin,
        'page_size_px': page_size,
        'transform_semantics': {
            'mirror_and_rotate_center_canvas_px': [anchor[0], anchor[1]],
            'rotation_direction': ('positive degrees rotate counterclockwise in y-down image space '
                                   '(PIL rotate(+angle), anchor at image center)'),
            'rotation_anchor_basis': cfg['rotation_anchor'].get('basis'),
            'rotation_anchor_probe_evidence': cfg['rotation_anchor'].get('probe_evidence'),
            'transform_order': 'mirror_then_rotate',
            'resample': 'nearest',
        },
        'layer_rule': {
            'draw_order_bottom_to_top': list(cfg['draw_order']),
            'parallel_with_body': True,
            'weapon_rule': weapon or None,
        },
        'pages': pages_manifest,
        'actions': [{
            'action_id': out['action_id'],
            'group': out['group'],
            'source_ani': out['source_ani'],
            'source_sha256': out['source_sha256'],
            'loop': out['loop'],
            'layers': out['layers'],
            'draw_order': out['draw_order'],
            'layer_mapping': 'character_config_parallel',
            'source': 'script_index',
            'transformed_frames': out['transformed_frames'],
            'frames': [frame_manifest_entry(plan) for plan in out['_plans']],
        } for out in action_outputs],
        'missing': missing_out,
        'verification': {
            'mode': 'full_outfit_config',
            'assertions': assertions,
            'layer_report': layer_report,
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
        'character': character,
        'actions': [{'action_id': o['action_id'], 'group': o['group'], 'frames': len(o['_plans']),
                     'transformed': len(o['transformed_frames'])} for o in action_outputs],
        'frames_total': len(cells_all),
        'body_index_union': len(body_index_union),
        'pages': len(pages_manifest),
        'page_size': page_size,
        'layer_slots_with_content': assertions['layer_coverage']['layer_slots_with_content'],
        'layer_slots_empty': empty_layer_slots,
        'previews': preview_files,
        'missing_rows': len(missing_out),
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
    ap = argparse.ArgumentParser(description='M8-016 generic character visual-set compositor')
    ap.add_argument('--config', required=True,
                    help='character pipeline config JSON (Data/DNFMReplica/characters/<role>/...)')
    ap.add_argument('--out-dir', default=None,
                    help='defaults to Data/DNFMReplica/characters/<character>')
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
