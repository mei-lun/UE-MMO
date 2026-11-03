"""M6-009: recover full Unity meshes and their actual renderer material references."""
import argparse
import collections
import hashlib
import json
import re
import shutil
import zipfile
from pathlib import Path
import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler

ROOT=Path(__file__).resolve().parents[1]
DEFAULT_SOURCE=Path('D:/Github-Poj/DNFM/DNFM_export_130.5.8.0')
STAGE=ROOT/'SourceAssets/Unpacked/DNFM130_5_8'

def dump(p,data):
    p.parent.mkdir(parents=True,exist_ok=True)
    p.write_text(json.dumps(data,ensure_ascii=True,indent=2),encoding='utf-8')

def key(o):return o.assets_file.name.lower()+':'+str(o.path_id)
def aid(prefix,o):
    name=o.peek_name()or o.type.name
    return prefix+'_'+re.sub('[^A-Za-z0-9_]+','_',name).strip('_')[:42]+'_'+hashlib.sha256(key(o).encode()).hexdigest()[:10]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,default=DEFAULT_SOURCE)
    ap.add_argument('--bootstrap',action='store_true',help='Only bootstrap target mesh index before running scan_dnfm_bindings.py; not a production import manifest.')
    args=ap.parse_args();source=args.source
    cross_path=ROOT/'Artifacts/Tasks/M6-009/cross_bundle_bindings.json'
    if not cross_path.exists()and not args.bootstrap:
        raise RuntimeError('Missing prefab material evidence. Run prepare_dnfm_static.py --bootstrap, scan_dnfm_bindings.py, then prepare_dnfm_static.py.')
    STAGE.mkdir(parents=True,exist_ok=True)
    cab_sources={};mesh_sources=[]
    for line in (source/'reports/bundle_inventory.jsonl').open(encoding='utf-8'):
        row=json.loads(line)
        for obj in row['objects']:
            cab_sources[obj['serialized_file'].lower()]=source/row['source']
        if row['counts'].get('Mesh'):mesh_sources.append(source/row['source'])
    env=UnityPy.Environment();loaded=set();file_sources={};issues=[]
    def load(path):
        if str(path)in loaded:return
        env.load_file(str(path));loaded.add(str(path))
        for cab,af in env.cabs.items():
            if hasattr(af,'objects'):file_sources.setdefault(af.name.lower(),str(path))
    with zipfile.ZipFile(source.parent/'10040714_com.tencent.tmgp.dnf_a4518332_130.5.8.0_Mwcs9R.apk')as z:
        for name in ['unity default resources','Resources/unity_builtin_extra']:
            p=STAGE/'unity_originals'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(z.read('assets/bin/Data/'+name))
            load(p);cab_sources[Path(name).name.lower()]=p
    for path in mesh_sources:load(path)
    def resolve(owner,ptr):
        if not ptr or ptr.get('m_PathID',0)==0:return None
        af=owner.assets_file
        if ptr['m_FileID']:
            external=af.externals[ptr['m_FileID']-1].path.replace('\\','/').split('/')[-1].lower()
            if external in cab_sources:load(cab_sources[external])
            af=next((v for v in env.cabs.values()if getattr(v,'name','').lower()==external),None)
            if af is None:
                issue={'kind':'missing_external','owner':key(owner),'file':external,'path_id':str(ptr['m_PathID'])}
                if issue not in issues:issues.append(issue)
                return None
        return af.objects[ptr['m_PathID']]
    materials={};textures={}
    def texture(o):
        if key(o)in textures:return textures[key(o)]['id']
        d=o.read();tid=aid('T',o);path=STAGE/'textures'/(tid+'.png');path.parent.mkdir(exist_ok=True)
        d.image.save(path)
        textures[key(o)]={'id':tid,'source_object':key(o),'source_name':d.m_Name,'file':str(path),
                          'width':d.m_Width,'height':d.m_Height,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        return tid
    def material(o):
        if o is None:return None
        if key(o)in materials:return materials[key(o)]['id']
        t=o.read_typetree();mid=aid('M',o);shader=resolve(o,t['m_Shader']);shader_name=shader.peek_name()if shader else 'unknown'
        if shader and not shader_name:
            sd=shader.read_typetree();shader_name=sd.get('m_ParsedForm',{}).get('m_Name','unknown')
        props=t['m_SavedProperties'];record={'id':mid,'source_object':key(o),'name':t['m_Name'],
            'shader':shader_name,'textures':{},'floats':dict(props['m_Floats']),'colors':dict(props['m_Colors']),
            'keywords':t.get('m_ValidKeywords',[]),'render_queue':t.get('m_CustomRenderQueue',-1),'tags':dict(t.get('stringTagMap',[]))}
        materials[key(o)]=record
        for slot,info in props['m_TexEnvs']:
            tex=resolve(o,info['m_Texture'])
            if tex is None:continue
            record['textures'][slot]={'id':texture(tex),'scale':info['m_Scale'],'offset':info['m_Offset']}
        dump(STAGE/'material_sources'/(mid+'.json'),t)
        return mid
    objects=[o for af in list(env.cabs.values())if hasattr(af,'objects')for o in af.objects.values()]
    # The built-in extras contains engine shaders/materials; only default resources meshes included.
    meshes={key(o):o for o in objects if o.type.name=='Mesh'}
    gameobject_mesh={};bindings=collections.defaultdict(list)
    for o in objects:
        if o.type.name=='MeshFilter':
            d=o.read_typetree();mesh=resolve(o,d['m_Mesh']);go=resolve(o,d['m_GameObject'])
            if mesh and go and key(mesh)in meshes:gameobject_mesh[key(go)]=mesh
    for o in objects:
        if o.type.name not in ['MeshRenderer','SkinnedMeshRenderer']:continue
        d=o.read_typetree();go=resolve(o,d['m_GameObject'])
        mesh=resolve(o,d['m_Mesh'])if 'm_Mesh'in d else gameobject_mesh.get(key(go))if go else None
        if mesh is None or key(mesh)not in meshes:continue
        mids=[material(resolve(o,p))for p in d['m_Materials']]
        entry={'renderer':key(o),'gameobject':go.peek_name()if go else None,'materials':mids}
        if entry not in bindings[key(mesh)]:bindings[key(mesh)].append(entry)
    rows=[]
    for k,o in sorted(meshes.items()):
        d=o.read();h=MeshHandler(d);h.process();mid=aid('SM',o);subs=h.get_triangles()
        # MeshHelper returns indices relative to vertex stream; preserve submesh baseVertex.
        subs=[[[int(v)+int(d.m_SubMeshes[si].baseVertex or 0)for v in tri]for tri in tris]for si,tris in enumerate(subs)]
        verts=h.m_Vertices;normals=h.m_Normals or []
        uv=[getattr(h,'m_UV'+str(i),None)or [] for i in range(8)]
        colors=getattr(h,'m_Colors',None)or []
        if colors and isinstance(colors[0][0],int):
            # UnityPy exposes UNorm8 color channels as byte integers.
            colors=[[c/255.0 for c in color]for color in colors]
        geo={'vertices':verts,'normals':normals,'uv_sets':uv,'colors':colors,'submeshes':subs}
        assert verts and all(0<=i<len(verts)for sub in subs for tri in sub for i in tri)
        assert all(len(u)==len(verts)for u in uv if u)
        assert not colors or len(colors)==len(verts)
        p=STAGE/'geometry'/(mid+'.json');dump(p,geo)
        variants=[]
        for binding in bindings[k]:
            if binding['materials']not in variants:variants.append(binding['materials'])
        if not variants:variants=[[None]*len(subs)]
        rows.append({'id':mid,'name':d.m_Name,'source_object':k,'source_bundle':file_sources[o.assets_file.name.lower()],
            'geometry':str(p),'vertex_count':len(verts),'triangle_count':sum(len(s)for s in subs),'submesh_count':len(subs),
            'uv_channels':[i for i,u in enumerate(uv)if u],'has_vertex_colors':bool(colors),
            'extent_unity_units':[max(v[i]for v in verts)-min(v[i]for v in verts)for i in range(3)],
            'builtin':'unity default resources'in o.assets_file.name,'renderer_bindings':bindings[k],
            'material_variants':variants,'selected_materials':variants[0]})
    assert len(rows)==279,len(rows)
    # Actual prefab renderers may override the materials stored alongside imported FBX meshes.
    cross_path=ROOT/'Artifacts/Tasks/M6-009/cross_bundle_bindings.json'
    if cross_path.exists()and not args.bootstrap:
        cross=json.loads(cross_path.read_text())
        def stable_id(prefix,name,source_key):
            return prefix+'_'+re.sub('[^A-Za-z0-9_]+','_',name).strip('_')[:42]+'_'+hashlib.sha256(source_key.encode()).hexdigest()[:10]
        material_ids={}
        for m in cross['materials']:
            mid=stable_id('M',m['name'],m['source_key']);material_ids[m['source_key']]=mid
            record={'id':mid,'source_object':m['source_key'],'name':m['name'],'shader':m['shader'].get('name')or'unknown',
                    'textures':{},'missing_textures':{},'floats':m['floats'],'colors':m['colors'],
                    'keywords':m['keywords'],'render_queue':m['render_queue'],'tags':m['tags']}
            for slot,tex in m['textures'].items():
                if tex['status']=='null':continue
                if tex['status']!='resolved':
                    record['missing_textures'][slot]=tex
                    continue
                tid=stable_id('T',tex['name'],tex['source_key']);dst=STAGE/'textures'/(tid+'.png')
                if not tex.get('exported_png'):raise ValueError('Resolved texture lacks exported PNG '+tex['source_key'])
                pngs=tex['exported_png'];original=Path(pngs[0]if isinstance(pngs,list)else pngs)
                if not original.is_absolute():original=source/original
                shutil.copyfile(original,dst)
                load(Path(tex['source_bundle']))
                cab,oid=tex['source_key'].rsplit(':',1)
                af=next(v for v in env.cabs.values()if getattr(v,'name','').lower()==cab)
                original_texture=af.objects[int(oid)].read_typetree()
                sampler=original_texture['m_TextureSettings']
                textures[tex['source_key']]={'id':tid,'source_object':tex['source_key'],'source_name':tex['name'],
                    'file':str(dst),'width':tex['width'],'height':tex['height'],'sha256':hashlib.sha256(dst.read_bytes()).hexdigest(),
                    'wrap_u':sampler.get('m_WrapU',0),'wrap_v':sampler.get('m_WrapV',0),'filter_mode':sampler.get('m_FilterMode',1)}
                record['textures'][slot]={'id':tid,'scale':tex['scale'],'offset':tex['offset']}
            materials[m['source_key']]=record
            dump(STAGE/'material_sources'/(mid+'.json'),m)
        grouped=collections.defaultdict(list)
        for b in cross['mesh_bindings']:grouped[b['mesh_key']].append(b)
        for row in rows:
            alternatives=grouped.get(row['source_object'],[])
            def rank(b):
                return (bool(b.get('prefab_paths'))and b['is_cross_bundle'],b['renderer_type']=='MeshRenderer',
                        all(m['status']=='resolved'for m in b['materials']),len(b['materials']))
            alternatives=sorted(alternatives,key=rank,reverse=True)
            variants=[]
            for b in alternatives:
                mids=[material_ids.get(m['source_key'])for m in b['materials']]
                if mids and mids not in variants:variants.append(mids)
            if variants:
                row['material_variants']=variants
                if not row['builtin']:row['selected_materials']=variants[0]
            row['cross_bundle_bindings']=alternatives
        issues.extend(cross['missing_references'])
    manifest={'source_export':str(source),'source_apk_sha256':'98a6feb1353092bafa077e995d79ba8c0b4446c68abb83652094dfcf7d9e6bb0',
              'mesh_count':len(rows),'meshes':rows,'materials':list(materials.values()),'textures':list(textures.values()),
              'issues':issues,'unit_convention':'1 Unity coordinate unit = 1 meter; this preserves numerical scale, not inferred original scene transforms.',
              'axis_convention':'Unity x,y,z to Blender x,-z,y; FBX converts to UE Z-up centimeters.',
              'cross_bundle_scan':cross.get('summary')if cross_path.exists()and not args.bootstrap else None,
              'ready_for_ue_import':not args.bootstrap,
              'material_limitations':'Original compiled Unity shaders are retained as metadata; UE graph conversion is an explicit approximation.'}
    dump(STAGE/'manifest.json',manifest)
    print(json.dumps({'meshes':len(rows),'materials':len(materials),'textures':len(textures),
                      'meshes_without_renderer':sum(not r['renderer_bindings']for r in rows),
                      'multi_material_variants':sum(len(r['material_variants'])>1 for r in rows),
                      'vertex_colors':sum(r['has_vertex_colors']for r in rows),
                      'uv2':sum(1 in r['uv_channels']for r in rows),'shaders':dict(collections.Counter(m['shader']for m in materials.values()))}))

if __name__=='__main__':main()
