#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M8-011 offline part: volcano map tile/object texture cropping from the DNFM
atlas (strict read-only) + deterministic UE placement intermediate data.

Source (READ-ONLY): D:/Github-Poj/DNFM (restored atlas.sqlite / maps.sqlite /
animations.sqlite / script_index.sqlite via sqlite mode=ro URI; page PNGs opened
read-only). Zero writes under D:/Github-Poj/DNFM. No UE involved.

Determinism: every output byte-identical on re-run (sort_keys JSON, fixed
BUILD_STAMP constant, no timestamps inside generated files, PIL PNG encoder).

Placement conventions (inherited from M8-005 evidence, see placement JSON
"conventions" section for full notes):
  - objects top_left = [src_x + frame_pos_x, src_y - src_z + frame_pos_y]
    (preview screen space, y-down; formula verified 50/50 against the
    official M8-005 preview JSON)
  - tiles x = 224*placement_index (native LoadBaseTile formula, origin 0,0);
    y_top = floor_height - 80 = 40 (native formula value, as tasked);
    the preview/M8-005 compose screen-space equivalent (80 - floor_height = -40)
    is recorded alongside as y_top_screen_ydown_px for M8-010 to resolve.
  - no px->cm conversion (M8-010 owns it); "world": null placeholders reserved.

Run:
  C:/Users/mei/AppData/Local/Python/pythoncore-3.14-64/python.exe -I <script>

Outputs (overwritten each run):
  Data/DNFMReplica/volcano_assets/tiles/tile_<hash16>.png          (2 unique defs)
  Data/DNFMReplica/volcano_assets/objects/L{n}_{ii}_{key8}.png     (50 first frames)
  Data/DNFMReplica/volcano_assets/objects_anim/L{n}_{ii}_{key8}_f{nn}.png
                                                                  (multi-frame objects, all image frames)
  Data/DNFMReplica/volcano_assets/manifest.json
  Data/DNFMReplica/volcano_placement.json
