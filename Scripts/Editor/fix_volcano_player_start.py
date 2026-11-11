"""M8-040: move the volcano map's PlayerStart into the calibrated walk band.

The volcano map is a training-arena duplicate, so its PlayerStart kept the
arena's spawn (y ~= 0, the checker floor) - outside the M8-029 calibrated
walk band (y_cm 986-1234, ground z 0). A player spawning there can never
reach the activation trigger at (900, 1050, 100): the play-flow fix moves
the start onto the band. Idempotent; verify mode checks the position only.
"""
from pathlib import Path

import unreal

MAP = '/Game/UEMMO/DNFM/Volcano/Maps/L_DNFM_Volcano01'
TARGET = unreal.Vector(x=300.0, y=1100.0, z=98.0)
MODE = 'verify' if 'UEMMOVolcanoSpawnVerify' in unreal.SystemLibrary.get_command_line() else 'fix'

LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def run():
    if not LEVELS.load_level(MAP):
        raise RuntimeError('cannot load ' + MAP)
    starts = [a for a in ACTORS.get_all_level_actors()
              if a.get_class().get_name() == 'PlayerStart']
    if len(starts) != 1:
        raise RuntimeError('expected exactly one PlayerStart, found %d' % len(starts))
    start = starts[0]
    if MODE == 'verify':
        loc = start.get_actor_location()
        ok = abs(loc.x - TARGET.x) < 1.0 and abs(loc.y - TARGET.y) < 1.0
        print('UEMMO_VOLCANO_SPAWN_VERIFY: (%.1f, %.1f, %.1f) ok=%s'
              % (loc.x, loc.y, loc.z, ok))
        if not ok:
            raise RuntimeError('PlayerStart not at the calibrated band')
        return
    start.set_actor_location(TARGET, sweep=False, teleport=False)
    start.set_actor_label('VolcanoPlayerStart')
    if not LEVELS.save_current_level():
        raise RuntimeError('save failed')
    loc = start.get_actor_location()
    print('UEMMO_VOLCANO_SPAWN_FIX: moved to (%.1f, %.1f, %.1f)' % (loc.x, loc.y, loc.z))


run()
