"""Unify structurally identical imported hero action skeletons and their montages."""
import unreal,json,os,shutil,traceback
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir())
OUT=Path(os.environ['UEMMO_REPAIR_EVIDENCE'])
VERIFY=os.environ.get('UEMMO_REPAIR_MODE')=='verify'
DEST='/Game/UEMMO/ArtValidation/DNFArt/Hero/'
report={'success':False,'verify_only':VERIFY,'assets':[]}
try:
    canonical=unreal.load_asset(DEST+'hero_skm_Skeleton')
    paths=sorted(unreal.EditorAssetLibrary.list_assets(DEST,True,False))
    rows=json.loads((ROOT/'Data/combat-attacks.json').read_text(encoding='utf-8-sig'))['attacks']
    paths += ['/Game/UEMMO/Animation/Montages/MNT_'+r['attack_id'] for r in rows]
    for path in paths:
        asset=unreal.load_asset(path)
        if not isinstance(asset,(unreal.AnimSequence,unreal.AnimMontage)):continue
        old=asset.get_editor_property('skeleton').get_path_name()
        if not VERIFY:
            disk=ROOT/('Content/'+path.split('/Game/',1)[1].split('.')[0]+'.uasset')
            backup=OUT/'before-unify'/disk.name
            backup.parent.mkdir(parents=True,exist_ok=True)
            if not backup.exists():shutil.copy2(disk,backup)
            assert unreal.DNFAnimationRepairLibrary.use_canonical_hero_skeleton(asset),path
            assert unreal.EditorAssetLibrary.save_loaded_asset(asset,False),path
        assert asset.get_editor_property('skeleton')==canonical,path
        report['assets'].append({'path':path,'old_skeleton':old,'canonical':canonical.get_path_name()})
    report['success']=len(report['assets'])>=24
except Exception:report['error']=traceback.format_exc()
(OUT/('unify-verify.json' if VERIFY else 'unify.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
if not report['success']:raise RuntimeError(str(report))
