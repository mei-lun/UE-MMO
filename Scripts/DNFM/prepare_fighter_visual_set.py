#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M6-020: offline "full outfit" fighter animation compositor (DNFM P1 pipeline).

Composites DNF Mobile restored animation frames (body + hair/coat/shoes outfit
layers) into paged RGBA atlases (<= 2048x2048) plus a machine-readable manifest
consumed by M6-021 (C++ data assets) and M6-022 (UE editor import), and a
per-action preview strip for human review.

Alignment contract (reproduces the verified exporter
DNFM_export_130.5.8.0/tools/export_restored_animation.py exactly):
  * atlas.sqlite frame rects are Unity bottom-left origin pixels on the page;
    crops are y-flipped into image space (restore_dnf_atlas.crop_frame).
  * each layer frame has a canvas (500x500 for the fighter outfit set) and an
    offset placing the crop inside that canvas; all outfit layers share the same
    canvas/anchor convention, so compositing every layer at (canvas content bbox
    corner + animation position) yields pixel-correct relative alignment.
  * the animation frame ``position`` places the shared canvas; the position-space
    origin (0,0) is the character feet/action anchor. Every frame cell keeps a
    transparent margin; per-frame pivot_px is that anchor inside the cell. The
    cross-action feet origin cannot jump: it is always the same position-space
    point.

Mapping input (M6-019 outfit_mapping.json, --mapping):
  Top level: {character, outfit, layers:{part:{atlas_id,...}}, draw_order:
  {bottom_to_top:[...]}, actions:[...], missing:[...], skill_animations:{...},
  layer_frames_semantics, ...}. Layer entries inside frames: {atlas_id,
  frame_index, page, offset, canvas, rect_pixels, reference_chain,
  resolved_frame_index}. ``offset`` is the atlas frame's own offset within the
  shared 500x500 canvas (cross-checked against atlas.sqlite; mismatch is a hard
  error); rect_pixels/canvas are cross-checked too. Actions covered by
  mapping["actions"] use their explicit layer_frames; all other actions (DB
  skills/states) composite the outfit layers with the body's frame index
  (M6-019 verified 136-frame parallelism across layers).

Transform support (plan section 10, per-branch):
  * negative scale components = horizontal (sx<0) / vertical (sy<0) mirror of
    the whole composited frame; |scale| != 1 is rejected.
  * rotation_degrees: the whole composited frame is rotated around the fixed
    canvas anchor R=(rotation-anchor-x, rotation-anchor-y) (default 250, 292).
    That anchor was reverse-engineered from the overturn/ftRecoveryRoll frames:
    it is the only candidate consistent with both the overturn damage box
    (body occupies 0..78 px above ground while handstanding) and roll-frame
    continuity (front roll f1 rot=0 body y -73..-25 vs f3 rot=180 body y
    -64..-16). Positive degrees rotate counterclockwise in y-down image space
    (PIL rotate(+angle) with the anchor at the image center). Not
    native-code-verified; recorded per frame and reviewed via previews.
  * order: mirror first, then rotate; nearest-neighbour resampling; bounding
    boxes and pivots recomputed after each transform and recorded.
  * non-white color_rgba, palette pages, unresolved textures, cells larger
    than the page and unknown mapping keys are hard errors. Nothing is
    silently dropped or shrunk.

Determinism: same inputs + same arguments -> byte-identical outputs (no
timestamps or machine-specific data in any artifact).

