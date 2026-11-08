"""M8-024: render the guild map through a fixed side-view SceneCapture and
export the frame (the honest visual check after the material completion)."""
from pathlib import Path

import unreal
unreal.log("PROBE_STARTED")

ROOT = Path(unreal.Paths.project_dir()).resolve()
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-024'
ART.mkdir(parents=True, exist_ok=True)
OUT_NAME = 'guild-materials-done.png'

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not levels.load_level('/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'):
    raise RuntimeError('cannot load the guild map')
world = unreal.EditorLevelLibrary.get_editor_world()

spawn = unreal.Vector(x=-1500.0, y=620.0, z=100.0)
dist = 1700.0
ca = actors.spawn_actor_from_class(
    unreal.SceneCapture2D,
    unreal.Vector(x=spawn.x, y=spawn.y + dist * 0.951, z=spawn.z + dist * 0.309),
    unreal.Rotator(pitch=-18.0, yaw=-90.0, roll=0.0))
ca.set_actor_label('M8_024_GuildProbe')
cap = ca.get_component_by_class(unreal.SceneCaptureComponent2D)
rt = unreal.RenderingLibrary.create_render_target2d(world, 1280, 720,
    unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.06, 0.07, 0.1, 1.0))
cap.set_editor_property('texture_target', rt)
cap.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
cap.set_editor_property('capture_every_frame', True)
cap.set_editor_property('fov_angle', 55.0)

state = {'frame': 0}


def tick(_dt):
    try:
        state['frame'] += 1
        if state['frame'] < 60:
            return
        unreal.RenderingLibrary.export_render_target(world, rt, str(ART), OUT_NAME)
        out = ART / OUT_NAME
        ok = out.is_file() and out.stat().st_size > 1024
        unreal.log('UEMMO M8-024: guild render %s (%s)' % ('saved' if ok else 'MISSING', out))
        cap.set_editor_property('capture_every_frame', False)
        actors.destroy_actor(ca)
        unreal.RenderingLibrary.release_render_target2d(rt)
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        unreal.SystemLibrary.quit_editor()
    except Exception:
        import traceback
        unreal.log_warning('[M8-024] probe failed: ' + traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)


handle = unreal.register_slate_post_tick_callback(tick)
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
