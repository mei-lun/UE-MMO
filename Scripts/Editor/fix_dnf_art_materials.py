"""Rebuild materials for the imported DNFArt meshes (M6-008).

Interchange imported the slot MaterialInstanceConstants without usable
texture wiring, so the preview renders black. This script creates one master
material (BaseColor texture parameter + Roughness scalar), one MIC per mesh
material slot wired to the imported <slot>_basecolor texture, and assigns the
MICs back onto the skeletal meshes.

Run with:  UnrealEditor.exe <uproject> -ExecutePythonScript=<this file>
"""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
DEST = '/Game/UEMMO/ArtValidation/DNFArt'
REPORT = ROOT / 'Artifacts' / 'Logs' / 'dnf-art-materials-report.json'

CHARACTERS = [
    {'id': 'hero', 'dest': DEST + '/Hero', 'mesh': 'hero_skm'},
    {'id': 'goblin', 'dest': DEST + '/Goblin', 'mesh': 'goblin_skm'},
]


def build_master(path):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.load_asset(path)
    factory = unreal.MaterialFactoryNew()
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat = asset_tools.create_asset('M_DNFArtBase', DEST, unreal.Material, factory)
    tex_node = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0)
    tex_node.set_editor_property('parameter_name', 'BaseColor')
    rough_node = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionScalarParameter, -400, 220)
    rough_node.set_editor_property('parameter_name', 'Roughness')
    rough_node.set_editor_property('default_value', 0.85)
    unreal.MaterialEditingLibrary.connect_material_property(tex_node, '', unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(rough_node, '', unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)
    return mat


def fix_character(cfg):
    out = {'slots': []}
    mesh = unreal.load_asset('%s/%s' % (cfg['dest'], cfg['mesh']))
    if not mesh:
        raise RuntimeError('mesh missing: %s' % cfg['mesh'])
    master = build_master(DEST + '/M_DNFArtBase')
    slot_names = [str(sm.get_editor_property('material_slot_name'))
                  for sm in mesh.get_editor_property('materials')]
    for idx, slot in enumerate(slot_names):
        tex = unreal.load_asset('%s/%s_basecolor' % (cfg['dest'], slot))
        mic_path = '%s/%s' % (cfg['dest'], slot)
        mic = unreal.load_asset(mic_path) if unreal.EditorAssetLibrary.does_asset_exist(mic_path) else None
        if not mic:
            out['slots'].append({'slot': slot, 'mic': None, 'texture': None, 'note': 'no imported MIC'})
            continue
        unreal.MaterialEditingLibrary.set_material_instance_parent(mic, master)
        if tex:
            unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(mic, 'BaseColor', tex)
        unreal.EditorAssetLibrary.save_asset(mic_path)
        out['slots'].append({'slot': slot, 'mic': str(mic.get_name()), 'texture': str(tex.get_name()) if tex else None})
    unreal.EditorAssetLibrary.save_asset('%s/%s' % (cfg['dest'], cfg['mesh']))
    return out


def main():
    report = {'success': False, 'characters': {}}
    try:
        for cfg in CHARACTERS:
            report['characters'][cfg['id']] = fix_character(cfg)
        report['success'] = True
    except Exception:
        import traceback
        report['error'] = traceback.format_exc()
    REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')


main()
