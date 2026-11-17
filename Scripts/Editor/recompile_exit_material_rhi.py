"""Dirty + recompile M_ExitPortal inside a RENDERED session.

The material was created in a -nullrhi commandlet and ended in a zombie state:
flagged compiled, empty shader map, no jobs ever queued (default-checker render).
Touching a property marks it dirty so recompile_material queues real jobs.
"""
import time

import unreal

mat = unreal.load_asset('/Game/UEMMO/Rooms/M_ExitPortal')
if mat is None:
    raise RuntimeError('M_ExitPortal missing')
lib = unreal.MaterialEditingLibrary
state = {'start': time.time(), 'kicked': False}


def tick(_dt):
    if not state['kicked']:
        rough = None
        for e in mat.get_editor_property('expressions') if hasattr(mat, 'get_editor_property') else []:
            pass
        # touch: re-run recompile after forcing a property change via the library
        try:
            lib.update_material_expressions(mat)  # marks the material dirty if available
        except Exception:
            pass
        ok = lib.recompile_material(mat)
        unreal.log('UEMMO_MATFIX2: kicked recompile -> %s' % (ok,))
        state['kicked'] = True
        state['start'] = time.time()
        return
    if time.time() - state['start'] > 30:
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        with open('D:/Github-Poj/UE-MMO/Artifacts/Tasks/M8-043/rhi-recompile.txt', 'w') as fh:
            fh.write('dirty_recompile_issued=True at=%s\n' % time.strftime('%H:%M:%S'))
        unreal.SystemLibrary.quit_editor()


handle = unreal.register_slate_post_tick_callback(tick)
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
