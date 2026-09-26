"""Create (or refresh) the four attack montages from Data/combat-attacks.json.

For every attack entry this script creates /Game/UEMMO/Animation/Montages/
MNT_<attack_id> from the entry's animation_path asset (MM_Attack_01/02/03,
MM_ChargedAttack) via unreal.AnimMontageFactory with source_animation set.
The factory embeds the whole source clip into the montage's first slot track,
sets the composite length to the source play length and never modifies the
source asset itself.

Clip policy (M1-032): every current JSON entry is a full clip (clip_start 0,
clip_end == the asset play length within 0.01 s), so the montage keeps the
full clip and the C++ side compresses playback into DurationFrames / 60 s via
UCombatPresentationComponent::ComputeMontagePlayRate. Sub-clip entries are
rejected in the validate-first phase instead of silently played too long
(UAnimMontage's slot track is not Python-writable through set_editor_property,
so segment trimming would need an editor-only C++ helper; see the task report).

Idempotency: an existing montage whose slot segment already references the
configured animation is verified in place (no _1 duplicates ever); a mismatch
is deleted and recreated under the same name.

Run inside the UE editor only:
  UnrealEditor-Cmd <uproject> -run=pythonscript -script=<this file>

Modes (environment variable UEMMO_MONTAGE_MODE):
  create (default): validate-first, create/verify montages, save, write
                    Artifacts/Logs/attack-montages-report.json
  verify          : fresh-process reload check of the saved montages (segment
                    reference + positive length), writes the same report name
                    with mode=verify and raises on failure

ASCII only; safe to re-run.
"""
import json
import os
import traceback
from pathlib import Path

import unreal

MODE = os.environ.get('UEMMO_MONTAGE_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
DATA_PATH = ROOT / 'Data' / 'combat-attacks.json'
REPORT_DIR = ROOT / 'Artifacts' / 'Logs'
REPORT_PATH = REPORT_DIR / 'attack-montages-report.json'

MONTAGE_DIR = '/Game/UEMMO/Animation/Montages'
FULL_CLIP_TOLERANCE = 0.01  # seconds between clip_end_seconds and asset length


def montage_name(attack_id):
    return 'MNT_' + str(attack_id)


def montage_path(attack_id):
    return MONTAGE_DIR + '/' + montage_name(attack_id)


def write_report(payload):
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(payload, indent=1, default=str), encoding='utf-8')
    unreal.log('UEMMO_M1_032 wrote ' + str(REPORT_PATH))


def read_length(asset):
    """Length in seconds via the serialized sequence_length property, or None."""
    try:
        return float(asset.get_editor_property('sequence_length'))
    except Exception:
        return None


def read_segment_info(montage):
    """First slot-track segment summary, or None when the track is empty."""
    try:
        tracks = montage.get_editor_property('slot_anim_tracks')
        if len(tracks) < 1:
            return None
        track = tracks[0].get_editor_property('anim_track')
        segments = track.get_editor_property('anim_segments')
        if len(segments) < 1:
            return None
        segment = segments[0]
        anim = segment.get_editor_property('anim_reference')
        info = {
            'slot_name': str(tracks[0].get_editor_property('slot_name')),
            'anim_path': anim.get_path_name() if anim is not None else None,
            'start_time': segment.get_editor_property('anim_start_time'),
            'end_time': segment.get_editor_property('anim_end_time'),
            'play_rate': segment.get_editor_property('anim_play_rate'),
            'looping_count': segment.get_editor_property('looping_count'),
        }
        if info['anim_path'] is not None:
            info['anim_asset_path'] = info['anim_path'].split('.', 1)[0]
        return info
    except Exception:
        return {'error': traceback.format_exc(limit=2)}


def segment_matches(info, animation_path):
    return (
        info is not None
        and 'error' not in info
        and info.get('anim_asset_path') == animation_path
    )


