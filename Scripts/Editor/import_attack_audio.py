"""M8-032: import the attacker-side impact/whoosh WAVs as USoundWave assets."""
import json
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
STAGE = ROOT / 'SourceAssets' / 'Unpacked' / 'DNFM130_5_8' / 'attack_audio'
DEST = '/Game/UEMMO/DNFM/Audio'
ASSETS = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

for wav in sorted(STAGE.glob('*.wav')):
    name = 'SND_' + wav.stem
    asset_path = DEST + '/' + name
    if ASSETS.does_asset_exist(asset_path):
        ASSETS.delete_asset(asset_path)
    task = unreal.AssetImportTask()
    task.filename = str(wav)
    task.destination_path = DEST
    task.destination_name = name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    asset = unreal.load_asset(asset_path)
    if asset is None:
        raise RuntimeError('import failed: ' + asset_path)
    unreal.log('UEMMO M8-032: imported %s (%s)' % (asset_path, wav.name))

unreal.log('UEMMO M8-032: attack audio import done (%d files)' % len(list(STAGE.glob('*.wav'))))
