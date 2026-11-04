import unreal,json,os,traceback
from pathlib import Path
OUT=Path(os.environ['UEMMO_REPAIR_EVIDENCE'])
verify=os.environ.get('UEMMO_REPAIR_MODE')=='verify'
report={'success':False,'verify_only':verify,'assets':[]}
try:
    sk=unreal.load_asset('/Game/UEMMO/ArtValidation/DNFArt/Hero/hero_skm_Skeleton')
    for name,action in [('RCT_Hero_Hit','hit'),('RCT_Hero_Dead','death')]:
        source=unreal.load_asset('/Game/UEMMO/ArtValidation/DNFArt/Hero/hero_'+action+'_Anim')
        path='/Game/UEMMO/Animation/Reactions/'+name
        if not verify and not unreal.EditorAssetLibrary.does_asset_exist(path):
            factory=unreal.AnimMontageFactory();factory.set_editor_property('source_animation',source)
            m=unreal.AssetToolsHelpers.get_asset_tools().create_asset(name,'/Game/UEMMO/Animation/Reactions',unreal.AnimMontage,factory)
            assert unreal.EditorAssetLibrary.save_loaded_asset(m,False),path
        m=unreal.load_asset(path)
        assert m and m.get_editor_property('skeleton')==sk,path
        seg=m.get_editor_property('slot_anim_tracks')[0].get_editor_property('anim_track').get_editor_property('anim_segments')[0]
        assert seg.get_editor_property('anim_reference')==source,path
        report['assets'].append({'path':path,'skeleton':sk.get_path_name(),'length':m.get_play_length()})
    report['success']=True
except Exception:report['error']=traceback.format_exc()
(OUT/('reactions-verify.json' if verify else 'reactions.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
if not report['success']:raise RuntimeError(str(report))
