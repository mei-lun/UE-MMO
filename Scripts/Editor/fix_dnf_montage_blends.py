"""Short hero clips need short transitions, not factory 0.25s defaults."""
import json,os,shutil,traceback
from pathlib import Path
import unreal
ROOT=Path(unreal.Paths.project_dir())
OUT=Path(os.environ['UEMMO_REPAIR_EVIDENCE'])
verify=os.environ.get('UEMMO_REPAIR_MODE')=='verify'
report={'success':False,'verify_only':verify,'montages':[]}
try:
 rows=json.loads((ROOT/'Data/combat-attacks.json').read_text(encoding='utf-8-sig'))['attacks']
 paths=['/Game/UEMMO/Animation/Montages/MNT_'+r['attack_id'] for r in rows]
 paths+=['/Game/UEMMO/Animation/Reactions/RCT_Hero_Hit','/Game/UEMMO/Animation/Reactions/RCT_Hero_Dead']
 for path in paths:
  m=unreal.load_asset(path)
  assert m and m.get_editor_property('skeleton').get_name()=='hero_skm_Skeleton',path
  target=min(0.05,m.get_play_length()*0.15)
  old={}
  if not verify:
   disk=ROOT/('Content/'+path.split('/Game/',1)[1]+'.uasset');backup=OUT/'before-blends'/disk.name
   backup.parent.mkdir(parents=True,exist_ok=True)
   if not backup.exists():shutil.copy2(disk,backup)
  for name in ['blend_in','blend_out']:
   blend=m.get_editor_property(name);old[name]=blend.get_editor_property('blend_time')
   if not verify:
    blend.set_editor_property('blend_time',target);m.set_editor_property(name,blend)
   assert abs(m.get_editor_property(name).get_editor_property('blend_time')-target)<1e-5,(path,name)
  if not verify:assert unreal.EditorAssetLibrary.save_loaded_asset(m,False),path
  report['montages'].append({'path':path,'old':old,'target_seconds':target})
 report['success']=True
except Exception:report['error']=traceback.format_exc()
(OUT/('blends-verify.json' if verify else 'blends-repair.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
if not report['success']:raise RuntimeError(str(report))
