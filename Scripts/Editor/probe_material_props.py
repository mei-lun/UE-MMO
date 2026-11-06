"""M8-019A diagnostic 2: dump material properties + output connections for
the fighter and goblin visual materials."""
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
    row = {}
    for prop in ('shading_model', 'blend_mode', 'two_sided', 'used_with_static_lighting'):
        try:
            row[prop] = str(mat.get_editor_property(prop))
        except Exception as exc:
            row[prop] = 'ERR %s' % exc
    mel = unreal.MaterialEditingLibrary
    for label, mp in [
        ('emissive', unreal.MaterialProperty.MP_EMISSIVE_COLOR),
        ('opacity_mask', unreal.MaterialProperty.MP_OPACITY_MASK),
        ('opacity', unreal.MaterialProperty.MP_OPACITY),
        ('base_color', unreal.MaterialProperty.MP_BASE_COLOR),
    ]:
        try:
            node = mel.get_material_property_input_node(mat, mp)
            row['input_' + label] = node.get_class().get_name() if node is not None else 'NOT_CONNECTED'
        except Exception as exc:
            row['input_' + label] = 'ERR %s' % exc
    return row


try:
    for name, path in [
        ('fighter', '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Materials/M_DNFMFighterVisual.M_DNFMFighterVisual'),
        ('goblin', '/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_GoblinthrowerVisual.M_GoblinthrowerVisual'),
    ]:
        REPORT['materials'][name] = dump(path)
    REPORT['success'] = True
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
finally:
    ART.mkdir(parents=True, exist_ok=True)
    (ART / 'material-props.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
