"""M8-025 diagnostic: the guild map's actor X distribution - where the
content actually sits, and what the densest band looks like."""
import collections

import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not levels.load_level('/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'):
    raise RuntimeError('load failed')
h = collections.Counter()
ys = []
count = 0
for a in actors.get_all_level_actors():
    comp = a.get_component_by_class(unreal.StaticMeshComponent)
    if comp is None:
        continue
    loc = a.get_actor_location()
    h[int(loc.x // 1000) * 1000] += 1
    ys.append(loc.y)
    count += 1
print('mesh actors:', count)
print('X histogram (1000cm bins):', dict(sorted(h.items())))
if ys:
    print('Y: %.0f .. %.0f' % (min(ys), max(ys)))
densest = max(h, key=h.get)
print('densest bin:', densest)