Standard library + Pillow only (plus optional import of the DNFM restore
tools for parsing .ani payloads from script_index.sqlite, mirroring
export_restored_animation.py). Run with ``py -I``.
"""

import argparse
import glob
import hashlib
import json
import sqlite3
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

GENERATOR = 'Scripts/DNFM/prepare_fighter_visual_set.py'
MANIFEST_VERSION = 2
PIVOT_DEFINITION = '脚底/动作原点在 cell 内的位置（相对 uv_rect 左上，y 向下）'
EXPORTER_MARGIN = 2          # transparent margin used by the verified exporter
DEFAULT_PAGE = 2048
DEFAULT_MARGIN = EXPORTER_MARGIN
PREVIEW_SCALE = 2
DEFAULT_DNFM_ROOT = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0')
REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / 'SourceAssets/Unpacked/DNFM130_5_8_202/Characters/Fighter/composite'
BODY_LAYER = 'ft_body0000'

# Canvas-space anchor for whole-frame mirror/rotation (see module docstring).
DEFAULT_ANCHOR = (250, 292)

BASE_ACTIONS = ('rest', 'move', 'dash', 'jump', 'attack1', 'attack2', 'attack3',
                'attack4', 'jumpattack', 'dashattack', 'down', 'sit')
# Non-ue_ready actions (M6-019 resource keys), loaded from script_index.sqlite.
DB_ACTIONS = {
    'damage1': 'script/character/fighter/animation/damage1.ani',
    'damage2': 'script/character/fighter/animation/damage2.ani',
    'overturn': 'script/character/fighter/animation/overturn.ani',
    'ftrecoveryrollback_body': 'script/character/fighter/animation/ftrecoveryrollback_body.ani',
    'ftrecoveryrollfront_body': 'script/character/fighter/animation/ftrecoveryrollfront_body.ani',
    'die': 'script/character/fighter/animation/die.ani',
    'liftupper': 'script/character/fighter/animation/liftupper.ani',
    'singlekick': 'script/character/fighter/animation/singlekick.ani',
    'damagelowkick': 'script/character/fighter/animation/damagelowkick.ani',
    'throwsand_sp': 'script/character/fighter/animation/throwsand_sp.ani',
    'earthbreak': 'script/character/fighter/animation/earthbreak.ani',
    'stomp': 'script/character/fighter/animation/stomp.ani',
    'stompfinish': 'script/character/fighter/animation/stompfinish.ani',
}
OUTFIT_PARTS = ('ft_hair0000a', 'ft_coat0000a', 'ft_shoes0000a')

LAYER_ENTRY_KEYS = {'atlas_id', 'frame_index', 'page', 'offset', 'src_rect_px',
                    'placement_delta', 'color_rgba', 'canvas', 'rect_pixels',
                    'reference_chain', 'resolved_frame_index'}
FRAME_KEYS = {'frame_index', 'index', 'duration_ms', 'source_position',
              'layer_frames', 'missing', 'scale', 'rotation_degrees',
              'flip_copy_type', 'note'}
ACTION_KEYS = {'action_id', 'source_ani', 'source_sha256', 'duration_ms',
               'frame_count', 'layer_canvas_size', 'layers', 'loop', 'frames',
               'missing', 'note', 'evidence'}
TOP_KEYS = {'generator', 'character', 'outfit', 'layers', 'draw_order', 'actions',
            'note', 'evidence', 'missing', 'animations_db', 'atlas_db',
            'script_index_db', 'deterministic', 'hash_algorithm',
            'layer_frames_semantics', 'skill_animations', 'source_root', 'task',
            'verified_poses'}


class PipelineError(RuntimeError):
    """Any unsupported or inconsistent input; never silently degraded."""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# DNFM restored library access (read-only; self-contained re-implementation of
# the verified restore_dnf_atlas.get_atlas / crop_frame semantics).
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

    def atlas(self, atlas_id):
        if atlas_id not in self._atlas_cache:
            row = self._db.execute('SELECT data_zlib FROM atlases WHERE id=?', (atlas_id,)).fetchone()
            if row is None:
                raise PipelineError('atlas id %s not present in atlas.sqlite' % atlas_id)
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
            self._atlas_cache[atlas_id] = obj
        return self._atlas_cache[atlas_id]

    def atlas_id_for_image_path(self, image_path):
        """Resolve a .img animation image path to a unique atlas id via page
        texture paths (.../<name>.img_<page>.png) plus exact atlas name match."""
        base = image_path.replace('\\', '/').rsplit('/', 1)[-1]
        stem = base[:-4] if base.lower().endswith('.img') else base
        if not stem:
            raise PipelineError('empty image stem for %r' % image_path)
        rows = self._db.execute(
            "SELECT atlas_id, page_index, texture_path, status FROM pages "
            "WHERE lower(texture_path) LIKE ?", ('%' + base.lower() + '_%.png',)).fetchall()
        ids = set()
        for atlas_id, page_index, texture_path, status in rows:
            # LIKE treats '_' as a wildcard; re-check the suffix literally.
            if not texture_path.lower().endswith('/%s_%d.png' % (base.lower(), page_index)):
                continue
            if status != 'exact_texture_path':
                raise PipelineError('image %r resolves to a page with status %r (atlas %s page %s); '
                                    'unresolved textures are unsupported (plan section 10)'
                                    % (image_path, status, atlas_id, page_index))
            ids.add(atlas_id)
        if not ids:
            raise PipelineError('image %r not found in atlas.sqlite pages' % image_path)
        if len(ids) != 1:
            raise PipelineError('image %r matches several atlases: %s' % (image_path, sorted(ids)))
        atlas_id = ids.pop()
        name = self.atlas(atlas_id).get('name')
        if name != stem:
            raise PipelineError('image %r resolved to atlas %s named %r (expected %r)'
                                % (image_path, atlas_id, name, stem))
        return atlas_id

    def page_image(self, atlas_id, page_index):
        key = (atlas_id, page_index)
        if key not in self._page_image_cache:
            page = self._page(atlas_id, page_index)
            if page.get('palette_path'):
                raise PipelineError('palette page material is unsupported (atlas %s page %s; plan '
                                    'section 10: no unverified palette decoding)' % (atlas_id, page_index))
            if page.get('format_code') == -1:
                raise PipelineError('format_code -1 page has no pixels (atlas %s page %s)' % (atlas_id, page_index))
            rel = page.get('resolved_png')
            if not rel:
                raise PipelineError('page texture unresolved for atlas %s page %s' % (atlas_id, page_index))
            path = self.base / rel
            if not path.is_file():
                raise PipelineError('page texture missing on disk: %s' % path)
            with Image.open(path) as im:
                self._page_image_cache[key] = (im.convert('RGBA'), path.name)
        return self._page_image_cache[key]

    def _page(self, atlas_id, page_index):
        pages = self.atlas(atlas_id)['pages']
        if not 0 <= page_index < len(pages):
            raise PipelineError('page %s out of range for atlas %s' % (page_index, atlas_id))
        return pages[page_index]

    def frame_record(self, atlas_id, frame_index, page_index=None):
        """Unique (page, frame) selection for a frame index, mirroring
        crop_frame's selection rule (page_index optional disambiguation)."""
        pages = self.atlas(atlas_id)['pages']
        matches = []
        for pi, page in enumerate(pages):
            for fr in page['frames']:
                if fr['index'] == frame_index:
                    if page_index is None or pi == page_index:
                        matches.append((pi, fr))
        if len(matches) != 1:
            raise PipelineError('frame selection is not unique or missing: atlas %s index %s -> %d matches'
                                % (atlas_id, frame_index, len(matches)))
        return matches[0]

    def load_layer_content(self, atlas_id, frame_index, page_index=None):
        """Return (content_image_or_None, meta) for one atlas frame.

        meta carries the original frame's canvas/offset and the content bbox in
        canvas coordinates, so callers can place the content exactly where
        crop_frame(full_canvas=True) would have placed it. Reference frames are
        followed only when the frame itself is empty, mirroring crop_frame."""
        page_idx, fr = self.frame_record(atlas_id, frame_index, page_index)
        original = fr
        seen = set()
        page = self._page(atlas_id, page_idx)
        rect = fr.get('rect_pixels', fr['rect'])
        while (not page.get('resolved_png') or rect[2] <= 0 or rect[3] <= 0) and fr['reference'] >= 0:
            if fr['index'] in seen:
                raise PipelineError('cyclic frame reference in atlas %s at index %s' % (atlas_id, fr['index']))
            seen.add(fr['index'])
            links = [(pi, f) for pi, pg in enumerate(self.atlas(atlas_id)['pages'])
                     for f in pg['frames'] if f['index'] == fr['reference']]
            if len(links) != 1:
                raise PipelineError('missing or ambiguous frame reference %s in atlas %s'
                                    % (fr['reference'], atlas_id))
            page_idx, fr = links[0]
            page = self._page(atlas_id, page_idx)
            rect = fr.get('rect_pixels', fr['rect'])
        meta = {'atlas_id': atlas_id, 'page': page_idx, 'frame_index': frame_index,
                'name': original.get('name'), 'canvas': list(original['canvas']),
                'offset': list(original['offset']), 'reference': original['reference']}
        if page.get('format_code') == -1 or rect[2] <= 0 or rect[3] <= 0:
            meta['rect_px'] = [int(v) for v in rect]
            meta['content_bbox_in_canvas'] = None
            return None, meta
        page_im, page_name = self.page_image(atlas_id, page_idx)
        meta['page_png'] = page_name
        if page.get('palette_path'):
            raise PipelineError('palette page unsupported (atlas %s page %s)' % (atlas_id, page_idx))
        if any(v != int(v) for v in rect):
            raise PipelineError('fractional rectangle requires a verified sampling rule '
                                '(atlas %s frame %s rect %s)' % (atlas_id, frame_index, rect))
        x, y, w, h = (int(round(v)) for v in rect)
        if w <= 0 or h <= 0:
            raise PipelineError('empty frame after reference following (atlas %s frame %s)' % (atlas_id, frame_index))
        if x < 0 or y < 0 or x + w > page_im.width or y + h > page_im.height:
            raise PipelineError('rectangle outside texture (atlas %s frame %s rect %s page %sx%s)'
                                % (atlas_id, frame_index, (x, y, w, h), page_im.width, page_im.height))
        # Unity bottom-left origin rect -> y-flipped image-space crop.
        crop = page_im.crop((x, page_im.height - y - h, x + w, page_im.height - y))
        bbox = crop.getbbox()
        cw, ch = original['canvas']
        ox, oy = original['offset']
        if cw <= 0 or ch <= 0 or cw * ch > 32000000:
            raise PipelineError('unsupported canvas dimensions %sx%s (atlas %s frame %s)' % (cw, ch, atlas_id, frame_index))
        if bbox is None:
            meta['rect_px'] = [x, y, w, h]
            meta['content_bbox_in_canvas'] = None
            return None, meta
        if ox < 0 or oy < 0 or ox + (bbox[2] - bbox[0]) > cw or oy + (bbox[3] - bbox[1]) > ch:
            raise PipelineError('offset %s places content outside canvas %sx%s (atlas %s frame %s)'
                                % ((ox, oy), cw, ch, atlas_id, frame_index))
        meta['rect_px'] = [x, y, w, h]
        meta['content_bbox_in_canvas'] = [ox + bbox[0], oy + bbox[1], ox + bbox[2], oy + bbox[3]]
        return crop.crop(bbox), meta


