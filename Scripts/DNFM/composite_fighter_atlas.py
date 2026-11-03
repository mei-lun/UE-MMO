#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M6-014: composite baked Spine fighter frame sequences into per-animation flipbook atlases.

Reads the bake output of Scripts/DNFM/bake_spine_fighter.py (101 RGBA frames per
animation, 527x640, 30 fps) and packs each animation into one row-major atlas
PNG laid out for the UE flipbook material convention (Engine/Shaders/Private/
Common.ush Tile1Dto2D: col = idx mod cols, row = floor(idx / cols), UV origin
top-left, V increasing downward - matches UE texture V=0 = top row).

Cells with index >= frame count stay fully transparent (recorded in the
manifest, never silently dropped). If the atlas would exceed the engine's
8192px texture dimension limit, the column count is increased and the
re-layout is recorded instead of failing.

Usage:
  py -I composite_fighter_atlas.py
     [--bake-dir Artifacts/Tasks/spine-fighter-bake/2026-11-04T0215_0800]
     [--out-dir Artifacts/Tasks/M6-014/2026-11-04T0330_0800/atlas]

Standard library + Pillow only. Deterministic; re-runnable (byte-identical
outputs for identical inputs).
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BAKE = REPO / 'Artifacts/Tasks/spine-fighter-bake/2026-11-04T0215_0800'
DEFAULT_OUT = REPO / 'Artifacts/Tasks/M6-014/2026-11-04T0330_0800/atlas'
ANIM_IDS = ('0', '1', '2')
FRAMES_PER_ANIM = 101
BASE_COLS = 10
MAX_DIM = 8192
ATLAS_NAME = 'fighter_anim%s_atlas.png'


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def pick_layout(frame_w, frame_h, num_frames):
    """10 columns first; grow columns (shrinking rows) if a side exceeds MAX_DIM."""
    cols = BASE_COLS
    rows = (num_frames + cols - 1) // cols
    relayout = None
    while cols * frame_w > MAX_DIM or rows * frame_h > MAX_DIM:
        if cols >= num_frames:
            raise RuntimeError('No column count fits %dx%d frames in %dpx' % (frame_w, frame_h, MAX_DIM))
        relayout = {'reason': 'exceeds_max_dimension', 'max_dimension': MAX_DIM,
                    'rejected_cols': cols, 'rejected_dims': [cols * frame_w, rows * frame_h]}
        cols += 1
        rows = (num_frames + cols - 1) // cols
    return cols, rows, relayout


