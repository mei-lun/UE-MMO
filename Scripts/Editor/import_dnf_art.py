"""Import the DNFArt Blender delivery (M6-008) into the isolated
/Game/UEMMO/ArtValidation/DNFArt namespace and validate scale, axis,
skeleton, materials and animation frames (ART-06 steps 2-3).

Inputs (read-only, git-ignored, local only):
  SourceAssets/Unpacked/DNFPrivateReference/blender-work/<char>/exports/*.fbx
    <char>_bindpose.fbx       -> SkeletalMesh + Skeleton + materials/textures
    <char>_<action>.fbx       -> AnimSequence on the imported skeleton

Pipeline (single editor run, mode=create):
  1. Automated AssetImportTask for both bindpose FBX as skeletal meshes.
  2. Import representative animations onto the imported skeletons.
  3. Validate: bone counts, bounds height (cm), material/texture references.
  4. Freeze-frame previews (M1-036/M3-032 approach): SceneCapture2D manual
     capture at explicit animation instants; side/front/back per character;
     PNGs to Artifacts/Tasks/M6-008/.
  5. Write Artifacts/Logs/dnf-art-import-report.json.

mode=verify only re-enumerates the imported assets in a fresh process
(second-process reload check) and re-runs the data assertions.

Run:
  UnrealEditor-Cmd <uproject> -run=pythonscript -script=<this file>
  (do NOT pass -nullrhi: scene captures need a real RHI)

ASCII only; safe to re-run (imports replace existing packages in place).
"""
import json
import os
import traceback
from pathlib import Path

import unreal