# ---------------------------------------------------------------------------
# .ani payloads for actions that have no ue_ready export (M6-019 resource keys).
# Uses the verified script_index.sqlite + payloads.bin path of
# export_restored_animation.py; the DNFM tools dir is imported read-only.
# ---------------------------------------------------------------------------

class ScriptAnimationStore:
    def __init__(self, dnfm_root):
        tools = Path(dnfm_root) / 'tools'
        if not tools.is_dir():
            raise PipelineError('DNFM tools dir not found: %s' % tools)
        if str(tools) not in sys.path:
            sys.path.insert(0, str(tools))
        try:
            from restore_script_index import path_hash          # noqa: E402
            from restore_dnf_animations import parse            # noqa: E402
        except Exception as exc:                                # pragma: no cover
            raise PipelineError('cannot import DNFM restore tools from %s: %s' % (tools, exc))
        self._path_hash = path_hash
        self._parse = parse
        self.restored_root = Path(dnfm_root) / 'downloaded_130.5.8.202'
        index = self.restored_root / 'restored/script_index.sqlite'
        if not index.is_file():
            raise PipelineError('script_index.sqlite not found: %s' % index)
        self._db = sqlite3.connect('file:%s?mode=ro' % index.as_posix(), uri=True)
        self._cache = {}

    def load(self, resource_key):
        if resource_key not in self._cache:
            h = self._path_hash(resource_key)
            row = self._db.execute(
                'SELECT shard, block, payload_offset, payload_bytes, sha256 FROM scripts WHERE hash=?',
                (h,)).fetchone()
            if row is None:
                raise PipelineError('resource key %r not present in script_index.sqlite' % resource_key)
            shard, block, off, size, sha = row
            payload = (self.restored_root / 'data/decoded_binary/script' /
                       ('script_compressed_chn_%d_%d_u_p.payloads.bin' % (shard, shard)))
            with open(payload, 'rb') as f:
                f.seek(off)
                data = f.read(size)
            if hashlib.sha256(data).hexdigest() != sha:
                raise PipelineError('payload sha256 mismatch for %r' % resource_key)
            anim = self._parse(data)
            self._cache[resource_key] = {'anim': anim, 'source': {
                'path': resource_key, 'hash': h, 'shard': shard, 'block': block, 'sha256': sha}}
        return self._cache[resource_key]


# ---------------------------------------------------------------------------
# Mapping plugin (M6-019 outfit_mapping.json).
# ---------------------------------------------------------------------------

def load_mapping(path):
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise PipelineError('mapping root must be an object')
    unknown = set(data) - TOP_KEYS
    if unknown:
        raise PipelineError('mapping has unknown top-level keys %s (allowed: %s)'
                            % (sorted(unknown), sorted(TOP_KEYS)))
    for key in ('character', 'outfit', 'layers', 'draw_order', 'actions'):
        if key not in data:
            raise PipelineError('mapping missing required key %r' % key)
    if data['character'] != 'fighter':
        raise PipelineError('mapping character %r is not fighter' % data['character'])
    layers = data['layers']
    if not isinstance(layers, dict) or not layers:
        raise PipelineError('mapping "layers" must be a non-empty table')
    for key, bound in layers.items():
        if not isinstance(bound, dict) or not isinstance(bound.get('atlas_id'), int):
            raise PipelineError('mapping layers[%r] must carry an integer atlas_id' % key)
    draw_order = data['draw_order']
    if isinstance(draw_order, dict):
        if draw_order.get('split_layers'):
            raise PipelineError('mapping declares split_layers; split-layer composition is not '
                                'implemented (plan section 10)')
        draw_order = draw_order.get('bottom_to_top')
    if not isinstance(draw_order, list) or not draw_order:
        raise PipelineError('mapping "draw_order" must yield a non-empty bottom->top list')
    for key in draw_order:
        if key not in layers:
            raise PipelineError('draw_order key %r not present in layers table' % key)
    data['_draw_order'] = draw_order
    return data


def mapping_resource_entries(mapping):
    """Collect M6-019 recorded animation entries keyed by resource key:
    {resource_key: entry} from skill_animations + _state."""
    out = {}
    skills = mapping.get('skill_animations') or {}
    for group, payload in skills.items():
        if group.startswith('_'):
            continue
        if not isinstance(payload, dict):
            continue
        for entry in (payload.get('animations') or {}).values():
            key = entry.get('resource_key')
            if key:
                if key in out:
                    raise PipelineError('duplicate resource key %r in mapping skill_animations' % key)
                out[key] = entry
    state = skills.get('_state') or {}
    for entry in state.values():
        key = entry.get('resource_key')
        if key:
            if key in out:
                raise PipelineError('duplicate resource key %r in mapping _state' % key)
            out[key] = entry
    aux = skills.get('_aux_effect_animations') or {}
    for entry in aux.values():
        key = entry.get('query_path')
        if key:
            out.setdefault(key, {'_aux_effect': True, **entry})
    return out


def validate_mapping_frame_entry(entry, frame_index):
    unknown = set(entry) - FRAME_KEYS
    if unknown:
        raise PipelineError('mapping frame %s has unknown keys %s (allowed: %s)'
                            % (frame_index, sorted(unknown), sorted(FRAME_KEYS)))
    if 'layer_frames' not in entry and not entry.get('missing'):
        raise PipelineError('mapping frame %s has neither layer_frames nor missing' % frame_index)
    scale = entry.get('scale')
    if scale is not None and (abs(float(scale[0])) != 1.0 or abs(float(scale[1])) != 1.0):
        raise PipelineError('mapping frame %s declares non-unit |scale| %s; unsupported (plan section 10)'
                            % (frame_index, scale))
    layer_frames = entry.get('layer_frames') or {}
    if not isinstance(layer_frames, dict):
        raise PipelineError('mapping frame %s layer_frames must be an object' % frame_index)
    for layer, lf in layer_frames.items():
        if not isinstance(lf, dict):
            raise PipelineError('mapping frame %s layer %r entry must be an object' % (frame_index, layer))
        unknown = set(lf) - LAYER_ENTRY_KEYS
        if unknown:
            raise PipelineError('mapping frame %s layer %r has unknown keys %s (allowed: %s)'
                                % (frame_index, layer, sorted(unknown), sorted(LAYER_ENTRY_KEYS)))
        for key in ('atlas_id', 'frame_index'):
            if key not in lf:
                raise PipelineError('mapping frame %s layer %r missing %r' % (frame_index, layer, key))
        if not isinstance(lf['atlas_id'], int) or not isinstance(lf['frame_index'], int):
            raise PipelineError('mapping frame %s layer %r atlas_id/frame_index must be ints' % (frame_index, layer))
        if 'offset' in lf and (not isinstance(lf['offset'], list) or len(lf['offset']) != 2
                               or any(not isinstance(v, int) for v in lf['offset'])):
            raise PipelineError('mapping frame %s layer %r offset must be [int, int]' % (frame_index, layer))
        if 'placement_delta' in lf:
            pd = lf['placement_delta']
            if not isinstance(pd, list) or len(pd) != 2 or any(not isinstance(v, (int, float)) for v in pd):
                raise PipelineError('mapping frame %s layer %r placement_delta must be [number, number]'
                                    % (frame_index, layer))
        if 'src_rect_px' in lf:
            sr = lf['src_rect_px']
            if (not isinstance(sr, list) or len(sr) != 4
                    or any(not isinstance(v, int) or v < 0 for v in sr)):
                raise PipelineError('mapping frame %s layer %r src_rect_px must be [int,int,int,int] >= 0'
                                    % (frame_index, layer))
        if 'color_rgba' in lf and list(lf['color_rgba']) != [255, 255, 255, 255]:
            raise PipelineError('mapping frame %s layer %r color_rgba %s; per-layer tint in a composite is '
                                'not verified, refusing (plan section 10)' % (frame_index, layer, lf['color_rgba']))


