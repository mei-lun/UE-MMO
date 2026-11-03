"""Read-only exact Unity PPtr investigation across all exported source bundles."""
import collections
import concurrent.futures
import functools
import json
import time
from pathlib import Path
import UnityPy

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path(json.loads((ROOT/'SourceAssets/Unpacked/DNFM130_5_8/manifest.json').read_text())['source_export'])
HERE=ROOT/'Artifacts/Tasks/M6-009'
HERE.mkdir(parents=True,exist_ok=True)
TARGETS={}

def okey(o):return o.assets_file.name.lower()+':'+str(o.path_id)
def pkey(o,p):
    if not p or not p.get('m_PathID'):return None
    cab=o.assets_file.name.lower()
    if p.get('m_FileID'):
        cab=o.assets_file.externals[p['m_FileID']-1].path.replace('\\','/').split('/')[-1].lower()
    return cab+':'+str(p['m_PathID'])

def init_worker():
    global TARGETS
    m=json.loads((ROOT/'SourceAssets/Unpacked/DNFM130_5_8/manifest.json').read_text(encoding='utf8'))
    TARGETS={r['source_object']:r for r in m['meshes']}

def scan(path):
    result={'source_bundle':str(path),'bindings':[],'errors':[]}
    try:
        env=UnityPy.load(str(path));objs=list(env.objects)
        go_mesh={};go_trans={};trans_parent={};trans_go={};gos={};renderers=[];original_paths=collections.defaultdict(list)
        for name,p in env.container.items():
            original_paths[str(p.path_id)].append(name)
        for o in objs:
            k=o.type.name
            if k=='MeshFilter':
                d=o.read_typetree();go_mesh[pkey(o,d['m_GameObject'])]=pkey(o,d['m_Mesh'])
            elif k in ['Transform','RectTransform']:
                d=o.read_typetree();go=pkey(o,d['m_GameObject']);tk=okey(o)
                go_trans[go]=tk;trans_go[tk]=go;trans_parent[tk]=pkey(o,d['m_Father'])
            elif k=='GameObject':gos[okey(o)]=o.peek_name()
            elif k in ['MeshRenderer','SkinnedMeshRenderer','ParticleSystemRenderer']:renderers.append(o)
        for o in renderers:
            d=o.read_typetree();go=pkey(o,d['m_GameObject']);meshkeys=[]
            if o.type.name=='MeshRenderer':meshkeys=[go_mesh.get(go)]
            else:meshkeys=[pkey(o,v) for k,v in d.items() if k in ['m_Mesh','m_Mesh1','m_Mesh2','m_Mesh3']]
            meshkeys=list(dict.fromkeys(k for k in meshkeys if k in TARGETS))
            if not meshkeys:continue
            t=go_trans.get(go);seen=set()
            while t and t not in seen and trans_parent.get(t):
                seen.add(t);t=trans_parent[t]
            rootgo=trans_go.get(t,go)
            paths=original_paths.get(rootgo.rsplit(':',1)[-1],[]) if rootgo else []
            for mk in meshkeys:
                result['bindings'].append({'mesh_key':mk,'renderer_source_key':okey(o),
                    'renderer_type':o.type.name,'source_bundle':str(path),'gameobject_name':gos.get(go),
                    'root_gameobject_name':gos.get(rootgo),'prefab_paths':paths,
                    'is_cross_bundle':mk.split(':')[0]!=o.assets_file.name.lower(),
                    'materials':[{'source_key':pkey(o,p)} for p in d.get('m_Materials',[])]})
    except Exception as e:result['errors'].append(str(e))
    return result

@functools.lru_cache(maxsize=24)
def load(path):return UnityPy.load(path)

