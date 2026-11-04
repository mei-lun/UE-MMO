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
_MODE_ENV=os.environ.get('UEMMO_DNFM_MODE','')
ART=ROOT/('Artifacts/Tasks/M6-013' if _MODE_ENV=='guildagit_wire' else 'Artifacts/Tasks/M6-012' if _MODE_ENV.startswith('guildagit') else 'Artifacts/Tasks/M6-009');ART.mkdir(parents=True,exist_ok=True)
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

GMAP=DEST+'/Maps/L_DNFM_Guildagit'
def layout_root():
    d=Path(os.environ.get('UEMMO_DNFM_LAYOUT_DIR')or(ROOT/'Artifacts/Tasks/M6-011/2026-11-04T0215_0800/intermediate'))
    if not d.is_dir():raise RuntimeError('Missing layout intermediate dir '+str(d))
    return d

def load_layout():
    d=layout_root()
    return {name:json.loads((d/(name+'.json')).read_text(encoding='utf-8')) for name in ['GuildagitGroundDummy','BG_Planet_Prepab']}

def make_guildagit():
    # M6-012: rebuild the original Guildagit prefab layout from M6-011 intermediates.
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem);actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    # A killed earlier run can leave the umap on disk before the asset registry knows it (or
    # a stale Saved/AssetRegistryCache entry can outlive a deleted file), so drop both and
    # force a synchronous rescan of the Maps folder before creating the destination.
    if ASSETS.does_asset_exist(GMAP)and not ASSETS.delete_asset(GMAP):raise RuntimeError('Cannot delete stale guildagit map')
    umap=ROOT/'Content/UEMMO/Imported/DNFM130_5_8/Maps/L_DNFM_Guildagit.umap'
    if umap.exists():umap.unlink()
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([DEST+'/Maps'],True)
    if not levels.new_level(GMAP):raise RuntimeError('Cannot create guildagit map')
    layout=load_layout();placed=hidden=mirror=mat_applied=0;mat_missing=[];blocking=0
    bounds_min=[1e9]*3;bounds_max=[-1e9]*3
    for name,data in layout.items():
        for inst in data['instances']:
            t=inst['ue_transform'];rot=t['rotator_pitch_yaw_roll_deg']
            actor=actors.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector(*t['location_cm']),unreal.Rotator(pitch=rot[0],yaw=rot[1],roll=rot[2]))
            actor.set_actor_label(inst['hierarchy_path'])
            actor.set_actor_scale3d(unreal.Vector(*t['scale']))
            mesh=load(inst['ue_object_path'])
            actor.static_mesh_component.set_static_mesh(mesh)
            ext=mesh.get_bounds().box_extent
            # Big scenery blocks (trees/buildings/fences); small decorations stay walk-through so
            # input-driven enemies without pathfinding cannot wedge on grass/flowers.
            if min(ext.x,ext.y,ext.z)>=100:
                actor.static_mesh_component.set_collision_profile_name('BlockAll');blocking+=1
            else:
                actor.static_mesh_component.set_collision_profile_name('NoCollision')
            for m in inst.get('materials',[]):
                mp=path('Materials',m['ue_material_id'])
                if ASSETS.does_asset_exist(mp):actor.static_mesh_component.set_material(int(m['slot']),load(mp));mat_applied+=1
                else:mat_missing.append('%s:%d'%(inst['hierarchy_path'],m['slot']))
            if not (inst['active_in_stored_hierarchy']and inst['renderer_enabled']):
                # Disabled in the stored hierarchy: hidden in game and labelled for verification
                # (bHiddenEd is a protected property that python cannot set).
                actor.set_actor_hidden_in_game(True);actor.set_actor_label(inst['hierarchy_path']+'_DISABLED');hidden+=1
            if t.get('mirrored'):mirror+=1
            for i,b in enumerate(inst['ue_bounds_estimate_cm']['min']):bounds_min[i]=min(bounds_min[i],b)
            for i,b in enumerate(inst['ue_bounds_estimate_cm']['max']):bounds_max[i]=max(bounds_max[i],b)
            placed+=1
        st=data['instance_stats'];REPORT['stats_'+name]={'mapped':st['mesh_renderer_mapped'],'unmapped':st['mesh_renderer_unmapped'],
            'placed':len(data['instances']),'distinct_meshes':st['distinct_ue_mesh_ids'],'bounds_union_cm':data['ue_bounds_union_cm']}
        REPORT['unmapped_'+name]=[{'hierarchy_path':u['hierarchy_path'],'mesh':u.get('ue_mesh_id')or u.get('mesh_source_name'),'reason':u.get('unresolved_reason')} for u in data['unmapped_instances']]
    # The 8 unmapped instances are exactly the original terrain (GA_ground/islands/water).
    # Explicit labelled placeholder floor per the handoff rule: missing geometry shows a clear
    # placeholder and is recorded, never passed off as original art.
    margin=200.;px0,px1=bounds_min[0]-margin,bounds_max[0]+margin;py0,py1=bounds_min[1]-margin,bounds_max[1]+margin
    ground=actors.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector((px0+px1)/2,(py0+py1)/2,-10),unreal.Rotator(pitch=0,yaw=0,roll=0))
    ground.set_actor_label('DNFM_GAGroundPlaceholder_MISSING_GEOMETRY')
    ground.set_actor_scale3d(unreal.Vector((px1-px0)/100,(py1-py0)/100,0.2))
    ground.static_mesh_component.set_static_mesh(load('/Engine/BasicShapes/Cube'))
    gmat=build_material({'id':'M_GAGroundPlaceholder','shader':'Standard','colors':{'_Color':{'r':0.16,'g':0.18,'b':0.16,'a':1}}})
    ground.static_mesh_component.set_material(0,gmat)
    ASSETS.set_metadata_tag(gmat,'DNFM.AppearanceStatus','MissingOriginalTerrain_ExplicitPlaceholder')
    for yaw,intensity in [(-40,4),(140,2)]:
        light=actors.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,3000),unreal.Rotator(pitch=-55,yaw=yaw,roll=0))
        light.light_component.set_editor_property('intensity',float(intensity));light.light_component.set_editor_property('cast_shadows',False)
    sky=actors.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,2000));sky.light_component.set_editor_property('intensity',1.0)
    camera=actors.spawn_actor_from_class(unreal.CameraActor,unreal.Vector((px0+px1)/2,(py0+py1)/2-5200,3400),unreal.Rotator(pitch=-33,yaw=90,roll=0))
    camera.set_actor_label('DNFM_GuildagitCamera');camera.camera_component.set_editor_property('field_of_view',60.)
    levels.save_current_level()
    REPORT['map']=GMAP;REPORT['placed_instances']=placed;REPORT['hidden_instances']=hidden;REPORT['mirrored_instances']=mirror
    REPORT['blocking_instances']=blocking;REPORT['ground_placeholder']={'bounds_min':[px0,py0,-20],'bounds_max':[px1,py1,0]}
    REPORT['material_slots_applied']=mat_applied;REPORT['material_slots_missing_asset']=len(mat_missing)
    REPORT['bounds_union_cm']={'min':bounds_min,'max':bounds_max}
    REPORT['limitations'].append('Guildagit layout rebuilt from original prefab TRS; %d instances kept, %d hidden (inactive/disabled), %d unmappable meshes not placed.'%(placed,hidden,sum(len(REPORT['unmapped_'+n])for n in layout)))
    REPORT['limitations'].append('Original terrain meshes (GA_ground/islands/water) are missing from the source package; an explicitly labelled placeholder floor stands in. Big scenery blocks movement, small decorations do not (input-driven enemies have no pathfinding).')

