"""Create and save UE-native foundation assets. Run inside the UE editor only."""
import json
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
REPORT = ROOT / 'Artifacts' / 'foundation-report.json'
LEVEL_PATH = '/Game/UEMMO/Maps/L_PrototypeArena'
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

def material(name, color):
    path = '/Game/UEMMO/Materials/' + name
    if assets.does_asset_exist(path):
        return unreal.load_asset(path)
    factory = unreal.MaterialFactoryNew()
    obj = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, '/Game/UEMMO/Materials', unreal.Material, factory)
    node = unreal.MaterialEditingLibrary.create_material_expression(obj, unreal.MaterialExpressionConstant3Vector)
    node.set_editor_property('constant', unreal.LinearColor(*color))
    unreal.MaterialEditingLibrary.connect_material_property(node, '', unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(obj)
    assets.save_loaded_asset(obj)
    return obj

def cube(label, location, scale, mat):
    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(*location))
    actor.set_actor_label(label)
    actor.set_actor_scale3d(unreal.Vector(*scale))
    component = actor.static_mesh_component
    component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube.Cube'))
    component.set_material(0, mat)
    component.set_collision_profile_name('BlockAll')
    return actor

created = not assets.does_asset_exist(LEVEL_PATH)
if created:
    if not levels.new_level(LEVEL_PATH):
        raise RuntimeError('Could not create the prototype arena')
    floor = material('M_Floor', (0.075, 0.11, 0.17, 1))
    wall = material('M_Boundary', (0.12, 0.22, 0.31, 1))
    lane = material('M_Lane', (0.1, 0.55, 0.62, 1))
    cube('ArenaFloor_24x10m', (0, 0, -30), (24, 10, 0.6), floor)
    cube('Boundary_Left', (-1220, 0, 75), (0.4, 10.8, 2.1), wall)
    cube('Boundary_Right', (1220, 0, 75), (0.4, 10.8, 2.1), wall)
    cube('Boundary_Back', (0, -520, 75), (24, 0.4, 2.1), wall)
    cube('Boundary_Front', (0, 520, 15), (24, 0.4, 0.6), wall)
    for y in (-300, 0, 300):
        line = cube('DepthGuide_' + str(y), (0, y, 0.2), (23.5, 0.035, 0.003), lane)
        line.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    start = actors.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-400, 0, 100))
    start.set_actor_label('PlayerStart')
    light = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 800), unreal.Rotator(pitch=-55, yaw=-35, roll=0))
    light.light_component.set_editor_property('intensity', 5.0)
    sky = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 600))
    sky.light_component.set_editor_property('intensity', 1.0)
    camera = actors.spawn_actor_from_class(unreal.CameraActor, unreal.Vector(0, 1900, 780), unreal.Rotator(pitch=-20, yaw=-90, roll=0))
    camera.set_actor_label('ArenaOverviewCamera')
    camera.camera_component.set_editor_property('field_of_view', 65.0)
    enemy = actors.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(300, 0, 0))
    enemy.set_actor_label('ResourcePreview_Quinn_NotCombatEnemy')
    enemy.skeletal_mesh_component.set_skeletal_mesh_asset(unreal.load_asset('/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple'))
    enemy.set_actor_rotation(unreal.Rotator(pitch=0, yaw=90, roll=0), False)
    enemy.skeletal_mesh_component.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
    enemy.skeletal_mesh_component.play_animation(unreal.load_asset('/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle'), True)
    if not levels.save_current_level():
        raise RuntimeError('Could not save the arena')
else:
    if not levels.load_level(LEVEL_PATH):
        raise RuntimeError('Existing arena could not be loaded')

# Repair only script-owned actors. Keyword Rotator arguments avoid Python's
# positional ordering, which differs from C++ FRotator(pitch, yaw, roll).
by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
for label in ('PlayerStart', 'ArenaFloor_24x10m', 'ResourcePreview_Quinn_NotCombatEnemy'):
    if label not in by_label:
        raise RuntimeError('Foundation map is incomplete; missing owned actor: ' + label)
