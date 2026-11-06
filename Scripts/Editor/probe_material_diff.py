"""M8-019A diagnostic: dump the expression graphs of the fighter and goblin
visual materials to find why the goblin one renders nothing."""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-019A'
REPORT = {'success': False, 'materials': {}, 'errors': []}


def dump(path):
    mat = unreal.load_asset(path)
    if mat is None:
        return {'error': 'missing'}
    rows = []
    exprs = unreal.MaterialEditingLibrary.get_material_expressions(mat) if hasattr(
        unreal.MaterialEditingLibrary, 'get_material_expressions') else []
    for e in exprs:
        row = {'class': e.get_class().get_name()}
        for prop in ('parameter_name', 'texture'):
            try:
                v = e.get_editor_property(prop)
                row[prop] = v.get_name() if hasattr(v, 'get_name') else str(v)
            except Exception:
                pass
        rows.append(row)
    return {'path': path, 'expressions': rows, 'count': len(rows)}


try:
    for name, path in [
        ('fighter', '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Materials/M_DNFMFighterVisual.M_DNFMFighterVisual'),
        ('goblin', '/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_GoblinthrowerVisual.M_GoblinthrowerVisual'),
    ]:
        REPORT['materials'][name] = dump(path)
    REPORT['success'] = True
except Exception:
    REPORT['errors'].append(traceback.format_exc() if (traceback := __import__('traceback')) else '')
finally:
    ART.mkdir(parents=True, exist_ok=True)
    (ART / 'material-diff.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
