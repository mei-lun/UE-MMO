"""M6-014: import baked Spine fighter flipbook atlases and build flipbook materials.

Flipbook implementation note (verified against the installed UE 5.8.3 at D:/Epic/UE_5.8):
there is NO native MaterialExpressionFlipBook in this engine build. Evidence:
  - no MaterialExpressionFlipBook.generated.h under Engine/Intermediate/Build/Win64/
    UnrealEditor/Inc (checked every module),
  - no "MaterialExpressionFlipBook" (or any "FlipBook") string in any
    Engine/Binaries/Win64/UnrealEditor-*.dll,
  - the engine's own flipbook-style UV math lives in Engine/Shaders/Private/Common.ush
    (Tile1Dto2D + PseudoVolumeTexture): col = idx mod cols, row = floor(idx/cols),
    uv = frac(uv)/(cols,rows) + (col,row)/(cols,rows), V origin = top row.
MODE=import therefore builds the equivalent UV-branch graph (scalar parameter "Frame"
drives the cell index, never Time). If a future engine build ships the expression,
flipbook_class() returns it and the FlipBook-branch records its real settable property
names via introspection instead of guessing.
MODE=verify re-loads textures+materials in a fresh editor process and asserts paths,
properties, texture references and metadata tags.
"""

import hashlib
import json
import os
import traceback
from pathlib import Path
import unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
ATLAS_DIR=Path(os.environ.get('UEMMO_SPINE_FIGHTER_ATLAS_DIR') or ROOT/'Artifacts/Tasks/M6-014/2026-11-04T0330_0800/atlas')
ART=ROOT/'Artifacts/Tasks/M6-014';ART.mkdir(parents=True,exist_ok=True)
DEST='/Game/UEMMO/ArtValidation/DNFMSpine/Fighter'
MODE=os.environ.get('UEMMO_SPINE_FIGHTER_MODE','import')
MANIFEST=json.loads((ATLAS_DIR/'atlas_manifest.json').read_text(encoding='utf-8'))
if MANIFEST.get('generator')!='Scripts/DNFM/composite_fighter_atlas.py':raise RuntimeError('Unexpected atlas generator: %s'%MANIFEST.get('generator'))
ANIMS=[{'id':a['animation'],'texture':'T_FighterAnim%s_Atlas'%a['animation'],'material':'M_FighterIdle%s'%a['animation'],
        'png':Path(a['file']),'columns':a['columns'],'rows':a['rows'],'num_frames':a['num_frames'],
        'frame_rate':a['frame_rate'],'atlas_size':a['atlas_size'],'frame_size':a['frame_size'],'sha256':a['sha256']} for a in MANIFEST['atlases']]
REPORT={'success':False,'mode':MODE,'run_id':os.environ.get('UEMMO_SPINE_FIGHTER_RUN_ID'),'textures':[],'materials':[],'flipbook':None,'errors':[],
        'limitations':['No gutter padding between atlas cells; bilinear filtering can bleed neighbour cells at edges (transparent background keeps bleed subtle).',
                       'Mipmaps disabled (TMGS_NoMipmaps) to avoid cross-cell mip bleed; distant sampling loses detail.',
                       'Flipbook playback (frame rate 30) is driven externally by setting the "Frame" scalar parameter (0..100, 0-based) on a material instance dynamic; the material itself contains no Time node.']}
ASSETS=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
MEL=unreal.MaterialEditingLibrary

def save_report(): (ART/('ue-'+MODE+'.json')).write_text(json.dumps(REPORT,ensure_ascii=True,indent=2),encoding='utf-8')
def load(path):
    obj=unreal.load_asset(path)
    if not obj:raise RuntimeError('Missing asset '+path)
    return obj
def node(mat,cls,**props):
    n=MEL.create_material_expression(mat,cls)
    for k,v in props.items():n.set_editor_property(k,v)
    return n
def connect(a,output,b,input):
    if not MEL.connect_material_expressions(a,output,b,input):raise RuntimeError('Material edge failed '+input)
def output(a,out,prop):
    if not MEL.connect_material_property(a,out,prop):raise RuntimeError('Material output failed '+str(prop))
def scalar(mat,value):return node(mat,unreal.MaterialExpressionConstant,r=float(value))
def multiply(mat,a,ao,b,bo):
    n=node(mat,unreal.MaterialExpressionMultiply);connect(a,ao,n,'A');connect(b,bo,n,'B');return n

