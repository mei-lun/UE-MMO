"""M8-036: build the skill-FX flipbook pages from the fighter source atlases.

The 08 plan problem-4.2 minimal VFX set. Eight fighter .img effect sheets
(the M8-009 exact_texture_path hits) are cropped frame-by-frame with the same
external toolkit the M8-007/M8-019B extractions used and assembled into
deterministic horizontal strip pages for the UE flipbook import (the
M_ThrowStoneVisual UVRect contract: one strip, N cells left-to-right).

Read-only on the DNFM source tree; outputs land in Data/DNFMReplica.
Determinism: two runs produce byte-identical outputs (sha256 pinned in the
manifest).
"""
import hashlib
import json
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\tools')
from restore_dnf_atlas import get_atlas_by_path, crop_frame

OUT_DIR = Path(r'D:\Github-Poj\UE-MMO\Data\DNFMReplica\characters\fighter\fx')
PX_TO_CM = 1.7615  # M6-022/M8-010 calibration

# logical page key -> (source .img path, frame ms for the play-once burst)
SHEETS = {
    'wcs_particle': ('Character/Fighter/Effect/ATWildCannonSpike/particle.img', 60),
    'wcs_big01': ('Character/Fighter/Effect/WildCannonSpike/big_01.img', 60),
    'wcs_big02': ('Character/Fighter/Effect/WildCannonSpike/big_02.img', 60),
    'wcs_big03': ('Character/Fighter/Effect/WildCannonSpike/big_03.img', 60),
    'blockbuster_particle': ('Character/Fighter/Effect/BlockBuster/BlockBuster_BlockParticle.img', 40),
    'risingupper_stone': ('Character/Fighter/Effect/RisingUpperEx/stone.img', 50),
    'skydrop_stone': ('Character/Fighter/Effect/ATDestroyEarthSkyDrop/particle_stone_normal.img', 50),
    'nenflower_particle': ('Character/Fighter/Effect/NenFlower/particle.img', 60),
}


def sha256_of(path):
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    pages = []
    for key, (img_path, frame_ms) in sorted(SHEETS.items()):
        atlas = get_atlas_by_path(img_path)
        page = atlas['pages'][0]
        frames = page['frames']
        if not frames:
            raise RuntimeError('no frames in ' + img_path)
        canvases = {tuple(f['canvas']) for f in frames}
        if len(canvases) != 1:
            raise RuntimeError('non-uniform canvas in %s: %s' % (img_path, sorted(canvases)))
        cw, ch = frames[0]['canvas']
        crops = [crop_frame(atlas, i, full_canvas=True) for i in range(len(frames))]
        for i, im in enumerate(crops):
            if im.size != (cw, ch):
                raise RuntimeError('frame %d of %s is %s, expected %dx%d' % (i, img_path, im.size, cw, ch))
        strip = Image.new('RGBA', (cw * len(frames), ch), (0, 0, 0, 0))
        for i, im in enumerate(crops):
            strip.paste(im, (i * cw, 0))
        page_png = OUT_DIR / ('fx_%s.png' % key)
        strip.save(page_png)
        entry = {
            'page': key,
            'source_img': img_path,
            'atlas_id': atlas['id'],
            'atlas_scale': atlas['scale'],
            'frames': len(frames),
            'cell_px': [cw, ch],
            'page_px': [cw * len(frames), ch],
            'frame_ms': frame_ms,
            'cell_cm': [round(cw * PX_TO_CM, 2), round(ch * PX_TO_CM, 2)],
            'png': page_png.name,
            'sha256': sha256_of(page_png),
        }
        pages.append(entry)
        print('%s: %df cell=%dx%d strip=%dx%d' % (key, len(frames), cw, ch, cw * len(frames), ch))
    manifest = {
        'task': 'M8-036',
        'cm_per_px': PX_TO_CM,
        'pages': pages,
    }
    manifest_path = OUT_DIR / 'skill_fx_manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    print('manifest: %d pages -> %s' % (len(pages), manifest_path))


if __name__ == '__main__':
    main()
