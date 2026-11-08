"""M8-024 diagnostic: the guild map's light/sky actors and one visible
object's material slot (the dark-render suspects)."""
from collections import Counter

import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not levels.load_level('/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'):
    raise RuntimeError('load failed')
all_actors = actors.get_all_level_actors()
print('total actors:', len(all_actors))
lights = []
for a in all_actors:
    cls = a.get_class().get_name()
    if 'Light' in cls or 'Sky' in cls or 'Fog' in cls or 'Atmosphere' in cls:
        lights.append(cls)
print('light-ish actors:', Counter(lights))
# one mesh actor's material slots
checked = 0
for a in all_actors:
    comp = a.get_component_by_class(unreal.StaticMeshComponent)
    if comp is None:
        continue
    mesh = comp.get_editor_property('static_mesh')
    if mesh is None:
        continue
    mats = [comp.get_material(i) for i in range(comp.get_num_materials())]
    names = [m.get_name() if m else 'NULL' for m in mats]
    print('mesh actor:', a.get_actor_label(), 'mesh:', mesh.get_name(), 'materials:', names[:4])
    checked += 1
    if checked >= 6:
        break