def action_missing_map(action_mapping, mapping_frames, draw_order):
    """Aggregate declared missing layers for one action: layer -> reason."""
    missing = {}
    for entry in action_mapping.get('missing') or []:
        layer = entry.get('layer')
        if not layer:
            raise PipelineError('action-level missing entry needs "layer"')
        missing.setdefault(layer, entry.get('reason', 'declared missing for whole action'))
    for fi, entry in enumerate(mapping_frames):
        for m in entry.get('missing') or []:
            layer = m.get('layer')
            if not layer:
                raise PipelineError('frame-level missing entry needs "layer" (frame %s)' % fi)
            missing.setdefault(layer, m.get('reason', 'declared missing on frame %s' % fi))
    for fi, entry in enumerate(mapping_frames):
        lfs = entry.get('layer_frames') or {}
        for layer in draw_order:
            # The body layer is always planned from the animation itself, so a
            # mapping layer_frames entry for it is optional (cross-checked when
            # present); every outfit layer must be planned or declared missing.
            if layer != BODY_LAYER and layer not in lfs and layer not in missing:
                raise PipelineError('action %r frame %s: draw_order layer %r has no layer_frames entry '
                                    'and is not declared missing'
                                    % (action_mapping['action_id'], fi, layer))
        for layer in lfs:
            if layer not in draw_order:
                raise PipelineError('action %r frame %s: layer_frames layer %r is not in draw_order %s'
                                    % (action_mapping['action_id'], fi, layer, draw_order))
    return missing


def mapping_top_missing(mapping):
    """Convert M6-019 top-level missing entries into manifest contract rows."""
    rows = []
    for entry in mapping.get('missing') or []:
        rows.append({'action_id': entry.get('action_id'),
                     'layer': entry.get('layer'),
                     'reason': '[%s] %s' % (entry.get('kind', 'unspecified'),
                                            entry.get('detail', '')),
                     'kind': entry.get('kind', 'unspecified')})
    return rows


# ---------------------------------------------------------------------------
# Frame composition (position space; origin = feet/action anchor).
# ---------------------------------------------------------------------------

def check_frame_transforms(action_id, frame_index, frame):
    scale = list(map(float, frame.get('scale', [1.0, 1.0])))
    if abs(scale[0]) != 1.0 or abs(scale[1]) != 1.0:
        raise PipelineError('action %s frame %s has non-unit |scale| %s; unsupported (plan section 10)'
                            % (action_id, frame_index, scale))
    if list(map(int, frame.get('color_rgba', [255, 255, 255, 255]))) != [255, 255, 255, 255]:
        raise PipelineError('action %s frame %s has non-white color_rgba %s; layer tint semantics in a '
                            'multi-layer composite are not verified, refusing (plan section 10)'
                            % (action_id, frame_index, frame.get('color_rgba')))
    if not frame.get('image_path'):
        raise PipelineError('action %s frame %s has no image_path; external avatar selection unsupported'
                            % (action_id, frame_index))


def make_layer(key, content, meta, position, delta=(0, 0)):
    bbox = meta.get('content_bbox_in_canvas')
    placement = None
    if bbox is not None:
        placement = [bbox[0] + int(position[0]) + int(delta[0]),
                     bbox[1] + int(position[1]) + int(delta[1])]
    return {'key': key, 'content': content, 'meta': meta, 'placement': placement,
            'delta': [int(delta[0]), int(delta[1])]}


def cross_check_layer_entry(action_id, fi, layer, lf, meta, lib):
    """Cross-check a mapping layer entry against the resolved atlas record.

    ``resolved_frame_index``/``reference_chain`` are M6-019 audit data for the
    atlas reference chain; content identity is enforced by the rect_pixels
    cross-check below (M6-019 verified that chain endpoints share the same
    rect), so the chain itself is not re-followed here."""
    if int(lf['atlas_id']) != meta['atlas_id']:
        raise PipelineError('action %s frame %s layer %r atlas_id %s != resolved %s'
                            % (action_id, fi, layer, lf['atlas_id'], meta['atlas_id']))
    if 'page' in lf and int(lf['page']) != meta['page']:
        raise PipelineError('action %s frame %s layer %r page %s != resolved %s'
                            % (action_id, fi, layer, lf['page'], meta['page']))
    if 'offset' in lf and list(lf['offset']) != list(meta['offset']):
        raise PipelineError('action %s frame %s layer %r offset %s != atlas offset %s; mapping/atlas '
                            'divergence must not be silently resolved'
                            % (action_id, fi, layer, lf['offset'], meta['offset']))
    if 'canvas' in lf and list(lf['canvas']) != list(meta['canvas']):
        raise PipelineError('action %s frame %s layer %r canvas %s != atlas canvas %s'
                            % (action_id, fi, layer, lf['canvas'], meta['canvas']))
    if 'rect_pixels' in lf:
        record = lib.frame_record(meta['atlas_id'], meta['frame_index'], meta['page'])[1]
        expected = [float(v) for v in record.get('rect_pixels', record['rect'])]
        if [float(v) for v in lf['rect_pixels']] != expected:
            raise PipelineError('action %s frame %s layer %r rect_pixels %s != atlas rect %s'
                                % (action_id, fi, layer, lf['rect_pixels'], expected))