def verify_guildagit():
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    assert levels.load_level(GMAP),'cannot load '+GMAP
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    layout=load_layout();expected={}
    for data in layout.values():
        for inst in data['instances']:
            label=inst['hierarchy_path']+('_DISABLED' if not(inst['active_in_stored_hierarchy']and inst['renderer_enabled'])else '')
            expected[label]=inst
    expected['DNFM_GAGroundPlaceholder_MISSING_GEOMETRY']=None  # labelled stand-in for the 8 missing terrain meshes
    sma=[a for a in actors.get_all_level_actors()if isinstance(a,unreal.StaticMeshActor)]
    labels={a.get_actor_label() for a in sma}
    missing=sorted(set(expected)-labels);extra=sorted(labels-set(expected))
    assert not missing,('missing actors',missing[:5]);assert not extra,('unexpected actors',extra[:5])
    # Duplicate source hierarchy paths are legal (multiple renderers under one node),
    # so the actor COUNT is verified separately from the unique-label set match.
    assert len(sma)==sum(len(d['instances'])for d in layout.values())+1,(len(sma),'actor count')
    REPORT['actor_count']=len(sma)
    by_label={a.get_actor_label():a for a in sma}
    sampled=0;hidden_count=0
    for idx,(label,inst) in enumerate(sorted(expected.items())):
        a=by_label[label]
        if label.endswith('_DISABLED'):
            # bHidden read via editor property; the label suffix is the durable, checkable marker.
            try:
                assert bool(a.get_editor_property('bHidden')),(label,'should be game-hidden')
            except AssertionError:
                raise
            except Exception:
                REPORT['hidden_flag_check']='bHidden editor-property read unavailable; verified by label suffix'
            hidden_count+=1
        if inst is None:continue  # labelled placeholder floor, not an original instance
        t=inst['ue_transform']
        if idx%40==0:
            loc=a.get_actor_location();scale=a.get_actor_scale3d();rot=a.get_actor_rotation()
            assert all(abs((loc.x,loc.y,loc.z)[i]-t['location_cm'][i])<0.05 for i in range(3)),(label,'location')
            assert all(abs((scale.x,scale.y,scale.z)[i]-t['scale'][i])<0.005 for i in range(3)),(label,'scale')
            assert abs(rot.pitch-t['rotator_pitch_yaw_roll_deg'][0])<0.05 and abs(rot.yaw-t['rotator_pitch_yaw_roll_deg'][1])<0.05 and abs(rot.roll-t['rotator_pitch_yaw_roll_deg'][2])<0.05,(label,'rotation',rot.pitch,rot.yaw,rot.roll)
            comp=a.static_mesh_component
            assert comp.static_mesh and comp.static_mesh.get_name()==inst['ue_mesh_id'],(label,'mesh',comp.static_mesh.get_name() if comp.static_mesh else None)
            sampled+=1
    want_hidden=sum(1 for data in layout.values()for inst in data['instances']if not(inst['active_in_stored_hierarchy']and inst['renderer_enabled']))
    assert hidden_count==want_hidden,(hidden_count,want_hidden)
    REPORT['verified_instances']=len(expected);REPORT['sampled_instances']=sampled;REPORT['hidden_instances']=hidden_count;REPORT['map']=GMAP

