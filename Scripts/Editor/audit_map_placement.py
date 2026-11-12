"""Read-only map placement audit (M8-042): dump actor inventory + per-map problem summary.

Run inside UE via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=Scripts/Editor/audit_map_placement.py
Environment:
  UEMMO_AUDIT_OUT  output directory (required; JSON per map + _summary.json)
  UEMMO_AUDIT_MAPS comma-separated map slugs to audit (default: all)
Never saves or modifies anything.
"""
import json
import os
import sys
from pathlib import Path

import unreal

MAPS = {
    'L_DNFM_Bamboo01': '/Game/UEMMO/DNFM/Bamboo/Maps/L_DNFM_Bamboo01',
    'L_DNFM_Volcano01': '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01',
    'L_DNFM_Guildagit': '/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit',
    'L_DNFM_ResourceGallery': '/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_ResourceGallery',
    'L_CombatRoom01': '/Game/UEMMO/Maps/L_CombatRoom01',
    'L_CombatRoom02': '/Game/UEMMO/Maps/L_CombatRoom02',
    'L_MainMenu': '/Game/UEMMO/Maps/L_MainMenu',
    'L_PrototypeArena': '/Game/UEMMO/Maps/L_PrototypeArena',
    'L_SystemTestRoom': '/Game/UEMMO/Maps/L_SystemTestRoom',
    'L_TrainingArena': '/Game/UEMMO/Maps/L_TrainingArena',
}

FLOAT_MIN_Z = 30.0    # bounds bottom above this = floating
BURIED_MAX_Z = -30.0  # bounds top below this = buried under ground

PLACEHOLDER_MARKERS = ('M_Unassigned', 'M_Default', 'M_GAGroundPlaceholder', 'Placeholder',
                       'MISSING', 'WorldGridMaterial', 'BasicShapeMaterial', 'Invalid')

# Actor labels that are allowed to be flagged-floating (structural, never game content)
FLOAT_WHITELIST = ('BoundsCollision', 'FoundationBackdrop', 'BG_Planet', 'TopFiller')

# Per-map extra placement rules: actor labels matching `pattern` must keep their
# bounds bottom edge inside [z_min_min, z_min_max]. Catches planes floating above
# the ground; deep burial is allowed (the art-level grounding is verified via the
# texture alpha pass + in-game screenshots, see M8-043).
LABEL_RANGE_RULES = {
    'L_DNFM_Volcano01': [
        {'name': 'vol_obj_not_floating', 'pattern': 'VOL_OBJ_',
         'z_min_min': -1800.0, 'z_min_max': 5.0},
    ],
}


def _vec(v):
    return [round(v.x, 2), round(v.y, 2), round(v.z, 2)]


def _safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def collect_components(actor):
    found = []
    try:
        found = list(actor.get_components_by_class(unreal.StaticMeshComponent))
    except Exception:
        found = []
    rows = []
    for c in found:
        mesh = _safe(lambda: c.get_editor_property('static_mesh'))
        mats = []
        n = _safe(lambda: c.get_num_materials(), 0) or 0
        for i in range(n):
            m = _safe(lambda: c.get_material(i))
            mats.append(m.get_path_name() if m else None)
        rows.append({'mesh': mesh.get_path_name() if mesh else None, 'mats': mats,
                     'visible': _safe(lambda: c.is_visible(), True)})
    return rows


def audit_map(slug, map_path):
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not les.load_level(map_path):
        return None, {'slug': slug, 'error': 'load failed'}
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    rows = []
    floats, buried, placeholders = [], [], []
    slot_count = 0
    for a in actors:
        origin, extent = a.get_actor_bounds(False)
        label = _safe(lambda: a.get_actor_label(), a.get_name())
        row = {
            'class': a.get_class().get_name(),
            'label': label,
            'loc': _vec(a.get_actor_location()),
            'bounds_origin': _vec(origin),
            'bounds_extent': _vec(extent),
            'z_min': round(origin.z - extent.z, 2),
            'z_max': round(origin.z + extent.z, 2),
            'components': collect_components(a),
        }
        rows.append(row)
        has_mesh = any(c['mesh'] for c in row['components'])
        if not has_mesh:
            continue
        if row['z_min'] > FLOAT_MIN_Z and not any(w in label for w in FLOAT_WHITELIST):
            floats.append({'label': label, 'loc': row['loc'], 'z_min': row['z_min'],
                           'z_max': row['z_max']})
        if row['z_max'] < BURIED_MAX_Z:
            buried.append({'label': label, 'loc': row['loc'], 'z_min': row['z_min'],
                           'z_max': row['z_max']})
        for c in row['components']:
            for m in c['mats']:
                slot_count += 1
                if m and any(k.lower() in m.lower() for k in PLACEHOLDER_MARKERS):
                    placeholders.append({'label': label, 'mat': m.split('/')[-1],
                                         'loc': row['loc']})
    rule_violations = []
    for rule in LABEL_RANGE_RULES.get(slug, []):
        for row in rows:
            if rule['pattern'] not in row['label']:
                continue
            if not (rule['z_min_min'] <= row['z_min'] <= rule['z_min_max']):
                rule_violations.append({'label': row['label'], 'rule': rule['name'],
                                        'z_min': row['z_min']})
    summary = {
        'slug': slug,
        'map': map_path,
        'actors': len(rows),
        'material_slots': slot_count,
        'floating': floats,
        'buried': buried,
        'placeholder_slots': placeholders,
        'rule_violations': rule_violations,
        'counts': {'floating': len(floats), 'buried': len(buried),
                   'placeholder_slots': len(placeholders),
                   'rule_violations': len(rule_violations)},
    }
    return rows, summary


def main():
    out_dir = os.environ.get('UEMMO_AUDIT_OUT')
    if not out_dir:
        print('UEMMO_AUDIT_FATAL: UEMMO_AUDIT_OUT not set')
        sys.exit(2)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    only = [s.strip() for s in (os.environ.get('UEMMO_AUDIT_MAPS') or '').split(',') if s.strip()]
    selected = {k: v for k, v in MAPS.items() if not only or k in only}

    all_summaries = []
    for slug, map_path in sorted(selected.items()):
        rows, summary = audit_map(slug, map_path)
        if rows is None:
            all_summaries.append(summary)
            print('UEMMO_AUDIT: %s ERROR' % slug)
            continue
        with open(out / ('detail_%s.json' % slug), 'w', encoding='utf-8') as fh:
            json.dump({'slug': slug, 'map': map_path, 'actors': rows}, fh, ensure_ascii=False)
        with open(out / ('summary_%s.json' % slug), 'w', encoding='utf-8') as fh:
            json.dump(summary, fh, ensure_ascii=False, indent=1)
        all_summaries.append({'slug': slug, 'map': map_path, 'actors': summary['actors'],
                              'material_slots': summary['material_slots'],
                              'counts': summary['counts']})
        print('UEMMO_AUDIT: %s actors=%d slots=%d floating=%d buried=%d placeholder=%d' % (
            slug, summary['actors'], summary['material_slots'],
            summary['counts']['floating'], summary['counts']['buried'],
            summary['counts']['placeholder_slots']))
        sys.stdout.flush()
    with open(out / '_summary.json', 'w', encoding='utf-8') as fh:
        json.dump(all_summaries, fh, ensure_ascii=False, indent=1)
    print('UEMMO_AUDIT_DONE')


main()