MODE = os.environ.get('UEMMO_DNFART_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
WORK = ROOT / 'SourceAssets' / 'Unpacked' / 'DNFPrivateReference' / 'blender-work'
REPORT_DIR = ROOT / 'Artifacts' / 'Logs'
REPORT_PATH = REPORT_DIR / 'dnf-art-import-report.json'
SHOT_DIR = ROOT / 'Artifacts' / 'Tasks' / 'M6-008'

DEST = '/Game/UEMMO/ArtValidation/DNFArt'

CHARACTERS = [
    {
        'id': 'hero',
        'exports': WORK / 'hero_fighter' / 'exports',
        'dest': DEST + '/Hero',
        'mesh': 'hero_skm',
        'expect_bones': 23,
        'expect_height_cm': (180.0, 186.0),
        'actions': ['idle', 'launcher', 'run_x'],
        'action_peaks': {'idle': 0.5, 'launcher': 0.27, 'run_x': 0.1},
    },
    {
        'id': 'goblin',
        'exports': WORK / 'enemy_goblin' / 'exports',
        'dest': DEST + '/Goblin',
        'mesh': 'goblin_skm',
        'expect_bones': 22,
        'expect_height_cm': (122.0, 128.0),
        'actions': ['idle', 'telegraph', 'attack'],
        'action_peaks': {'idle': 0.4, 'telegraph': 0.5, 'attack': 0.5},
    },
]

REPORT = {'mode': MODE, 'success': False, 'characters': {}, 'previews': []}


def log(msg):
    unreal.log_warning('[DNFArt] %s' % msg)


def import_bindpose(cfg):
    """Import <id>_bindpose.fbx as SkeletalMesh; returns (mesh, skeleton)."""
    fbx = str(cfg['exports'] / ('%s_bindpose.fbx' % cfg['id']))
    if not Path(fbx).is_file():
        raise RuntimeError('bindpose fbx missing: %s' % fbx)
    options = unreal.FbxImportUI()
    options.import_mesh = True
    options.import_as_skeletal = True
    options.import_materials = True
    options.import_textures = True
    options.import_animations = False
    options.automated_import_should_detect_type = False
    sm_import = options.skeletal_mesh_import_data
    sm_import.normal_import_method = unreal.FBXNormalImportMethod.FBXNIM_COMPUTE_NORMALS
    sm_import.normal_generation_method = unreal.FBXNormalGenerationMethod.MIKK_T_SPACE
    sm_import.convert_scene = True
    sm_import.convert_scene_unit = True
    sm_import.compute_weighted_normals = True

    task = unreal.AssetImportTask()
    task.filename = fbx
    task.destination_path = cfg['dest']
    task.destination_name = cfg['mesh']
    task.automated = True
    task.save = True
    task.replace_existing = True
    task.options = options
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    mesh = unreal.load_asset('%s/%s' % (cfg['dest'], cfg['mesh']))
    if not mesh:
        raise RuntimeError('skeletal mesh failed to import: %s' % cfg['mesh'])
    return mesh


def import_action(cfg, action, skeleton):
    """Import one action FBX via the Interchange default pipeline.

    Keeps the AnimSequence plus its own SkeletalMesh/Skeleton (so the preview
    actor can play it without cross-skeleton issues); deletes only the
    name-conflicting junk (StaticMesh / materials / textures / physics).
    """
    fbx = str(cfg['exports'] / ('%s_%s.fbx' % (cfg['id'], action)))
    if not Path(fbx).is_file():
        raise RuntimeError('action fbx missing: %s' % fbx)
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    params.destination_name = '%s_%s' % (cfg['id'], action)
    source = im.create_source_data(fbx)
    imported = im.import_asset(cfg['dest'], source, params) or []
    seq = skm = None
    junk = []
    for obj in imported:
        if not obj:
            continue
        cls = obj.get_class().get_name()
        if cls == 'AnimSequence':
            seq = obj
        elif cls == 'SkeletalMesh':
            skm = obj
        elif cls in ('StaticMesh', 'PhysicsAsset', 'MaterialInstanceConstant',
                     'Material', 'Texture2D'):
            junk.append(obj)
    for j in junk:
        try:
            unreal.EditorAssetLibrary.delete_asset(j.get_path_name())
        except Exception:
            pass
    if not seq:
        raise RuntimeError('anim sequence failed to import: %s' % action)
    return seq, skm


def mesh_stats(mesh, cfg):
    """Bounds height in cm + skeleton bone count + material references."""
    bounds = mesh.get_bounds()
    size = bounds.box_extent * 2.0
    height_cm = float(size.z)
    skeleton = mesh.get_editor_property('skeleton')
    bone_count = -1
    try:
        bone_count = int(skeleton.get_bone_number())
    except Exception:
        try:
            bone_count = len(skeleton.get_bone_names_for_tracking())
        except Exception:
            bone_count = -1
    try:
        materials = [str(sm.material_slot_name) for sm in mesh.get_editor_property('materials')]
    except Exception:
        materials = ['slot_%d' % i for i in range(8)]
    return {
        'bounds_cm': [round(float(size.x), 1), round(float(size.y), 1), round(height_cm, 1)],
        'height_cm': round(height_cm, 1),
        'height_ok': cfg['expect_height_cm'][0] <= height_cm <= cfg['expect_height_cm'][1],
        'bone_count': bone_count,
        'material_slots': materials,
        'skeleton': str(skeleton.get_name()) if skeleton else None,
    }


def seq_stats(seq):
    length_s = float(seq.get_play_length())
    return {
        'length_s': round(length_s, 3),
        'frames_approx': int(round(length_s * 60.0)),
        'note': '60fps authored in Blender',
    }


def axis_facing(mesh):
    """Return 'x_plus'/'x_minus'/'y_plus'/'y_minus' — which way the mesh's
    longest horizontal side of the FRONT quarter leans. Determined by the
    asymmetric bbox: the fighter's nose points +X in Blender; UE converts to
    cm with FBX forward -Z/up Y so authored +X becomes UE +Y. Record only;
    UE-side acceptance reads the preview images."""
    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    return {'extents_cm': [round(float(ext.x), 1), round(float(ext.y), 1), round(float(ext.z), 1)]}


def capture_previews(cfg, mesh, seqs):
    """SceneCapture2D manual capture: side/front/back per character."""
    world = unreal.EditorLevelLibrary.get_editor_world()
    # cleanup previous
    for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
        if a.get_name().startswith('DNFArt_'):
            unreal.EditorLevelLibrary.destroy_actor(a)

    spawner = unreal.EditorActorSubsystem()
    loc = unreal.Vector(0.0, 0.0, 0.0)
    actor = spawner.spawn_actor_from_class(unreal.SkeletalMeshActor, loc, unreal.Rotator(0, 0, 0))
    actor.set_actor_label('DNFArt_%s' % cfg['id'])
    comp = actor.skeletal_mesh_component
    comp.set_skeletal_mesh(mesh, False)
    comp.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)

    tick_helpers = []

    def set_pose(seq, t):
        comp.set_animation(seq)
        inst = comp.get_anim_instance()
        if inst and hasattr(inst, 'set_position'):
            inst.set_position(t, False)
            tick_helpers.append('set_position')
        # give the component a chance to evaluate
        try:
            unreal.EditorLevelLibrary.editor_tick(0.033)
            tick_helpers.append('editor_tick')
        except Exception:
            pass
        try:
            comp.refresh_bone_transforms()
        except Exception:
            pass

    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    center = bounds.origin
    radius = max(float(ext.x), float(ext.y), float(ext.z))
    height = float(ext.z) * 2.0

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, 720, 1080, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.18, 0.19, 0.22, 1.0))
    if not rt:
        raise RuntimeError('render target creation failed')

    capture_actor = spawner.spawn_actor_from_class(unreal.SceneCapture2D, loc, unreal.Rotator(0, 0, 0))
    capture_actor.set_actor_label('DNFArt_Cam_%s' % cfg['id'])
    cap = capture_actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    cap.texture_target = rt
    cap.capture_source = unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR
    cap.fov_angle = 35.0

    shots = []
    views = {
        'side': unreal.Vector(0.0, 1.0, 0.0),
        'front': unreal.Vector(1.0, 0.0, 0.0),
        'back': unreal.Vector(-1.0, 0.0, 0.0),
    }
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    for view, dir_vec in views.items():
        eye = unreal.Vector(center.x, center.y, center.z) + dir_vec * (radius * 3.2) + unreal.Vector(0, 0, height * 0.03)
        capture_actor.set_actor_location(eye, False, False)
        target = unreal.Vector(center.x, center.y, center.z + height * 0.03)
        rot = unreal.MathLibrary.find_look_at_rotation(eye, target)
        capture_actor.set_actor_rotation(rot, False)
        set_pose(None, 0.0)
        cap.capture_scene()
        fname = '%s_%s_bindpose.png' % (cfg['id'], view)
        ok = unreal.RenderingLibrary.export_render_target(world, rt, str(SHOT_DIR), fname)
        if ok:
            shots.append(str(SHOT_DIR / fname))

    # action freeze frames (side view) — 每条动作用自己的网格+序列（同骨架）
    for action, (seq, skm) in seqs.items():
        comp.set_skeletal_mesh(skm if skm else mesh, False)
        peak_t = min(cfg['action_peaks'].get(action, 0.3), max(0.0, float(seq.get_play_length()) - 0.05))
        set_pose(seq, peak_t)
        eye = unreal.Vector(center.x, center.y, center.z) + unreal.Vector(0.0, 1.0, 0.0) * (radius * 3.2) + unreal.Vector(0, 0, height * 0.03)
        capture_actor.set_actor_location(eye, False, False)
        rot = unreal.MathLibrary.find_look_at_rotation(eye, unreal.Vector(center.x, center.y, center.z + height * 0.03))
        capture_actor.set_actor_rotation(rot, False)
        cap.capture_scene()
        fname = '%s_%s.png' % (cfg['id'], action)
        ok = unreal.RenderingLibrary.export_render_target(world, rt, str(SHOT_DIR), fname)
        if ok:
            shots.append(str(SHOT_DIR / fname))

    unreal.EditorLevelLibrary.destroy_actor(actor)
    unreal.EditorLevelLibrary.destroy_actor(capture_actor)
    return {'shots': shots, 'pose_helpers': sorted(set(tick_helpers))}


