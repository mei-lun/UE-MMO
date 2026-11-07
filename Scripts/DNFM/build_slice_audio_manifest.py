"""M8-021: build the slice audio manifest from the M8-009 located index.

Reads Data/DNFMReplica/verify/audio_effects.json, picks the slice's six
victim-voice clips (the fighter's ft_dmg_01..03, the goblin's gbn_dmg_01..03),
verifies each source WAV against its recorded duration/format, hashes the
bytes, and writes Data/DNFMReplica/slice_audio_manifest.json (the import
script's source of truth). Read-only on the DNFM source tree.
"""
import hashlib
import json
import struct
import wave
from pathlib import Path

INDEX = Path(r'D:\Github-Poj\UE-MMO\Data\DNFMReplica\verify\audio_effects.json')
OUT = Path(r'D:\Github-Poj\UE-MMO\Data\DNFMReplica\slice_audio_manifest.json')
SOURCE_ROOT = Path(r'D:\Github-Poj\DNFM\DNFM_export_130.5.8.0\downloaded_130.5.8.202')
CLIPS = ('ft_dmg_01', 'ft_dmg_02', 'ft_dmg_03', 'gbn_dmg_01', 'gbn_dmg_02', 'gbn_dmg_03')


def wav_duration_s(path):
    with wave.open(str(path), 'rb') as w:
        return w.getnframes() / float(w.getframerate())


def main():
    index = json.loads(INDEX.read_text(encoding='utf-8'))
    found = {}
    def walk(o):
        if isinstance(o, dict):
            if 'clip' in o and 'wav' in o and o.get('clip') in CLIPS:
                found[o['clip']] = o
            else:
                for v in o.values():
                    walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(index['groups'])
    missing = [c for c in CLIPS if c not in found]
    if missing:
        raise RuntimeError('clips not located in the M8-009 index: %s' % missing)
    rows = []
    for clip in CLIPS:
        e = found[clip]
        wav_path = SOURCE_ROOT / e['wav']
        data = wav_path.read_bytes()
        duration = wav_duration_s(wav_path)
        if abs(duration - float(e['duration_s'])) > 0.02:
            raise RuntimeError('%s duration drift: %.3f vs %.3f' % (clip, duration, float(e['duration_s'])))
        rows.append({
            'clip': clip,
            'wav_source': e['wav'],
            'bytes': len(data),
            'duration_s': round(duration, 3),
            'rate_hz': e.get('rate_hz'),
            'channels': e.get('channels'),
            'sha256': hashlib.sha256(data).hexdigest(),
        })
    manifest = {
        'generator': 'Scripts/DNFM/build_slice_audio_manifest.py (M8-021)',
        'source_index': 'Data/DNFMReplica/verify/audio_effects.json (M8-009)',
        'source_root': str(SOURCE_ROOT),
        'clips': rows,
        'dest_path': '/Game/UEMMO/DNFM/Audio',
        'note': 'victim-side hit voices; wired through the existing CombatPresentationComponent SetAudioSounds chain (the slice minimum; attack whooshes/impacts belong to the P13 sweep)',
    }
    OUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=1), encoding='utf-8')
    for r in rows:
        print('%s %.3fs %dB %s' % (r['clip'], r['duration_s'], r['bytes'], r['sha256'][:12]))
    print('manifest:', OUT)


if __name__ == '__main__':
    main()