def resolve_layer_plan(action_id, anim, mapping, action_mapping, lib, frame_index_list=None):
    """Per frame: ordered layer list (bottom -> top) with resolved content and
    placement in position space. Body comes from the animation itself; outfit
    layers come from the mapping's explicit layer_frames, or (for actions not
    covered by mapping["actions"]) from the verified same-index parallelism."""
    frames = anim['frames']
    body_atlas_id = None
    implicit_parallel = action_mapping is None
    declared_missing = set(action_mapping['_missing']) if action_mapping is not None else set()
    plans = []
    for fi, frame in enumerate(frames):
        check_frame_transforms(action_id, fi, frame)
        layers_for_frame = []
        # --- body layer from the animation itself -------------------------
        atlas_id = lib.atlas_id_for_image_path(frame['image_path'])
        if body_atlas_id is None:
            body_atlas_id = atlas_id
            if mapping is not None and BODY_LAYER in mapping['layers']:
                bound = mapping['layers'][BODY_LAYER]
                if int(bound['atlas_id']) != atlas_id:
                    raise PipelineError('mapping binds %r to atlas %s but the animation image path '
                                        'resolves to atlas %s' % (BODY_LAYER, bound['atlas_id'], atlas_id))
        elif atlas_id != body_atlas_id:
            raise PipelineError('action %s frame %s switches body atlas (%s -> %s); unsupported'
                                % (action_id, fi, body_atlas_id, atlas_id))
        content, meta = lib.load_layer_content(atlas_id, frame['image_frame_index'])
        layers_for_frame.append(make_layer(BODY_LAYER, content, meta, frame['position']))
        # --- outfit layers -------------------------------------------------
        frame_declared = set()
        if mapping is None:
            pass
        elif implicit_parallel:
            for part in OUTFIT_PARTS:
                bound = mapping['layers'].get(part)
                if bound is None:
                    continue
                pc, pm = lib.load_layer_content(int(bound['atlas_id']), int(frame['image_frame_index']),
                                                bound.get('page'))
                layers_for_frame.append(make_layer(part, pc, pm, frame['position']))
        else:
            entry = action_mapping['frames'][fi]
            if 'index' in entry and int(entry['index']) != fi:
                raise PipelineError('action %s mapping frame index %s != animation frame %s'
                                    % (action_id, entry['index'], fi))
            if 'frame_index' in entry and int(entry['frame_index']) != fi:
                raise PipelineError('action %s mapping frame_index %s != animation frame %s'
                                    % (action_id, entry['frame_index'], fi))
            if 'source_position' in entry and list(entry['source_position']) != list(frame['position']):
                raise PipelineError('action %s frame %s mapping source_position %s != animation position %s'
                                    % (action_id, fi, entry['source_position'], frame['position']))
            if 'duration_ms' in entry and int(entry['duration_ms']) != int(frame['delay_ms']):
                raise PipelineError('action %s frame %s mapping duration_ms %s != animation delay_ms %s'
                                    % (action_id, fi, entry['duration_ms'], frame['delay_ms']))
            lfs = entry.get('layer_frames') or {}
            body_entry = lfs.get(BODY_LAYER)
            if body_entry is not None:
                cross_check_layer_entry(action_id, fi, BODY_LAYER, body_entry, meta, lib)
            for layer in action_mapping['_draw_order']:
                if layer == BODY_LAYER:
                    continue
                lf = lfs.get(layer)
                if lf is None:
                    if layer in declared_missing:
                        frame_declared.add(layer)
                    continue  # declared missing; absent for this frame
                bound = mapping['layers'].get(layer)
                if bound is not None and int(bound['atlas_id']) != int(lf['atlas_id']):
                    raise PipelineError('layer %r bound to atlas %s but frame entry uses atlas %s'
                                        % (layer, bound['atlas_id'], lf['atlas_id']))
                layer_content, layer_meta = lib.load_layer_content(int(lf['atlas_id']),
                                                                   int(lf['frame_index']), lf.get('page'))
                cross_check_layer_entry(action_id, fi, layer, lf, layer_meta, lib)
                if layer_content is not None and 'src_rect_px' in lf:
                    sx, sy, sw, sh = lf['src_rect_px']
                    cw, chh = layer_content.size
                    if sx + sw > cw or sy + sh > chh:
                        raise PipelineError('action %s frame %s layer %r src_rect_px %s exceeds content %sx%s'
                                            % (action_id, fi, layer, lf['src_rect_px'], cw, chh))
                    base = layer_meta['content_bbox_in_canvas']
                    sub_bbox = layer_content.crop((sx, sy, sx + sw, sy + sh)).getbbox()
                    if sub_bbox is None:
                        layer_content = None
                        layer_meta['content_bbox_in_canvas'] = None
                    else:
                        layer_content = layer_content.crop(
                            (sx + sub_bbox[0], sy + sub_bbox[1],
                             sx + sub_bbox[0] + sub_bbox[2], sy + sub_bbox[1] + sub_bbox[3]))
                        layer_meta['content_bbox_in_canvas'] = [
                            base[0] + sx + sub_bbox[0], base[1] + sy + sub_bbox[1],
                            base[0] + sx + sub_bbox[0] + sub_bbox[2], base[1] + sy + sub_bbox[1] + sub_bbox[3]]
                        layer_meta['src_rect_px'] = list(lf['src_rect_px'])
                delta = lf.get('placement_delta', [0, 0])
                layers_for_frame.append(make_layer(layer, layer_content, layer_meta,
                                                   frame['position'], delta))
        if frame_index_list is not None and frame_index_list[fi] != int(frame['image_frame_index']):
            raise PipelineError('action %s frame %s animation image_frame_index %s != mapping '
                                'frame_index_list %s' % (action_id, fi, frame['image_frame_index'],
                                                         frame_index_list[fi]))
        plans.append({'frame_index': fi, 'layers': layers_for_frame,
                      'missing_layers': sorted(frame_declared),
                      '_position': list(frame['position']),
                      '_transform': {'scale': list(map(float, frame.get('scale', [1.0, 1.0]))),
                                     'rotation_degrees': float(frame.get('rotation_degrees', 0.0))}})
    return plans, body_atlas_id, implicit_parallel


