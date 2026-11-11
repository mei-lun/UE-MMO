"""M8-040: make the volcano map playable.

Two persisted defects (the M8-028 placement and its no-collision setting did
not survive the map save; the whole set was absent again at the M8-040 play
flow diagnosis):
1. The RoomTrigger/RoomExit actors are restored (with the enemy definition
   wired) so the activation overlap can start the wave run.
2. Every VOL_* decorative quad is set to NO_COLLISION so the player walks
   through the scenery (the M8-028 runtime-only setting is re-applied as a
   persisted edit).
Idempotent; verifies the final state before saving.
"""
import unreal

MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
TRIGGER_CLS = '/Script/UEMMO.RoomTrigger'
EXIT_CLS = '/Script/UEMMO.RoomExit'
GOBLIN_ENEMY = '/Game/UEMMO/Enemy/Definitions/DA_monster_goblinthrower.DA_monster_goblinthrower'

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def ensure_actor(cls_path, label, loc):
    cls = unreal.load_class(None, cls_path)
    found = [a for a in ACTORS.get_all_level_actors() if a.get_class() == cls]
    if len(found) > 1:
        raise RuntimeError('multiple %s found' % cls_path)
    if len(found) == 0:
        spawned = ACTORS.spawn_actor_from_class(cls, loc, unreal.Rotator(0.0, 0.0, 0.0))
        if spawned is None:
            raise RuntimeError('spawn failed: ' + cls_path)
        spawned.set_actor_label(label)
        return spawned, True
    found[0].set_actor_location(loc, sweep=False, teleport=False)
    return found[0], False


if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)

trigger, t_created = ensure_actor(TRIGGER_CLS, 'VolcanoRoomTrigger_01',
    unreal.Vector(x=900.0, y=1050.0, z=100.0))
exit_actor, e_created = ensure_actor(EXIT_CLS, 'VolcanoRoomExit_01',
    unreal.Vector(x=1900.0, y=1050.0, z=110.0))
enemy_def = unreal.load_asset(GOBLIN_ENEMY)
if enemy_def is None:
    raise RuntimeError('missing enemy def ' + GOBLIN_ENEMY)
trigger.set_editor_property('enemy_definition_path', enemy_def)

# M8-040: the walk band has NO collision floor (the duplicated training
# arena's floor only spans y -500..500; the M8-029 calibrated band sits at
# y_cm 986-1234 with ground z 0). Lay a collision floor across the object
# span so the player can spawn and walk to the trigger.
FLOOR_LABEL = 'VolcanoWalkFloor'
floors = [a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == FLOOR_LABEL]
if not floors:
    plane = unreal.load_asset('/Engine/BasicShapes/Plane')
    floor = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor,
        unreal.Vector(x=1229.0, y=1100.0, z=0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if floor is None:
        raise RuntimeError('floor spawn failed')
    floor.set_actor_label(FLOOR_LABEL)
    smc = floor.get_editor_property('static_mesh_component')
    smc.set_static_mesh(plane)
    # The engine plane is 100x100cm: x 0..2458 (the object span), y 900..1300
    # (the band with margin).
    floor.set_actor_scale3d(unreal.Vector(x=24.58, y=4.0, z=1.0))
    smc.set_collision_profile_name('BlockAll')
    smc.set_cast_shadow(False)
    floors = [floor]
    print('UEMMO_VOLCANO_PLAYABILITY: walk floor created')
else:
    print('UEMMO_VOLCANO_PLAYABILITY: walk floor already present')

# The decorative volcano quads: walk-through scenery. The collision profile
# (unlike the runtime set_collision_enabled call) serializes with the map.
fixed = 0
for a in ACTORS.get_all_level_actors():
    if not a.get_actor_label().startswith('VOL_'):
        continue
    smc = a.get_editor_property('static_mesh_component') if a.get_class().get_name() == 'StaticMeshActor' else None
    if smc is not None and smc.get_collision_enabled() != unreal.CollisionEnabled.NO_COLLISION:
        smc.set_collision_profile_name('NoCollision')
        fixed += 1

if not LEVELS.save_current_level():
    raise RuntimeError('save failed')

# Re-read the SAVED state (fresh load) as the verification.
LEVELS.load_level(MAP)
triggers = [a for a in ACTORS.get_all_level_actors()
            if a.get_class().get_name() == 'RoomTrigger']
exits = [a for a in ACTORS.get_all_level_actors()
         if a.get_class().get_name() == 'RoomExit']
blocking = 0
for a in ACTORS.get_all_level_actors():
    if a.get_actor_label().startswith('VOL_') and a.get_class().get_name() == 'StaticMeshActor':
        smc = a.get_editor_property('static_mesh_component')
        if smc is not None and smc.get_collision_enabled() != unreal.CollisionEnabled.NO_COLLISION:
            blocking += 1
if len(triggers) != 1 or len(exits) != 1 or blocking != 0 or not floors:
    raise RuntimeError('verify failed: triggers=%d exits=%d blocking=%d floors=%d'
                       % (len(triggers), len(exits), blocking, len(floors)))
print('UEMMO_VOLCANO_PLAYABILITY: trigger=%s exit=%s decor_collision_fixed=%d floor=1 verified'
      % (t_created, e_created, fixed))