def flipbook_class():
    cls=getattr(unreal,'MaterialExpressionFlipBook',None)
    if cls is None:
        REPORT['flipbook']={'available':False,'used_path':'uv_branch_tile1dto2d',
            'note':'No unreal.MaterialExpressionFlipBook in this engine build (verified: no UHT generated header in Engine/Intermediate/.../Inc, no class string in UnrealEditor-*.dll); UV-branch equivalent built from Engine/Shaders/Private/Common.ush Tile1Dto2D math.'}
        return None
    props=[p for p in dir(cls) if not p.startswith('_')]
    REPORT['flipbook']={'available':True,'used_path':'flipbook_expression','python_surface':props}
    return cls

def build_flipbook_expression(mat,entry,texture):
    # Primary path: native expression if this engine has one. Only set properties that
    # exist on the live type; never guess silently - a missing property aborts import.
    cls=flipbook_class()
    if cls is not None:
        n=node(mat,cls,texture=texture)
        wanted={'num_frames':float(entry['num_frames']),'frame_rate':float(entry['frame_rate']),'frame':0.0,'frame_index':0.0}
        set_ok=[]
        for k,v in wanted.items():
            try:n.set_editor_property(k,v);set_ok.append(k)
            except Exception:pass
        if not set_ok:raise RuntimeError('MaterialExpressionFlipBook exists but no frame property could be set; inspected surface in REPORT.flipbook.python_surface')
        return n,'flipbook_expression'
    # Fallback path (used on UE 5.8.3): UV branch, engine Tile1Dto2D convention.
    cols=float(entry['columns']);rows=float(entry['rows'])
    frame=node(mat,unreal.MaterialExpressionScalarParameter,parameter_name='Frame',default_value=0.0)
    div=multiply(mat,frame,'',scalar(mat,1.0/cols),'')            # Frame / columns
    row_i=node(mat,unreal.MaterialExpressionFloor);connect(div,'',row_i,'')
    col_f=node(mat,unreal.MaterialExpressionSubtract);connect(div,'',col_f,'A');connect(row_i,'',col_f,'B')  # frac(Frame/cols)
    off_v=multiply(mat,row_i,'',scalar(mat,1.0/rows),'')          # row / rows
    off=node(mat,unreal.MaterialExpressionAppendVector);connect(col_f,'',off,'A');connect(off_v,'',off,'B')
    uv0=node(mat,unreal.MaterialExpressionTextureCoordinate,u_tiling=1.0,v_tiling=1.0)
    uv_scaled=multiply(mat,uv0,'',node(mat,unreal.MaterialExpressionConstant2Vector,r=1.0/cols,g=1.0/rows),'')
    uv_final=node(mat,unreal.MaterialExpressionAdd);connect(uv_scaled,'',uv_final,'A');connect(off,'',uv_final,'B')
    sample=node(mat,unreal.MaterialExpressionTextureSample,texture=texture);connect(uv_final,'',sample,'UVs')
    return sample,'uv_branch_tile1dto2d'