def composite_frame_cell(plan, margin, anchor=DEFAULT_ANCHOR):
    """One RGBA cell for the frame: union of layer content boxes + margin.

    Frames without transform take the verified per-layer path (pixel-identical
    to export_restored_animation.py). Frames with mirror/rotation composite all
    layers into one canvas-space image, apply the transforms around the fixed
    canvas anchor R (see module docstring), then recompute bbox and pivot."""
    sx, sy = plan['_transform']['scale']
    rot = plan['_transform']['rotation_degrees']
    transformed = (sx, sy) != (1.0, 1.0) or rot != 0.0
    if not transformed:
        boxes = [tuple(l['placement']) + (l['content'].width, l['content'].height)
                 for l in plan['layers'] if l['content'] is not None]
        if boxes:
            x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
            x1 = max(b[0] + b[2] for b in boxes); y1 = max(b[1] + b[3] for b in boxes)
        else:
            x0, y0, x1, y1 = 0, 0, 0, 0  # fully empty frame keeps the anchor point
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

    # ---- transformed frame: whole-frame pattern around canvas anchor R -----
    ax, ay = int(anchor[0]), int(anchor[1])
    canvases = {tuple(l['meta']['canvas']) for l in plan['layers'] if l['content'] is not None}
    if len(canvases) != 1:
        raise PipelineError('action frame %s has layers with different canvases %s; a shared canvas '
                            'anchor is required for whole-frame transforms'
                            % (plan['frame_index'], sorted(canvases)))
    canvas = canvases.pop()
    if ax < 0 or ay < 0 or ax > canvas[0] or ay > canvas[1]:
        raise PipelineError('rotation anchor %s outside layer canvas %s (action frame %s)'
                            % ((ax, ay), canvas, plan['frame_index']))
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
    # bbox (big-image space) -> canvas space: R sits at the big-image center and
    # never moves, so canvas coords = big coords - (center - R).
    off_x, off_y = center[0] - ax, center[1] - ay
    cbx0, cby0 = bbox[0] - off_x, bbox[1] - off_y  # canvas-space content box
    # place: canvas space -> position space by +position (mirror/rotation move
    # content within the canvas; position is applied after the transform)
    pos_x, pos_y = plan['_position']
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
# Packing, previews, manifest.
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
            raise PipelineError('frame cell %dx%d exceeds page size %dx%d; no silent shrinking '
                                '(plan section 10)' % (w, h, page_size, page_size))
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
        im = c['cell'].resize((w * scale, h * scale), Image.Resampling.NEAREST)
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
                 'page': meta['page'], 'offset': meta['offset']}
        if layer['delta'] != [0, 0]:
            entry['placement_delta'] = layer['delta']
        if 'src_rect_px' in meta:
            entry['src_rect_px'] = meta['src_rect_px']
        entry['content_bbox_px'] = layer['placement']
        layer_frames[layer['key']] = entry
    events = []
    for tag in frame.get('audio_tags') or []:
        events.append({'type': 'audio', 'name': tag.get('tag')})
    for tag in frame.get('audio_loop_tags') or []:
        events.append({'type': 'audio_loop', 'name': tag.get('tag')})
    out = {
        'index': plan['frame_index'],
        # M6-022: the M6-021 import contract refuses zero-duration frames (a
        # frame owning no instant can never be current; validation-first). The
        # source data carries delay_ms=0 on exactly one row (earthbreak frame
        # 5), so the manifest clamps it to 1ms: same frame order, same pixels,
        # honest "negligible instant" playback semantics.
        'duration_ms': max(int(frame['delay_ms']), 1),
        'page': plan['page'],
        'uv_rect_px': list(plan['uv_rect_px']),
        'canvas_size_px': list(plan['cell'].size),
        'pivot_px': list(plan['pivot_px']),
        'pivot_definition': PIVOT_DEFINITION,
        'layer_frames': layer_frames,
        'flip_copy_type': int(frame.get('flip_copy_type', 0)),
        'scale': [float(v) for v in plan['_transform']['scale']],
        'rotation_degrees': float(plan['_transform']['rotation_degrees']),
        'source_frame': {'image_path': frame['image_path'],
                         'image_pack_index': int(frame.get('image_pack_index', 0)),
                         'image_frame_index': int(frame['image_frame_index'])},
        'events': events,
    }
    if plan['transform_applied'] is not None:
        out['transform_applied'] = plan['transform_applied']
    if plan['missing_layers']:
        out['missing_layers'] = list(plan['missing_layers'])
    return out


# ---------------------------------------------------------------------------
# Verified-exporter cross-check (body-only mode).
# ---------------------------------------------------------------------------

