"""Generic side-view render probe (M8-024 pattern): load a map, capture one frame
from a fixed camera, export PNG, quit the editor.

Run via the FULL editor (-ExecutePythonScript), not the commandlet (slate tick needed).
Environment:
  UEMMO_PROBE_MAP   map asset path (required)
  UEMMO_PROBE_OUT   output PNG path (required, absolute or project-relative)
  UEMMO_PROBE_CAM   capture camera "x,y,z" (required)
  UEMMO_PROBE_ROT   camera rotation "pitch,yaw,roll" (default "-15,-90,0")
  UEMMO_PROBE_FOV   fov (default 55)
"""
import os
import time
import traceback
from pathlib import Path

import unreal

unreal.log('PROBE_STARTED')

MAP = os.environ.get('UEMMO_PROBE_MAP', '')
OUT = os.environ.get('UEMMO_PROBE_OUT', '')
CAM = os.environ.get('UEMMO_PROBE_CAM', '0,0,400')
ROT = os.environ.get('UEMMO_PROBE_ROT', '-15,-90,0')
FOV = float(os.environ.get('UEMMO_PROBE_FOV', '55'))
if not MAP or not OUT:
    raise RuntimeError('UEMMO_PROBE_MAP / UEMMO_PROBE_OUT required')

out_path = Path(OUT)
out_path.parent.mkdir(parents=True, exist_ok=True)

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not levels.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)
world = unreal.EditorLevelLibrary.get_editor_world()

cx, cy, cz = (float(v) for v in CAM.split(','))
pitch, yaw, roll = (float(v) for v in ROT.split(','))

ca = actors.spawn_actor_from_class(
    unreal.SceneCapture2D, unreal.Vector(x=cx, y=cy, z=cz),
    unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll))
ca.set_actor_label('UEMMO_MapProbe')
cap = ca.get_component_by_class(unreal.SceneCaptureComponent2D)
rt = unreal.RenderingLibrary.create_render_target2d(
    world, 1280, 720, unreal.TextureRenderTargetFormat.RTF_RGBA8,
    unreal.LinearColor(0.05, 0.05, 0.06, 1.0))
cap.set_editor_property('texture_target', rt)
cap.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
cap.set_editor_property('capture_every_frame', True)
cap.set_editor_property('fov_angle', FOV)

state = {'frame': 0, 'start': time.time()}
# New materials compile their shaders asynchronously (~35s on first use); a
# capture taken too early shows the default-material checker instead (known
# artifact, see the M8-041/DNFM gotchas). Warm up on real wall-clock time —
# slate tick dt is unreliable offscreen.
warm = float(os.environ.get('UEMMO_PROBE_WARM_SECONDS', '8'))


def tick(_dt):
    try:
        state['frame'] += 1
        if time.time() - state['start'] < warm or state['frame'] < 30:
            return
        unreal.RenderingLibrary.export_render_target(world, rt, str(out_path.parent), out_path.name)
        ok = out_path.is_file() and out_path.stat().st_size > 1024
        unreal.log('UEMMO_PROBE: %s (%s)' % ('saved' if ok else 'MISSING', out_path))
        cap.set_editor_property('capture_every_frame', False)
        actors.destroy_actor(ca)
        unreal.RenderingLibrary.release_render_target2d(rt)
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        unreal.SystemLibrary.quit_editor()
    except Exception:
        unreal.log_warning('[probe] failed: ' + traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)


handle = unreal.register_slate_post_tick_callback(tick)
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
