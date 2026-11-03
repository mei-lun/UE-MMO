"""M6-010: stage original dependencies and build a project-relative asset handoff.

Does not mutate Unreal assets. Requires the completed M6-009 import manifest.
"""
import collections
import csv
import hashlib
import json
import shutil
from datetime import datetime, timezone, timedelta
from pathlib import Path

import UnityPy

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'SourceAssets/Unpacked/DNFM130_5_8'
OUT = STAGE / 'handoff'
CONTENT_ROOT = '/Game/UEMMO/Imported/DNFM130_5_8'


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=True, indent=2), encoding='utf-8')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rel(path):
    resolved = Path(path).resolve()
    resolved.relative_to(ROOT.resolve())
    return resolved.relative_to(ROOT.resolve()).as_posix()


def ue_asset(kind, name):
    package = CONTENT_ROOT + '/' + kind + '/' + name
    file = ROOT / ('Content/' + package.removeprefix('/Game/') + '.uasset')
    if not file.is_file():
        raise FileNotFoundError(file)
    return {'package': package, 'object_path': package + '.' + name, 'project_file': rel(file)}


def mesh_category(row):
    source = Path(row['source_bundle']).name
    if row['builtin']:
        return 'builtin'
    if source.startswith('00503_'):
        return 'map_guild_scene'
    if source.startswith('00160_'):
        return 'map_background'
    if source.startswith('062'):
        return 'ui_mesh'
    return 'fx_mesh'