def build_atlas(anim, bake_dir, out_dir, bake_manifest):
    frame_dir = bake_dir / 'frames' / anim
    frames = sorted(frame_dir.glob('frame_*.png'))
    if len(frames) != FRAMES_PER_ANIM:
        raise RuntimeError('anim %s: expected %d frames, found %d in %s' % (anim, FRAMES_PER_ANIM, len(frames), frame_dir))
    with Image.open(frames[0]) as probe:
        frame_w, frame_h = probe.size
    cols, rows, relayout = pick_layout(frame_w, frame_h, FRAMES_PER_ANIM)
    canvas = Image.new('RGBA', (cols * frame_w, rows * frame_h), (0, 0, 0, 0))
    frame_hashes = []
    for idx, f in enumerate(frames):
        with Image.open(f) as img:
            if img.size != (frame_w, frame_h):
                raise RuntimeError('anim %s: %s is %s, expected %s' % (anim, f.name, img.size, (frame_w, frame_h)))
            if img.mode not in ('RGBA', 'LA'):
                raise RuntimeError('anim %s: %s mode %s is not alpha-carrying' % (anim, f.name, img.mode))
            canvas.paste(img.convert('RGBA'), ((idx % cols) * frame_w, (idx // cols) * frame_h))
        frame_hashes.append({'file': f.name, 'sha256': sha256_file(f)})
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / (ATLAS_NAME % anim)
    canvas.save(dest, 'PNG')
    atlas_sha = sha256_file(dest)
    entry = {
        'animation': anim,
        'file': str(dest),
        'sha256': atlas_sha,
        'bytes': dest.stat().st_size,
        'frame_size': [frame_w, frame_h],
        'atlas_size': [cols * frame_w, rows * frame_h],
        'columns': cols,
        'rows': rows,
        'cells_total': cols * rows,
        'num_frames': FRAMES_PER_ANIM,
        'spare_cells': cols * rows - FRAMES_PER_ANIM,
        'spare_cell_policy': 'transparent_blank',
        'layout': 'row-major from top-left; UE texture V=0 is the top row, matching the grid',
        'frame_rate': bake_manifest['fps'],
        'frame_hashes_sha256': frame_hashes,
    }
    if relayout:
        entry['relayout'] = relayout
        entry['base_columns'] = BASE_COLS
    return entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bake-dir', default=str(DEFAULT_BAKE))
    ap.add_argument('--out-dir', default=str(DEFAULT_OUT))
    args = ap.parse_args()
    bake_dir = Path(args.bake_dir)
    out_dir = Path(args.out_dir)
    bake_manifest = json.loads((bake_dir / 'manifest.json').read_text(encoding='utf-8'))
    if bake_manifest.get('unsupported_effects') or bake_manifest.get('empty_frames'):
        raise RuntimeError('Bake manifest reports unsupported effects or empty frames; fix the bake first.')
    if bake_manifest.get('generator') != 'Scripts/DNFM/bake_spine_fighter.py':
        raise RuntimeError('Unexpected bake generator: %s' % bake_manifest.get('generator'))
    anim_names = [a['name'] for a in bake_manifest['animations']]
    if anim_names != list(ANIM_IDS) or any(a['frame_count'] != FRAMES_PER_ANIM for a in bake_manifest['animations']):
        raise RuntimeError('Bake manifest animations %s do not match expected %s x %d frames' % (anim_names, list(ANIM_IDS), FRAMES_PER_ANIM))
    atlases = [build_atlas(anim, bake_dir, out_dir, bake_manifest) for anim in ANIM_IDS]
    manifest = {
        'generator': 'Scripts/DNFM/composite_fighter_atlas.py',
        'pil_version': Image.__version__,
        'source_bake_dir': str(bake_dir),
        'bake_manifest_sha256': sha256_file(bake_dir / 'manifest.json'),
        'bake_source_hashes': bake_manifest['source'],
        'max_texture_dimension': MAX_DIM,
        'atlas_format': 'RGBA PNG, transparent background, no gutter padding (see limitations)',
        'flipbook': {
            'columns_manifest_key': 'columns',
            'index_formula': 'col = floor(mod(Frame, columns)); row = floor(Frame / columns); cell_uv = frac(uv) / (columns, rows) + (col, row) / (columns, rows)',
            'engine_reference': 'Engine/Shaders/Private/Common.ush Tile1Dto2D + PseudoVolumeTexture UV math',
        },
        'limitations': [
            'No gutter padding between cells; bilinear filtering can bleed neighbour-cell pixels at cell edges.',
            'Mipmaps are disabled at import to avoid cross-cell mip bleed, so distant sampling loses detail.',
            'Spare cells (cells with index >= num_frames) are fully transparent and must not be sampled.',
        ],
        'atlases': atlases,
    }
    dest = out_dir / 'atlas_manifest.json'
    dest.write_text(json.dumps(manifest, ensure_ascii=True, indent=2), encoding='utf-8')
    print('wrote %s (%d atlases)' % (dest, len(atlases)))
    for a in atlases:
        print('  anim %s: %dx%d cols=%d rows=%d frames=%d spare=%d sha256=%s'
              % (a['animation'], a['atlas_size'][0], a['atlas_size'][1], a['columns'], a['rows'], a['num_frames'], a['spare_cells'], a['sha256'][:12]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
