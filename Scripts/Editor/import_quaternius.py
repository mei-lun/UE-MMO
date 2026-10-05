"""Import the two fetched Quaternius Universal Animation Library packs into
UE under the isolated /Game/ThirdParty/Quaternius namespace and enumerate
every imported animation.

Inputs (already produced by Scripts/Editor/fetch_quaternius.py):
  SourceAssets/Downloads/universal-animation-library.zip       (UAL1, CC0)
  SourceAssets/Downloads/universal-animation-library-2.zip     (UAL2, CC0)

Pipeline (single editor run, mode=create):
  1. Recompute each archive SHA256 and check it against the sidecar
     SourceAssets/Downloads/<slug>.sha256 before touching the engine.
  2. Extract only the Unreal-Godot glTF file of each pack into Saved/.
  3. Import each glTF with an automated AssetImportTask into
     /Game/ThirdParty/Quaternius/UAL1 and .../UAL2 (no Mannequins path is
     touched; importer defaults apply).
  4. Enumerate imported UAnimSequence assets (name + package + play length)
     through the asset registry.
  5. Render pose previews for a small candidate set with a temporary
     SkeletalMeshActor driven at explicit instants through a SceneCapture2D
     (the M1-036 freeze-frame approach) so the horizontal-vs-rising motion
     judgement is backed by images, not names alone.
  6. Write Artifacts/Logs/quaternius-import-report.json.

mode=verify only reloads the enumeration (fresh process check).

Run inside the UE editor only:
  UnrealEditor-Cmd <uproject> -run=pythonscript -script=<this file>

ASCII only; safe to re-run (imports replace existing packages in place).
"""
import hashlib
import json
import os
import traceback
import zipfile
from pathlib import Path

import unreal

MODE = os.environ.get('UEMMO_QUATERNIUS_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
DOWNLOAD_DIR = ROOT / 'SourceAssets' / 'Downloads'
REPORT_DIR = ROOT / 'Artifacts' / 'Logs'
REPORT_PATH = REPORT_DIR / 'quaternius-import-report.json'
EXTRACT_DIR = ROOT / 'Saved' / 'QuaterniusImport'
PREVIEW_DIR = ROOT / 'Artifacts' / 'Tasks' / 'M3-032'

PACKS = [
    {
        'id': 'quaternius_animation_library_standard',
        'slug': 'universal-animation-library',
        'glb_member': 'Universal Animation Library[Standard]/Unreal-Godot/UAL1_Standard.glb',
        'destination': '/Game/ThirdParty/Quaternius/UAL1',
        'label': 'UAL1',
    },
    {
        'id': 'quaternius_animation_library_2_standard',
        'slug': 'universal-animation-library-2',
        'glb_member': 'Universal Animation Library 2[Standard]/Unreal-Godot/UAL2_Standard.glb',
        'destination': '/Game/ThirdParty/Quaternius/UAL2',
        'label': 'UAL2',
    },
]

# Animations rendered as pose previews (label -> (pack label, anim name)).
# These are the only free-tier clips near the launcher/aerial/hit gaps.
PREVIEW_CANDIDATES = [
    ('UAL1', 'Punch_Jab'),
    ('UAL1', 'Punch_Cross'),
    ('UAL2', 'Melee_Hook'),
    ('UAL2', 'Hit_Knockback'),
]
PREVIEW_TIMES = [0.30, 0.50, 0.70]  # fraction of the clip length


def write_report(payload):
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(payload, indent=1, default=str), encoding='utf-8')
    unreal.log('UEMMO_M3_032 wrote ' + str(REPORT_PATH))


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_archives(report):
    """Recompute the archive hashes against the fetch-time sidecars."""
    for pack in PACKS:
        archive = DOWNLOAD_DIR / (pack['slug'] + '.zip')
        sidecar = DOWNLOAD_DIR / (pack['slug'] + '.sha256')
        entry = {
            'pack_id': pack['id'],
            'archive': str(archive),
            'exists': archive.exists() and sidecar.exists(),
        }
        if entry['exists']:
            actual = sha256_of(archive)
            expected = sidecar.read_text(encoding='ascii').strip().split(' ')[0].lower()
            entry['sha256_actual'] = actual
            entry['sha256_expected'] = expected
            entry['sha256_match'] = actual == expected
            entry['bytes'] = archive.stat().st_size
        else:
            entry['sha256_match'] = False
        report['archives'].append(entry)
        if not entry['sha256_match']:
            raise RuntimeError(
                'archive verification failed for %s (see %s)' % (pack['id'], REPORT_PATH))
    unreal.log('UEMMO_M3_032 archives verified against fetch-time sidecars')


def extract_glbs(report):
    for pack in PACKS:
        archive = DOWNLOAD_DIR / (pack['slug'] + '.zip')
        target_dir = EXTRACT_DIR / pack['label']
        target_dir.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(str(archive)) as bundle:
            bundle.extract(pack['glb_member'], str(EXTRACT_DIR))
        glb_path = EXTRACT_DIR / pack['glb_member']
        if not glb_path.exists():
            raise RuntimeError('glb member missing after extraction: ' + pack['glb_member'])
        pack['glb_path'] = str(glb_path)
        report['extraction'].append({'pack_id': pack['id'], 'glb': str(glb_path)})
    unreal.log('UEMMO_M3_032 glbs extracted under ' + str(EXTRACT_DIR))