def create_mode():
    # ---- validate-first phase: no asset is touched before this passes ------
    if not DATA_PATH.exists():
        raise RuntimeError('combat-attacks.json missing: ' + str(DATA_PATH))
    data = json.loads(DATA_PATH.read_text(encoding='utf-8'))
    if int(data.get('schema_version', 0)) != 1:
        raise RuntimeError('unexpected combat-attacks.json schema_version: ' + str(data.get('schema_version')))
    entries = data.get('attacks')
    if not entries:
        raise RuntimeError('combat-attacks.json has no attack entries')

    factory_class = getattr(unreal, 'AnimMontageFactory', None)
    report = {
        'mode': 'create',
        'montage_factory_found': factory_class is not None,
        'entries': [],
        'created': [],
        'verified': [],
        'recreated': [],
        'failed': [],
    }
    if factory_class is None:
        report['failed'] = ['unreal.AnimMontageFactory is not exposed to Python']
        report['success'] = False
        write_report(report)
        raise RuntimeError('unreal.AnimMontageFactory not available; montage generation aborted before any write.')

    validated = []
    for entry in entries:
        attack_id = entry.get('attack_id')
        record = {
            'attack_id': attack_id,
            'montage_path': montage_path(attack_id) if attack_id else None,
        }
        try:
            for key in ('duration_frames', 'animation_path', 'clip_start_seconds', 'clip_end_seconds'):
                if key not in entry:
                    raise RuntimeError('entry is missing ' + key)
            if int(entry['duration_frames']) <= 0:
                raise RuntimeError('duration_frames must be a positive integer')
            animation_path = str(entry['animation_path'])
            anim = unreal.load_asset(animation_path)
            if anim is None:
                raise RuntimeError('source animation not found: ' + animation_path)
            if not isinstance(anim, unreal.AnimSequence):
                raise RuntimeError('source animation is not an AnimSequence: ' + animation_path)
            skeleton = anim.get_editor_property('skeleton')
            if skeleton is None:
                raise RuntimeError('source animation has no skeleton: ' + animation_path)
            length = read_length(anim)
            if length is None or length <= 0.0:
                raise RuntimeError('source animation length unreadable or non-positive: ' + animation_path)
            clip_start = float(entry['clip_start_seconds'])
            clip_end = float(entry['clip_end_seconds'])
            if abs(clip_start) > FULL_CLIP_TOLERANCE:
                raise RuntimeError(
                    'clip_start_seconds %r is not a full clip start (0.0); sub-clip entries are rejected by this generator' % clip_start)
            if abs(clip_end - length) > FULL_CLIP_TOLERANCE:
                raise RuntimeError(
                    'clip_end_seconds %r does not match the asset length %r; sub-clip entries are rejected by this generator' % (clip_end, length))
            record.update({
                'animation_path': animation_path,
                'skeleton': skeleton.get_path_name(),
                'animation_length': length,
                'clip_start_seconds': clip_start,
                'clip_end_seconds': clip_end,
                'duration_frames': int(entry['duration_frames']),
            })
            validated.append((entry, anim, record))
        except Exception as error:  # noqa: BLE001 - collected into the report
            record['error'] = str(error)
            report['failed'].append(attack_id or '<unnamed>')
        report['entries'].append(record)

    if report['failed']:
        report['success'] = False
        write_report(report)
        raise RuntimeError('validation failed for entries: ' + ', '.join(report['failed']) + '; nothing was written.')

    # ---- generation phase ---------------------------------------------------
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

    for entry, anim, record in validated:
        attack_id = entry['attack_id']
        path = montage_path(attack_id)
        try:
            if assets.does_asset_exist(path):
                existing = unreal.load_asset(path)
                info = read_segment_info(existing) if existing is not None else None
                existing_length = read_length(existing) if existing is not None else None
                if (
                    isinstance(existing, unreal.AnimMontage)
                    and segment_matches(info, record['animation_path'])
                    and existing_length is not None
                    and abs(existing_length - record['animation_length']) <= FULL_CLIP_TOLERANCE
                ):
                    record['status'] = 'verified'
                    record['montage_length'] = existing_length
                    record['segment'] = info
                    assets.save_loaded_asset(existing, only_if_is_dirty=False)
                    report['verified'].append(path)
                    unreal.log('UEMMO_M1_032 verified existing montage ' + path)
                    continue
                # Wrong content under the mandated name: replace it in place
                # (delete + recreate) so no _1 duplicate is ever created.
                if not assets.delete_asset(path):
                    raise RuntimeError('existing montage could not be deleted: ' + path)
                record['replaced_wrong_content'] = True

            factory = factory_class()
            factory.set_editor_property('source_animation', anim)
            montage = tools.create_asset(montage_name(attack_id), MONTAGE_DIR, unreal.AnimMontage, factory)
            if montage is None:
                raise RuntimeError('AnimMontageFactory returned no montage for ' + path)
            montage_length = read_length(montage)
            if montage_length is not None and montage_length <= 0.0:
                raise RuntimeError('created montage has a non-positive composite length: ' + path)
            info = read_segment_info(montage)
            if not segment_matches(info, record['animation_path']):
                raise RuntimeError('created montage does not reference the source animation: ' + path)
            assets.save_loaded_asset(montage, only_if_is_dirty=False)
            record['status'] = record.get('replaced_wrong_content') and 'recreated' or 'created'
            record['montage_length'] = montage_length
            record['segment'] = info
            if record.get('replaced_wrong_content'):
                report['recreated'].append(path)
            else:
                report['created'].append(path)
            unreal.log('UEMMO_M1_032 created montage ' + path)
        except Exception as error:  # noqa: BLE001 - abort before further writes
            record['status'] = 'failed'
            record['error'] = str(error)
            report['failed'].append(attack_id)
            report['success'] = False
            write_report(report)
            raise RuntimeError('montage generation failed for %s; earlier results were saved. See %s' % (attack_id, REPORT_PATH))

    report['success'] = True
    write_report(report)
    unreal.log('UEMMO_M1_032_CREATE_SUCCESS created=%d verified=%d recreated=%d' % (
        len(report['created']), len(report['verified']), len(report['recreated'])))


