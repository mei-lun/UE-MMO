"""Create /Game/UEMMO/Maps/L_TrainingArena from L_PrototypeArena. Run inside the UE editor only.

Two modes, selected by a token on the UE command line:
  create (default)  - duplicate the M0 base map, remove the Quinn resource
                      preview actor, place exactly one ATrainingEnemy, save.
  verify            - after a fresh process start, reload both maps and assert
                      the actor inventory; writes Artifacts/training-room-report.json.

The base map L_PrototypeArena is never modified: it is only duplicated.
ASCII only: this file must not contain non-ASCII characters.
"""
import json
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
REPORT = ROOT / 'Artifacts' / 'training-room-report.json'
BASE_MAP = '/Game/UEMMO/Maps/L_PrototypeArena'
TRAINING_MAP = '/Game/UEMMO/Maps/L_TrainingArena'
ENEMY_CLASS_PATH = '/Script/UEMMO.TrainingEnemy'
PREVIEW_LABEL_PREFIX = 'ResourcePreview_Quinn'
ENEMY_LABEL = 'TrainingEnemy_01'
# Place the enemy where the preview actor stood. Actor yaw 180 makes Quinn
# face -X toward the player spawn at (-400, 0): with the mannequin convention
# (mesh local +Y is forward, mesh relative yaw -90) actor yaw 0 faces +X.
ENEMY_LOCATION = unreal.Vector(300, 0, 88)
ENEMY_ROTATION = unreal.Rotator(pitch=0, yaw=180, roll=0)
QUINN_MESH = '/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple'
QUINN_IDLE = '/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle'

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)


def command_line():
    try:
        return unreal.SystemLibrary.get_command_line() or ''
    except Exception:
        return ''


def is_verify_mode():
    return 'UEMMOTrainingRoomVerify' in command_line()


def enemy_class():
    candidate = getattr(unreal, 'TrainingEnemy', None)
    if candidate is not None:
        return candidate
    loaded = unreal.load_class(None, ENEMY_CLASS_PATH)
    if loaded is None:
        raise RuntimeError('UEMMO.TrainingEnemy not found; build the UEMMO module first.')
    return loaded


def inventory():
    rows = []
    for actor in actors.get_all_level_actors():
        rows.append({'label': actor.get_actor_label(), 'class': actor.get_class().get_name()})
    return rows


def count_class(rows, class_name):
    return len([row for row in rows if row['class'] == class_name])


def count_preview(rows):
    return len([row for row in rows if row['label'].startswith(PREVIEW_LABEL_PREFIX)])


def find_enemy():
    for actor in actors.get_all_level_actors():
        if actor.get_class().get_name() == 'TrainingEnemy':
            return actor
    return None


def ensure_quinn_appearance(enemy):
    # The C++ constructor helper usually applies the Quinn mesh already; only
    # patch when the asset is missing (for example content not yet prepared).
    mesh_component = enemy.get_component_by_class(unreal.SkeletalMeshComponent)
    if mesh_component is None:
        raise RuntimeError('Training enemy has no skeletal mesh component.')
    mesh = mesh_component.get_editor_property('skeletal_mesh_asset')
    if mesh is None:
        mesh = unreal.load_asset(QUINN_MESH)
        if mesh is None:
            raise RuntimeError('Quinn skeletal mesh asset is missing; run PrepareContent.ps1 first.')
        mesh_component.set_skeletal_mesh_asset(mesh)
        mesh_component.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
        data = mesh_component.get_editor_property('animation_data')
        data.set_editor_property('anim_to_play', unreal.load_asset(QUINN_IDLE))
        data.set_editor_property('saved_looping', True)
        data.set_editor_property('saved_playing', True)
        mesh_component.set_editor_property('animation_data', data)
    return mesh.get_path_name() if mesh else ''


def has_component_of_class(actor, class_name):
    for component in actor.get_components_by_class(unreal.ActorComponent):
        if component.get_class().get_name() == class_name:
            return True
    return False