def import_glbs(report):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    for pack in PACKS:
        task = unreal.AssetImportTask()
        task.set_editor_property('filename', pack['glb_path'])
        task.set_editor_property('destination_path', pack['destination'])
        task.set_editor_property('automated', True)
        task.set_editor_property('save', True)
        task.set_editor_property('replace_existing', True)
        tools.import_asset_tasks([task])
        root_name = pack['destination'].rsplit('/', 1)[-1]
        if not assets.does_directory_exist(pack['destination']):
            raise RuntimeError('import produced no directory: ' + pack['destination'])
        report['imports'].append({
            'pack_id': pack['id'],
            'destination': pack['destination'],
            'root_asset_name': root_name,
        })
        unreal.log('UEMMO_M3_032 imported %s into %s' % (pack['glb_path'], pack['destination']))


def enumerate_anims(report):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for pack in PACKS:
        assets = registry.get_assets_by_path(pack['destination'], recursive=True)
        animations = []
        other = []
        for asset in assets:
            asset_class = str(asset.asset_class_path.asset_name)
            name = str(asset.asset_name)
            package = str(asset.package_name)
            if asset_class == 'AnimSequence':
                loaded = unreal.load_asset(package)
                length = None
                if loaded is not None:
                    try:
                        length = float(loaded.get_editor_property('sequence_length'))
                    except Exception:
                        length = None
                animations.append({'name': name, 'package': package, 'length': length})
            else:
                other.append({'class': asset_class, 'name': name, 'package': package})
        animations.sort(key=lambda item: item['name'])
        report['packs'][pack['label']] = {
            'pack_id': pack['id'],
            'destination': pack['destination'],
            'animation_count': len(animations),
            'animations': animations,
            'other_assets': other,
        }
        unreal.log('UEMMO_M3_032 enumerated %d animations for %s' % (len(animations), pack['label']))


def _find_anim(pack_label, anim_name):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for pack in PACKS:
        if pack['label'] != pack_label:
            continue
        assets = registry.get_assets_by_path(pack['destination'], recursive=True)
        for asset in assets:
            if str(asset.asset_class_path.asset_name) == 'AnimSequence' and \
                    str(asset.asset_name).endswith(anim_name):
                return unreal.load_asset(str(asset.package_name))
    return None


