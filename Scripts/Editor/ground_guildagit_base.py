"""M8-045: real guildagit ground + backdrop + grounded exit.

- Import the flattened stone-brick crop (source atlas GA_GA_wall, brick region
  alpha-flattened; no dedicated ground texture exists in the 74-texture source
  manifest) as T_GAGroundStone.
- Build M_GAGroundStone (tiled 8x3 on the ground cube's top face) and assign it
  to DNFM_GAGroundPlaceholder_MISSING_GEOMETRY.
- Add the room-family backdrop wall (M_Backdrop) north of the content band so
  the upper frame is no longer a black void.
- Ensure lighting: the import has no light actors — add a town dusk set
  (DirectionalLight 2.0 + SkyLight 0.7) so the stone reads.
- Ground the exit (bottom edge z=0) and give it M_ExitPortal.
"""
import unreal

MAP = '/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'
PNG = 'D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-045/T_GAGroundStone.png'
TEX_PATH = '/Game/UEMMO/Imported/DNFM130_5_8/Textures/T_GAGroundStone'
MAT_PATH = '/Game/UEMMO/Imported/DNFM130_5_8/Materials/M_GAGroundStone'
BACKDROP_MAT = '/Game/UEMMO/Materials/M_Backdrop'
EXIT_MAT = '/Game/UEMMO/Rooms/M_ExitPortal'

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MEL = unreal.MaterialEditingLibrary
notes = []

# 1. import the texture
if not unreal.EditorAssetLibrary.does_asset_exist(TEX_PATH):
    task = unreal.AssetImportTask()
    task.filename = PNG
    task.destination_path = '/Game/UEMMO/Imported/DNFM130_5_8/Textures'
    task.destination_name = 'T_GAGroundStone'
    task.automated = True
    task.replace_existing = False
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
notes.append('texture_imported=%s' % unreal.EditorAssetLibrary.does_asset_exist(TEX_PATH))
tex = unreal.load_asset(TEX_PATH)

# 2. build the tiled ground material
mat = unreal.load_asset(MAT_PATH)
if mat is None:
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_GAGroundStone', '/Game/UEMMO/Imported/DNFM130_5_8/Materials',
        unreal.Material, unreal.MaterialFactoryNew())
    for e in MEL.get_material_expressions(mat):
        MEL.delete_material_expression(mat, e)
    uv = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -500, 0)
    uv.set_editor_property('u_tiling', 8.0)
    uv.set_editor_property('v_tiling', 3.0)
    samp = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -300, 0)
    samp.set_editor_property('texture', tex)
    MEL.connect_material_expressions(uv, '', samp, 'UVs')
    MEL.connect_material_property(samp, '', unreal.MaterialProperty.MP_BASE_COLOR)
    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 220)
    rough.set_editor_property('r', 0.9)
    MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    ASSETS.save_loaded_asset(mat)
notes.append('ground_mat_ready')

# 3. backdrop wall + lights + exit, then save
if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)

backdrop_mat = unreal.load_asset(BACKDROP_MAT)
sm_class = unreal.StaticMeshActor
cube = unreal.load_asset('/Engine/BasicShapes/Cube')
bd = None
for a in ACTORS.get_all_level_actors():
    if a.get_actor_label() == 'GABackdrop_North':
        bd = a
        break
if bd is None:
    bd = ACTORS.spawn_actor_from_class(sm_class, unreal.Vector(-146.0, -700.0, 700.0),
                                       unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    bd.set_actor_label('GABackdrop_North')
    bd.set_actor_scale3d(unreal.Vector(61.0, 0.2, 16.0))
notes.append('backdrop_spawned')
for c in bd.get_components_by_class(unreal.StaticMeshComponent):
    c.set_static_mesh(cube)
    c.set_material(0, backdrop_mat)

light_count = 0
for a in ACTORS.get_all_level_actors():
    if 'Light' in a.get_class().get_name():
        light_count += 1
if light_count == 0:
    sun = ACTORS.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 400),
                                        unreal.Rotator(pitch=-45.0, yaw=30.0, roll=0.0))
    sun.set_actor_label('GADuskSun')
    for c in sun.get_components_by_class(unreal.DirectionalLightComponent):
        c.set_editor_property('intensity', 2.0)
    sky = ACTORS.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 400),
                                        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    sky.set_actor_label('GADuskSky')
    for c in sky.get_components_by_class(unreal.SkyLightComponent):
        c.set_editor_property('intensity', 0.7)
    notes.append('lights_added=2')
else:
    notes.append('lights_existing=%d' % light_count)

ground_fixed = 0
exit_fixed = 0
for a in ACTORS.get_all_level_actors():
    lb = a.get_actor_label()
    if lb.startswith('DNFM_GAGroundPlaceholder'):
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, mat)
        ground_fixed += 1
    elif lb == 'DNFM_GARoomExit':
        loc = a.get_actor_location()
        origin, extent = a.get_actor_bounds(False)
        z_min = origin.z - extent.z
        if abs(z_min) > 0.5:
            a.set_actor_location(unreal.Vector(loc.x, loc.y, loc.z - z_min),
                                 sweep=False, teleport=True)
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, unreal.load_asset(EXIT_MAT))
        exit_fixed += 1

if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-045/fix-result.txt', 'w') as fh:
    fh.write('; '.join(notes) + ' | ground_fixed=%d exit_fixed=%d\n' % (ground_fixed, exit_fixed))
