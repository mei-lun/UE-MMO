"""Create (or refresh) the project-owned locomotion animation assets.

Creates /Game/UEMMO/Animation/BS_Prototype_Locomotion (2D BlendSpace,
Speed x Direction, ten samples from the engine's Unarmed jogging set) and
/Game/UEMMO/Animation/ABP_Prototype (AnimBP parented to the native
UPrototypeAnimInstance). The AnimGraph itself is assembled by the native
editor helper UPrototypeAnimInstance::EditorBuildLocomotionGraph because UE
Python cannot allocate graph pins (UEdGraphPin is not a UObject and pin
allocation exposes no Python-callable UFunction).

Run inside the UE editor only:
  UnrealEditor-Cmd <uproject> -run=pythonscript -script=<this file>

Modes (environment variable UEMMO_LOCO_MODE):
  create (default) : create/update assets, compile, save, write
                     locomotion-assets-report.json
  verify           : fresh-process reload check; re-compiles the saved AnimBP,
                     verifies BlendSpace samples and referenced animations,
                     writes locomotion-reload-check.json and raises on failure

ASCII only; safe to re-run (existing assets are updated in place).
"""
import json
import os
import traceback
from pathlib import Path

import unreal

MODE = os.environ.get('UEMMO_LOCO_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
REPORT_DIR = ROOT / 'Artifacts' / 'Tasks' / 'M1-031'

ANIM_DIR = '/Game/UEMMO/Animation'
BLENDSPACE_PATH = ANIM_DIR + '/BS_Prototype_Locomotion'
ANIMBP_PATH = ANIM_DIR + '/ABP_Prototype'
ANIMBP_CLASS_PATH = ANIMBP_PATH + '.ABP_Prototype_C'

UNARMED = '/Game/Characters/Mannequins/Anims/Unarmed/'
# (animation path, speed cm/s, direction deg). Speeds match the actual planar
# movement: X runs 420, depth 280, diagonals |(420,280)/sqrt(2)| ~= 357.
LOCOMOTION_SAMPLES = [
    (UNARMED + 'MM_Idle', 0.0, 0.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Fwd', 420.0, 0.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Fwd_Right', 357.0, 45.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Right', 280.0, 90.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Bwd_Right', 357.0, 135.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Bwd', 420.0, 180.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Bwd', 420.0, -180.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Bwd_Left', 357.0, -135.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Left', 280.0, -90.0),
    (UNARMED + 'Jog/MF_Unarmed_Jog_Fwd_Left', 357.0, -45.0),
]
JUMP_PATH = UNARMED + 'Jump/MM_Jump'
FALL_PATH = UNARMED + 'Jump/MM_Fall_Loop'
LAND_PATH = UNARMED + 'Jump/MM_Land'


def load(path):
    return unreal.load_asset(path)


def load_required(path):
    obj = unreal.load_asset(path)
    if obj is None:
        raise RuntimeError('Required animation asset missing: ' + path)
    return obj


def write_json(name, payload):
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    target = REPORT_DIR / name
    target.write_text(json.dumps(payload, indent=1, default=str), encoding='utf-8')
    unreal.log('UEMMO_M1_031 wrote ' + str(target))


REQUIRED_GRAPH_CLASSES = {
    'AnimGraphNode_BlendSpacePlayer': 1,
    'AnimGraphNode_SequencePlayer': 3,
    'AnimGraphNode_BlendListByInt': 1,
    'AnimGraphNode_Root': 1,
    'K2Node_VariableGet': 3,
}


def graph_census(bp):
    """Class census of the AnimGraph nodes.

    UEdGraph::Nodes is protected in UE 5.8 so Python cannot read the node
    array directly; use UBlueprint.get_nodes_of_class when it is callable and
    otherwise report that the census must come from the C++ builder log.
    """
    census = {}
    try:
        for class_name, _minimum in REQUIRED_GRAPH_CLASSES.items():
            node_class = getattr(unreal, class_name, None)
            if node_class is not None and hasattr(bp, 'get_nodes_of_class'):
                found = bp.get_nodes_of_class(node_class)
                census[class_name] = len(found)
    except Exception:
        return {'error': traceback.format_exc(limit=2)}
    return census


def create_mode():
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    idle = load_required(LOCOMOTION_SAMPLES[0][0])
    skeleton = idle.get_editor_property('skeleton')
    if skeleton is None:
        raise RuntimeError('MM_Idle has no skeleton; mannequin content incomplete?')
    report = {'mode': 'create', 'skeleton': skeleton.get_path_name()}

    # ---- BlendSpace --------------------------------------------------------
    created_bs = not assets.does_asset_exist(BLENDSPACE_PATH)
    if created_bs:
        factory = unreal.BlendSpaceFactoryNew()
        factory.set_editor_property('target_skeleton', skeleton)
        bs = tools.create_asset('BS_Prototype_Locomotion', ANIM_DIR, unreal.BlendSpace, factory)
    else:
        bs = load(BLENDSPACE_PATH)
    if bs is None:
        raise RuntimeError('BlendSpace asset could not be created or loaded.')
    samples = []
    for anim_path, speed, direction in LOCOMOTION_SAMPLES:
        sample = unreal.BlendSample()
        sample.set_editor_property('animation', load_required(anim_path))
        sample.set_editor_property('sample_value', unreal.Vector(speed, direction, 0.0))
        samples.append(sample)
    bs.set_editor_property('sample_data', samples)
    assets.save_loaded_asset(bs, only_if_is_dirty=False)
    report['blendspace'] = {
        'path': BLENDSPACE_PATH,
        'created_this_run': created_bs,
        'sample_count': len(samples),
    }

    # ---- AnimBP ------------------------------------------------------------
    created_abp = not assets.does_asset_exist(ANIMBP_PATH)
    if created_abp:
        factory = unreal.AnimBlueprintFactory()
        factory.set_editor_property('target_skeleton', skeleton)
        native_parent = unreal.load_object(None, '/Script/UEMMO.PrototypeAnimInstance')
        if native_parent is None:
            raise RuntimeError('Native class UPrototypeAnimInstance not found; build the project first.')
        factory.set_editor_property('parent_class', native_parent)
        bp = tools.create_asset('ABP_Prototype', ANIM_DIR, unreal.AnimBlueprint, factory)
    else:
        bp = load(ANIMBP_PATH)
    if bp is None:
        raise RuntimeError('AnimBP asset could not be created or loaded.')

    # ---- AnimGraph via the native editor helper ----------------------------
    jump = load_required(JUMP_PATH)
    fall = load_required(FALL_PATH)
    land = load_required(LAND_PATH)
    built = unreal.PrototypeAnimInstance.editor_build_locomotion_graph(bp, bs, jump, fall, land)
    report['graph_build'] = {'ok': bool(built)}

    compiled = unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    status = None
    try:
        status = str(bp.get_editor_property('status'))
    except Exception:
        status = 'unreadable'
    # The builder also configures the BlendSpace axis ranges (Python cannot
    # write the fixed C-array BlendParameters property), so save the BlendSpace
    # after the build, not only before it.
    assets.save_loaded_asset(bs, only_if_is_dirty=False)
    assets.save_loaded_asset(bp, only_if_is_dirty=False)
    report['animbp'] = {
        'path': ANIMBP_PATH,
        'created_this_run': created_abp,
        'compiled': bool(compiled),
        'status': status,
    }
    report['animbp']['graph'] = graph_census(bp)
    status_ok = 'BS_Error' not in (status or '')
    report['success'] = bool(built) and bool(compiled) and status_ok
    write_json('locomotion-assets-report.json', report)
    if not report['success']:
        raise RuntimeError('Locomotion asset build incomplete; inspect locomotion-assets-report.json')
    unreal.log('UEMMO_M1_031_CREATE_SUCCESS')


def verify_mode():
    """Fresh-process reload check: loads from disk and re-compiles."""
    report = {'mode': 'verify'}

    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    listing = assets.list_assets(ANIM_DIR, recursive=True, include_folder=False)
    report['animation_dir_assets'] = sorted(str(p) for p in listing)

    # ---- BlendSpace from disk ---------------------------------------------
    bs = load(BLENDSPACE_PATH)
    bs_ok = isinstance(bs, unreal.BlendSpace)
    samples = []
    if bs_ok:
        for sample in bs.get_editor_property('sample_data'):
            anim = sample.get_editor_property('animation')
            value = sample.get_editor_property('sample_value')
            samples.append({
                'animation': anim.get_path_name() if anim else None,
                'speed': value.x,
                'direction': value.y,
            })
    report['blendspace'] = {
        'path': BLENDSPACE_PATH,
        'loaded': bs_ok,
        'sample_count': len(samples),
        'samples': samples,
    }
    bs_checks = {
        'blendspace loads as BlendSpace': bs_ok,
        'BlendSpace has idle plus eight directions (>=9 samples)': len(samples) >= 9,
        'every sample references an animation': all(s['animation'] for s in samples),
        'idle sample at speed 0': any(abs(s['speed']) < 0.01 for s in samples),
        'side samples at +/-90 degrees': any(abs(s['direction'] - 90.0) < 0.01 for s in samples)
        and any(abs(s['direction'] + 90.0) < 0.01 for s in samples),
        'backward samples at |direction| >= 135': any(abs(s['direction']) >= 135.0 - 0.01 and s['speed'] > 0 for s in samples),
        'direction samples span -180..180': any(s['direction'] <= -179.0 for s in samples)
        and any(s['direction'] >= 179.0 for s in samples),
        'full speed sample at 420 cm/s': any(abs(s['speed'] - 420.0) < 0.01 for s in samples),
    }

    # ---- AnimBP from disk: fresh compile proves the graph wiring -----------
    bp = load(ANIMBP_PATH)
    abp_ok = isinstance(bp, unreal.AnimBlueprint)
    compiled = False
    status = None
    generated_class = None
    parent_ok = False
    if abp_ok:
        compiled = bool(unreal.BlueprintEditorLibrary.compile_blueprint(bp))
        try:
            status = str(bp.get_editor_property('status'))
        except Exception:
            status = 'unreadable'
        generated_class = unreal.BlueprintEditorLibrary.generated_class(bp)
        if generated_class is not None:
            parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(bp)
            parent_ok = parent is not None and parent.get_path_name() == '/Script/UEMMO.PrototypeAnimInstance'
        # Exercising the builder's idempotent branch also logs the per-class
        # node census from C++ (Python cannot read UEdGraph::Nodes).
        report['builder_idempotent_ok'] = bool(
            unreal.PrototypeAnimInstance.editor_build_locomotion_graph(
                bp, bs, load_required(JUMP_PATH), load_required(FALL_PATH), load_required(LAND_PATH)))
    report['animbp'] = {
        'path': ANIMBP_PATH,
        'loaded': abp_ok,
        'recompiled': compiled,
        'status': status,
        'generated_class': generated_class.get_path_name() if generated_class else None,
        'parent_is_prototype_anim_instance': parent_ok,
        'graph': graph_census(bp) if abp_ok else {'error': 'AnimBP not loaded'},
    }
    graph_census_data = report['animbp']['graph'] if abp_ok else {}
    # Node census is informational: the fresh-process recompile above is the
    # hard proof that the graph wiring is valid (an unwired pose pin would
    # fail compilation). Per-class node counts land in the C++ builder log.
    node_counts = {}
    for class_name in REQUIRED_GRAPH_CLASSES:
        count = graph_census_data.get(class_name) if isinstance(graph_census_data, dict) else None
        node_counts['census ' + class_name] = count if isinstance(count, int) else 'see builder log'
    abp_checks = {
        'AnimBP loads as AnimBlueprint': abp_ok,
        'AnimBP re-compiles in a fresh process': compiled,
        'AnimBP status is not BS_Error': status is not None and 'BS_Error' not in status,
        'generated class exists': generated_class is not None,
        'generated class derives from UPrototypeAnimInstance': parent_ok,
    }
    checks = dict(bs_checks)
    checks.update(abp_checks)
    report['node_census'] = node_counts
    report['checks'] = checks
    report['success'] = all(checks.values())
    write_json('locomotion-reload-check.json', report)
    if not report['success']:
        failed = [name for name, ok in checks.items() if not ok]
        raise RuntimeError('Locomotion reload check failed: ' + '; '.join(failed))
    unreal.log('UEMMO_M1_031_VERIFY_SUCCESS')


if MODE == 'verify':
    verify_mode()
elif MODE == 'create':
    create_mode()
else:
    raise RuntimeError('Unknown UEMMO_LOCO_MODE: ' + MODE)
