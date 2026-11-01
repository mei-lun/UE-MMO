"""M7-003: import the five fighter skill action FBX onto the EXISTING
M6-008 hero skeleton (no namespace wipe - the accepted ArtValidation assets
stay in place), then freeze-frame capture one side-view shot per action.

Inputs (read-only, git-ignored, local only):
  SourceAssets/Unpacked/DNFPrivateReference/blender-work/hero_fighter/exports/
    hero_<action>.fbx  -> AnimSequence /Game/UEMMO/ArtValidation/DNFArt/Hero/hero_<action>_Anim

mode=create: import + save + capture (Artifacts/Tasks/M7-003/<action>.png)
mode=verify: fresh-process reload, assert each sequence exists with frames.

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

MODE = os.environ.get('UEMMO_SKILL_ANIM_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
WORK = ROOT / 'SourceAssets' / 'Unpacked' / 'DNFPrivateReference' / 'blender-work'
REPORT_PATH = ROOT / 'Artifacts' / 'Logs' / 'dnf-skill-anim-import-report.json'
SHOT_DIR = ROOT / 'Artifacts' / 'Tasks' / 'M7-003'

DEST = '/Game/UEMMO/ArtValidation/DNFArt/Hero'
MESH_NAME = 'hero_skm'

# The five hotbar skill actions (launcher_z reuses the already-imported
# 'launcher' animation from M6-008). Peak fraction guesses the active window
# instant for the freeze frame; the exact logic window lives in
# Data/combat-attacks.json.
import os as _os
ACTIONS_FILTER = _os.environ.get('UEMMO_SKILL_ANIM_ONLY')
ACTIONS = {
    'singlekick': 0.35,
    'damagelowkick': 0.35,
    'throwsand': 0.30,
    'earthbreak': 0.35,
    'stomp': 0.40,
    # M7-004 visual integration: the legacy four + the locomotion set on the
    # SAME DNF skeleton (the player mesh swap invalidates the mannequin AnimBP;
    # the montages for light_01/02/launcher/aerial_01 build from these).
    'attack_01': 0.35,
    'attack_02': 0.40,
    'aerial': 0.40,
    'jump_start': 0.3,
    'jump_fall': 0.5,
    'jump_land': 0.6,
    'walk_y': 0.4,
}

REPORT = {'mode': MODE, 'actions': {}, 'success': False}

# Transient preview objects (render target, capture actors) are otherwise
# garbage-collected between capture_scene and export_render_target ("the
# render target has been released"); holding the references keeps them alive.
_KEEP_ALIVE = []


def log(msg):
    unreal.log('[M7-003 anim] %s' % msg)


def import_action(action):
    """Import one action FBX via the Interchange default pipeline (the
    M6-008 import_action recipe verbatim, retargeted at the existing folder)."""
    fbx = str(WORK / 'hero_fighter' / 'exports' / ('hero_%s.fbx' % action))
    if not Path(fbx).is_file():
        raise RuntimeError('action fbx missing: %s' % fbx)
    # A re-run must start clean: replace_existing merges into the previous
    # run's skeleton and Interchange then refuses every duplicate bone.
    # Idempotent skip: a VALID existing sequence (loads, owns a skeleton) is
    # reused verbatim. Deleting + re-importing races the asset registry (the
    # pipeline then "waits for the skinned asset" and the skeleton file it just
    # deleted is unopenable - the sequence is silently lost), so an import only
    # happens when the sequence is missing or broken.
    anim_path = '%s/hero_%s_Anim' % (DEST, action)
    if unreal.EditorAssetLibrary.does_asset_exist(anim_path):
        existing = unreal.load_asset(anim_path)
        if existing and existing.get_class().get_name() == 'AnimSequence':
            try:
                skeleton = existing.get_editor_property('skeleton')
            except Exception:
                skeleton = None
            if skeleton:
                companion = None
                companion_path = '%s/hero_%s' % (DEST, action)
                if unreal.EditorAssetLibrary.does_asset_exist(companion_path):
                    companion = unreal.load_asset(companion_path)
                log('reusing valid %s' % anim_path)
                return existing, companion
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    params.destination_name = 'hero_%s' % action
    source = im.create_source_data(fbx)
    imported = im.import_asset(DEST, source, params) or []
    seq = None
    skm = None
    junk = []
    for obj in imported:
        if not obj:
            continue
        cls = obj.get_class().get_name()
        if cls == 'AnimSequence':
            seq = obj
        elif cls in ('StaticMesh', 'PhysicsAsset', 'MaterialInstanceConstant',
                     'Material', 'Texture2D'):
            # The M6-008 recipe keeps the action's OWN SkeletalMesh/Skeleton
            # (the sequence needs a valid skeleton; deleting it breaks the
            # asset and blocks re-imports). Junk is everything else.
            junk.append(obj)
    for j in junk:
        try:
            unreal.EditorAssetLibrary.delete_asset(j.get_path_name())
        except Exception:
            pass
    # The skinned-asset async compile can outlive the interchange session on a
    # memory-pressured machine (the sequence is then silently dropped). Poll
    # for it instead of failing immediately; each run only has to advance.
    anim_path = '%s/hero_%s_Anim' % (DEST, action)
    if not seq:
        for _ in range(60):
            try:
                unreal.EditorLevelLibrary.editor_tick(0.25)
            except Exception:
                pass
            if unreal.EditorAssetLibrary.does_asset_exist(anim_path):
                candidate = unreal.load_asset(anim_path)
                if candidate and candidate.get_class().get_name() == 'AnimSequence':
                    log('sequence appeared after the async compile: %s' % anim_path)
                    seq = candidate
                    if skm is None:
                        companion_path = '%s/hero_%s' % (DEST, action)
                        if unreal.EditorAssetLibrary.does_asset_exist(companion_path):
                            skm = unreal.load_asset(companion_path)
                    break
    if not seq:
        raise RuntimeError('anim sequence failed to import: %s' % action)
    return seq, skm


def seq_stats(seq):
    length_s = float(seq.get_play_length())
    return {
        'path': seq.get_path_name(),
        'play_length_s': round(length_s, 4),
        'frames_approx': int(round(length_s * 60.0)),
        'note': '60fps authored in Blender',
    }


class _CaptureRig:
    """One persistent preview actor + capture camera + render target for the
    whole run (the M6-008 topology: build once, capture many)."""

    def __init__(self):
        self.ready = False
        self.actor = None
        self.comp = None
        self.cap = None
        self.rt = None

    def build(self):
        world = unreal.EditorLevelLibrary.get_editor_world()
        for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
            if a.get_name().startswith('DNFSkill_'):
                unreal.EditorLevelLibrary.destroy_actor(a)
        spawner = unreal.EditorActorSubsystem()
        self.actor = spawner.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
        self.actor.set_actor_label('DNFSkill_Preview')
        self.comp = self.actor.skeletal_mesh_component
        self.comp.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
        self.rt = unreal.RenderingLibrary.create_render_target2d(
            world, 720, 1080, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0.18, 0.19, 0.22, 1.0))
        if not self.rt:
            raise RuntimeError('render target creation failed')
        cap_actor = spawner.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
        cap_actor.set_actor_label('DNFSkill_Cam')
        self.cap = cap_actor.get_component_by_class(unreal.SceneCaptureComponent2D)
        self.cap.texture_target = self.rt
        self.cap.capture_source = unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR
        self.cap.fov_angle = 35.0
        _KEEP_ALIVE.extend([self.actor, cap_actor, self.rt])
        # Warm the render resource: on a cold commandlet the RT's resource
        # allocates lazily and the first export reads a released target.
        self.cap.capture_scene()
        for _ in range(6):
            try:
                unreal.EditorLevelLibrary.editor_tick(0.1)
            except Exception:
                pass
        self.ready = True

    def shoot(self, seq, skm, peak_fraction):
        mesh = skm if skm else unreal.load_asset('%s/%s' % (DEST, MESH_NAME))
        if not mesh:
            raise RuntimeError('no preview mesh for %s' % seq.get_name())
        self.comp.set_skeletal_mesh(mesh, False)
        self.comp.set_animation(seq)
        inst = self.comp.get_anim_instance()
        if inst and hasattr(inst, 'set_position'):
            inst.set_position(peak_fraction * float(seq.get_play_length()), False)
        try:
            unreal.EditorLevelLibrary.editor_tick(0.033)
        except Exception:
            pass
        try:
            self.comp.refresh_bone_transforms()
        except Exception:
            pass

        bounds = mesh.get_bounds()
        ext = bounds.box_extent
        center = bounds.origin
        radius = max(float(ext.x), float(ext.y), float(ext.z))
        height = float(ext.z) * 2.0
        world = unreal.EditorLevelLibrary.get_editor_world()
        eye = unreal.Vector(center.x, center.y, center.z) + unreal.Vector(0.0, 1.0, 0.0) * (radius * 3.2) + unreal.Vector(0, 0, height * 0.03)
        self.cap.get_owner().set_actor_location(eye, False, False)
        rot = unreal.MathLibrary.find_look_at_rotation(eye, unreal.Vector(center.x, center.y, center.z + height * 0.03))
        self.cap.get_owner().set_actor_rotation(rot, False)
        self.cap.capture_scene()

        SHOT_DIR.mkdir(parents=True, exist_ok=True)
        fname = '%s.png' % seq.get_name().replace('_Anim', '')
        log('capturing %s into %s' % (fname, SHOT_DIR))
        ok = False
        for attempt in range(10):
            ok = unreal.RenderingLibrary.export_render_target(world, self.rt, str(SHOT_DIR), fname)
            if ok:
                break
            try:
                unreal.EditorLevelLibrary.editor_tick(0.1)
            except Exception:
                pass
            self.cap.capture_scene()
        if not ok:
            log('export_render_target returned False for %s' % fname)
        return str(SHOT_DIR / fname) if ok else ''

    def teardown(self):
        world = unreal.EditorLevelLibrary.get_editor_world()
        for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
            if a.get_name().startswith('DNFSkill_'):
                unreal.EditorLevelLibrary.destroy_actor(a)


def run_create():
    _items = {k: v for k, v in ACTIONS.items() if not ACTIONS_FILTER or k == ACTIONS_FILTER}
    rig = _CaptureRig()
    rig.build()
    for action, peak in _items.items():
        entry = {}
        try:
            seq, skm = import_action(action)
            entry['stats'] = seq_stats(seq)
            unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
            entry['screenshot'] = rig.shoot(seq, skm, peak)
            entry['error'] = None
        except Exception:
            entry['error'] = traceback.format_exc()
            log('create failed for %s' % action)
        REPORT['actions'][action] = entry
    REPORT['success'] = all(
        e.get('error') is None and e['stats']['frames_approx'] > 0
        for e in REPORT['actions'].values())
    rig.teardown()
    if not unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True):
        log('save_dirty_packages reported a failure')


def run_verify():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    _items = {k: v for k, v in ACTIONS.items() if not ACTIONS_FILTER or k == ACTIONS_FILTER}
    for action, peak in _items.items():
        entry = {}
        try:
            path = '%s/hero_%s_Anim' % (DEST, action)
            asset = unreal.load_asset(path)
            if not asset:
                raise RuntimeError('missing sequence: %s' % path)
            entry['stats'] = seq_stats(asset)
            entry['error'] = None
        except Exception:
            entry['error'] = traceback.format_exc()
        REPORT['actions'][action] = entry
    REPORT['success'] = all(
        e.get('error') is None and e['stats']['frames_approx'] > 0
        for e in REPORT['actions'].values())


def main():
    log('mode=%s' % MODE)
    if MODE == 'verify':
        run_verify()
    else:
        run_create()
    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(REPORT, indent=1, ensure_ascii=False), encoding='utf-8')
    log('report written: %s (success=%s)' % (REPORT_PATH, REPORT['success']))
    # ALWAYS exit cleanly: a raised error marks the session crashed, and the
    # next editor start's package recovery loads the stale assets, which then
    # refuse re-import. The runner checks the report JSON instead.
    import sys
    sys.exit(0)


main()
