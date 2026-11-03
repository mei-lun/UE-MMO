"""M6-009 source geometry -> FBX. Execute with Blender --background --python."""
import json
import os
from pathlib import Path
import bpy

ROOT=Path(__file__).resolve().parents[1]
STAGE=ROOT/'SourceAssets/Unpacked/DNFM130_5_8'
manifest=json.loads((STAGE/'manifest.json').read_text())
FBX=STAGE/'fbx';FBX.mkdir(exist_ok=True)
ART=ROOT/'Artifacts/Tasks/M6-009';ART.mkdir(parents=True,exist_ok=True)
VERIFY=os.environ.get('DNFM_BLENDER_VERIFY')=='1'
report={'success':False,'mode':'verify'if VERIFY else 'export','meshes':[],'errors':[]}

def vec(v):return (v[0],-v[2],v[1])

try:
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene;scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1.0
    for row in manifest['meshes']:
        name=row['id'];dest=FBX/(name+'.fbx')
        if VERIFY:
            bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
            bpy.ops.import_scene.fbx(filepath=str(dest),use_custom_normals=True,colors_type='LINEAR')
            objs=[o for o in bpy.context.scene.objects if o.type=='MESH']
            assert len(objs)==1,(name,len(objs))
            data=objs[0].data
            assert len(data.vertices)==row['vertex_count'],name
            assert len(data.polygons)==row['triangle_count'],name
            assert len(data.uv_layers)==len(row['uv_channels']),(name,len(data.uv_layers),row['uv_channels'])
            assert bool(data.color_attributes)==row['has_vertex_colors'],name
            if row['has_vertex_colors']:
                source=json.loads(Path(row['geometry']).read_text())['colors']
                layer=data.color_attributes[0]
                for li,loop in enumerate(data.loops):
                    expected=source[loop.vertex_index]
                    actual=layer.data[li if layer.domain=='CORNER'else loop.vertex_index].color
                    assert all(abs(float(actual[c])-float(expected[c]))<0.006 for c in range(4)),(name,'color changed',list(actual),expected)
            report['meshes'].append({'id':name,'vertices':len(data.vertices),'triangles':len(data.polygons),
                                    'uv_layers':len(data.uv_layers),'vertex_colors':bool(data.color_attributes)})
            continue
        g=json.loads(Path(row['geometry']).read_text())
        vertices=[vec(v)for v in g['vertices']];faces=[tri for sub in g['submeshes']for tri in sub]
        data=bpy.data.meshes.new(name);data.from_pydata(vertices,[],faces);data.update()
        obj=bpy.data.objects.new(name,data);scene.collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT');obj.select_set(True);bpy.context.view_layer.objects.active=obj
        for i,values in enumerate(g['uv_sets']):
            if not values:continue
            layer=data.uv_layers.new(name='UV'+str(i))
            for poly in data.polygons:
                for li in poly.loop_indices:layer.data[li].uv=values[data.loops[li].vertex_index][:2]
        if g['colors']:
            layer=data.color_attributes.new(name='Color',type='FLOAT_COLOR',domain='CORNER')
            for li,loop in enumerate(data.loops):
                c=g['colors'][loop.vertex_index];layer.data[li].color=tuple(c[:4])if len(c)>=4 else (*c[:3],1.0)
        if g['normals']:
            for poly in data.polygons:poly.use_smooth=True
            data.normals_split_custom_set_from_vertices([vec(n)for n in g['normals']])
        for i in range(row['submesh_count']):
            slotname='Slot_%02d'%i
            mat=bpy.data.materials.get(slotname)or bpy.data.materials.new(slotname)
            data.materials.append(mat)
        face=0
        for si,tris in enumerate(g['submeshes']):
            for _ in tris:data.polygons[face].material_index=si;face+=1
        bpy.ops.export_scene.fbx(filepath=str(dest),use_selection=True,object_types={'MESH'},
            axis_forward='-Y',axis_up='Z',global_scale=1.0,apply_unit_scale=True,
            apply_scale_options='FBX_SCALE_NONE',bake_space_transform=False,
            use_mesh_modifiers=False,mesh_smooth_type='OFF',use_tspace=True,
            path_mode='AUTO',bake_anim=False,add_leaf_bones=False,colors_type='LINEAR')
        report['meshes'].append({'id':name,'fbx_bytes':dest.stat().st_size,'uv_layers':len(data.uv_layers),
                                'vertex_colors':bool(data.color_attributes)})
        obj.hide_set(True)
    if not VERIFY:
        for o in scene.objects:o.hide_set(False)
        bpy.ops.wm.save_as_mainfile(filepath=str(STAGE/'DNFM_StaticMeshes.blend'))
    report['success']=len(report['meshes'])==279
except Exception as exc:
    import traceback
    report['errors'].append(traceback.format_exc())
    raise
finally:
    (ART/('blender-'+report['mode']+'.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
