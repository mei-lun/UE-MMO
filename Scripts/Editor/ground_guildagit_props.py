"""M8-046: fix the guildagit prop placement (per Docs/Audit-2026-11-12.md G3-G7).

Passes:
 1. Floating deco (84: deco_structure/farmaround/land_flowers/bushes_under/trees1)
    -> shift down so each actor's bounds bottom sits at -5cm.
 2. Lake (deco_lake, 135 pieces at z=[-30..-20] under the opaque ground) -> raise
    each so its top reaches +2cm (visible above the ground top).
 3. Foreground trees (trees_under, 70 pieces y 961..1561 between camera and play
    band) -> uniform +839cm south so the whole group starts at y=1800 (D1 default).
 4. BG_Planet (31 pieces around the origin, 8 fully buried) -> it is the DNFM sky
    planet: lift the cluster to the sky in front of the backdrop (D2 default);
    whitelisted as an intentional sky object in the gate.
"""
import unreal

MAP = '/Game/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit'
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if not LEVELS.load_level(MAP):
    raise RuntimeError('cannot load ' + MAP)

counts = {'grounded': 0, 'lake': 0, 'trees': 0, 'planet': 0}


def shift(a, dx, dy, dz):
    loc = a.get_actor_location()
    a.set_actor_location(unreal.Vector(loc.x + dx, loc.y + dy, loc.z + dz),
                         sweep=False, teleport=True)


for a in ACTORS.get_all_level_actors():
    lb = a.get_actor_label()
    origin, extent = a.get_actor_bounds(False)
    z_min, z_max = origin.z - extent.z, origin.z + extent.z
    loc = a.get_actor_location()
    if lb.startswith('GuildagitGroundDummy/deco_lake'):
        shift(a, 0, 0, 2.0 - z_max)
        counts['lake'] += 1
    elif lb.startswith('GuildagitGroundDummy/trees_under'):
        if loc.y < 1800.0:
            shift(a, 0, 1800.0 - loc.y, 0)
        counts['trees'] += 1
    elif lb.startswith('BG_Planet'):
        if loc.z < 1000.0:
            shift(a, 0, -690.0 - loc.y, 1150.0 - loc.z)
        counts['planet'] += 1
    elif lb.startswith('GuildagitGroundDummy/') and z_min > 10.0:
        shift(a, 0, 0, -5.0 - z_min)
        counts['grounded'] += 1

if not LEVELS.save_current_level():
    raise RuntimeError('save failed')
with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-046/fix-result.txt', 'w') as fh:
    fh.write(str(counts) + '\n')
