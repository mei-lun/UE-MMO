"""M8-043 pass 2: ground each volcano OBJ plane by its texture's alpha bottom.

Pass 1 put every plane's geometric bottom at -30; the texture manifest shows 30
of 44 planes carry transparent padding below the art (art_bottom_v < 1), so the
visible art still floats. This pass shifts each plane so the ART bottom sits at
TARGET_Z (-8cm, a small bury to avoid gap flicker), reading the manifest at
Artifacts/Tasks/M8-043/alpha-manifest.json (material M_OBJ_XX_* -> T_OBJ_XX_*).
"""
import json

import unreal

MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
MANIFEST = 'D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-043/alpha-manifest.json'
TARGET_Z = -8.0

with open(MANIFEST, encoding='utf-8') as fh:
    manifest = json.load(fh)

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)

shifted = []
missing = []
for a in ACTORS.get_all_level_actors():
    label = a.get_actor_label()
    if not label.startswith('VOL_OBJ_'):
        continue
    comps = a.get_components_by_class(unreal.StaticMeshComponent)
    tex_key = None
    if comps:
        m = comps[0].get_material(0)
        if m is not None:
            mat_name = m.get_path_name().split('/')[-1].split('.')[0]  # M_OBJ_10_CONTACT
            tex_key = mat_name.replace('M_OBJ_', 'T_OBJ_')
    entry = manifest.get(tex_key) if tex_key else None
    if entry is None or entry.get('art_bottom_v') is None:
        missing.append(label)
        continue
    origin, extent = a.get_actor_bounds(False)
    z_min, z_max = origin.z - extent.z, origin.z + extent.z
    art_bottom_world = z_max - entry['art_bottom_v'] * (z_max - z_min)
    delta = art_bottom_world - TARGET_Z  # shift down when the art floats
    if abs(delta) < 0.5:
        shifted.append((label, 0.0))
        continue
    loc = a.get_actor_location()
    a.set_actor_location(unreal.Vector(loc.x, loc.y, loc.z - delta),
                         sweep=False, teleport=True)
    shifted.append((label, round(-delta, 1)))

if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
print('UEMMO_VOL_PASS2: shifted=%d missing_manifest=%s' % (len(shifted), missing))
print('UEMMO_VOL_PASS2: sample=%s' % shifted[:8])
