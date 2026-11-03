"""M6-009: import/reload/capture DNFM static assets through UE editor APIs."""
import json
import math
import os
import time
import traceback
from pathlib import Path
import unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
STAGE=ROOT/'SourceAssets/Unpacked/DNFM130_5_8'
ART=ROOT/'Artifacts/Tasks/M6-009';ART.mkdir(parents=True,exist_ok=True)
DEST='/Game/UEMMO/Imported/DNFM130_5_8'
MAP=DEST+'/Maps/L_DNFM_ResourceGallery'
MODE=os.environ.get('UEMMO_DNFM_MODE','import')
LIMIT=int(os.environ.get('UEMMO_DNFM_LIMIT','0'))
DATA=json.loads((STAGE/'manifest.json').read_text())
if not DATA.get('ready_for_ue_import'):raise RuntimeError('Source manifest is bootstrap-only; complete cross-bundle scan and prepare again.')
ROWS=DATA['meshes'][:LIMIT]if LIMIT else DATA['meshes']
REPORT={'success':False,'mode':MODE,'run_id':os.environ.get('UEMMO_DNFM_RUN_ID'),'meshes':[],'materials':[],'textures':[],'errors':[],
        'limitations':['Custom Unity shaders are approximated with UE material graphs; original parameter JSON retained.',
                       'Static mesh migration only; animation curves/Spine/particle simulations are not converted.',
                       'Source numerical units interpreted as meters; gallery actors scaled for inspection.']}
ASSETS=unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
MEL=unreal.MaterialEditingLibrary

def save_report(): (ART/('ue-'+MODE+'.json')).write_text(json.dumps(REPORT,ensure_ascii=True,indent=2),encoding='utf-8')
def load(path):
    obj=unreal.load_asset(path)
    if not obj:raise RuntimeError('Missing asset '+path)
    return obj