def create_training_room():
    if not assets.does_asset_exist(TRAINING_MAP):
        if not assets.does_asset_exist(BASE_MAP):
            raise RuntimeError('Base map missing: ' + BASE_MAP)
        if not unreal.EditorAssetLibrary.duplicate_asset(BASE_MAP, TRAINING_MAP):
            raise RuntimeError('Could not duplicate ' + BASE_MAP + ' into ' + TRAINING_MAP)
    if not levels.load_level(TRAINING_MAP):
        raise RuntimeError('Could not load ' + TRAINING_MAP)

    # Remove the M0 resource preview copy; the training enemy replaces it.
    for actor in list(actors.get_all_level_actors()):
        if actor.get_actor_label().startswith(PREVIEW_LABEL_PREFIX):
            actors.destroy_actor(actor)

    enemies = [actor for actor in actors.get_all_level_actors()
               if actor.get_class().get_name() == 'TrainingEnemy']
    if len(enemies) == 0:
        enemy = actors.spawn_actor_from_class(enemy_class(), ENEMY_LOCATION, ENEMY_ROTATION)
        if enemy is None:
            raise RuntimeError('Could not spawn ATrainingEnemy.')
        enemy.set_actor_label(ENEMY_LABEL)
    elif len(enemies) > 1:
        raise RuntimeError('Training arena already holds ' + str(len(enemies)) + ' training enemies; refusing to add more.')
    else:
        enemy = enemies[0]

    mesh_path = ensure_quinn_appearance(enemy)
    labels = [actor.get_actor_label() for actor in actors.get_all_level_actors()]
    if 'PlayerStart' not in labels:
        raise RuntimeError('PlayerStart is missing from the training arena.')
    if count_preview(inventory()) != 0:
        raise RuntimeError('Preview actor removal failed.')
    if not levels.save_current_level():
        raise RuntimeError('Could not save ' + TRAINING_MAP)
    unreal.log('UEMMO_TRAINING_ROOM_CREATED mesh=' + mesh_path)


def verify_maps():
    checks = []

    def record(name, ok, detail=''):
        checks.append({'check': name, 'passed': bool(ok), 'detail': detail})

    if not levels.load_level(TRAINING_MAP):
        raise RuntimeError('Verify: could not load ' + TRAINING_MAP)
    training_rows = inventory()
    enemy = find_enemy()
    # Capture everything about the enemy before switching levels: actor
    # wrappers of the previous level become stale after load_level.
    enemy_has_health = enemy is not None and has_component_of_class(enemy, 'HealthComponent')
    enemy_mesh_path = ensure_quinn_appearance(enemy) if enemy else ''
    enemy_location = enemy.get_actor_location() if enemy else unreal.Vector(0, 0, 0)
    record('training_map_exactly_one_training_enemy',
           count_class(training_rows, 'TrainingEnemy') == 1,
           'count=' + str(count_class(training_rows, 'TrainingEnemy')))
    record('training_map_no_quinn_preview', count_preview(training_rows) == 0,
           'previews=' + str(count_preview(training_rows)))
    record('training_map_player_start_present',
           any(row['class'] == 'PlayerStart' or row['label'] == 'PlayerStart' for row in training_rows))
    record('training_enemy_has_health_component', enemy_has_health)
    record('training_enemy_has_quinn_mesh', enemy_mesh_path != '', enemy_mesh_path)

    if not levels.load_level(BASE_MAP):
        raise RuntimeError('Verify: could not load ' + BASE_MAP)
    base_rows = inventory()
    record('prototype_arena_actor_count_unchanged', len(base_rows) == 19,
           'count=' + str(len(base_rows)))
    record('prototype_arena_preview_still_present', count_preview(base_rows) == 1,
           'previews=' + str(count_preview(base_rows)))
    record('prototype_arena_has_no_training_enemy', count_class(base_rows, 'TrainingEnemy') == 0)

    report = {
        'mode': 'verify',
        'training_arena': {
            'path': TRAINING_MAP,
            'actor_count': len(training_rows),
            'training_enemy_count': count_class(training_rows, 'TrainingEnemy'),
            'preview_actor_count': count_preview(training_rows),
            'enemy_has_health_component': enemy_has_health,
            'enemy_mesh_path': enemy_mesh_path,
            'enemy_location': {'x': enemy_location.x, 'y': enemy_location.y, 'z': enemy_location.z},
            'actors': training_rows,
        },
        'prototype_arena': {
            'path': BASE_MAP,
            'actor_count': len(base_rows),
            'preview_actor_count': count_preview(base_rows),
            'training_enemy_count': count_class(base_rows, 'TrainingEnemy'),
            'actors': base_rows,
        },
        'checks': checks,
        'success': all(check['passed'] for check in checks),
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not report['success']:
        failed = [check['check'] for check in checks if not check['passed']]
        raise RuntimeError('Training room verification failed: ' + ', '.join(failed))
    unreal.log('UEMMO_TRAINING_ROOM_VERIFIED ' + str(REPORT))


if is_verify_mode():
    verify_maps()
else:
    create_training_room()