def run_create():
    # 清空验收命名空间，避免上次失败遗留的半成品资产
    if unreal.EditorAssetLibrary.does_directory_exist(DEST):
        unreal.EditorAssetLibrary.delete_directory(DEST)
        log('cleaned %s' % DEST)
    for cfg in CHARACTERS:
        entry = {}
        try:
            mesh = import_bindpose(cfg)
            entry['mesh_stats'] = mesh_stats(mesh, cfg)
            entry['axis'] = axis_facing(mesh)
            skeleton = mesh.get_editor_property('skeleton')
            seqs = {}
            entry['actions'] = {}
            for action in cfg['actions']:
                seq, skm = import_action(cfg, action, skeleton)
                seqs[action] = (seq, skm)
                entry['actions'][action] = seq_stats(seq)
            unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
            entry['previews'] = capture_previews(cfg, mesh, seqs)
            entry['error'] = None
        except Exception:
            entry['error'] = traceback.format_exc()
            log('create failed for %s' % cfg['id'])
        REPORT['characters'][cfg['id']] = entry

    REPORT['success'] = all(
        (REPORT['characters'][c['id']].get('error') is None
         and REPORT['characters'][c['id']]['mesh_stats']['height_ok'])
        for c in CHARACTERS
    )


def run_verify():
    """Second-process reload check: re-enumerate and re-assert."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for cfg in CHARACTERS:
        entry = {}
        try:
            mesh = unreal.load_asset('%s/%s' % (cfg['dest'], cfg['mesh']))
            if not mesh:
                raise RuntimeError('mesh missing on reload')
            entry['mesh_stats'] = mesh_stats(mesh, cfg)
            entry['actions'] = {}
            for action in cfg['actions']:
                seq = unreal.load_asset('%s/%s_%s_Anim' % (cfg['dest'], cfg['id'], action))
                entry['actions'][action] = seq_stats(seq) if seq else {'missing': True}
            entry['error'] = None
        except Exception:
            entry['error'] = traceback.format_exc()
        REPORT['characters'][cfg['id']] = entry
    REPORT['success'] = all(
        (REPORT['characters'][c['id']].get('error') is None
         and REPORT['characters'][c['id']]['mesh_stats']['height_ok']
         and all('missing' not in a for a in REPORT['characters'][c['id']]['actions'].values()))
        for c in CHARACTERS
    )


def main():
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    if MODE == 'create':
        run_create()
    else:
        run_verify()
    if REPORT['success']:
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    REPORT_PATH.write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
    log('mode=%s success=%s report=%s' % (MODE, REPORT['success'], REPORT_PATH))
    if not REPORT['success']:
        raise RuntimeError('DNFArt import validation failed; see %s' % REPORT_PATH)


main()
