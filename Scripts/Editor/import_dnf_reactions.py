"""Import existing authored hit/death FBX onto the runtime hero skeleton."""
import unreal,json,os,traceback
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir())
OUT=Path(os.environ['UEMMO_REPAIR_EVIDENCE'])
DEST='/Game/UEMMO/ArtValidation/DNFArt/Hero'
report={'success':False,'assets':[]}
try:
    unreal.SystemLibrary.execute_console_command(None,'Interchange.FeatureFlags.Import.FBX 0')
    skeleton=unreal.load_asset(DEST+'/hero_skm_Skeleton')
    for action in ['hit','death']:
        name='hero_'+action+'_Anim'
        path=DEST+'/'+name
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            ui=unreal.FbxImportUI()
            ui.set_editor_property('automated_import_should_detect_type',False)
            ui.set_editor_property('mesh_type_to_import',unreal.FBXImportType.FBXIT_ANIMATION)
            ui.set_editor_property('import_mesh',False)
            ui.set_editor_property('import_animations',True)
            ui.set_editor_property('skeleton',skeleton)
            ui.set_editor_property('import_materials',False)
            ui.set_editor_property('import_textures',False)
            task=unreal.AssetImportTask()
            task.set_editor_property('filename',str(ROOT/('SourceAssets/Unpacked/DNFPrivateReference/blender-work/hero_fighter/exports/hero_'+action+'.fbx')))
            task.set_editor_property('destination_path',DEST)
            task.set_editor_property('destination_name',name)
            task.set_editor_property('automated',True)
            task.set_editor_property('save',True)
            task.set_editor_property('options',ui)
            unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        asset=unreal.load_asset(path)
        assert isinstance(asset,unreal.AnimSequence),path
        assert asset.get_editor_property('skeleton')==skeleton,path
        report['assets'].append({'path':path,'length':asset.get_play_length(),'skeleton':skeleton.get_path_name()})
    report['success']=True
except Exception:report['error']=traceback.format_exc()
(OUT/'reaction-import.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
if not report['success']:raise RuntimeError(str(report))
