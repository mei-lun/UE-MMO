"""Capture DNFArt (M6-008) preview shots inside the FULL editor.

Run with:  UnrealEditor.exe <uproject> -ExecutePythonScript=<this file>
The full editor has a real renderer; the commandlet's RT export is a no-op.

Spawns temporary SkeletalMeshActors for the imported DNFArt meshes, drives
action frames at explicit instants (single-node + set_position), and exports
side/front/back bind-pose views plus per-action freeze frames to
Artifacts/Tasks/M6-008/. Cleans up its actors afterwards.
"""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
DEST = '/Game/UEMMO/ArtValidation/DNFArt'
SHOT_DIR = ROOT / 'Artifacts' / 'Tasks' / 'M6-008'
REPORT = ROOT / 'Artifacts' / 'Logs' / 'dnf-art-previews-report.json'

CHARACTERS = [
    {
        'id': 'hero',
        'dest': DEST + '/Hero',
        'mesh': 'hero_skm',
        'actions': ['idle', 'launcher', 'run_x'],
        'action_peaks': {'idle': 0.5, 'launcher': 0.27, 'run_x': 0.1},
    },
    {
        'id': 'goblin',
        'dest': DEST + '/Goblin',
        'mesh': 'goblin_skm',
        'actions': ['idle', 'telegraph', 'attack'],
        'action_peaks': {'idle': 0.4, 'telegraph': 0.5, 'attack': 0.5},
    },
]


def capture_previews(cfg):
    out = {'shots': [], 'notes': []}
    world = unreal.EditorLevelLibrary.get_editor_world()
    mesh = unreal.load_asset('%s/%s' % (cfg['dest'], cfg['mesh']))
    if not mesh:
        raise RuntimeError('mesh missing: %s' % cfg['mesh'])
    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    center = bounds.origin
    radius = max(float(ext.x), float(ext.y), float(ext.z))
    height = float(ext.z) * 2.0

    spawner = unreal.EditorActorSubsystem()
    actor = spawner.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0),
                                           unreal.Rotator(0, 0, 0))
    actor.set_actor_label('DNFArtShot_%s' % cfg['id'])
    comp = actor.skeletal_mesh_component
    comp.set_skeletal_mesh(mesh, False)
    comp.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)

    cap_actor = spawner.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0),
                                               unreal.Rotator(0, 0, 0))
    cap_actor.set_actor_label('DNFArtCam_%s' % cfg['id'])
    cap = cap_actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, 720, 1080, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0.18, 0.19, 0.22, 1.0))
    light = spawner.spawn_actor_from_class(unreal.PointLight, unreal.Vector(120, -160, 260),
                                           unreal.Rotator(0, 0, 0))
    light.set_actor_label('DNFArtLight_%s' % cfg['id'])
    light.get_component_by_class(unreal.PointLightComponent).set_editor_property('intensity', 1200.0)
    light.get_component_by_class(unreal.PointLightComponent).set_editor_property('light_color', unreal.Color(255, 247, 235, 255))
    cap.texture_target = rt
    cap.capture_source = unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR
    cap.fov_angle = 35.0

    SHOT_DIR.mkdir(parents=True, exist_ok=True)

    def aim(view_dir):
        eye = unreal.Vector(center.x, center.y, center.z + height * 0.03) + view_dir * (radius * 3.4)
        cap_actor.set_actor_location(eye, False, False)
        rot = unreal.MathLibrary.find_look_at_rotation(
            eye, unreal.Vector(center.x, center.y, center.z + height * 0.03))
        cap_actor.set_actor_rotation(rot, False)

    views = {'side': unreal.Vector(0, 1, 0), 'front': unreal.Vector(1, 0, 0), 'back': unreal.Vector(-1, 0, 0)}

    def shoot(name):
        cap.capture_scene()
        fname = name + '.png'
        unreal.RenderingLibrary.export_render_target(world, rt, str(SHOT_DIR), fname)
        p = SHOT_DIR / fname
        if p.exists() and p.stat().st_size > 0:
            out['shots'].append(str(p))
        else:
            out['notes'].append('export missing: %s' % fname)

    # bind pose, three views
    comp.set_animation(None)
    for view, dir_vec in views.items():
        aim(dir_vec)
        shoot('%s_%s_bindpose' % (cfg['id'], view))

    # action freeze frames (side)
    for action in cfg['actions']:
        seq = unreal.load_asset('%s/%s_%s_Anim' % (cfg['dest'], cfg['id'], action))
        if not seq:
            out['notes'].append('seq missing: %s' % action)
            continue
        comp.set_animation(seq)
        try:
            inst = comp.get_anim_instance()
            if inst and hasattr(inst, 'set_position'):
                inst.set_position(min(cfg['action_peaks'].get(action, 0.3),
                                      max(0.0, float(seq.get_play_length()) - 0.05)), False)
        except Exception as e:
            out['notes'].append('set_position failed %s: %s' % (action, str(e)[:80]))
        try:
            comp.refresh_bone_transforms()
        except Exception:
            pass
        aim(views['side'])
        shoot('%s_%s' % (cfg['id'], action))

    unreal.EditorLevelLibrary.destroy_actor(light)
    unreal.EditorLevelLibrary.destroy_actor(actor)
    unreal.EditorLevelLibrary.destroy_actor(cap_actor)
    return out


def main():
    report = {'success': False, 'characters': {}}
    try:
        for cfg in CHARACTERS:
            report['characters'][cfg['id']] = capture_previews(cfg)
        total = sum(len(v['shots']) for v in report['characters'].values())
        report['success'] = total >= 6
        unreal.log_warning('[DNFArt] previews captured: %d' % total)
    except Exception:
        import traceback
        report['error'] = traceback.format_exc()
    REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')


main()