def verify_mode():
    """Fresh-process reload check: load the saved montages from disk only."""
    data = json.loads(DATA_PATH.read_text(encoding='utf-8'))
    report = {'mode': 'verify', 'entries': [], 'failed': []}
    for entry in data.get('attacks', []):
        attack_id = entry.get('attack_id')
        animation_path = str(entry.get('animation_path'))
        record = {'attack_id': attack_id, 'montage_path': montage_path(attack_id)}
        try:
            montage = unreal.load_asset(montage_path(attack_id))
            if montage is None:
                raise RuntimeError('montage missing: ' + montage_path(attack_id))
            if not isinstance(montage, unreal.AnimMontage):
                raise RuntimeError('asset is not an AnimMontage: ' + montage_path(attack_id))
            length = read_length(montage)
            if length is None or length <= 0.0:
                raise RuntimeError('montage length unreadable or non-positive: ' + montage_path(attack_id))
            info = read_segment_info(montage)
            if not segment_matches(info, animation_path):
                raise RuntimeError('montage segment does not reference ' + animation_path)
            record['montage_length'] = length
            record['segment'] = info
            record['status'] = 'verified'
        except Exception as error:  # noqa: BLE001 - collected into the report
            record['status'] = 'failed'
            record['error'] = str(error)
            report['failed'].append(attack_id)
        report['entries'].append(record)
    report['success'] = not report['failed']
    write_report(report)
    if not report['success']:
        raise RuntimeError('montage reload check failed: ' + ', '.join(str(f) for f in report['failed']))
    unreal.log('UEMMO_M1_032_VERIFY_SUCCESS')


if MODE == 'create':
    create_mode()
elif MODE == 'verify':
    verify_mode()
else:
    raise RuntimeError('Unknown UEMMO_MONTAGE_MODE: ' + MODE)
