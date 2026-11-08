"""M8-024: count the guild map's actor material slots still carrying the
placeholder materials (M_Unassigned / M_Default_*)."""
from collections import Counter

import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not levels.load_level('/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'):
    raise RuntimeError('load failed')
placeholder = Counter()
textured = Counter()
examples = {}
total_slots = 0
for a in actors.get_all_level_actors():
    comp = a.get_component_by_class(unreal.StaticMeshComponent)
    if comp is None:
        continue
    for i in range(comp.get_num_materials()):
        m = comp.get_material(i)
        if m is None:
            placeholder['NULL'] += 1
            continue
        name = m.get_name()
        total_slots += 1
        if name.startswith('M_Unassigned') or name.startswith('M_Default'):
            placeholder[name] += 1
            examples.setdefault(name, a.get_actor_label())
        else:
            textured[name.split('_')[1] if name.startswith('M_GA_') else name] += 1
print('total material slots:', total_slots)
print('placeholder slots:', sum(placeholder.values()), dict(placeholder))
print('textured slots:', sum(textured.values()))
print('textured texture kinds:', len(textured))
for k, v in examples.items():
    print('placeholder example:', k, '->', v)
