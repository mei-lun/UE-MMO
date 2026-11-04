"""Restore unkeyed bone channels to bind pose, preserving authored action tracks.

The Blender exporter switched actions without clearing non-keyed pose channels.
Their previous pose was baked into every frame. Use the original ACTIONS table
to identify channels owned by each action; never synthesize new choreography.
MODE=verify reloads without writing. Backups and numeric evidence are mandatory.
"""
import ast,json,os,shutil,traceback
from pathlib import Path
import unreal
ROOT=Path(unreal.Paths.project_dir())
OUT=Path(os.environ['UEMMO_REPAIR_EVIDENCE'])
OUT.mkdir(parents=True,exist_ok=True)
VERIFY=os.environ.get('UEMMO_REPAIR_MODE')=='verify'
SOURCE=ROOT/'SourceAssets/Unpacked/DNFPrivateReference/blender-work/scripts/art05_hero_actions.py'
tree=ast.parse(SOURCE.read_text(encoding='utf-8-sig'))
nodes=[n for n in tree.body if (isinstance(n,ast.FunctionDef) and n.name in ('guard_arms','idle_key'))
       or (isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='ACTIONS' for t in n.targets))]
scope={'__builtins__':{}}
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(SOURCE),'exec'),scope)
DEST='/Game/UEMMO/ArtValidation/DNFArt/Hero/'
APL=unreal.AnimPoseExtensions
LOCAL=unreal.AnimPoseSpaces.LOCAL
WORLD=unreal.AnimPoseSpaces.WORLD
EXPECTED={'idle','run_x','walk_y','jump_start','jump_fall','jump_land','attack_01','attack_02','launcher','aerial','singlekick','damagelowkick','throwsand','earthbreak','stomp','hit','death'}
report={'success':False,'verify_only':VERIFY,'source':str(SOURCE),'expected_actions':sorted(EXPECTED),'actions':[]}
def angular_close(a,b):
    return abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w)>0.99999
try:
    mesh=unreal.load_asset(DEST+'hero_skm')
    reference=APL.get_reference_pose(mesh.get_editor_property('skeleton'))
    names=APL.get_bone_names(reference)
    opts=unreal.AnimPoseEvaluationOptions()
    opts.set_editor_property('should_retarget',False)
    for spec in scope['ACTIONS']:
        if spec['id'] not in EXPECTED:continue
        path=DEST+'hero_'+spec['id']+'_Anim'
        seq=unreal.load_asset(path)
        assert isinstance(seq,unreal.AnimSequence),('required animation missing',path)
        authored={b for frame in spec['keys'].values() for b in frame}
        length=seq.get_play_length()
        keys=unreal.AnimationLibrary.get_num_keys(seq)
        poses=[APL.get_anim_pose_at_time(seq,length*i/(keys-1),opts) for i in range(keys)]
        fixed=[]
        if not VERIFY:
            disk=ROOT/('Content/'+path.split('/Game/',1)[1]+'.uasset')
            backup=OUT/'before-assets'/disk.name
            backup.parent.mkdir(parents=True,exist_ok=True)
            if not backup.exists():shutil.copy2(disk,backup)
            controller=seq.get_editor_property('controller')
            controller.open_bracket('Clear unkeyed DNF action residue',False)
        try:
            for bone in names:
                b=str(bone)
                ref=APL.get_bone_pose(reference,bone,LOCAL)
                ts=[APL.get_bone_pose(p,bone,LOCAL) for p in poses]
                rotations_owned=b in authored and b!='pelvis'
                translation_owned=b=='pelvis' and b in authored
                bad_rot=not rotations_owned and any(not angular_close(t.rotation,ref.rotation) for t in ts)
                bad_pos=not translation_owned and any((t.translation-ref.translation).length()>0.01 for t in ts)
                if VERIFY:
                    assert not bad_rot and not bad_pos,(spec['id'],b,'residual channel',bad_rot,bad_pos)
                elif bad_rot or bad_pos:
                    pos=[t.translation if translation_owned else ref.translation for t in ts]
                    rot=[t.rotation if rotations_owned else ref.rotation for t in ts]
                    scales=[t.scale3d for t in ts]
                    assert controller.set_bone_track_keys(bone,pos,rot,scales,False),(path,b)
                    fixed.append(b)
        finally:
            if not VERIFY:controller.close_bracket(False)
        if not VERIFY:assert unreal.EditorAssetLibrary.save_loaded_asset(seq,False),path
        final=APL.get_anim_pose_at_time(seq,0.,opts)
        feet=[APL.get_bone_pose(final,b,WORLD).translation.z for b in ['foot_l','foot_r']]
        head=APL.get_bone_pose(final,'head',WORLD).translation.z
        if spec['id']=='idle':assert min(feet)<15 and head>150,('idle grounding',feet,head)
        report['actions'].append({'id':spec['id'],'keys':keys,'fixed_bones':fixed,'foot_z':feet,'head_z':head})
    assert {a['id'] for a in report['actions']}==EXPECTED,'Incomplete required animation set'
    report['success']=True
except Exception:report['error']=traceback.format_exc()
(OUT/('verify.json' if VERIFY else 'repair.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
if not report['success']:raise RuntimeError(report['error'])