def main():
    init_worker();cabmap={};objectinfo={};candidates=[]
    for l in (SOURCE/'reports/bundle_inventory.jsonl').open(encoding='utf8'):
        r=json.loads(l);p=str(SOURCE/r['source'])
        for o in r['objects']:
            cab=o['serialized_file'].lower();cabmap[cab]=p
            if o['type']in ['Material','Shader','Texture2D']:
                objectinfo[cab+':'+o['path_id']]={'source_bundle':p,**o}
        if any(r['counts'].get(k) for k in ['MeshRenderer','SkinnedMeshRenderer','ParticleSystemRenderer']):candidates.append(p)
    # Include decoded built-in originals created by parent; no new source extraction.
    for path in (ROOT/'SourceAssets/Unpacked/DNFM130_5_8/unity_originals').rglob('*'):
        if not path.is_file():continue
        env=load(str(path))
        for o in env.objects:
            cabmap[o.assets_file.name.lower()]=str(path)
    def resolve(key):
        if not key:return None
        cab,pid=key.rsplit(':',1);p=cabmap.get(cab)
        if p is None:return None
        env=load(p)
        return next((o for o in env.objects if o.assets_file.name.lower()==cab and str(o.path_id)==pid),None)
    start=time.monotonic();bindings=[];errors=[]
    with concurrent.futures.ProcessPoolExecutor(max_workers=4,initializer=init_worker)as pool:
        for i,r in enumerate(pool.map(scan,candidates,chunksize=2),1):
            bindings.extend(r['bindings']);errors.extend(r['errors'])
            if i%100==0:print(json.dumps({'scanned':i,'of':len(candidates),'bindings':len(bindings),'seconds':round(time.monotonic()-start)}),flush=True)
    matkeys=sorted({m['source_key']for b in bindings for m in b['materials']if m['source_key']})
    materials=[];missing=[]
    def refinfo(key):
        if not key:return {'source_key':None,'status':'null'}
        obj=resolve(key)
        if obj is None:
            miss={'source_key':key,'source_bundle':cabmap.get(key.rsplit(':',1)[0]),'status':'missing'}
            if miss not in missing:missing.append(miss)
            return miss
        return {'source_key':key,'source_bundle':cabmap.get(key.rsplit(':',1)[0]),'name':obj.peek_name(),'type':obj.type.name,'status':'resolved'}
    for mk in matkeys:
        o=resolve(mk)
        if o is None:refinfo(mk);continue
        d=o.read_typetree();props=d['m_SavedProperties'];shaderkey=pkey(o,d['m_Shader']);shaderinfo=refinfo(shaderkey)
        if shaderinfo.get('status')=='resolved'and not shaderinfo.get('name'):
            sh=resolve(shaderkey).read_typetree();shaderinfo['name']=sh.get('m_ParsedForm',{}).get('m_Name','')
        textures={}
        for slot,info in props['m_TexEnvs']:
            tk=pkey(o,info['m_Texture']);tinfo=refinfo(tk)
            if tinfo['status']=='resolved':
                td=resolve(tk).read();tinfo.update(width=getattr(td,'m_Width',None),height=getattr(td,'m_Height',None))
            tinfo.update(scale=info['m_Scale'],offset=info['m_Offset']);textures[slot]=tinfo
        materials.append({'source_key':mk,'source_bundle':cabmap[mk.rsplit(':',1)[0]],'name':d['m_Name'],
            'shader':shaderinfo,'textures':textures,'floats':dict(props['m_Floats']),'colors':dict(props['m_Colors']),
            'keywords':d.get('m_ValidKeywords',[]),'render_queue':d.get('m_CustomRenderQueue',-1),
            'tags':dict(d.get('stringTagMap',[]))})
    texby_bundle=collections.defaultdict(dict)
    for m in materials:
        for t in m['textures'].values():
            if t.get('status')=='resolved'and t.get('source_bundle'):
                texby_bundle[Path(t['source_bundle']).name][t['source_key'].rsplit(':',1)[1]]=t
    for l in (SOURCE/'reports/art_export.jsonl').open(encoding='utf8'):
        r=json.loads(l)
        if r['bundle']not in texby_bundle:continue
        for o in r['objects']:
            if o['type']=='Texture2D'and o['id']in texby_bundle[r['bundle']]:
                info=texby_bundle[r['bundle']][o['id']]
                # All material references share same source key; propagate below.
                info['exported_png']=[str(SOURCE/p)for p in o.get('outputs',[])]
    pngs={t['source_key']:t.get('exported_png')for m in materials for t in m['textures'].values()if t.get('exported_png')}
    for m in materials:
        for t in m['textures'].values():
            if t['source_key']in pngs:t['exported_png']=pngs[t['source_key']]
    for b in bindings:
        b['materials']=[refinfo(m['source_key'])for m in b['materials']]
    bound={b['mesh_key']for b in bindings};cross={b['mesh_key']for b in bindings if b['is_cross_bundle']}
    guild={k for k,v in TARGETS.items()if '00503_'in v['source_bundle']}
    summary={'target_meshes':len(TARGETS),'renderer_bundles_scanned':len(candidates),'bindings':len(bindings),
        'meshes_with_bindings':len(bound),'meshes_with_cross_bundle_bindings':len(cross),
        'guild_meshes':len(guild),'guild_meshes_with_cross_bundle_bindings':len(guild&cross),
        'materials_resolved':len(materials),'texture_objects_resolved':len({t['source_key']for m in materials for t in m['textures'].values()if t['status']=='resolved'}),
        'texture_objects_missing':len({t['source_key']for m in materials for t in m['textures'].values()if t['status']=='missing'}),
        'missing_refs':len(missing),'scan_errors':len(errors),'elapsed_seconds':round(time.monotonic()-start,1)}
    result={'summary':summary,'mesh_bindings':bindings,'materials':materials,'missing_references':missing,
        'scan_errors':errors,'source_cab_map':cabmap,
        'limitations':['Static PPtr bindings only; runtime material overrides are unknown.',
            'Multiple prefab variants are preserved, not collapsed to arbitrary first material.',
            'ParticleSystemRenderer mesh slots also recorded; renderer appearance depends on particle vertex data and shader.']}
    dest=HERE/'cross_bundle_bindings.json';dest.write_text(json.dumps(result,ensure_ascii=True,indent=2),encoding='utf8')
    print(json.dumps(summary),flush=True)

if __name__=='__main__':main()