preview = by_label['ResourcePreview_Quinn_NotCombatEnemy']
preview.set_actor_rotation(unreal.Rotator(pitch=0, yaw=90, roll=0), False)
data = preview.skeletal_mesh_component.get_editor_property('animation_data')
data.set_editor_property('anim_to_play', unreal.load_asset('/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle'))
data.set_editor_property('saved_looping', True)
data.set_editor_property('saved_playing', True)
preview.skeletal_mesh_component.set_editor_property('animation_data', data)
if 'DirectionalLight' in by_label:
    by_label['DirectionalLight'].set_actor_rotation(unreal.Rotator(pitch=-55, yaw=-35, roll=0), False)
    by_label['DirectionalLight'].light_component.set_editor_property('forward_shading_priority', 1)
if 'ArenaOverviewCamera' in by_label:
    by_label['ArenaOverviewCamera'].set_actor_rotation(unreal.Rotator(pitch=-20, yaw=-90, roll=0), False)
if 'FoundationBackdrop' not in by_label:
    back_mat = material('M_Backdrop', (0.1, 0.16, 0.24, 1))
    cube('FoundationBackdrop', (0, -620, 500), (45, 0.2, 12), back_mat)
if 'FoundationFillLight' not in by_label:
    fill = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 300, 600), unreal.Rotator(pitch=-35, yaw=120, roll=0))
    fill.set_actor_label('FoundationFillLight')
    fill.light_component.set_editor_property('intensity', 2.0)
    fill.light_component.set_editor_property('cast_shadows', False)
for label, position, scale in (
    ('BoundsCollision_Front', (0, 550, 600), (26, 0.2, 14)),
    ('BoundsCollision_Back', (0, -550, 600), (26, 0.2, 14)),
    ('BoundsCollision_Left', (-1250, 0, 600), (0.2, 12, 14)),
    ('BoundsCollision_Right', (1250, 0, 600), (0.2, 12, 14))
):
    if label not in by_label:
        barrier = cube(label, position, scale, unreal.load_asset('/Game/UEMMO/Materials/M_Boundary'))
        barrier.set_actor_hidden_in_game(True)
if not levels.save_current_level():
    raise RuntimeError('Could not persist foundation corrections')

required = [
    '/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple',
    '/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple',
    '/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle',
    '/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd',
    '/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01',
    '/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_02',
    '/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_03',
    '/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_ChargedAttack',
    '/Game/Characters/Mannequins/Anims/Unarmed/Jump/MM_Jump',
    '/Game/Characters/Mannequins/Anims/Unarmed/Jump/MM_Fall_Loop',
    '/Game/Characters/Mannequins/Anims/Death/MM_Death_Front_01'
]
checks = [{'path': p, 'loaded': unreal.load_asset(p) is not None} for p in required]
sound_source = ROOT / 'SourceAssets' / 'Unpacked' / 'KenneyImpact' / 'Audio'
sounds = ['impactPunch_medium_000', 'impactPunch_heavy_000', 'footstep_concrete_000', 'impactSoft_heavy_000']
audio_checks = []
for name in sounds:
    destination = '/Game/ThirdParty/Kenney/Impact/' + name
    if not assets.does_asset_exist(destination):
        source = sound_source / (name + '.ogg')
        if not source.exists():
            raise RuntimeError('Required sound archive not prepared: ' + str(source))
        task = unreal.AssetImportTask()
        task.filename = str(source)
        task.destination_path = '/Game/ThirdParty/Kenney/Impact'
        task.destination_name = name
        task.automated = True
        task.save = True
        task.replace_existing = False
        task.factory = unreal.SoundFactory()
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    audio_checks.append({'path': destination, 'loaded': unreal.load_asset(destination) is not None})
inventory = []
for path in assets.list_assets('/Game/Characters/Mannequins/Anims', recursive=True, include_folder=False):
    obj = unreal.load_asset(path)
    if isinstance(obj, unreal.AnimSequence):
        inventory.append({'path': path, 'duration_seconds': obj.get_editor_property('sequence_length')})
(ROOT / 'Artifacts' / 'animation-inventory.json').write_text(json.dumps(inventory, indent=2), encoding='utf-8')
report = {'level': LEVEL_PATH, 'created_this_run': created, 'assets': checks,
          'audio': audio_checks, 'animation_count': len(inventory),
          'actor_count': len(actors.get_all_level_actors()), 'success': all(c['loaded'] for c in checks + audio_checks)}
REPORT.parent.mkdir(parents=True, exist_ok=True)
REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
if not report['success']:
    raise RuntimeError('Missing required foundation assets; inspect foundation-report.json')
unreal.log('UEMMO_FOUNDATION_SUCCESS ' + str(REPORT))
