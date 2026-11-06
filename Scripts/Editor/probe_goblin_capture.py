"""M8-019A diagnostic: spawn the goblin in the training arena under the
editor world and capture it with SceneCapture2D (bypasses the -game view
path; if the picture renders here but not in-game, the difference is in the
game view, not the actor)."""
import json
import math
import traceback
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-019A'
REPORT = {'success': False, 'errors': [], 'previews': []}


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.load_level('/Game/UEMMO/Maps/L_TrainingArena'):
        raise RuntimeError('cannot load training arena')
    world = unreal.EditorLevelLibrary.get_editor_world()

    # clean previous probes
    for a in list(actors.get_all_level_actors()):
        label = a.get_actor_label()
        if label.startswith('M8_019A_Probe'):
            actors.destroy_actor(a)

    spawn_loc = unreal.Vector(x=-87.0, y=181.0, z=100.0)
    goblin_cls = unreal.load_class(None, '/Script/UEMMO.GoblinThrowerEnemy')
    if goblin_cls is None:
        raise RuntimeError('GoblinThrowerEnemy class not found')
    goblin = actors.spawn_actor_from_class(goblin_cls, spawn_loc,
        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    goblin.set_actor_label('M8_019A_ProbeGoblin')
    # BeginPlay runs on spawn; the visual activates in the editor world too.

    ca = actors.spawn_actor_from_class(unreal.SceneCapture2D,
        unreal.Vector(x=-87.0, y=181.0 + 1200.0 * math.cos(math.radians(18.0)),
                      z=100.0 + 1200.0 * math.sin(math.radians(18.0))),
        unreal.Rotator(pitch=-18.0, yaw=-90.0, roll=0.0))
    ca.set_actor_label('M8_019A_ProbeCapture')
    cap = ca.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(world, 1280, 720,
        unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.06, 0.07, 0.1, 1.0))
    cap.set_editor_property('texture_target', rt)
    cap.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    cap.set_editor_property('capture_every_frame', True)
    cap.set_editor_property('fov_angle', 60.0)

    state = {'frame': 0}
    dest = ART / 'probe'
    dest.mkdir(parents=True, exist_ok=True)

    def tick(_dt):
        try:
            state['frame'] += 1
            if state['frame'] < 180:
                return
            if state['frame'] % 90 == 0 and len(REPORT['previews']) < 2:
                name = 'probe_goblin_%d.png' % len(REPORT['previews'])
                unreal.RenderingLibrary.export_render_target(world, rt, str(dest), name)
                out = dest / name
                if out.is_file() and out.stat().st_size > 1024:
                    REPORT['previews'].append(str(out))
                else:
                    raise RuntimeError('capture missing: %s' % out)
                if len(REPORT['previews']) == 2:
                    REPORT['success'] = True
                    save_report()
                    cap.set_editor_property('capture_every_frame', False)
                    actors.destroy_actor(ca)
                    unreal.RenderingLibrary.release_render_target2d(rt)
                    unreal.unregister_slate_post_tick_callback(handle)
                    unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        except Exception:
            REPORT['errors'].append(traceback.format_exc())
            save_report()
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    handle = unreal.register_slate_post_tick_callback(tick)
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    globals()['_probe_keepalive'] = (ca, cap, rt, handle)


def save_report():
    (ART / 'probe-report.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')


try:
    main()
except Exception:
    REPORT['errors'].append(traceback.format_exc())
    save_report()
    raise
