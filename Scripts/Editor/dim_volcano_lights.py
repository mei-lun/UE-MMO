"""M8-044: dim the volcano-family maps' lights to a volcanic dusk so the dark
rock palette reads dark and the emissive lava dominates (sun 5->1.5, fill
2->0.8, sky 1->0.6)."""
import unreal

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MAPS = ('/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01',
        '/Game/UEMMO/Maps/L_MainMenu')
TARGETS = {'DirectionalLight': (5.0, 1.5), 'FoundationFillLight': (2.0, 0.8),
           'SkyLight': (1.0, 0.6)}
result = []
for map_path in MAPS:
    if not LEVELS.load_level(map_path):
        raise RuntimeError('cannot load ' + map_path)
    changes = []
    for a in ACTORS.get_all_level_actors():
        lb = a.get_actor_label()
        base = lb.replace('MainMenu', '')
        for key, (old, new) in TARGETS.items():
            if base == key or lb == key:
                for comp in a.get_components_by_class(unreal.LightComponentBase):
                    cur = comp.get_editor_property('intensity')
                    if abs(cur - old) < 0.01:
                        comp.set_editor_property('intensity', new)
                        changes.append('%s %.2f->%.2f' % (key, old, new))
    if not LEVELS.save_current_level():
        raise RuntimeError('save failed ' + map_path)
    result.append('%s: %s' % (map_path.split('/')[-1], '; '.join(changes) or 'no matches'))
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-044/dim-lights.txt', 'w') as fh:
    fh.write('\n'.join(result))