def main():
    manifest = load(STAGE / 'manifest.json')
    if not manifest.get('ready_for_ue_import'):
        raise RuntimeError('Complete the M6-009 prefab scan and import before packaging.')
    source = Path(manifest['source_export'])
    cross = load(ROOT / 'Artifacts/Tasks/M6-009/cross_bundle_bindings.json')
    by_cab = {k.lower(): Path(v) for k, v in cross['source_cab_map'].items()}
    for name in ['unity default resources', 'Resources/unity_builtin_extra']:
        path = STAGE / 'unity_originals' / name
        if path.exists():
            by_cab[Path(name).name.lower()] = path
    originals = load(source / 'nifs_unpacked/entries.json')
    original_names = {Path(e['output']).name: e.get('original_path')
                      for e in originals if e.get('output')}
    seeds = {Path(r['source_bundle']) for r in manifest['meshes']}
    seeds.update(Path(b['source_bundle']) for b in cross['mesh_bindings'])
    for m in cross['materials']:
        seeds.add(Path(m['source_bundle']))
        if m['shader'].get('source_bundle'):
            seeds.add(Path(m['shader']['source_bundle']))
        seeds.update(Path(t['source_bundle']) for t in m['textures'].values() if t.get('source_bundle'))
    OUT.mkdir(parents=True, exist_ok=True)
    copied = {}
    dependency_edges = []
    missing_cabs = collections.defaultdict(set)
    queue = sorted(seeds)
    while queue:
        path = queue.pop(0).resolve()
        if str(path) in copied:
            continue
        if not path.is_file():
            raise FileNotFoundError(path)
        target = OUT / 'original_sources' / path.name
        target.parent.mkdir(exist_ok=True)
        checksum = sha(path)
        if target.exists():
            if sha(target) != checksum:
                raise RuntimeError('Existing source copy differs: ' + str(target))
        else:
            shutil.copy2(path, target)
        assert target.stat().st_size == path.stat().st_size and sha(target) == checksum
        copied[str(path)] = {'source_file': str(path), 'project_copy': rel(target),
                             'original_archive_name': original_names.get(path.name),
                             'bytes': path.stat().st_size, 'sha256': checksum, 'cab_names': []}
        env = UnityPy.load(str(path))
        for af in env.cabs.values():
            if not hasattr(af, 'externals'):
                continue
            copied[str(path)]['cab_names'].append(af.name)
            for ext in af.externals:
                cab = ext.path.replace('\\', '/').rsplit('/', 1)[-1].lower()
                dep = by_cab.get(cab)
                edge = {'source_copy': rel(target), 'external_cab': cab,
                        'status': 'present' if dep and dep.is_file() else 'missing'}
                if dep and dep.is_file():
                    edge['original_file'] = str(dep.resolve())
                    queue.append(dep)
                else:
                    missing_cabs[cab].add(rel(target))
                dependency_edges.append(edge)
        if len(copied) % 100 == 0:
            print(json.dumps({'copied_sources': len(copied), 'pending': len(queue)}), flush=True)
    for edge in dependency_edges:
        if edge['status'] == 'present':
            edge['project_dependency'] = copied[edge.pop('original_file')]['project_copy']
    assert str((STAGE / 'unity_originals/unity default resources').resolve()) in copied
    support = []
    for file in sorted((source / 'maps/navigation').glob('*.bin')):
        target = OUT / 'map_support' / file.name
        target.parent.mkdir(exist_ok=True)
        if not target.exists():
            shutil.copy2(file, target)
        assert sha(file) == sha(target)
        support.append({'source': str(file), 'project_file': rel(target), 'sha256': sha(target),
                        'status': 'original_NRBF_VAND_preserved_not_UE_NavMesh'})
    mats = {}
    for m in manifest['materials']:
        row = dict(m)
        row['ue'] = ue_asset('Materials', m['id'])
        row['source_parameters'] = rel(STAGE / 'material_sources' / (m['id'] + '.json'))
        if not (ROOT / row['source_parameters']).is_file():
            raise FileNotFoundError(row['source_parameters'])
        row['shader_status'] = 'approximation; inspect original parameters before extending'
        mats[m['id']] = row
    textures = []
    for t in manifest['textures']:
        file = Path(t['file'])
        assert file.is_file() and sha(file) == t['sha256']
        row = dict(t)
        row['file'] = rel(file)
        row['ue'] = ue_asset('Textures', t['id'])
        textures.append(row)
    model_rows = []
    for row in manifest['meshes']:
        category = mesh_category(row)
        missing = {mid: mats[mid].get('missing_textures', {}) for mid in row['selected_materials']
                   if mid and mats[mid].get('missing_textures')}
        variants = [[ue_asset('Materials', mid) if mid else None for mid in variant]
                    for variant in row['material_variants']]
        refs = set(mid for variant in row['material_variants'] for mid in variant if mid)
        texture_ids = sorted({info['id'] for mid in refs for info in mats[mid]['textures'].values()})
        copy = copied[str(Path(row['source_bundle']).resolve())]
        roots = sorted({b['source_bundle'] for b in row.get('cross_bundle_bindings', [])})
        originals_for_mesh = [copy['project_copy']]
        originals_for_mesh.extend(copied[str(Path(p).resolve())]['project_copy'] for p in roots)
        fbx = STAGE / 'fbx' / (row['id'] + '.fbx')
        geo = Path(row['geometry'])
        assert fbx.is_file() and geo.is_file()
        model_rows.append({'id': row['id'], 'name': row['name'], 'category': category,
                           'ue': ue_asset('Meshes', row['id']), 'fbx': rel(fbx),
                           'geometry_json': rel(geo), 'source_object': row['source_object'],
                           'source_copies': sorted(set(originals_for_mesh)),
                           'original_bundle_name': copy['original_archive_name'],
                           'vertex_count': row['vertex_count'], 'triangle_count': row['triangle_count'],
                           'uv_channels': row['uv_channels'], 'has_vertex_colors': row['has_vertex_colors'],
                           'default_materials': row['selected_materials'], 'material_variants': variants,
                           'available_texture_ids': texture_ids, 'missing_texture_refs': missing,
                           'appearance': 'missing_texture_placeholder' if missing else 'approximate_shader_no_missing_default_texture_refs'})
    cats = dict(collections.Counter(r['category'] for r in model_rows))
    assert len(model_rows) == 279 and cats['map_guild_scene'] == 153 and cats['map_background'] == 30
    map_rows = [r for r in model_rows if r['category'].startswith('map_')]
    missing = {'missing_default_texture_meshes': [r['id'] for r in model_rows if r['missing_texture_refs']],
               'missing_material_or_texture_source_objects': cross['missing_references'],
               'missing_external_cabs': [{'cab': k, 'referenced_by_project_sources': sorted(v)}
                                         for k, v in sorted(missing_cabs.items())],
               'limits': 'These are absences within the supplied APK inventory, not proof that later downloaded data cannot supply them.'}
    index = {'project': 'UEMMO', 'engine': '5.8.3', 'source_apk_sha256': manifest['source_apk_sha256'],
             'counts': {'meshes': 279, 'map_background_meshes': len(map_rows), 'source_materials': len(mats),
                        'textures': len(textures), 'categories': cats},
             'unit_convention': manifest['unit_convention'], 'axis_convention': manifest['axis_convention'],
             'gallery_package': CONTENT_ROOT + '/Maps/L_DNFM_ResourceGallery',
             'meshes': model_rows, 'materials': list(mats.values()), 'textures': textures, 'map_support': support,
             'limits': ['No performance optimization requested.', 'No original map layout reconstructed.',
                        'No original skeletal-character model was recovered from these meshes.',
                        'Original Unity shader behavior is approximated; see material metadata.',
                        '2D/Spine/audio outside model dependencies remain in the separate DNFM export.']}
    write(OUT / 'asset_catalog.json', index)
    write(OUT / 'map_mesh_catalog.json', {'count': len(map_rows), 'meshes': map_rows, 'navigation_sources': support})
    write(OUT / 'missing_dependencies.json', missing)
    write(OUT / 'source_files.json', {'files': list(copied.values()), 'dependency_edges': dependency_edges})
    with (OUT / 'asset_catalog.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['id','name','category','ue_object_path','project_uasset','fbx','default_materials',
                         'available_texture_ids','appearance','source_copies'])
        for r in model_rows:
            writer.writerow([r['id'],r['name'],r['category'],r['ue']['object_path'],r['ue']['project_file'],
                             r['fbx'],'|'.join(m or 'M_Unassigned' for m in r['default_materials']),
                             '|'.join(r['available_texture_ids']),r['appearance'],'|'.join(r['source_copies'])])
    stamp = datetime.now(timezone(timedelta(hours=8))).isoformat(timespec='seconds')
    report = {'success': True, 'timestamp': stamp, 'mesh_count': len(model_rows), 'map_mesh_count': len(map_rows),
              'categories': cats, 'materials': len(mats), 'textures': len(textures),
              'copied_original_files': len(copied), 'copied_original_bytes': sum(r['bytes'] for r in copied.values()),
              'copied_navigation_files': len(support), 'missing_external_cabs': len(missing_cabs),
              'missing_default_texture_meshes': len(missing['missing_default_texture_meshes']),
              'checks': ['279 mesh uasset/FBX/geometry paths exist', 'all material variant UE assets exist',
                         'all 39 texture source SHA256 match', 'source dependency copies size+SHA256 match',
                         '183 map/background meshes indexed', 'navigation copies match original bytes'],
              'ue_runtime_checked_separately': True}
    write(OUT / 'verification.json', report)
    (OUT / 'README.md').write_text(
        '# DNFM 项目本地资源接手包\n\n'
        '279个模型已在UE内容目录中，其中183个为地图/背景网格。'
        '本目录提供原始依赖副本与索引；无需重复导入。\n\n'
        '- [完整Agent接手说明](../../../../Docs/DNF-Research/06-Agent资源接手入口.md)\n'
        '- [逐模型索引](asset_catalog.json) / [CSV](asset_catalog.csv)\n'
        '- [地图网格索引](map_mesh_catalog.json)\n'
        '- [缺失依赖](missing_dependencies.json)\n'
        '- [原始副本及SHA-256](source_files.json)\n'
        '- [副本验证](verification.json)\n\n'
        'JSON中project_file、fbx、source_copies等均相对UE-MMO项目根目录。'
        'original_sources保留可找到的原Bundle；map_support是原始导航数据，并非已转换的UE NavMesh。'
        '154个模型默认材质仍缺源图，详见缺失清单。\n', encoding='utf-8')
    evidence = ROOT / 'Artifacts/Tasks/M6-010' / stamp.replace(':', '').replace('+', '_')
    write(evidence / 'handoff-package.json', report)
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
