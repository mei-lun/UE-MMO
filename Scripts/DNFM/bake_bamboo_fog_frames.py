#!/usr/bin/env python3
"""M8-013: bake the bamboo fog frames (vertical flip + source tint) and the
frame-cycling timing manifest.

The fog objects (BossMap_fog, placement indices 46-49) carry per-frame
flip_y=True and color_rgba=[255,255,100,255] in the source data
(bamboo_placement.json frames[]); both transforms are baked into standalone
PNGs here so the UE side stays on the standard unlit material path. Output is
deterministic: same inputs -> byte-identical PNGs and timing JSON.

Usage: python -I bake_bamboo_fog_frames.py
"""
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'Data' / 'DNFMReplica'
PLACEMENT = DATA / 'bamboo_placement.json'
ASSETS = DATA / 'bamboo_assets'
OUT_DIR = DATA / 'bamboo_fog'
OUT_JSON = OUT_DIR / 'fog_timing.json'

FOG_INDICES = (46, 47, 48, 49)


def decode_rgba(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a png: %s' % path)
    pos = 8
    w = h = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', chunk[:10])
            if depth != 8 or color != 6:
                raise ValueError('expected 8-bit RGBA png: %s' % path)
        elif ctype == b'IDAT':
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    bpp = 4
    stride = w * bpp
    prev = bytearray(stride)
    out = bytearray(h * stride)
    pos = 0
    for row in range(h):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ftype != 0:
            raise ValueError('unsupported PNG filter %d' % ftype)
        out[row * stride:(row + 1) * stride] = line
        prev = line
    return w, h, out


def encode_rgba(w, h, rgba, path):
    def chunk(ctype, payload):
        body = ctype + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    stride = w * 4
    raw = bytearray()
    for row in range(h):
        raw.append(0)
        raw += rgba[row * stride:(row + 1) * stride]
    path.write_bytes(b'\x89PNG\r\n\x1a\n'
                     + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
                     + chunk(b'IEND', b''))


def flip_vertical(w, h, px):
    stride = w * 4
    out = bytearray(len(px))
    for row in range(h):
        src = (h - 1 - row) * stride
        out[row * stride:(row + 1) * stride] = px[src:src + stride]
    return out


def apply_tint(px, tint_rgb):
    out = bytearray(px)
    for i in range(0, len(out), 4):
        out[i] = (out[i] * tint_rgb[0]) // 255
        out[i + 1] = (out[i + 1] * tint_rgb[1]) // 255
        out[i + 2] = (out[i + 2] * tint_rgb[2]) // 255
    return out


def main():
    place = json.loads(PLACEMENT.read_text(encoding='utf-8'))
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    timing = {'source': 'bamboo_placement.json frames[] (M8-013)',
              'bake': 'vertical flip (flip_y=True) + RGB tint (color_rgba/255) baked into PNG',
              'objects': []}
    seen_source = {}
    for obj in place['objects']:
        if obj.get('index') not in FOG_INDICES:
            continue
        frames = obj['frames']
        if len(frames) != 2:
            raise ValueError('fog object %s: expected 2 frames, got %d' % (obj['index'], len(frames)))
        entries = []
        first_stem = Path(obj['first_frame']['image_file']).stem
        files = [obj['first_frame']['image_file'],
                 'objects_anim/%s_f01.png' % first_stem]
        # frames[] carries the authoritative flip/tint/delay per frame; the
        # anim file list gives the on-disk PNG for each frame in order.
        for frame_idx, (frame, image_file) in enumerate(zip(frames, files)):
            src = ASSETS / image_file
            if not src.is_file():
                raise FileNotFoundError(src)
            tint = frame['color_rgba'][:3]
            key = (str(src), tuple(tint))
            if key in seen_source:
                out_name = seen_source[key]
            else:
                w, h, px = decode_rgba(src)
                baked = apply_tint(flip_vertical(w, h, px), tint)
                stem = Path(image_file).stem
                out_name = 'fog_%s_baked.png' % stem
                encode_rgba(w, h, baked, OUT_DIR / out_name)
                seen_source[key] = out_name
            entries.append({'frame': frame_idx,
                            'source_png': str(src.relative_to(DATA)),
                            'baked_png': out_name,
                            'delay_ms': frame['delay_ms'],
                            'flip_y': bool(frame['flip_y']),
                            'tint_rgb': tint})
        timing['objects'].append({'index': obj['index'], 'draw_layer': obj['draw_layer'],
                                  'loop_ms': obj.get('animation_duration_ms'),
                                  'frames': entries})
    if len(timing['objects']) != len(FOG_INDICES):
        raise ValueError('expected %d fog objects, matched %d' % (len(FOG_INDICES), len(timing['objects'])))
    OUT_JSON.write_text(json.dumps(timing, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
    print('baked %d fog objects, %d unique pngs -> %s' % (len(timing['objects']), len(seen_source), OUT_DIR))


if __name__ == '__main__':
    sys.exit(main())