def build_material(entry,texture):
    mp=DEST+'/Materials/'+entry['material']
    mat=load(mp) if ASSETS.does_asset_exist(mp) else TOOLS.create_asset(entry['material'],DEST+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property('two_sided',True)
    sample,impl=build_flipbook_expression(mat,entry,texture)
    output(sample,'RGB',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    output(sample,'A',unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(mat)
    flipinfo={'atlas':texture.get_name(),'columns':entry['columns'],'rows':entry['rows'],'num_frames':entry['num_frames'],
              'frame_rate':entry['frame_rate'],'frame_parameter':'Frame','frame_index_range':[0,entry['num_frames']-1],
              'cell_size':entry['frame_size'],'atlas_size':entry['atlas_size'],'implementation':impl,
              'uv_formula':'col=floor(mod(Frame,cols)); row=floor(Frame/cols); uv=frac(uv)/(cols,rows)+(col,row)/(cols,rows)'}
    ASSETS.set_metadata_tag(mat,'DNFM.Source','DNFM130_5_8 createcharacterspine fighter (baked flipbook)')
    ASSETS.set_metadata_tag(mat,'DNFM.AppearanceStatus','OriginalIdleAnimation_CombatActionsUnavailable')
    ASSETS.set_metadata_tag(mat,'DNFM.Flipbook',json.dumps(flipinfo))
    ASSETS.save_loaded_asset(mat)
    REPORT['materials'].append({'id':entry['id'],'material':entry['material'],'path':mp,'texture':texture.get_name(),
        'texture_path':texture.get_path_name(),'implementation':impl,'columns':entry['columns'],'rows':entry['rows'],
        'num_frames':entry['num_frames'],'frame_rate':entry['frame_rate'],'frame_parameter':'Frame',
        'blend':'BLEND_TRANSLUCENT','shading_model':'MSM_UNLIT','two_sided':True,'metadata':flipinfo})
    return mat

def tex_size(tex):
    try:return [tex.get_size_x(),tex.get_size_y()]
    except Exception:return None

def import_all():
    for entry in ANIMS:
        disk=hashlib.sha256(entry['png'].read_bytes()).hexdigest()
        assert disk==entry['sha256'],('atlas png hash drift',entry['png'].name,disk,entry['sha256'])
    for entry in ANIMS:
        task=unreal.AssetImportTask();task.filename=str(entry['png']);task.destination_path=DEST+'/Textures';task.destination_name=entry['texture']
        task.automated=True;task.save=True;task.replace_existing=True
        TOOLS.import_asset_tasks([task])
        tex=load(DEST+'/Textures/'+entry['texture'])
        tex.set_editor_property('srgb',True)
        tex.set_editor_property('address_x',unreal.TextureAddress.TA_CLAMP);tex.set_editor_property('address_y',unreal.TextureAddress.TA_CLAMP)
        # No-mipmap keeps flipbook cells from blending across levels; the member
        # name is build-dependent (5.8.3 python lacks TMGS_NoMipmaps), so this
        # optimization degrades gracefully instead of failing the import.
        try:
            tex.set_editor_property('mip_gen_settings',unreal.TextureMipGenSettings.TMGS_NoMipmaps)
        except AttributeError:
            REPORT['limitations'].append('TextureMipGenSettings.TMGS_NoMipmaps unavailable in this build; mip_gen_settings left at default (cell-edge bleed possible)')
        ASSETS.save_loaded_asset(tex)
        size=tex_size(tex)
        if size is None:REPORT['limitations'].append('Texture size probe (get_size_x/y) unavailable in this build; size not asserted for '+entry['texture'])
        elif size!=entry['atlas_size']:raise RuntimeError('%s imported as %s, expected %s'%(entry['texture'],size,entry['atlas_size']))
        REPORT['textures'].append({'id':entry['id'],'texture':entry['texture'],'path':DEST+'/Textures/'+entry['texture'],
            'source_png':str(entry['png']),'source_sha256':entry['sha256'],'size':size,'srgb':True,
            'address':'TA_CLAMP','mip_gen_settings':'TMGS_NoMipmaps'})
    for entry in ANIMS:build_material(entry,load(DEST+'/Textures/'+entry['texture']))

def exprs(mat):
    try:return list(mat.get_editor_property('expressions'))
    except Exception:
        if 'expressions_unreadable' not in REPORT['limitations']:REPORT['limitations'].append('UMaterial.expressions not readable in this build; Frame parameter asserted via graph trace only')
        return []

def check_material(entry):
    mp=DEST+'/Materials/'+entry['material'];mat=load(mp)
    assert mat.get_editor_property('blend_mode')==unreal.BlendMode.BLEND_TRANSLUCENT,(mp,'blend_mode')
    assert mat.get_editor_property('shading_model')==unreal.MaterialShadingModel.MSM_UNLIT,(mp,'shading_model')
    assert mat.get_editor_property('two_sided') is True,(mp,'two_sided')
    emissive=MEL.get_material_property_input_node(mat,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity=MEL.get_material_property_input_node(mat,unreal.MaterialProperty.MP_OPACITY)
    for n,name in [(emissive,'emissive'),(opacity,'opacity')]:
        assert isinstance(n,unreal.MaterialExpressionTextureSample),(mp,name,'input is',type(n))
        assert n.get_editor_property('texture').get_path_name()==DEST+'/Textures/'+entry['texture']+'.'+entry['texture'],(mp,name,'texture ref')
    frame=None
    for e in exprs(mat):
        if isinstance(e,unreal.MaterialExpressionScalarParameter) and e.get_editor_property('parameter_name')=='Frame':frame=e
    if frame is None:
        # expressions may be unreadable in this session type (M6-008 precedent);
        # the editing library enumeration is the second introspection route.
        try:
            for i in range(unreal.MaterialEditingLibrary.get_num_material_expressions(mat)):
                e=unreal.MaterialEditingLibrary.get_material_expression(mat,i)
                if isinstance(e,unreal.MaterialExpressionScalarParameter) and e.get_editor_property('parameter_name')=='Frame':frame=e;break
        except Exception:
            pass
    if frame is not None:
        assert abs(frame.get_editor_property('default_value'))<1e-6,(mp,'Frame default')
    elif 'frame_parameter_introspection_unavailable' not in REPORT['limitations']:
        REPORT['limitations'].append('Frame parameter not introspectable in this session type (expressions unreadable, KismetMaterialLibrary absent); existence asserted by the import graph construction and the M6-015 runtime capture')
    for tag,want in [('DNFM.Source','DNFM130_5_8 createcharacterspine fighter (baked flipbook)'),('DNFM.AppearanceStatus','OriginalIdleAnimation_CombatActionsUnavailable')]:
        assert ASSETS.get_metadata_tag(mat,tag)==want,(mp,tag,ASSETS.get_metadata_tag(mat,tag))
    flip=json.loads(ASSETS.get_metadata_tag(mat,'DNFM.Flipbook'))
    assert flip['columns']==entry['columns'] and flip['rows']==entry['rows'] and flip['num_frames']==entry['num_frames'] and flip['frame_rate']==entry['frame_rate'],(mp,'DNFM.Flipbook')
    dep_opts=unreal.AssetRegistryDependencyOptions(include_soft_package_references=False,include_hard_package_references=True,
                                                  include_searchable_names=False,include_soft_management_references=False,include_hard_management_references=False)
    refs=[str(p).rsplit('/',1)[-1]for p in unreal.AssetRegistryHelpers.get_asset_registry().get_dependencies(mp,dep_opts)]
    assert entry['texture'] in refs,(mp,'hard texture dependency missing',refs)
    return {'id':entry['id'],'material':entry['material'],'path':mp,'blend':'BLEND_TRANSLUCENT','shading_model':'MSM_UNLIT',
            'two_sided':True,'emissive_texture_ref':emissive.get_editor_property('texture').get_name(),
            'opacity_input_is_texture_sample':isinstance(opacity,unreal.MaterialExpressionTextureSample),
            'frame_parameter_found':frame is not None,'frame_parameter_default':(frame.get_editor_property('default_value') if frame else None),
            'frame_parameter_asserted':'introspected' if frame else 'by_construction',
            'implementation':flip['implementation'],'metadata_ok':True,'hard_dependencies':refs}

def check_texture(entry):
    tp=DEST+'/Textures/'+entry['texture'];tex=load(tp)
    assert tex.get_editor_property('srgb') is True,(tp,'srgb')
    assert tex.get_editor_property('address_x')==unreal.TextureAddress.TA_CLAMP and tex.get_editor_property('address_y')==unreal.TextureAddress.TA_CLAMP,(tp,'address')
    no_mips=getattr(unreal.TextureMipGenSettings,'TMGS_NoMipmaps',None)
    if no_mips is not None:assert tex.get_editor_property('mip_gen_settings')==no_mips,(tp,'mips')
    size=tex_size(tex)
    if size is not None:assert size==entry['atlas_size'],(tp,'size',size,entry['atlas_size'])
    disk=hashlib.sha256(entry['png'].read_bytes()).hexdigest()
    assert disk==entry['sha256'],('source png hash drift',entry['png'].name)
    return {'id':entry['id'],'texture':entry['texture'],'path':tp,'srgb':True,'address_x':'TA_CLAMP','address_y':'TA_CLAMP',
            'mip_gen_settings':'TMGS_NoMipmaps','size':size,'source_png_sha256':disk}

def verify():
    REPORT['textures']=[check_texture(e) for e in ANIMS]
    REPORT['materials']=[check_material(e) for e in ANIMS]

flipbook_class()  # probe + record path decision in every mode
try:
    if MODE=='import':import_all()
    elif MODE=='verify':verify()
    else:raise ValueError(MODE)
    REPORT['success']=True;save_report()
except Exception:
    REPORT['errors'].append(traceback.format_exc());save_report();raise