"""
import hashlib
import json
import shutil
import sqlite3
import sys
import zlib
from pathlib import Path

from PIL import Image

REST = Path('D:/Github-Poj/DNFM/DNFM_export_130.5.8.0/downloaded_130.5.8.202/restored')
DNFM_ROOT = Path('D:/Github-Poj/DNFM/DNFM_export_130.5.8.0')
sys.path.insert(0, str(DNFM_ROOT / 'tools'))
from restore_dnf_atlas import atlas_hash, crop_frame  # noqa: E402  (pure helpers; DB access replaced below)

UE_ROOT = Path('D:/Github-Poj/UE-MMO')
ASSETS = UE_ROOT / 'Data' / 'DNFMReplica' / 'volcano_assets'
PLACEMENT = UE_ROOT / 'Data' / 'DNFMReplica' / 'volcano_placement.json'

MAP_HASH = 'f7d486ec4dcbc7d5'
BUILD_STAMP = '2026-11-09T00:00+0800'
TASK_LINE = 'M8-027 P07-A 火山图资产提取与标定输入 — 离线裁切与放置中间数据（UE 部分由协调者执行）'
DRAW_LAYERS = ['DEFAULT', 'DISTANTBACK', 'MIDDLEBACK', 'TILE', 'CLOSEBACK',
               'BOTTOM', 'NORMAL', 'CONTACT', 'COVER', 'INVISIBLE', 'MAX']

TILE_LAYER_NAME = 3  # draw layer of base tiles (M8-005 TILE layer)


def connect_ro(p):
    return sqlite3.connect('file:' + p.as_posix() + '?mode=ro', uri=True)


def blob(b):
    if isinstance(b, (bytes, bytearray)):
        return json.loads(zlib.decompress(b) if b[:1] == b'\x78' else b)
    return json.loads(b)


def get_atlas_ro(image_path):
    """restore_dnf_atlas.get_atlas_by_path with strict mode=ro URI connect."""
    db = connect_ro(REST / 'atlas/atlas.sqlite')
    try:
        row = db.execute('SELECT data_zlib FROM atlases WHERE lookup_hash=?',
                         (atlas_hash(image_path),)).fetchone()
    finally:
        db.close()
    if row is None:
        raise KeyError(image_path)
    return json.loads(zlib.decompress(row[0]))


def atlas_meta_ro(image_path):
    db = connect_ro(REST / 'atlas/atlas.sqlite')
    try:
        row = db.execute('SELECT id, name, lookup_hash, sha256 FROM atlases WHERE lookup_hash=?',
                         (atlas_hash(image_path),)).fetchone()
    finally:
        db.close()
    if row is None:
        raise KeyError(image_path)
    return {'atlas_id': row[0], 'atlas_name': row[1], 'atlas_lookup_hash': row[2],
            'atlas_payload_sha256': row[3]}


def locate_page_frame(atlas, frame_index):
    matches = [(pi, p, f) for pi, p in enumerate(atlas['pages'])
               for f in p['frames'] if f['index'] == frame_index]
    if len(matches) != 1:
        raise ValueError('frame selection not unique: %d' % len(matches))
    return matches[0]


def norm_scale(s):
    if isinstance(s, dict):
        return [s.get('x', 1.0), s.get('y', 1.0)]
    return list(s) if s is not None else [1.0, 1.0]


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def save_png(im, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path, format='PNG')


# ---------------------------------------------------------------------------
# Per-PNG provenance record (manifest entry source block)
# ---------------------------------------------------------------------------

def frame_source(kind, atlas, image_path, frame_index, frame_pos, extra, png_path,
                 png_size, png_sha):
    pi, page, fr = locate_page_frame(atlas, frame_index)
    rec = {
        'kind': kind,
        'file': png_path.as_posix().replace('D:/Github-Poj/UE-MMO/', ''),
        'png_size_px': list(png_size),
        'png_sha256': png_sha,
        'source': {
            'atlas_image_path': image_path,
            'atlas_frame_index': frame_index,
            'frame_name': fr.get('name'),
            'rect_pixels': fr.get('rect_pixels', fr.get('rect')),
            'canvas_px': list(fr.get('canvas', [0, 0])),
            'offset_px': list(fr.get('offset', [0, 0])),
            'page_index': pi,
            'page_texture_path': page.get('texture_path'),
            'page_resolved_png': page.get('resolved_png'),
            'page_format_code': page.get('format_code'),
            'page_palette': bool(page.get('palette_path')),
            'page_declared_size_px': [page.get('width'), page.get('height')],
            'crop_rule': 'atlas rect cropped then alpha-composited at frame offset on '
                         'frame canvas (restore_dnf_atlas.crop_frame full_canvas=True); '
                         'no 1-v flip, no scale baked (transforms left to UE material layer)',
        },
    }
    rec['source'].update(atlas_meta_ro(image_path))
    rec.update(extra)
    return rec


def animation_transform_fields(f):
    """Per-frame transform/render fields attached for UE-side handling (as-is)."""
    scale = norm_scale(f.get('scale', [1.0, 1.0]))
    interp_val = f.get('interpolation_value')
    return {
        'scale_xy': scale,
        'flip_x': scale[0] < 0,
        'flip_y': scale[1] < 0,
        'rotation_degrees': f.get('rotation_degrees', 0.0),
        'frame_position_offset_px': list(f.get('position', [0, 0])),
        'delay_ms': f.get('delay_ms'),
        'color_rgba': list(f.get('color_rgba', [255, 255, 255, 255])),
        'source_range': f.get('source_range'),
        'flip_copy_type': f.get('flip_copy_type'),
        'offsets': f.get('offsets'),
        'clip_area': f.get('clip_area'),
        'interpolation': bool(f.get('interpolation', False)),
        'interpolation_value': interp_val,
        'interpolation_center_pos': f.get('interpolation_center_pos'),
        'udim': None,
    }


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build():
    maps_db = connect_ro(REST / 'maps/maps.sqlite')
    scripts_db = connect_ro(REST / 'script_index.sqlite')
    anim_db = connect_ro(REST / 'animations/animations.sqlite')

    row = maps_db.execute('select shard, block, scene_json from maps where hash=?',
                          (MAP_HASH,)).fetchone()
    if row is None:
        raise SystemExit('map hash not found: ' + MAP_HASH)
    shard, block, scene_raw = row
    scene_j = blob(scene_raw)
    preview = json.loads((REST / 'maps/previews/map_013_00595.json').read_text(encoding='utf-8'))

    # fresh managed output dirs (only these three are cleaned)
    tiles_dir = ASSETS / 'tiles'
    objects_dir = ASSETS / 'objects'
    anim_dir = ASSETS / 'objects_anim'
    for d in (tiles_dir, objects_dir, anim_dir):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)

    manifest_files = []

    # ---------------- tiles ----------------
    tile_defs = {}
    tile_instances = []
    for i, key in enumerate(scene_j['base_tile_keys']):
        h = key.get('hash64')
        trow = maps_db.execute('select semantic_json from tiles where hash=?', (h,)).fetchone()
        if trow is None:
            raise SystemExit('tile definition missing from tiles table: ' + h)
        sem = blob(trow[0])
        if h not in tile_defs:
            layers = sem['image_layers']
            if len(layers) != 1:
                # M8-027: the volcano map carries tile rows with ZERO image
                # layers (the ground is object-based there, per the
                # map_volcano.json extended_tiles audit) - record them as
                # empty defs and keep going instead of aborting.
                print('tile %s has %d image layers; recorded as empty' % (h, len(layers)))
                tile_defs[h] = {
                    'tile_hash64': h,
                    'floor_height_px': sem['floor_height'],
                    'tile_horizontal_step_px': sem['tile_horizontal_step'],
                    'image_file': None,
                    'empty': True,
                }
                print('tile %s recorded as empty (no image layers)' % h)
            else:
                layer = sem['image_layers'][0]
                atlas = get_atlas_ro(layer['image_path'])
                im = crop_frame(atlas, layer['image_frame_index'])
                fname = 'tiles/tile_%s.png' % h
                out_path = ASSETS / fname
                save_png(im, out_path)
                png_sha = sha256_file(out_path)
                tile_defs[h] = {
                    'tile_hash64': h,
                    'floor_height_px': sem['floor_height'],
                    'tile_horizontal_step_px': sem['tile_horizontal_step'],
                    'image_file': fname,
                    'png_size_px': list(im.size),
                    'png_sha256': png_sha,
                    'atlas_image_path': layer['image_path'],
                    'atlas_frame_index': layer['image_frame_index'],
                }
                manifest_files.append(frame_source(
                    'tile', atlas, layer['image_path'], layer['image_frame_index'], None,
                    {'tile_hash64': h, 'placement_indices': []},
                    out_path, im.size, png_sha))
        inst = {
            'placement_index': i,
            'tile_hash64': h,
            'image_file': tile_defs[h]['image_file'],
            'x_px': 224 * i,
            'y_top_px': sem['floor_height'] - 80,
            'y_top_screen_ydown_px': 80 - sem['floor_height'],
            'floor_height_px': sem['floor_height'],
            'world': None,
        }
        tile_instances.append(inst)
        mrow = next((d for d in manifest_files if d.get('tile_hash64') == h), None)
        if mrow is not None:
            mrow['placement_indices'].append(i)

    # ---------------- objects ----------------
    objects_out = []
    skipped_objects = []
    total_image_frames = 0
    for oi, o in enumerate(scene_j['animation_objects']):
        layer = o['draw_layer']
        key_hash = o['script_key'].get('hash64')
        src = o['position']  # [x, y, z]
        srow = scripts_db.execute('select shard, block from scripts where hash=?',
                                  (key_hash,)).fetchone()
        if srow is None:
            raise SystemExit('object script key absent from script_index: obj %d' % oi)
        arow = anim_db.execute('select frame_count, data from animations where shard=? and block=?',
                               (f'script_compressed_chn_{srow[0]}_{srow[0]}_u_p.payloads',
                                srow[1])).fetchone()
        if arow is None:
            raise SystemExit('animation payload absent: obj %d' % oi)
        data = json.loads(zlib.decompress(arow[1]))
        key8 = key_hash[:8]
        base = 'L%d_%02d_%s' % (layer, oi, key8)

        frames_out = []
        img_seen = 0
        pending_anim = []  # (fpos, atlas, frame dict, im, scale, pos) — written if multi-frame
        for fpos, f in enumerate(data['frames']):
            if not f.get('image_path'):
                continue
            atlas = get_atlas_ro(f['image_path'])
            im = crop_frame(atlas, f['image_frame_index'])
            scale = norm_scale(f.get('scale', [1.0, 1.0]))
            pos = list(f.get('position', [0, 0]))
            first = img_seen == 0
            fname = 'objects/%s.png' % base
            if first:
                out_path = ASSETS / fname
                save_png(im, out_path)
                png_sha = sha256_file(out_path)
                manifest_files.append(frame_source(
                    'object_first', atlas, f['image_path'], f['image_frame_index'], fpos,
                    {'object_index': oi, 'draw_layer': layer, 'script_hash64': key_hash,
                     'frame_position': fpos},
                    out_path, im.size, png_sha))
            pending_anim.append((fpos, atlas, f, im, fname))
            tf = animation_transform_fields(f)
            scaled_w = max(1, round(im.width * abs(scale[0])))
            scaled_h = max(1, round(im.height * abs(scale[1])))
            frame_rec = {
                'frame_position': fpos,
                'image_file': fname,
                'png_size_px': list(im.size),
                'scaled_size_px': [scaled_w, scaled_h],
                'top_left_px': [src[0] + pos[0], src[1] - src[2] + pos[1]],
                'atlas_image_path': f['image_path'],
                'atlas_frame_index': f['image_frame_index'],
            }
            frame_rec.update(tf)
            frames_out.append(frame_rec)
            img_seen += 1
        if img_seen == 0:
            # M8-027: pure-logic animation objects (no images at all - the
            # map_volcano.json objects_by_layer audit lists them) are recorded
            # and skipped; they have no visual to import.
            print('object %d has no image frame; skipped' % oi)
            skipped_objects.append({'index': oi, 'reason': 'no_image_frame'})
            continue
        total_image_frames += img_seen
        if img_seen > 1:
            # multi-frame object: full _f{nn} sequence into objects_anim (first frame kept
            # un-suffixed in objects/ as well, so _f00 duplicates it byte-for-byte)
            frames_out[0]['anim_copy_file'] = 'objects_anim/%s_f00.png' % base
            for fpos, atlas, f, im, fname_first in pending_anim:
                anim_name = 'objects_anim/%s_f%02d.png' % (base, fpos)
                out_path = ASSETS / anim_name
                save_png(im, out_path)
                png_sha = sha256_file(out_path)
                manifest_files.append(frame_source(
                    'object_anim', atlas, f['image_path'], f['image_frame_index'], fpos,
                    {'object_index': oi, 'draw_layer': layer, 'script_hash64': key_hash,
                     'frame_position': fpos,
                     'sequence_note': 'multi-frame object full sequence; _f00 duplicates the '
                                      'objects/ first frame byte-for-byte'},
                    out_path, im.size, png_sha))

        ff = frames_out[0]
        rec = {
            'index': oi,
            'draw_layer': layer,
            'draw_layer_name': DRAW_LAYERS[layer] if 0 <= layer < len(DRAW_LAYERS) else 'UNKNOWN',
            'script_hash64': key_hash,
            'script_shard': srow[0],
            'script_block': srow[1],
            'animation_shard_block': [srow[0], srow[1]],
            'source_position_xyz_px': list(src),
            'position_xy_px': [src[0], src[1]],
            'z_correction_px': src[2],
            'first_frame': {
                'image_file': ff['image_file'],
                'png_size_px': ff['png_size_px'],
                'scaled_size_px': ff['scaled_size_px'],
                'top_left_px': ff['top_left_px'],
                'foot_point_px': {
                    'x': src[0],
                    'y': ff['top_left_px'][1] + ff['scaled_size_px'][1],
                    'y_basis': 'first-frame image bottom edge y (M8-005 foot_points 口径); '
                               'x 沿用源 position x（图像锚点偏移未解释，M8-010 标定裁决）',
                },
                'atlas_image_path': ff['atlas_image_path'],
                'atlas_frame_index': ff['atlas_frame_index'],
                'delay_ms': ff['delay_ms'],
            },
            'decoded_frame_count': arow[0],
            'image_frame_count': img_seen,
            'loop': data.get('loop'),
            'animation_duration_ms': data.get('duration_ms'),
            'world': None,
            'frames': frames_out,
        }
        objects_out.append(rec)

    # ---------------- manifest ----------------
    manifest_files.sort(key=lambda r: r['file'])
    manifest = {
        'build_stamp': BUILD_STAMP,
        'task': TASK_LINE,
        'map_hash': MAP_HASH,
        'map_shard_block': [shard, block],
        'assets_root': 'Data/DNFMReplica/volcano_assets/',
        'counts': {
            'tile_png': len([r for r in manifest_files if r['kind'] == 'tile']),
            'object_first_frame_png': len([r for r in manifest_files if r['kind'] == 'object_first']),
            'object_anim_png': len([r for r in manifest_files if r['kind'] == 'object_anim']),
            'png_total': len(manifest_files),
            'object_image_frames_exported': total_image_frames,
        },
        'read_only_statement': '源库以 sqlite mode=ro URI 打开，页面 PNG 只读打开；D:/Github-Poj/DNFM 零写入。',
        'files': manifest_files,
    }
    manifest_path = ASSETS / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True),
                             encoding='utf-8')

    # ---------------- placement JSON ----------------
    placement = {
        'build_stamp': BUILD_STAMP,
        'task': TASK_LINE,
        'map': {'maps_hash': MAP_HASH, 'shard': shard, 'block': block,
                'scene_content_bounds_px': preview['bounds']},
        'units': '源 2D 像素坐标（原始游戏像素，非 Unity 米）；本文件不做 px→cm 换算',
        'px_to_world': {
            'owner_task': 'M8-010',
            'status': 'not_converted_here',
            'world_field': '每个对象/地砖实例的 "world" 字段为 null 占位，'
                           '按 M8-010 JSON 字段口径由其换算结果回填',
        },
        'assets_root': 'Data/DNFMReplica/volcano_assets/',
        'manifest_file': 'Data/DNFMReplica/volcano_assets/manifest.json',
        'conventions': {
            'object_top_left_formula': 'top_left = [x + frame_pos_x, y - z + frame_pos_y] '
                                       '(M8-005 预览口径，y-down 屏幕空间；50/50 与官方预览 JSON 一致)',
            'foot_point': 'y = 首帧图像底边（top_left_y + 缩放后高，M8-005 foot_points 口径）；'
                          'x = 源 position x（图像锚点偏移未解释）',
            'tile_placement_formula': 'x = 224*placement_index + originX; '
                                      'y_top = floor_height - 80 - originY（native 证据，M8-005）',
            'controller_origin': 'originX/originY 未恢复，按 (0,0) 处理（M8-005 G5）',
            'y_axis_note': '地砖 y_top_px=40 为 native 公式值（floorHeight-80，origin 0,0）；'
                           'M8-005 预览/合成在同一屏幕 y-down 空间用 80-floorHeight=-40 放置地砖'
                           '（同乘 -1 的两种口径），本文件两值都给出（y_top_px 与 y_top_screen_ydown_px），'
                           '由 M8-010 标定裁决',
            'depth_note': 'z 为 y 修正（投影 y-z，M8-005 预览口径）；UE 纵深映射由 M8-010 定',
            'frame_transforms': '翻转/缩放/旋转/逐帧位置偏移等变换字段按帧附于 objects[].frames[]，'
                                'PNG 未烘焙任何翻转或缩放（地砖与对象一致），由 UE 侧处理；'
                                '本图 rotation 全 0、scale 仅 ±1（无数量级缩放）；'
                                '无 udim 字段（逐帧 udim=null 占位）',
            'anim_sequence': '多帧对象的 objects_anim/ 含全帧 _f{nn} 序列（_f00 与 objects/ 首帧'
                             '字节相同，便于按序取帧）；单帧对象只有 objects/ 首帧文件',
            'painter_order': 'draw_layer 升序绘制（远景→前景），同层内按源 y 排序为 M8-005 预览口径',
        },
        'tiles': {
            'unique_definitions': [tile_defs[h] for h in sorted(tile_defs)],
            'instances': tile_instances,
            'instance_count': len(tile_instances),
        },
        'objects': objects_out,
        'summary': {
            'object_count': len(objects_out),
            'layer_counts': {str(l): sum(1 for r in objects_out if r['draw_layer'] == l)
                             for l in sorted({r['draw_layer'] for r in objects_out})},
            'distinct_script_keys': len({r['script_hash64'] for r in objects_out}),
            'image_frames_exported_total': total_image_frames,
            'multi_frame_object_indices': [r['index'] for r in objects_out if r['image_frame_count'] > 1],
            'png_file_total': len(manifest_files),
        },
        'evidence': {
            'build_script': 'Artifacts/Tasks/M8-011/2026-11-07T0227+0800/scripts/build_volcano_assets.py',
            'probe_script': 'Artifacts/Tasks/M8-011/2026-11-07T0227+0800/scripts/p1_probe_frames.py',
            'source_databases_read_only': ['restored/maps/maps.sqlite', 'restored/atlas/atlas.sqlite',
                                           'restored/animations/animations.sqlite',
                                           'restored/script_index.sqlite'],
            'm8_005_evidence': 'Artifacts/Tasks/M8-005/2026-11-07T0030+0800/',
        },
    }
    PLACEMENT.write_text(json.dumps(placement, ensure_ascii=False, indent=2, sort_keys=True),
                         encoding='utf-8')

    maps_db.close(); scripts_db.close(); anim_db.close()
    print('tiles:', len(tile_defs), 'objects:', len(objects_out),
          'image frames exported:', total_image_frames, 'png files:', len(manifest_files))
    print('wrote', manifest_path)
    print('wrote', PLACEMENT)
    return manifest


if __name__ == '__main__':
    build()