def path(kind,name):return DEST+'/'+kind+'/'+name
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
def texture_node(mat,entry,normal=False):
    n=node(mat,unreal.MaterialExpressionTextureSample,texture=load(path('Textures',entry['id'])))
    if normal:n.set_editor_property('sampler_type',unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    scale=entry.get('scale',{'x':1,'y':1});offset=entry.get('offset',{'x':0,'y':0})
    uv=node(mat,unreal.MaterialExpressionTextureCoordinate,u_tiling=scale['x'],v_tiling=scale['y'])
    # FBX importer flips V. Unity v'=v*scale+offset becomes
    # UE v'=v*scale+(1-scale-offset), not the same offset.
    offset_ue={'x':offset['x'],'y':1-scale['y']-offset['y']}
    if offset_ue['x']or offset_ue['y']:
        add=node(mat,unreal.MaterialExpressionAdd);off=node(mat,unreal.MaterialExpressionConstant2Vector,r=offset_ue['x'],g=offset_ue['y'])
        connect(uv,'',add,'A');connect(off,'',add,'B');uv=add
    connect(uv,'',n,'UVs');return n

def build_material(row):
    name=row['id'];mp=path('Materials',name)
    mat=load(mp)if ASSETS.does_asset_exist(mp)else TOOLS.create_asset(name,DEST+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    shader=(row.get('shader')or'').lower();f=row.get('floats',{});cs=row.get('colors',{});ts=row.get('textures',{})
    missing_base=any(k in row.get('missing_textures',{})for k in ['_MainTex','_BaseMap','_BaseColorMap','_DiffuseTex'])
    standard=shader in ['standard','autodesk interactive']or missing_base
    unlit=not standard
    additive='additive'in shader or f.get('_DstBlend')==1
    masked=f.get('_Mode')==1 or row.get('tags',{}).get('RenderType')=='TransparentCutout'
    translucent=f.get('_Mode')in [2,3]or row.get('render_queue',0)>=3000 or 'transparent'in shader or 'alpha blend'in shader or (f.get('_SrcBlend')==5 and f.get('_DstBlend')==10)
    blend=unreal.BlendMode.BLEND_ADDITIVE if additive else unreal.BlendMode.BLEND_MASKED if masked else unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_OPAQUE
    if missing_base:blend=unreal.BlendMode.BLEND_OPAQUE
    mat.set_editor_property('blend_mode',blend)
    mat.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_UNLIT if unlit else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('two_sided',unlit or f.get('_Cull')==0)
    mat.set_editor_property('opacity_mask_clip_value',float(f.get('_Cutoff',0.333)))
    color=cs.get('_Color',cs.get('_BaseColor',{'r':1,'g':1,'b':1,'a':1}))
    if missing_base:color={'r':0.38,'g':0.42,'b':0.5,'a':1}
    tint=node(mat,unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(color['r'],color['g'],color['b'],1))
    base=next((ts[k]for k in ['_MainTex','_BaseMap','_BaseColorMap','_DiffuseTex']if k in ts),None)
    rgb=tint;alpha=scalar(mat,color.get('a',1));tex=None
    if base:
        tex=texture_node(mat,base);rgb=multiply(mat,tex,'RGB',tint,'');alpha=multiply(mat,tex,'A',alpha,'')
    if unlit:
        vc=node(mat,unreal.MaterialExpressionVertexColor)
        rgb=multiply(mat,rgb,'',vc,'');alpha=multiply(mat,alpha,'',vc,'A')
    output(rgb,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR if unlit else unreal.MaterialProperty.MP_BASE_COLOR)
    if blend==unreal.BlendMode.BLEND_MASKED:output(alpha,'',unreal.MaterialProperty.MP_OPACITY_MASK)
    elif blend in [unreal.BlendMode.BLEND_TRANSLUCENT,unreal.BlendMode.BLEND_ADDITIVE]:output(alpha,'',unreal.MaterialProperty.MP_OPACITY)
    if not unlit:
        output(scalar(mat,1-f.get('_Glossiness',0)),'',unreal.MaterialProperty.MP_ROUGHNESS)
        output(scalar(mat,f.get('_Metallic',0)),'',unreal.MaterialProperty.MP_METALLIC)
        if '_BumpMap'in ts:output(texture_node(mat,ts['_BumpMap'],True),'RGB',unreal.MaterialProperty.MP_NORMAL)
        ec=cs.get('_EmissionColor',{})
        if any(ec.get(k,0)for k in ['r','g','b']):
            emission=node(mat,unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(ec.get('r',0),ec.get('g',0),ec.get('b',0),1))
            if '_EmissionMap'in ts:emission=multiply(mat,texture_node(mat,ts['_EmissionMap']),'RGB',emission,'')
            output(emission,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    ASSETS.set_metadata_tag(mat,'DNFM.SourceShader',row.get('shader',''))
    ASSETS.set_metadata_tag(mat,'DNFM.AppearanceStatus','MissingBaseTexture_NeutralPlaceholder'if missing_base else 'ApproximateUnityShader')
    ASSETS.set_metadata_tag(mat,'DNFM.MissingTextures',json.dumps(row.get('missing_textures',{})))
    implemented={'_MainTex','_BaseMap','_BaseColorMap','_DiffuseTex','_BumpMap','_EmissionMap'}
    omitted=[slot for slot in ts if slot not in implemented]
    ASSETS.set_metadata_tag(mat,'DNFM.UnimplementedTextureInputs',json.dumps(omitted))
    MEL.recompile_material(mat);ASSETS.save_loaded_asset(mat)
    REPORT['materials'].append({'id':name,'shader_source':row.get('shader'),'unlit':unlit,'blend':str(blend),'base_texture':base['id']if base else None,'missing_base_placeholder':missing_base,'unimplemented_texture_inputs':omitted})
    return mat

def import_all():
    unreal.SystemLibrary.execute_console_command(None,'Interchange.FeatureFlags.Import.FBX 0')
    normal_ids={entry['id']for m in DATA['materials']for slot,entry in m['textures'].items()if slot=='_BumpMap'}
    for row in DATA['textures']:
        t=unreal.AssetImportTask();t.filename=row['file'];t.destination_path=DEST+'/Textures';t.destination_name=row['id'];t.automated=True;t.save=True;t.replace_existing=True
        TOOLS.import_asset_tasks([t]);texture=load(path('Textures',row['id']))
        texture.set_editor_property('srgb',row['id']not in normal_ids)
        wrap={0:unreal.TextureAddress.TA_WRAP,1:unreal.TextureAddress.TA_CLAMP,2:unreal.TextureAddress.TA_MIRROR}
        texture.set_editor_property('address_x',wrap[row.get('wrap_u',0)])
        texture.set_editor_property('address_y',wrap[row.get('wrap_v',0)])
        if row.get('filter_mode')==0:texture.set_editor_property('filter',unreal.TextureFilter.TF_NEAREST)
        if row['id']in normal_ids:texture.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP)
        ASSETS.save_loaded_asset(texture);REPORT['textures'].append({'id':row['id']})
    mats={row['id']:build_material(row)for row in DATA['materials']}
    neutral=build_material({'id':'M_Unassigned','shader':'Standard','colors':{'_Color':{'r':0.55,'g':0.55,'b':0.55,'a':1}}})
    for i,row in enumerate(ROWS):
        task=unreal.AssetImportTask();task.filename=str(STAGE/'fbx'/(row['id']+'.fbx'))
        task.destination_path=DEST+'/Meshes';task.destination_name=row['id'];task.automated=True;task.save=True;task.replace_existing=True
        opts=unreal.FbxImportUI();opts.import_mesh=True;opts.import_as_skeletal=False;opts.import_materials=False;opts.import_textures=False;opts.import_animations=False
        opts.automated_import_should_detect_type=False;opts.mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH
        imp=opts.static_mesh_import_data;imp.combine_meshes=True;imp.convert_scene=True;imp.convert_scene_unit=True
        imp.generate_lightmap_u_vs=False;imp.auto_generate_collision=False;imp.remove_degenerates=False
        imp.vertex_color_import_option=unreal.VertexColorImportOption.REPLACE
        imp.normal_import_method=unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS_AND_TANGENTS
        task.options=opts;task.factory=unreal.FbxFactory();TOOLS.import_asset_tasks([task])
        mesh=load(path('Meshes',row['id']))
        settings=SM.get_lod_build_settings(mesh,0)
        settings.recompute_normals=False;settings.recompute_tangents=False;settings.remove_degenerates=False;settings.use_full_precision_u_vs=True
        SM.set_lod_build_settings(mesh,0,settings)
        selected=row['selected_materials']
        for slot in range(row['submesh_count']):mesh.set_material(slot,mats.get(selected[slot]if slot<len(selected)else None,neutral))
        ASSETS.set_metadata_tag(mesh,'DNFM.Source',row['source_object'])
        ASSETS.set_metadata_tag(mesh,'DNFM.MaterialVariants',json.dumps(row['material_variants']))
        ASSETS.set_metadata_tag(mesh,'DNFM.UnitConvention','Unity numeric units interpreted as meters; FBX imported centimeters.')
        # Accurate source surfaces without automatic convex approximation; no physics simulation promise.
        body=mesh.get_editor_property('body_setup')
        if body:body.set_editor_property('collision_trace_flag',unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        ASSETS.save_loaded_asset(mesh)
        REPORT['meshes'].append(check_mesh(row,mesh))
        if i%25==0:unreal.log_warning('[DNFM] imported %d/%d'%(i+1,len(ROWS)))

def check_mesh(row,mesh):
    tris=mesh.get_num_triangles(0);uv=SM.get_num_uv_channels(mesh,0);colors=SM.has_vertex_colors(mesh)
    assert tris==row['triangle_count'],(row['id'],'triangles',tris,row['triangle_count'])
    assert uv==max(1,len(row['uv_channels'])),(row['id'],'UV',uv,row['uv_channels'])
    if row['has_vertex_colors']:
        geometry=json.loads(Path(row['geometry']).read_text())
        nonwhite=any(abs(float(c)-1)>0.005 for v in geometry['colors']for c in v)
        if nonwhite:assert colors,(row['id'],'vertex colors missing')
    bounds=mesh.get_bounds().box_extent
    extent=sorted([bounds.x*2,bounds.y*2,bounds.z*2]);expected=sorted(v*100 for v in row['extent_unity_units'])
    assert all(abs(a-b)<max(0.05,abs(b)*0.002)for a,b in zip(extent,expected)),(row['id'],'scale',extent,expected)
    slots=[]
    for slot in range(row['submesh_count']):
        mat=mesh.get_material(slot);assert mat,(row['id'],'null material',slot);slots.append(mat.get_path_name())
        intended=row['selected_materials'][slot]if slot<len(row['selected_materials'])else None
        assert mat.get_name()==(intended or 'M_Unassigned'),(row['id'],'material mismatch',slot)
    return {'id':row['id'],'triangles':tris,'uv_channels':uv,'has_vertex_colors':colors,
            'source_has_vertex_colors':row['has_vertex_colors'],'extent_cm':extent,'materials':slots,'asset':mesh.get_path_name()}

def verify():
    for row in ROWS:REPORT['meshes'].append(check_mesh(row,load(path('Meshes',row['id']))))
    for row in DATA['materials']:
        mat=load(path('Materials',row['id']))
        dep_opts=unreal.AssetRegistryDependencyOptions(include_soft_package_references=False,include_hard_package_references=True,
                                                      include_searchable_names=False,include_soft_management_references=False,include_hard_management_references=False)
        refs=[str(p).rsplit('/',1)[-1]for p in unreal.AssetRegistryHelpers.get_asset_registry().get_dependencies(path('Materials',row['id']),dep_opts)]
        base=next((row['textures'][k]['id']for k in ['_MainTex','_BaseMap','_BaseColorMap','_DiffuseTex']if k in row['textures']),None)
        if base:assert base in refs,(row['id'],'base texture missing',base,refs)
        REPORT['materials'].append({'id':row['id'],'used_textures':refs})
    for row in DATA['textures']:
        tex=load(path('Textures',row['id']));wrap={0:unreal.TextureAddress.TA_WRAP,1:unreal.TextureAddress.TA_CLAMP,2:unreal.TextureAddress.TA_MIRROR}
        assert tex.get_editor_property('address_x')==wrap[row.get('wrap_u',0)]
        assert tex.get_editor_property('address_y')==wrap[row.get('wrap_v',0)]
        REPORT['textures'].append({'id':row['id'],'address_x':str(tex.get_editor_property('address_x')),'address_y':str(tex.get_editor_property('address_y'))})

def make_gallery():
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem);actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not levels.new_level(MAP):raise RuntimeError('Cannot create gallery map')
    cols=18;spacing=310
    for i,row in enumerate(DATA['meshes']):
        mesh=load(path('Meshes',row['id']));bounds=mesh.get_bounds();extent=bounds.box_extent;longest=max(extent.x,extent.y,extent.z)*2
        scale=200/max(longest,0.01);x=(i%cols-8.5)*spacing;y=(i//cols-7.5)*spacing
        loc=unreal.Vector(x-bounds.origin.x*scale,y-bounds.origin.y*scale,(extent.z-bounds.origin.z)*scale+10)
        actor=actors.spawn_actor_from_class(unreal.StaticMeshActor,loc,unreal.Rotator(pitch=0,yaw=0,roll=0))
        missing=any('_MainTex'in m.get('missing_textures',{})for m in DATA['materials']if m['id']in row['selected_materials'])
        actor.set_actor_label('DNFM_%03d_%s%s'%(i,row['name'],'_MISSING_TEXTURE'if missing else ''))
        actor.set_actor_scale3d(unreal.Vector(scale,scale,scale));actor.static_mesh_component.set_static_mesh(mesh)
        actor.static_mesh_component.set_collision_profile_name('NoCollision')
        REPORT['meshes'].append({'id':row['id'],'actor':actor.get_actor_label(),'inspection_scale':scale})
    floor=actors.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector(0,0,-30));floor.set_actor_label('DNFM_GalleryFloor')
    floor.static_mesh_component.set_static_mesh(load('/Engine/BasicShapes/Cube'));floor.set_actor_scale3d(unreal.Vector(65,58,0.5))
    mat=build_material({'id':'M_GalleryFloor','shader':'Standard','colors':{'_Color':{'r':0.12,'g':0.14,'b':0.18,'a':1}}})
    floor.static_mesh_component.set_material(0,mat)
    for yaw,intensity in [(-40,4),(140,2)]:
        light=actors.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,2000),unreal.Rotator(pitch=-55,yaw=yaw,roll=0))
        light.light_component.set_editor_property('intensity',float(intensity));light.light_component.set_editor_property('cast_shadows',False)
    sky=actors.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,1500));sky.light_component.set_editor_property('intensity',1.0)
    camera=actors.spawn_actor_from_class(unreal.CameraActor,unreal.Vector(0,-5000,6500),unreal.Rotator(pitch=-52,yaw=90,roll=0))
    camera.set_actor_label('DNFM_GalleryCamera');camera.camera_component.set_editor_property('field_of_view',65.)
    levels.save_current_level();REPORT['map']=MAP