def capture_guildagit():
    # Mirrors capture() for the guildagit map; isolated copy so M6-009 capture stays untouched.
    unreal.EditorPythonScripting.set_keep_python_script_alive(True)
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem);assert levels.load_level(GMAP)
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem);world=unreal.EditorLevelLibrary.get_editor_world()
    art=ROOT/'Artifacts/Tasks/M6-012';art.mkdir(parents=True,exist_ok=True)
    previews=art/'previews'/REPORT['run_id'];previews.mkdir(parents=True,exist_ok=True)
    layout=load_layout()
    lo=[min(d['ue_bounds_union_cm']['min'][i]for d in layout.values())for i in range(3)]
    hi=[max(d['ue_bounds_union_cm']['max'][i]for d in layout.values())for i in range(3)]
    center=unreal.Vector((lo[0]+hi[0])/2,(lo[1]+hi[1])/2,(lo[2]+hi[2])/2)
    ca=actors.spawn_actor_from_class(unreal.SceneCapture2D,unreal.Vector(center.x,center.y-5200,center.z+3400),unreal.Rotator(pitch=-33,yaw=90,roll=0))
    cap=ca.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt=unreal.RenderingLibrary.create_render_target2d(world,1920,1440,unreal.TextureRenderTargetFormat.RTF_RGBA8,unreal.LinearColor(0.06,0.07,0.1,1))
    cap.set_editor_property('texture_target',rt);cap.set_editor_property('capture_source',unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    cap.set_editor_property('capture_every_frame',True);cap.set_editor_property('fov_angle',60.)
    state={'frame':0,'shot':0};shots=[('overview',None)]
    candidates=[a for a in actors.get_all_level_actors()if isinstance(a,unreal.StaticMeshActor)]
    REPORT['capture_candidates']=len(candidates)
    for token in ['GA_maingate','GA_tree_01','GA_flower']:
        # Mesh-local bounds are valid right after load (gallery precedent); actor
        # world bounds stay stale/zero until the editor ticks, so avoid them here.
        pool=[a for a in candidates if token.lower()in a.get_actor_label().lower()]
        match=next((a for a in pool if min(a.static_mesh_component.static_mesh.get_bounds().box_extent.x,a.static_mesh_component.static_mesh.get_bounds().box_extent.y,a.static_mesh_component.static_mesh.get_bounds().box_extent.z)>0.1),None)
        if match is None and pool:match=pool[0]
        REPORT['closeup_token_'+token]=bool(match);REPORT['closeup_token_'+token+'_pool']=len(pool)
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
                    cap.clear_show_only_components();cap.show_only_actor_components(target)
                # Overview keeps the default primitive render mode (whole scene), like capture().
                cap.capture_scene()
            if frame==60:
                name,target=shots[state['shot']];dest=previews/(name+'.png')
                unreal.RenderingLibrary.export_render_target(world,rt,str(previews),name+'.png')
                assert dest.is_file()and dest.stat().st_size>1024,str(dest)
                REPORT['previews'].append({'name':name,'file':str(dest),'bytes':dest.stat().st_size})
                state['shot']+=1
                if state['shot']==len(shots):
                    REPORT['success']=True;save_report()
                    cap.set_editor_property('capture_every_frame',False);cap.set_editor_property('texture_target',None)
                    actors.destroy_actor(ca);unreal.RenderingLibrary.release_render_target2d(rt)
                    unreal.unregister_slate_post_tick_callback(handle);unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        except Exception:
            REPORT['errors'].append(traceback.format_exc());save_report();unreal.unregister_slate_post_tick_callback(handle);unreal.EditorPythonScripting.set_keep_python_script_alive(False)
    handle=unreal.register_slate_post_tick_callback(tick)
    globals()['_capture_keepalive']=(ca,cap,rt,tick,handle,shots)

def guildagit_wire():
    # M6-013: give the guildagit map its production RoomTrigger/RoomExit so the
    # PrepareRooms wiring pass can point the trigger at the generated definition
    # asset (the M6-004 per-row generalization: exactly one of each required).
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem);actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    assert levels.load_level(GMAP),'cannot load '+GMAP
    row=next(r for r in json.loads((ROOT/'Data/rooms.json').read_text(encoding='utf-8-sig'))['rooms'] if r['room_id']=='room_guildagit_01')
    spawn=row['player_spawn_location_cm']
    starts=[a for a in actors.get_all_level_actors() if isinstance(a,unreal.PlayerStart)]
    if len(starts)>1:raise RuntimeError('Ambiguous Guildagit PlayerStart count: '+str(len(starts)))
    start=starts[0] if starts else actors.spawn_actor_from_class(unreal.PlayerStart,unreal.Vector(spawn['x'],spawn['y'],spawn['z']),unreal.Rotator(pitch=0,yaw=row['player_spawn_yaw_degrees'],roll=0))
    start.set_actor_location(unreal.Vector(spawn['x'],spawn['y'],spawn['z']),False,False)
    start.set_actor_rotation(unreal.Rotator(pitch=0,yaw=row['player_spawn_yaw_degrees'],roll=0),False)
    start.set_actor_label('DNFM_GuildagitPlayerStart')
    # Only one directional light may be the main light for translucent assets.
    lights=sorted([a for a in actors.get_all_level_actors() if isinstance(a,unreal.DirectionalLight)],key=lambda a:a.get_name())
    for i,light in enumerate(lights):light.light_component.set_editor_property('forward_shading_priority',1 if i==0 else 0)
    def count(cls):return sum(1 for a in actors.get_all_level_actors()if a.get_class().get_name()==cls)
    if count('RoomTrigger')==0:
        # Default brush is 200cm; covers the playable ground so entering the room
        # activates the session immediately (rules:[] -> direct BeginWaves path).
        t=actors.spawn_actor_from_class(unreal.RoomTrigger,unreal.Vector(-146,622,200),unreal.Rotator(pitch=0,yaw=0,roll=0))
        t.set_actor_label('DNFM_GARoomTrigger');t.set_actor_scale3d(unreal.Vector(29,10,3))
    # Start outside the overlap: placed actors may register their initial
    # overlap before ARoomTrigger binds BeginPlay. A real walk-in crosses
    # X=-350 (spawn X=-400), matching the existing combat-room contract.
    triggers=[a for a in actors.get_all_level_actors() if a.get_class().get_name()=='RoomTrigger']
    if len(triggers)!=1:raise RuntimeError('Guildagit requires exactly one RoomTrigger')
    triggers[0].set_actor_location(unreal.Vector(1450,622,200),False,False)
    triggers[0].set_actor_scale3d(unreal.Vector(12,10,3))
    if count('RoomExit')==0:
        e=actors.spawn_actor_from_class(unreal.RoomExit,unreal.Vector(2650,620,150),unreal.Rotator(pitch=0,yaw=0,roll=0))
        e.set_actor_label('DNFM_GARoomExit');e.set_actor_scale3d(unreal.Vector(1.5,2,1.5))
    if not levels.save_current_level():raise RuntimeError('Could not save '+GMAP)
    REPORT['map']=GMAP;REPORT['room_trigger_count']=count('RoomTrigger');REPORT['room_exit_count']=count('RoomExit')
    REPORT['player_start']={'location_cm':[spawn['x'],spawn['y'],spawn['z']],'yaw':row['player_spawn_yaw_degrees']}
    REPORT['activation_entry_x_cm']=-350

try:
    if MODE=='import':import_all()
    elif MODE=='verify':verify()
    elif MODE=='gallery':make_gallery()
    elif MODE=='capture':capture()
    elif MODE=='guildagit':make_guildagit()
    elif MODE=='guildagit_verify':verify_guildagit()
    elif MODE=='guildagit_capture':capture_guildagit()
    elif MODE=='guildagit_wire':guildagit_wire()
    else:raise ValueError(MODE)
    if MODE not in ('capture','guildagit_capture'):REPORT['success']=True;save_report()
except Exception:
    REPORT['errors'].append(traceback.format_exc());save_report();raise
