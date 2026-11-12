"""M8-044: fix the volcano material palette, restyle the lava strips, fill the
black zones. Runs on the volcano map AND the main menu (same dressing family).

Root causes fixed here:
- M_VolcanoRockDark / M_VolcanoFloorBase base colors were authored in the linear
  domain (0.075 / 0.16) and read as medium/light gray under the scene light
  (sRGB ~= 0.30 / 0.43) — set true dark values.
- M_VolcanoLavaPulse low phase (0.55 linear) washed the strips to cream — widen
  the pulse swing (deep red .. bright orange).
- The 44 M_OBJ rock planes are single-sided: their backs render as black slabs.
  Set two_sided on all of them.
- BackWall top 1700cm leaves black above; raise to 2200cm. The FoundationBackdrop
  sits inside the opaque wall volume (fully hidden) — remove it.
- Lava strips restyle (decision point D3 default): 120cm -> 60cm wide, flush at
  z=2 on the walk floor, plus dark rock rails framing each strip.
"""
import unreal

MEL = unreal.MaterialEditingLibrary
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

MAPS = ('/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01',
        '/Game/UEMMO/Maps/L_MainMenu')

ROCK = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoRockDark'
FLOOR = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoFloorBase'
LAVA = '/Game/UEMMO/DNFM/Volcano/M/M_VolcanoLavaPulse'


def set_base_color(mat, linear):
    for e in MEL.get_material_expressions(mat):
        cls = e.get_class().get_name()
        if cls == 'MaterialExpressionConstant3Vector':
            e.set_editor_property('constant', linear)
            return True
    return False


def fix_materials():
    rock = unreal.load_asset(ROCK)
    assert set_base_color(rock, unreal.LinearColor(0.026, 0.020, 0.020, 1.0))
    floor = unreal.load_asset(FLOOR)
    assert set_base_color(floor, unreal.LinearColor(0.042, 0.032, 0.030, 1.0))
    lava = unreal.load_asset(LAVA)
    fixed_lo = 0
    for e in MEL.get_material_expressions(lava):
        if e.get_class().get_name() != 'MaterialExpressionConstant3Vector':
            continue
        c = e.get_editor_property('constant')
        if abs(c.r - 0.550) < 0.01 and abs(c.g - 0.120) < 0.01:
            e.set_editor_property('constant', unreal.LinearColor(0.10, 0.016, 0.008, 1.0))
            fixed_lo += 1
    MEL.recompile_material(rock)
    MEL.recompile_material(floor)
    MEL.recompile_material(lava)
    ASSETS.save_loaded_asset(rock)
    ASSETS.save_loaded_asset(floor)
    ASSETS.save_loaded_asset(lava)
    return fixed_lo


def fix_obj_two_sided():
    n = 0
    for i in range(1, 45):
        for s in ('NORMAL', 'MIDDLEBACK', 'DISTANTBACK', 'CONTACT'):
            p = '/Game/UEMMO/DNFM/Volcano/M/M_OBJ_%02d_%s' % (i, s)
            if not unreal.EditorAssetLibrary.does_asset_exist(p):
                continue
            m = unreal.load_asset(p)
            if not m.get_editor_property('two_sided'):
                m.set_editor_property('two_sided', True)
                MEL.recompile_material(m)
                ASSETS.save_loaded_asset(m)
            n += 1
            break
    return n


def restyle_map(map_path):
    if not LEVELS.load_level(map_path):
        raise RuntimeError('cannot load ' + map_path)
    changes = {'strips': 0, 'rails': 0, 'backwall': 0, 'backdrop_removed': 0, 'ridges': 0}
    strips = []
    for a in ACTORS.get_all_level_actors():
        lb = a.get_actor_label()
        if lb.startswith('VolcanoDress_LavaStrip'):
            loc = a.get_actor_location()
            sc = a.get_actor_scale3d()
            a.set_actor_scale3d(unreal.Vector(sc.x, 0.6, sc.z))
            a.set_actor_location(unreal.Vector(loc.x, loc.y, 2.0), sweep=False, teleport=True)
            strips.append(unreal.Vector(loc.x, loc.y, 2.0))
            changes['strips'] += 1
        elif lb == 'VolcanoDress_BackWall':
            sc = a.get_actor_scale3d()
            a.set_actor_scale3d(unreal.Vector(sc.x, sc.y, 22.0))  # top 1700 -> 2200
            changes['backwall'] += 1
        elif lb == 'FoundationBackdrop':
            ACTORS.destroy_actor(a)
            changes['backdrop_removed'] += 1

    # dark rock rails framing each lava strip (D3 default: lava channel read)
    rock = unreal.load_asset(ROCK)
    plane = unreal.load_asset('/Engine/BasicShapes/Plane')
    rock_class = None
    for a in ACTORS.get_all_level_actors():
        if a.get_actor_label() == 'VolcanoDress_BackWall':
            rock_class = a.get_class()
            break
    for st in strips:
        for side in (-1.0, 1.0):
            rail = ACTORS.spawn_actor_from_class(
                rock_class, unreal.Vector(st.x, st.y + side * 38.0, 1.5),
                unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
            rail.set_actor_label('VolcanoDress_LavaRail_%s' % ('A' if side < 0 else 'B'))
            rail.set_actor_scale3d(unreal.Vector(37.0, 0.08, 1.0))
            for c in rail.get_components_by_class(unreal.StaticMeshComponent):
                c.set_material(0, rock)
            changes['rails'] += 1

    # distant ridge planes behind the far rock rows (fill inter-panel gaps)
    ridges = ((604.0, 180.0, 300.0, 37.0, 14.0, 6.0), (604.0, 420.0, 500.0, 37.0, 10.0, 10.0))
    for x, y, z, sx, sy, sz in ridges:
        ridge = ACTORS.spawn_actor_from_class(
            rock_class, unreal.Vector(x, y, z), unreal.Rotator(pitch=90.0, yaw=0.0, roll=0.0))
        ridge.set_actor_label('VolcanoDress_Ridge_%d' % changes['ridges'])
        ridge.set_actor_scale3d(unreal.Vector(sx, sy, sz))
        for c in ridge.get_components_by_class(unreal.StaticMeshComponent):
            c.set_material(0, rock)
        changes['ridges'] += 1

    if not LEVELS.save_current_level():
        raise RuntimeError('save failed ' + map_path)
    return changes


mat_fixed = fix_materials()
obj_count = fix_obj_two_sided()
results = [restyle_map(m) for m in MAPS]
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-044/restyle-result.txt', 'w') as fh:
    fh.write('lava_lo_fixed=%d obj_two_sided=%d\n' % (mat_fixed, obj_count))
    for m, c in zip(MAPS, results):
        fh.write('%s %s\n' % (m.split('/')[-1], c))
