"""M8-019A diagnostic: render T_goblinthrower_page0 through the known-good
fighter material on a plane and capture it - isolates whether the UE-side
goblin page texture itself has pixels."""
import math
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

    fighter_mat = unreal.load_asset(
        '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Materials/M_DNFMFighterVisual.M_DNFMFighterVisual')
    goblin_tex = unreal.load_asset('/Game/UEMMO/DNFM/Monsters/Goblinthrower/T/T_goblinthrower_page0.T_goblinthrower_page0')
    if fighter_mat is None or goblin_tex is None:
        raise RuntimeError('fighter material or goblin texture missing')
    # Clone the known-good fighter material, retarget its PageTex node to the
    # goblin page and open the UVRect defaults to the full page (no MID needed
    # - the parameter defaults carry the probe config).
    probe_mat_path = '/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_019A_TexProbe'
    if unreal.EditorAssetLibrary.does_asset_exist(probe_mat_path):
        unreal.EditorAssetLibrary.delete_asset(probe_mat_path)
    probe_mat = unreal.EditorAssetLibrary.duplicate_asset(
        '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Materials/M_DNFMFighterVisual.M_DNFMFighterVisual',
        probe_mat_path)
    if probe_mat is None:
        raise RuntimeError('material duplication failed')
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(probe_mat):
        cls = expr.get_class().get_name()
        if cls == 'MaterialExpressionTextureSampleParameter2D':
            expr.set_editor_property('texture', goblin_tex)
        elif cls == 'MaterialExpressionVectorParameter':
            pname = expr.get_editor_property('parameter_name')
            if pname == 'UVRectMin':
                expr.set_editor_property('default_value', unreal.LinearColor(0.639, 0.000, 0.0, 0.0))
            elif pname == 'UVRectSize':
                # M8-019A: the goblin stay f0 cell (the values the entity MID
                # carries) - if the quad goes invisible with these, the small
                # UV rect itself is the render problem.
                expr.set_editor_property('default_value', unreal.LinearColor(0.026, 0.043, 0.0, 0.0))
    unreal.MaterialEditingLibrary.recompile_material(probe_mat)
    unreal.EditorAssetLibrary.save_loaded_asset(probe_mat)
    mid = probe_mat

    plane = actors.spawn_actor_from_class(unreal.StaticMeshActor,
        unreal.Vector(x=0.0, y=0.0, z=500.0), unreal.Rotator(pitch=0.0, yaw=90.0, roll=0.0))
    plane.set_actor_label('M8_019A_TexProbePlane')
    smc = plane.static_mesh_component
    smc.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Plane'))
    smc.set_material(0, mid)
    plane.set_actor_scale3d(unreal.Vector(x=20.48, y=20.48, z=1.0))

    ca = actors.spawn_actor_from_class(unreal.SceneCapture2D,
        unreal.Vector(x=0.0, y=0.0, z=3200.0), unreal.Rotator(pitch=-89.9, yaw=0.0, roll=0.0))
    ca.set_actor_label('M8_019A_TexProbeCapture')
    cap = ca.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(world, 1024, 1024,
        unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.05, 0.05, 0.08, 1.0))
    cap.set_editor_property('texture_target', rt)
    cap.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    cap.set_editor_property('capture_every_frame', True)
    cap.set_editor_property('fov_angle', 50.0)

    state = {'frame': 0}
    dest = ART / 'probe'
    dest.mkdir(parents=True, exist_ok=True)

    def tick(_dt):
        try:
            state['frame'] += 1
            if state['frame'] < 180:
                return
            if state['frame'] % 90 == 0 and len(REPORT['previews']) < 1:
                unreal.RenderingLibrary.export_render_target(world, rt, str(dest), 'texture_probe.png')
                out = dest / 'texture_probe.png'
                if out.is_file() and out.stat().st_size > 1024:
                    REPORT['previews'].append(str(out))
                    REPORT['success'] = True
                    save_report()
                    cap.set_editor_property('capture_every_frame', False)
                    actors.destroy_actor(ca)
                    actors.destroy_actor(plane)
                    unreal.RenderingLibrary.release_render_target2d(rt)
                    unreal.unregister_slate_post_tick_callback(handle)
                    unreal.EditorPythonScripting.set_keep_python_script_alive(False)
                else:
                    raise RuntimeError('capture missing')
        except Exception:
            import traceback
            REPORT['errors'].append(traceback.format_exc())
            save_report()
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.EditorPythonScripting.set_keep_python_script_alive(False)

    handle = unreal.register_slate_post_tick_callback(tick)
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    globals()['_probe_keepalive'] = (ca, cap, rt, handle, plane)


def save_report():
    (ART / 'texture-probe.json').write_text(
        str(REPORT), encoding='utf-8')


try:
    main()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
    save_report()
    raise