def render_previews(report, on_done):
    """Freeze-frame previews for the candidate clips (M1-036 approach).

    A USkeletalMeshComponent only re-evaluates its single-node animation on
    a world tick, so set_position() between two python statements still
    renders the stale pose. The capture therefore runs as a slate post-tick
    state machine: one tick to switch the clip, one tick to apply the pose
    time, then capture+export on the third tick.
    """
    subsystems = unreal.UnrealEditorSubsystem
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # Load the training arena so the capture has a floor and reliable
    # lighting (the transient startup map has neither).
    arena = '/Game/UEMMO/Maps/L_TrainingArena'
    if unreal.EditorAssetLibrary.does_asset_exist(arena):
        unreal.EditorLoadingAndSavingUtils.load_map(arena)
    world = unreal.get_editor_subsystem(subsystems).get_editor_world()
    report['previews'] = {'world': str(world.get_path_name())}
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    mesh_path = None
    for asset in registry.get_assets_by_path('/Game/ThirdParty/Quaternius/UAL1', recursive=True):
        if str(asset.asset_class_path.asset_name) == 'SkeletalMesh':
            mesh_path = str(asset.package_name)
            break
    if mesh_path is None:
        report['previews']['error'] = 'no imported Quaternius SkeletalMesh found'
        on_done()
        return
    mesh = unreal.load_asset(mesh_path)
    report['previews']['skeletal_mesh'] = mesh_path

    stage = actors.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0))
    if stage is None:
        report['previews']['error'] = 'SkeletalMeshActor could not be spawned'
        on_done()
        return
    stage.set_actor_rotation(unreal.Rotator(pitch=0.0, yaw=180.0, roll=0.0), False)
    component = stage.get_editor_property('skeletal_mesh_component')
    component.set_editor_property('skeletal_mesh_asset', mesh)
    component.set_editor_property('animation_mode', unreal.AnimationMode.ANIMATION_SINGLE_NODE)
    # In an editor world the component only evaluates animation when this
    # flag is on; without it every capture would freeze the T-pose.
    for args in ((True,), (True, True)):
        try:
            component.set_update_animation_in_editor(*args)
            break
        except Exception:
            continue
    camera = actors.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, -340, 100))
    if camera is None:
        report['previews']['error'] = 'SceneCapture2D could not be spawned'
        on_done()
        return
    camera.set_actor_rotation(unreal.Rotator(pitch=0.0, yaw=90.0, roll=0.0), False)
    capture = camera.get_editor_property('capture_component2d')
    capture.set_editor_property('capture_source', unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    # UE 5.8 renamed the helper to create_render_target2d.
    target = unreal.RenderingLibrary.create_render_target2d(world, 640, 640, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0.05, 0.05, 0.05, 1.0))
    capture.set_editor_property('texture_target', target)

    stage_origin, stage_extent = stage.get_actor_bounds(False)
    report['previews']['debug'] = {
        'stage_location': str(stage.get_actor_location()),
        'stage_bounds_origin': str(stage_origin),
        'stage_bounds_extent': str(stage_extent),
        'camera_location': str(camera.get_actor_location()),
        'camera_rotation': str(camera.get_actor_rotation()),
    }

    entries = []
    steps = []
    for pack_label, anim_name in PREVIEW_CANDIDATES:
        anim = _find_anim(pack_label, anim_name)
        if anim is None:
            entries.append({'anim': anim_name, 'pack': pack_label, 'error': 'animation not found'})
            continue
        steps.append(('prep', pack_label, anim_name, anim, 0.0))
        for fraction in PREVIEW_TIMES:
            steps.append(('pose', pack_label, anim_name, anim, fraction))
            steps.append(('capture', pack_label, anim_name, anim, fraction))
    unreal.log('UEMMO_M3_032 preview plan: %d ticks, %d frames' % (len(steps), len(PREVIEW_CANDIDATES) * len(PREVIEW_TIMES)))

    state = {'index': 0, 'handle': None}

    def finish():
        report['previews']['entries'] = entries
        unreal.log('UEMMO_M3_032 rendered %d preview frames' % len(
            [e for e in entries if e.get('png')]))
        on_done()

    def run_step(delta_time=None):
        index = state['index']
        state['index'] = index + 1
        if index >= len(steps):
            if state['handle'] is not None:
                unreal.unregister_slate_post_tick_callback(state['handle'])
                state['handle'] = None
            finish()
            return
        kind, pack_label, anim_name, anim, fraction = steps[index]
        if kind == 'prep':
            # UE 5.8 exposes USkeletalMeshComponent.set_animation() as a
            # function (there is no writable 'animation' python property).
            for args in ((anim,), (anim, False)):
                try:
                    component.set_animation(*args)
                    break
                except Exception:
                    continue
            try:
                component.set_play_rate(0.0)
            except Exception:
                pass
        elif kind == 'pose':
            length = float(anim.get_editor_property('sequence_length'))
            applied = None
            target_time = max(0.0, min(0.99, fraction)) * length
            for args in ((target_time, False), (target_time,)):
                try:
                    component.set_position(*args)
                    applied = 'set_position(%s) -> %0.3f' % (
                        ', '.join(str(a) for a in args), component.get_position())
                    break
                except Exception:
                    continue
            for entry in entries:
                if entry.get('anim') == anim_name and entry.get('pack') == pack_label \
                        and entry.get('fraction') == fraction:
                    applied = None
                    break
            else:
                entries.append({
                    'anim': anim_name,
                    'pack': pack_label,
                    'fraction': fraction,
                    'pose': applied,
                })
        else:  # capture
            capture.capture_scene()
            out_name = 'quat_%s_%s_t%d.png' % (
                pack_label.lower(), anim_name.lower(), int(round(fraction * 100)))
            unreal.RenderingLibrary.export_render_target(world, target, str(PREVIEW_DIR), out_name)
            for entry in reversed(entries):
                if entry.get('anim') == anim_name and entry.get('pack') == pack_label \
                        and entry.get('fraction') == fraction:
                    entry['png'] = out_name
                    break

    state['handle'] = unreal.register_slate_post_tick_callback(run_step)


def create_mode():
    report = {
        'mode': 'create',
        'archives': [],
        'extraction': [],
        'imports': [],
        'packs': {},
        'previews': {},
    }

    def finalize():
        report['success'] = True
        write_report(report)
        unreal.log('UEMMO_M3_032_IMPORT_SUCCESS packs=%d' % len(PACKS))
        # SceneCapture previews need a rendering editor world (the M1-036
        # freeze frames were captured the same way), so this script is meant
        # to run in the GUI editor via -ExecutePythonScript; close it
        # automatically when driven by a script.
        if os.environ.get('UEMMO_QUATERNIUS_AUTOCLOSE', '') == '1':
            unreal.SystemLibrary.quit_editor()

    try:
        verify_archives(report)
        extract_glbs(report)
        import_glbs(report)
        enumerate_anims(report)
        render_previews(report, finalize)
    except Exception as error:  # noqa: BLE001 - collected into the report
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc(limit=6)
        report['success'] = False
        write_report(report)
        raise


def verify_mode():
    report = {'mode': 'verify', 'packs': {}}
    enumerate_anims(report)
    missing = [label for label, info in report['packs'].items()
               if info['animation_count'] < 40]
    report['success'] = not missing
    write_report(report)
    if missing:
        raise RuntimeError('animation enumeration below 40 for packs: %s' % ', '.join(missing))
    unreal.log('UEMMO_M3_032_VERIFY_SUCCESS')


if MODE == 'create':
    create_mode()
elif MODE == 'verify':
    verify_mode()
else:
    raise RuntimeError('Unknown UEMMO_QUATERNIUS_MODE: ' + MODE)