def verify_against_ue_ready(anim_dir, plans):
    """Pixel-exact comparison against the shipped ue_ready spritesheet: every
    body-only cell must equal the verified exporter's rendering of the same
    frame (same 2px transparent margin)."""
    ue_manifest = json.loads((anim_dir / 'ue_manifest.json').read_text(encoding='utf-8'))
    with Image.open(anim_dir / 'spritesheet.png') as sheet_im:
        sheet = sheet_im.convert('RGBA')
    left, top = -ue_manifest['pivot_pixels'][0], -ue_manifest['pivot_pixels'][1]
    cols = ue_manifest['columns']
    cell_w, cell_h = ue_manifest['frame_size']
    checked = 0
    for plan in plans:
        if plan['transform_applied'] is not None:
            raise PipelineError('ue_ready cross-check hit a transformed frame; exporter comparison '
                                'requires unit transforms')
        boxes = [tuple(l['placement']) + (l['content'].width, l['content'].height)
                 for l in plan['layers'] if l['content'] is not None]
        if not boxes:
            continue
        x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
        x1 = max(b[0] + b[2] for b in boxes); y1 = max(b[1] + b[3] for b in boxes)
        m = EXPORTER_MARGIN
        fi = plan['frame_index']
        ox = (fi % cols) * cell_w + (x0 - left - m)
        oy = (fi // cols) * cell_h + (y0 - top - m)
        w = (x1 - x0) + 2 * m
        h = (y1 - y0) + 2 * m
        if ox < 0 or oy < 0 or ox + w > sheet.width or oy + h > sheet.height:
            raise PipelineError('ue_ready region out of bounds for frame %s' % fi)
        expected = sheet.crop((ox, oy, ox + w, oy + h))
        mine = plan['cell']
        if mine.size != expected.size:
            raise PipelineError('ue_ready cell size mismatch for frame %s: mine %s vs expected %s'
                                % (fi, mine.size, expected.size))
        if mine.tobytes() != expected.tobytes():
            raise PipelineError('ue_ready pixel mismatch for frame %s (alignment differs from the '
                                'verified exporter)' % fi)
        checked += 1
    return {'ue_ready_dir': anim_dir.name, 'frames_checked': checked,
            'expected_pivot_pixels': ue_manifest['pivot_pixels'],
            'result': 'pixel_exact_match'}


# ---------------------------------------------------------------------------
# Animation sources (ue_ready dir or script_index payloads) + M6-019 checks.
# ---------------------------------------------------------------------------

def find_animation_dir(restored_root, action_id):
    pattern = str(Path(restored_root) / 'restored/ue_ready/animations'
                  / ('script_character_fighter_animation_%s_ani_*' % action_id))
    matches = sorted(glob.glob(pattern))
    if not matches:
        return None
    if len(matches) != 1:
        raise PipelineError('ambiguous animation directories for %r: %s' % (action_id, matches))
    return Path(matches[0])


def cross_check_mapping_action(action_id, action_mapping, anim, frames_meta):
    """Cross-check mapping action-level records against the loaded animation."""
    if 'source_ani' in action_mapping and action_mapping['source_ani'] != anim['source']['path']:
        raise PipelineError('action %r mapping source_ani %r != animation source %r'
                            % (action_id, action_mapping['source_ani'], anim['source']['path']))
    if 'source_sha256' in action_mapping and action_mapping['source_sha256'] != anim['source']['sha256']:
        raise PipelineError('action %r mapping source_sha256 != animation source sha256' % action_id)
    if 'frame_count' in action_mapping and int(action_mapping['frame_count']) != len(anim['frames']):
        raise PipelineError('action %r mapping frame_count %s != animation frames %s'
                            % (action_id, action_mapping['frame_count'], len(anim['frames'])))
    if 'loop' in action_mapping and bool(action_mapping['loop']) != bool(anim['loop']):
        raise PipelineError('action %r mapping loop %s != animation loop %s'
                            % (action_id, action_mapping['loop'], anim['loop']))
    if 'duration_ms' in action_mapping:
        expect = frames_meta['duration_ms']
        if int(action_mapping['duration_ms']) != int(expect):
            raise PipelineError('action %r mapping duration_ms %s != animation %s'
                                % (action_id, action_mapping['duration_ms'], expect))


def cross_check_resource_entry(action_id, entry, anim):
    """Cross-check a M6-019 skill/state record against the parsed animation."""
    if entry.get('_aux_effect'):
        raise PipelineError('action %r resolves to an aux effect animation %r; passive-object effects '
                            'are out of scope' % (action_id, entry.get('query_path')))
    if 'sha256' in entry and entry['sha256'] != anim['source']['sha256']:
        raise PipelineError('action %r M6-019 sha256 != script_index payload sha256' % action_id)
    if 'frame_count' in entry and int(entry['frame_count']) != len(anim['frames']):
        raise PipelineError('action %r M6-019 frame_count %s != parsed frames %s'
                            % (action_id, entry['frame_count'], len(anim['frames'])))
    if 'duration_ms' in entry:
        expect = sum(int(f['delay_ms']) for f in anim['frames'])
        if int(entry['duration_ms']) != expect:
            raise PipelineError('action %r M6-019 duration_ms %s != parsed %s'
                                % (action_id, entry['duration_ms'], expect))
    if 'delay_ms' in entry:
        parsed = [int(f['delay_ms']) for f in anim['frames']]
        if [int(v) for v in entry['delay_ms']] != parsed:
            raise PipelineError('action %r M6-019 delay_ms %s != parsed %s'
                                % (action_id, entry['delay_ms'], parsed))
    if 'frame_index_list' in entry:
        parsed = [int(f['image_frame_index']) for f in anim['frames']]
        if [int(v) for v in entry['frame_index_list']] != parsed:
            raise PipelineError('action %r M6-019 frame_index_list %s != parsed %s'
                                % (action_id, entry['frame_index_list'], parsed))
    def parse_notes(notes):
        scale = None
        rot = None
        for note in notes:
            if note.startswith('scale='):
                scale = [float(v) for v in note[len('scale='):].strip().strip('[]').split(',')]
            elif note.startswith('rotation='):
                rot = float(note[len('rotation='):])
        return scale, rot

    marked = {}
    for t in entry.get('nonstandard_transform_frames') or []:
        scale, rot = parse_notes([str(x) for x in t.get('nonstandard', [])])
        marked[int(t['frame'])] = (scale, rot)
    actual = {}
    for fi, frame in enumerate(anim['frames']):
        scale = list(map(float, frame.get('scale', [1.0, 1.0])))
        rot = float(frame.get('rotation_degrees', 0.0))
        if scale != [1.0, 1.0] or rot != 0.0:
            actual[fi] = (scale if scale != [1.0, 1.0] else None,
                          rot if rot != 0.0 else None)
    for fi, (scale, rot) in actual.items():
        if fi not in marked:
            raise PipelineError('action %r frame %s carries transform (scale=%s rot=%s) but M6-019 '
                                'marks none; mapping must record every nonstandard frame'
                                % (action_id, fi, scale, rot))
    for fi, (scale, rot) in actual.items():
        m_scale, m_rot = marked[fi]
        if (scale is not None) and (m_scale is None or any(abs(a - b) > 1e-6 for a, b in zip(scale, m_scale))):
            raise PipelineError('action %r frame %s scale %s != M6-019 marked %s'
                                % (action_id, fi, scale, m_scale))
        if (rot is not None) and (m_rot is None or abs(rot - m_rot) > 1e-6):
            raise PipelineError('action %r frame %s rotation %s != M6-019 marked %s'
                                % (action_id, fi, rot, m_rot))
    for fi in marked:
        if fi not in actual:
            raise PipelineError('action %r M6-019 marks transform on frame %s but the parsed animation '
                                'has none; refusing to run' % (action_id, fi))


# ---------------------------------------------------------------------------
# Main pipeline.
# ---------------------------------------------------------------------------

def load_action(action_id, restored_root, store):
    """Load one action animation: ue_ready export first, then script_index."""
    anim_dir = find_animation_dir(restored_root, action_id)
    if anim_dir is not None:
        anim = json.loads((anim_dir / 'animation.json').read_text(encoding='utf-8'))
        return anim, anim_dir, 'ue_ready'
    resource_key = DB_ACTIONS.get(action_id)
    if resource_key is None:
        raise PipelineError('action %r has no ue_ready directory and is not a known DB action' % action_id)
    loaded = store.load(resource_key)
    return {'source': loaded['source'], **loaded['anim']}, None, 'script_index'


def run(args):
    dnfm_root = Path(args.dnfm_root)
    restored_root = dnfm_root / 'downloaded_130.5.8.202'
    out_dir = Path(args.out_dir)
    page_size = int(args.page_size)
    margin = int(args.margin)
    anchor = (int(args.rotation_anchor_x), int(args.rotation_anchor_y))
    if args.actions.strip().lower() == 'all':
        actions = list(BASE_ACTIONS) + list(DB_ACTIONS)
    else:
        actions = [a.strip() for a in args.actions.split(',') if a.strip()]
    if not actions:
        raise PipelineError('--actions is empty')
    if args.body_only and args.mapping:
        raise PipelineError('--body-only and --mapping are mutually exclusive')
    if not args.body_only and not args.mapping:
        raise PipelineError('either --mapping <outfit_mapping.json> or --body-only is required '
                            '(refusing to run an undefined layer set)')
    if page_size <= 0 or page_size > 2048:
        raise PipelineError('--page-size must be within (0, 2048] (engine limit); got %s' % page_size)

    mapping = None
    mapping_sha = None
    if args.mapping:
        mapping_path = Path(args.mapping)
        if not mapping_path.is_file():
            raise PipelineError('mapping file not found: %s' % mapping_path)
        mapping_sha = sha256_file(mapping_path)
        mapping = load_mapping(mapping_path)
        mapping['_path'] = str(mapping_path)

    lib = AtlasLibrary(restored_root)
    store = (ScriptAnimationStore(dnfm_root)
             if any(a not in BASE_ACTIONS for a in actions) else None)
    resource_entries = mapping_resource_entries(mapping) if mapping else {}

    action_outputs = []
    missing_out = mapping_top_missing(mapping) if mapping else []
    cells_all = []
    ue_ready_checks = []
    for action_id in actions:
        anim, anim_dir, origin = load_action(action_id, restored_root, store)
        action_mapping = None
        draw_order = [BODY_LAYER]
        frame_index_list = None
        if mapping is not None:
            action_mapping = next((a for a in mapping['actions'] if a.get('action_id') == action_id), None)
            if action_mapping is not None:
                unknown = set(action_mapping) - ACTION_KEYS
                if unknown:
                    raise PipelineError('mapping action %r has unknown keys %s (allowed: %s)'
                                        % (action_id, sorted(unknown), sorted(ACTION_KEYS)))
                mf = action_mapping.get('frames')
                if not isinstance(mf, list) or len(mf) != len(anim['frames']):
                    raise PipelineError('action %r mapping frames count %s != animation frames %s'
                                        % (action_id, len(mf) if isinstance(mf, list) else None,
                                           len(anim['frames'])))
                for fi, entry in enumerate(mf):
                    validate_mapping_frame_entry(entry, fi)
                draw_order = list(action_mapping.get('draw_order') or mapping['_draw_order'])
                action_mapping['_draw_order'] = draw_order
                action_mapping['_missing'] = action_missing_map(action_mapping, mf, draw_order)
            else:
                draw_order = list(mapping['_draw_order'])
                if action_id in DB_ACTIONS:
                    entry = resource_entries.get(DB_ACTIONS[action_id])
                    if entry is None:
                        raise PipelineError('M6-019 mapping has no record for resource %r (action %r)'
                                            % (DB_ACTIONS[action_id], action_id))
                    cross_check_resource_entry(action_id, entry, anim)
                    frame_index_list = ([int(v) for v in entry['frame_index_list']]
                                        if 'frame_index_list' in entry else None)
        plans, _body_atlas_id, implicit_parallel = resolve_layer_plan(
            action_id, anim, mapping, action_mapping, lib, frame_index_list)
        if action_mapping is not None:
            cross_check_mapping_action(action_id, action_mapping, anim,
                                       {'duration_ms': sum(int(f['delay_ms']) for f in anim['frames'])})
        for plan in plans:
            plan['_action_id'] = action_id
            plan['_anim_frame'] = anim['frames'][plan['frame_index']]
            composite_frame_cell(plan, margin, anchor)
            cells_all.append(plan)
        if args.body_only and margin == EXPORTER_MARGIN and anim_dir is not None:
            ue_ready_checks.append(verify_against_ue_ready(anim_dir, plans))
        if action_mapping is not None:
            for layer, reason in sorted(action_mapping['_missing'].items()):
                missing_out.append({'action_id': action_id, 'layer': layer, 'reason': reason})
        for plan in plans:
            for layer_key in plan['missing_layers']:
                missing_out.append({'action_id': action_id, 'layer': layer_key,
                                    'reason': 'declared missing by mapping for frame %s' % plan['frame_index']})
        layers_used = sorted({l['key'] for plan in plans for l in plan['layers']})
        transformed_frames = [p['frame_index'] for p in plans if p['transform_applied'] is not None]
        action_outputs.append({
            'action_id': action_id,
            'source_ani': anim['source']['path'],
            'source_sha256': anim['source']['sha256'],
            'loop': bool(anim['loop']),
            'layers': layers_used,
            'draw_order': draw_order,
            '_origin': origin,
            '_implicit_parallel': implicit_parallel,
            '_transformed_frames': transformed_frames,
        })

    pages = pack_pages(cells_all, page_size)

    # ---- write outputs (deterministic) ------------------------------------
    previews_dir = out_dir / 'previews'
    out_dir.mkdir(parents=True, exist_ok=True)
    previews_dir.mkdir(parents=True, exist_ok=True)
    pages_manifest = []
    for page in pages:
        canvas = Image.new('RGBA', (page_size, page_size), (0, 0, 0, 0))
        for cell, x, y in page['placements']:
            canvas.alpha_composite(cell['cell'], (x, y))
        name = 'fighter_page_%d.png' % page['index']
        canvas.save(out_dir / name, 'PNG')
        pages_manifest.append({'file': name, 'sha256': sha256_file(out_dir / name),
                               'size': [page_size, page_size],
                               'cells_on_page': len(page['placements'])})
    for action in action_outputs:
        action_plans = [c for c in cells_all if c['_action_id'] == action['action_id']]
        build_preview(action_plans, PREVIEW_SCALE).save(
            previews_dir / ('preview_%s.png' % action['action_id']), 'PNG')

    manifest = {
        'generator': GENERATOR,
        'manifest_version': MANIFEST_VERSION,
        'character': 'fighter',
        'outfit': mapping['outfit'] if mapping else 'body_only',
        'mode': 'body_only' if args.body_only else 'full_outfit_mapping',
        'source_root': str(dnfm_root),
        'coordinate_space': ('Position space, X right / Y down, origin at the feet/action anchor; '
                             'per-frame pivot_px is that anchor inside the cell (uv top-left relative, y down)'),
        'cell_margin_px': margin,
        'page_size_px': page_size,
        'transform_semantics': {
            'mirror_and_rotate_center_canvas_px': [anchor[0], anchor[1]],
            'rotation_direction': ('positive degrees rotate counterclockwise in y-down image space '
                                   '(PIL rotate(+angle), anchor at image center)'),
            'rotation_anchor_basis': ('reverse-engineered from overturn/ftRecoveryRoll frames: consistent '
                                      'with the overturn damage box (body 0..78 px above ground while '
                                      'handstanding) and roll-frame continuity; not native-code verified'),
            'transform_order': 'mirror_then_rotate',
            'resample': 'nearest',
        },
        'pages': pages_manifest,
        'actions': [{
            'action_id': action['action_id'],
            'source_ani': action['source_ani'],
            'source_sha256': action['source_sha256'],
            'loop': action['loop'],
            'layers': action['layers'],
            'draw_order': action['draw_order'],
            'layer_mapping': ('implicit_parallel' if action['_implicit_parallel'] else 'explicit_mapping'),
            'source': action['_origin'],
            'transformed_frames': action['_transformed_frames'],
            'frames': [frame_manifest_entry(plan)
                       for plan in cells_all if plan['_action_id'] == action['action_id']],
        } for action in action_outputs],
        'missing': missing_out,
        'verification': {'mode': 'body_only' if args.body_only else 'full_outfit_mapping',
                         'ue_ready_alignment': ue_ready_checks},
        'inputs': {'mapping_file': Path(mapping['_path']).name if mapping else None,
                   'mapping_sha256': mapping_sha},
        'determinism_note': 'Same inputs and arguments reproduce byte-identical outputs.',
    }
    manifest_path = out_dir / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')

    print(json.dumps({
        'actions': [{'action_id': a['action_id'], 'frames': len(a['frames']),
                     'transformed': len(a['transformed_frames'])} for a in manifest['actions']],
        'frames_total': sum(len(a['frames']) for a in manifest['actions']),
        'pages': len(pages_manifest),
        'page_size': page_size,
        'out_dir': str(out_dir),
        'manifest': manifest_path.name,
        'missing': missing_out,
        'ue_ready_alignment': [c['result'] for c in ue_ready_checks],
    }, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description='M6-020 fighter full-outfit composite pipeline')
    ap.add_argument('--dnfm-root', default=str(DEFAULT_DNFM_ROOT))
    ap.add_argument('--out-dir', default=str(DEFAULT_OUT))
    ap.add_argument('--actions', default='all',
                    help="comma-separated action ids, or 'all' (12 base + 13 DB actions)")
    ap.add_argument('--body-only', action='store_true',
                    help='composite the body layer only (milestone 1 verification mode)')
    ap.add_argument('--mapping', default=None,
                    help='path to outfit_mapping.json (M6-019); enables outfit layers')
    ap.add_argument('--page-size', type=int, default=DEFAULT_PAGE)
    ap.add_argument('--margin', type=int, default=DEFAULT_MARGIN)
    ap.add_argument('--rotation-anchor-x', type=int, default=DEFAULT_ANCHOR[0])
    ap.add_argument('--rotation-anchor-y', type=int, default=DEFAULT_ANCHOR[1])
    args = ap.parse_args()
    try:
        sys.exit(run(args))
    except PipelineError as exc:
        print(json.dumps({'error': str(exc)}), file=sys.stderr)
        sys.exit(2)
