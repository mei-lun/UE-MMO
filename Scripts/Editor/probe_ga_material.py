"""M8-024 diagnostic: the map's actual M_GA_* material - is it textured?"""
import unreal

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
MEL = unreal.MaterialEditingLibrary

mat = unreal.load_asset('/Game/UEMMO/Imported/DNFM130_5_8/Materials/M_GA_tree_02_c781eb915d')
print('found:', mat is not None)
if mat:
    print('status:', ASSETS.get_metadata_tag(mat, 'DNFM.AppearanceStatus'))
    print('blend:', mat.get_editor_property('blend_mode'),
          'shading:', mat.get_editor_property('shading_model'))
    for e in MEL.get_material_expressions(mat):
        cls = e.get_class().get_name()
        info = cls
        if 'TextureSample' in cls:
            tex = e.get_editor_property('texture')
            info += ' texture=' + (tex.get_name() if tex else 'NULL')
        print(' ', info)
# list ALL M_GA_* materials and their statuses
ASSET_REGISTRY = unreal.AssetRegistryHelpers.get_asset_registry()
assets = ASSET_REGISTRY.get_assets_by_path('/Game/UEMMO/Imported/DNFM130_5_8/Materials', recursive=False)
print('materials dir count:', len(assets))
status_count = {}
ga = 0
for a in assets:
    name = str(a.asset_name)
    obj = a.get_asset()
    if not name.startswith('M_GA_'):
        continue
    ga += 1
    st = ''
    try:
        st = ASSETS.get_metadata_tag(obj, 'DNFM.AppearanceStatus') or 'no-tag'
    except Exception:
        st = 'tag-error'
    status_count[st] = status_count.get(st, 0) + 1
print('M_GA_* materials:', ga, 'statuses:', status_count)
