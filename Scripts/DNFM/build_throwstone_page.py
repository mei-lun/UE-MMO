"""M8-019B: build the throwstone projectile page from the source atlas.

M8-007 provenance: ThrowStone.img = atlas 16665, a 60x40 page carrying four
17x17 stone frames (indices 0..3); the renewal flight animation loops them at
30 ms/frame (4f x 30ms = 120ms). This script re-extracts the four frames with
the same external toolkit the M8-007 spotcheck used and assembles a
deterministic 68x17 horizontal strip page for the UE import, plus a small
manifest documenting the source facts.

Read-only on the DNFM source tree; outputs land in Data/DNFMReplica.
Determinism: two runs produce byte-identical outputs (sha256 pinned in the
manifest).
"""
import hashlib
import json
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

from PIL import Image

sys.path.insert(0, r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\tools')
from restore_dnf_atlas import get_atlas_by_path, crop_frame

OUT_DIR = Path(r'D:\Github-Poj\UE-MMO\Data\DNFMReplica\monsters\goblinthrower')
PAGE_PNG = OUT_DIR / 'throwstone_page_0.png'
MANIFEST = OUT_DIR / 'throwstone_manifest.json'
ATLAS_PATH = 'Monster/Goblin/PassiveObject/ThrowStone.img'
FRAME_COUNT = 4
CELL = 17
PX_TO_CM = 1.7615  # M6-022/M8-010 calibration
FRAME_MS = 30


def main():
    atlas = get_atlas_by_path(ATLAS_PATH)
    frames = [crop_frame(atlas, i, full_canvas=True) for i in range(FRAME_COUNT)]
    for i, im in enumerate(frames):
        if im.size != (CELL, CELL):
            raise RuntimeError('frame %d is %s, expected %dx%d' % (i, im.size, CELL, CELL))

    page = Image.new('RGBA', (CELL * FRAME_COUNT, CELL), (0, 0, 0, 0))
    for i, im in enumerate(frames):
        page.paste(im.convert('RGBA'), (i * CELL, 0), im.convert('RGBA'))
    page.save(PAGE_PNG)

    sha = hashlib.sha256(PAGE_PNG.read_bytes()).hexdigest()
    manifest = {
        'generator': 'Scripts/DNFM/build_throwstone_page.py (M8-019B)',
        'generated_at': datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'source_atlas_path': ATLAS_PATH,
        'source_atlas_id': 16665,
        'px_to_cm': PX_TO_CM,
        'frame_count': FRAME_COUNT,
        'frame_ms': FRAME_MS,
        'loop': True,
        'cell_px': [CELL, CELL],
        'page_size_px': [CELL * FRAME_COUNT, CELL],
        'page_sha256': sha,
        'note': ('M8-007 provenance: 4f x 30ms looped flight, 20x20x20 attack box '
                 'centered on the stone active the whole flight, anchor [-8,-8]. '
                 'Release window in attack_0 (240-320ms) is visual inference.'),
    }
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding='utf-8')
    print('page:', PAGE_PNG, page.size, 'sha256:', sha[:16])
    print('manifest:', MANIFEST)


if __name__ == '__main__':
    main()
