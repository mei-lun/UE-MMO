import json
from pathlib import Path
import unreal

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels.load_level('/Game/UEMMO/Maps/L_PrototypeArena')
result = []
for actor in actors.get_all_level_actors():
    entry = {'label': actor.get_actor_label(), 'rotation': str(actor.get_actor_rotation()), 'location': str(actor.get_actor_location())}
    if isinstance(actor, unreal.SkeletalMeshActor):
        component = actor.skeletal_mesh_component
        entry['mesh_rotation'] = str(component.get_editor_property('relative_rotation'))
        entry['mesh_scale'] = str(component.get_editor_property('relative_scale3d'))
        entry['animation_mode'] = str(component.get_editor_property('animation_mode'))
        entry['animation_data'] = str(component.get_editor_property('animation_data'))
    result.append(entry)
Path(unreal.Paths.project_dir(), 'Artifacts', 'scene-inspection.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
