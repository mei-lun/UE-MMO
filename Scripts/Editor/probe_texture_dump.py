"""M8-019A diagnostic 3: export the imported goblin page texture back to PNG
and report basic stats (does the UE-side texture actually contain pixels?)."""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
ART = ROOT / 'Artifacts' / 'Tasks' / 'M8-019A'
REPORT = {'success': False, 'errors': [], 'textures': {}}


def main():
    dest = ART / 'texture-export'
    dest.mkdir(parents=True, exist_ok=True)
    for name, path in [
        ('goblin', '/Game/UEMMO/DNFM/Monsters/Goblinthrower/T/T_goblinthrower_page0.T_goblinthrower_page0'),
        ('fighter', '/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Textures/T_DNFMFighter_page0.T_DNFMFighter_page0'),
    ]:
        tex = unreal.load_asset(path)
        if tex is None:
            REPORT['textures'][name] = {'error': 'missing'}
            continue
        task = unreal.AssetExportTask()
        task.object = tex
        task.filename = str(dest / (name + '_export.png'))
        task.automated = True
        task.exporter = unreal.TextureExporterPNG()
        unreal.AssetToolsHelpers.get_asset_tools().run_asset_export_task(task) if hasattr(
            unreal.AssetToolsHelpers.get_asset_tools(), 'run_asset_export_task') else None
        # Fallback: the editor-only export helper
        if not (dest / (name + '_export.png')).is_file():
            unreal.EditorAssetLibrary.save_package? = None
        out = dest / (name + '_export.png')
        REPORT['textures'][name] = {
            'export': str(out), 'exists': out.is_file(),
            'bytes': out.stat().st_size if out.is_file() else 0,
            'size': [tex.get_editor_property('source').get_editor_property('size_x'),
                     tex.get_editor_property('source').get_editor_property('size_y')]
            if True else None,
        }
    REPORT['success'] = True


try:
    main()
except Exception:
    import traceback
    REPORT['errors'].append(traceback.format_exc())
finally:
    ART.mkdir(parents=True, exist_ok=True)
    (ART / 'texture-dump.json').write_text(
        json.dumps(REPORT, ensure_ascii=False, indent=1, sort_keys=True), encoding='utf-8')
