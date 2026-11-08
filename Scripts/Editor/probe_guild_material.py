"""M8-024 diagnostic: inspect one rebuilt material's graph and one mesh's
vertex colors (the dark-render suspects)."""
import unreal

ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
MEL = unreal.MaterialEditingLibrary

mat = unreal.load_asset('/Game/UEMMO/Imported/DNFM130_5_8/Materials/M_Material_3727_363224bb38')
print('status:', ASSETS.get_metadata_tag(mat, 'DNFM.AppearanceStatus'))
print('blend:', mat.get_editor_property('blend_mode'),
      'shading:', mat.get_editor_property('shading_model'))
exprs = MEL.get_material_expressions(mat)
print('expressions:', len(exprs))
for e in exprs:
    cls = e.get_class().get_name()
    info = cls
    if cls == 'MaterialExpressionTextureSample':
        tex = e.get_editor_property('texture')
        info += ' texture=' + (tex.get_name() if tex else 'NULL')
    print(' ', info)
