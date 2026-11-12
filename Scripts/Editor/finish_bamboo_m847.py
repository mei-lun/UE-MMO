"""M8-047: bamboo visual finish-up (per Docs/Audit-2026-11-12.md B1-B5).

- M_BambooShell: dark bamboo-night solid (shell walls + top filler).
- Exit marker shrunk to a low landmark (40x200x90) with M_ExitPortal; the
  RoomExit cube mesh hidden (volume/collision untouched).
- Shell (FloorSlab + 4 walls) off WorldGridMaterial onto M_BambooShell.
- Ground tiles raised z=0 -> 2 (no more coplanar fight with the slab top).
- Fog46/47: F1 duplicate layers offset 40cm behind F0 (no coplanar fight);
  Fog48/49 (x=3688, unreachable) deleted; a dark top filler plane spans
  z 620..1100 above the backdrop cluster (no black strip at the frame top).
"""
import unreal

MAP = '/Game/UEMMO/DNFM/Bamboo/Maps/L_DNFM_Bamboo01'
MEL = unreal.MaterialEditingLibrary
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

SHELL_MAT = '/Game/UEMMO/DNFM/Bamboo/M/M_BambooShell'
EXIT_MAT = '/Game/UEMMO/Rooms/M_ExitPortal'

shell = unreal.load_asset(SHELL_MAT)
if shell is None:
    shell = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_BambooShell', '/Game/UEMMO/DNFM/Bamboo/M', unreal.Material,
        unreal.MaterialFactoryNew())
    color = MEL.create_material_expression(shell, unreal.MaterialExpressionConstant3Vector, -400, -200)
    color.set_editor_property('constant', unreal.LinearColor(0.010, 0.016, 0.011, 1.0))
    MEL.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
    rough = MEL.create_material_expression(shell, unreal.MaterialExpressionConstant, -400, 40)
    rough.set_editor_property('r', 1.0)
    MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(shell)
    ASSETS.save_loaded_asset(shell)

exit_mat = unreal.load_asset(EXIT_MAT)
if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)

counts = {'marker': 0, 'exit_hidden': 0, 'shell': 0, 'tiles': 0,
          'fog_offset': 0, 'fog_deleted': 0, 'filler': 0}
cube = unreal.load_asset('/Engine/BasicShapes/Cube')
plane = unreal.load_asset('/Engine/BasicShapes/Plane')

to_delete = []
for a in ACTORS.get_all_level_actors():
    lb = a.get_actor_label()
    if lb == 'DNFM_Bamboo_ExitMarker':
        a.set_actor_scale3d(unreal.Vector(0.4, 2.0, 0.9))
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, exit_mat)
        counts['marker'] += 1
    elif lb == 'BambooRoomExit_01':
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_editor_property('visible', False)
            c.set_material(0, shell)
        counts['exit_hidden'] += 1
    elif lb in ('DNFM_Bamboo_FloorSlab', 'DNFM_Bamboo_WallFar', 'DNFM_Bamboo_WallNear',
                'DNFM_Bamboo_WallLeft', 'DNFM_Bamboo_WallRight'):
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, shell)
        counts['shell'] += 1
    elif lb.startswith('DNFM_Bamboo_Tile0'):
        loc = a.get_actor_location()
        a.set_actor_location(unreal.Vector(loc.x, loc.y, 2.0), sweep=False, teleport=True)
        counts['tiles'] += 1
    elif lb in ('DNFM_Bamboo_Fog46_F1', 'DNFM_Bamboo_Fog47_F1'):
        loc = a.get_actor_location()
        a.set_actor_location(unreal.Vector(loc.x, loc.y - 40.0, loc.z), sweep=False, teleport=True)
        counts['fog_offset'] += 1
    elif lb in ('DNFM_Bamboo_Fog48_F0', 'DNFM_Bamboo_Fog48_F1',
                'DNFM_Bamboo_Fog49_F0', 'DNFM_Bamboo_Fog49_F1'):
        to_delete.append(a)

for a in to_delete:
    ACTORS.destroy_actor(a)
counts['fog_deleted'] = len(to_delete)

filler = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor,
                                       unreal.Vector(1053.0, 786.0, 860.0),
                                       unreal.Rotator(pitch=90.0, yaw=0.0, roll=0.0))
filler.set_actor_label('DNFM_Bamboo_TopFiller')
filler.set_actor_scale3d(unreal.Vector(26.35, 4.8, 1.0))
for c in filler.get_components_by_class(unreal.StaticMeshComponent):
    c.set_static_mesh(plane)
    c.set_material(0, shell)
counts['filler'] = 1

if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-047/fix-result.txt', 'w') as fh:
    fh.write(str(counts) + '\n')
