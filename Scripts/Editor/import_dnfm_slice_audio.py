"""M8-021: import the slice's six victim-voice WAVs as USoundWave assets.

Source of truth: Data/DNFMReplica/slice_audio_manifest.json (built by
Scripts/DNFM/build_slice_audio_manifest.py from the M8-009 located index).
Each WAV imports at /Game/UEMMO/DNFM/Audio/SND_<clip>; the verify mode
(UEMMOSliceAudioVerify) reloads every asset in a fresh process and compares
the durations against the manifest.
"""
import json
import os
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
MANIFEST = ROOT / 'Data' / 'DNFMReplica' / 'slice_audio_manifest.json'
SOURCE_ROOT = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\downloaded_130.5.8.202')
DEST = '/Game/UEMMO/DNFM/Audio'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Tasks' / 'M8-021' / 'slice-audio-import.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Tasks' / 'M8-021' / 'slice-audio-verify.json'

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()


def generate():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    imported = []
    for row in manifest['clips']:
        wav = SOURCE_ROOT / row['wav_source']
        name = 'SND_' + row['clip']
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
            raise RuntimeError('import produced no asset: ' + asset_path)
        imported.append({'name': name, 'clip': row['clip'],
                         'sha256': row['sha256'], 'duration_s': row['duration_s']})
        unreal.log('UEMMO_SLICE_AUDIO_IMPORTED %s (%s)' % (asset_path, row['clip']))
    report = {'mode': 'generate', 'imported': imported, 'success': True}
    GENERATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    GENERATE_REPORT.write_text(json.dumps(report, indent=1), encoding='utf-8')


def verify():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    problems = []
    for row in manifest['clips']:
        asset = unreal.load_asset(DEST + '/SND_' + row['clip'])
        if asset is None:
            problems.append(row['clip'] + ': missing')
            continue
        duration = float(asset.get_editor_property('duration'))
        if abs(duration - float(row['duration_s'])) > 0.02:
            problems.append('%s: duration %.3f vs %.3f' % (row['clip'], duration, float(row['duration_s'])))
    report = {'mode': 'verify', 'problems': problems,
              'success': not problems}
    VERIFY_REPORT.parent.mkdir(parents=True, exist_ok=True)
    VERIFY_REPORT.write_text(json.dumps(report, indent=1), encoding='utf-8')
    if problems:
        raise RuntimeError('slice audio verify failed: %s' % problems)
    unreal.log('UEMMO_SLICE_AUDIO_VERIFIED')


if 'UEMMOSliceAudioVerify' in unreal.SystemLibrary.get_command_line():
    verify()
else:
    generate()