def capture():
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem);assert levels.load_level(MAP)
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem);world=unreal.EditorLevelLibrary.get_editor_world()
    previews=ART/'previews'/REPORT['run_id'];previews.mkdir(parents=True,exist_ok=True)
    ca=actors.spawn_actor_from_class(unreal.SceneCapture2D,unreal.Vector(0,-5000,6500),unreal.Rotator(pitch=-52,yaw=90,roll=0))
    cap=ca.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt=unreal.RenderingLibrary.create_render_target2d(world,1920,1440,unreal.TextureRenderTargetFormat.RTF_RGBA8,unreal.LinearColor(0.06,0.07,0.1,1))
    cap.set_editor_property('texture_target',rt);cap.set_editor_property('capture_source',unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    cap.set_editor_property('capture_every_frame',True)
    cap.set_editor_property('fov_angle',65.)
    # Real editor ticks allow shaders, lighting, streaming and render commands to settle.
    state={'frame':0,'shot':0};shots=[('gallery',None)]
    candidates=[a for a in actors.get_all_level_actors()if a.get_actor_label().startswith('DNFM_')and isinstance(a,unreal.StaticMeshActor)and a.get_actor_label()!='DNFM_GalleryFloor']
    for token in ['GA_fishing','GA_pot','GA_stone','GA_hay']:
        match=next((a for a in candidates if token.lower()in a.get_actor_label().lower() and min(a.static_mesh_component.static_mesh.get_bounds().box_extent.x,a.static_mesh_component.static_mesh.get_bounds().box_extent.y,a.static_mesh_component.static_mesh.get_bounds().box_extent.z)>0.1),None)
        if match:shots.append((token,match))
    REPORT['previews']=[]
    def tick(dt):
        try:
            state['frame']+=1
            if state['frame']<180:return
            frame=(state['frame']-180)%90
            if frame==0:
                name,target=shots[state['shot']]
                if target:
                    origin,extent=target.get_actor_bounds(False);loc=origin+unreal.Vector(240,-300,230)
                    ca.set_actor_location(loc,False,False);ca.set_actor_rotation(unreal.MathLibrary.find_look_at_rotation(loc,origin),False)
                    cap.set_editor_property('primitive_render_mode',unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
                    cap.clear_show_only_components()
                    cap.show_only_actor_components(target)
                cap.capture_scene()
            if frame==60:
                name,target=shots[state['shot']];dest=previews/(name+'.png')
                unreal.RenderingLibrary.export_render_target(world,rt,str(previews),name+'.png')
                assert dest.is_file()and dest.stat().st_size>1024,str(dest)
                REPORT['previews'].append({'name':name,'file':str(dest),'bytes':dest.stat().st_size})
                state['shot']+=1
                if state['shot']==len(shots):
                    REPORT['success']=True;save_report()
                    cap.set_editor_property('capture_every_frame',False)
                    cap.set_editor_property('texture_target',None)
                    actors.destroy_actor(ca)
                    unreal.RenderingLibrary.release_render_target2d(rt)
                    unreal.unregister_slate_post_tick_callback(handle)
                    unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        except Exception:
            REPORT['errors'].append(traceback.format_exc());save_report();unreal.unregister_slate_post_tick_callback(handle);unreal.EditorPythonScripting.set_keep_python_script_alive(False)
    handle=unreal.register_slate_post_tick_callback(tick)
    globals()['_capture_keepalive']=(ca,cap,rt,tick,handle,shots)

try:
    if MODE=='import':import_all()
    elif MODE=='verify':verify()
    elif MODE=='gallery':make_gallery()
    elif MODE=='capture':capture()
    else:raise ValueError(MODE)
    if MODE!='capture':REPORT['success']=True;save_report()
except Exception:
    REPORT['errors'].append(traceback.format_exc());save_report();raise
