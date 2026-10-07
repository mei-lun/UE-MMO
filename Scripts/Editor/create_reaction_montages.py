"""Create (or refresh) the M5-017 victim reaction montages.

Two rows carry real assets (the engine-template mannequin clips on the UE5
skeleton the player and enemies wear):

  RCT_Hit  <- /Game/Characters/Mannequins/Anims/Rifle/HitReact/MM_HitReact_Front_Med_01
  RCT_Dead <- /Game/Characters/Mannequins/Anims/Death/MM_Death_Back_01

For every row this script creates /Game/UEMMO/Animation/Reactions/<name> via
unreal.AnimMontageFactory with source_animation set. The factory embeds the
whole source clip into the montage's first slot track (DefaultSlot - the same
slot node the attack montages render through), sets the composite length to
the source play length and never modifies the source asset itself. The
launch/down/recover rows of Data/CombatSystem/presentations.json stay honest
placeholders: the engine template and the free Quaternius tier carry no
mannequin clips for those phases, so no asset is fabricated here.

Idempotency: an existing montage whose slot segment already references the
configured animation is verified in place (no _1 duplicates ever); a mismatch
is deleted and recreated under the same name.

Run inside the UE editor only:
  UnrealEditor-Cmd <uproject> -run=pythonscript -script=<this file>

Modes (environment variable UEMMO_REACTION_MONTAGE_MODE):
  create (default): validate-first, create/verify montages, save, write
                    Artifacts/Logs/reaction-montages-report.json
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

MODE = os.environ.get('UEMMO_REACTION_MONTAGE_MODE', 'create').lower()

ROOT = Path(unreal.Paths.project_dir())
REPORT_DIR = ROOT / 'Artifacts' / 'Logs'
REPORT_PATH = REPORT_DIR / 'reaction-montages-report.json'

MONTAGE_DIR = '/Game/UEMMO/Animation/Reactions'

# (montage name, source animation package path)
ROWS = [
    ('RCT_Hit', '/Game/Characters/Mannequins/Anims/Rifle/HitReact/MM_HitReact_Front_Hvy_01'),
    ('RCT_Dead', '/Game/Characters/Mannequins/Anims/Death/MM_Death_Back_01'),
]


def montage_path(name):
    return MONTAGE_DIR + '/' + name


def write_report(payload):
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(payload, indent=1, default=str), encoding='utf-8')
    unreal.log('UEMMO_M5_017 wrote ' + str(REPORT_PATH))


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


def validate_row(name, animation_path):
    """Loads the source animation and returns (anim, length) or raises."""
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
    return anim, length


def create_mode():
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

    # ---- validate-first phase: no asset is touched before this passes ------
    validated = []
    for name, animation_path in ROWS:
        record = {'montage': name, 'montage_path': montage_path(name)}
        try:
            anim, length = validate_row(name, animation_path)
            record.update({
                'animation_path': animation_path,
                'skeleton': anim.get_editor_property('skeleton').get_path_name(),
                'animation_length': length,
            })
            validated.append((name, animation_path, anim, length, record))
        except Exception as error:  # noqa: BLE001 - collected into the report
            record['error'] = str(error)
            report['failed'].append(name)
        report['entries'].append(record)

    if report['failed']:
        report['success'] = False
        write_report(report)
        raise RuntimeError('validation failed for rows: ' + ', '.join(report['failed']) + '; nothing was written.')

    # ---- generation phase ---------------------------------------------------
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)

    for name, animation_path, anim, length, record in validated:
        path = montage_path(name)
        try:
            if assets.does_asset_exist(path):
                existing = unreal.load_asset(path)
                info = read_segment_info(existing) if existing is not None else None
                existing_length = read_length(existing) if existing is not None else None
                if (
                    isinstance(existing, unreal.AnimMontage)
                    and segment_matches(info, animation_path)
                    and existing_length is not None
                    and abs(existing_length - length) <= 0.01
                ):
                    record['status'] = 'verified'
                    record['montage_length'] = existing_length
                    record['segment'] = info
                    assets.save_loaded_asset(existing, only_if_is_dirty=False)
                    report['verified'].append(path)
                    unreal.log('UEMMO_M5_017 verified existing montage ' + path)
                    continue
                if not assets.delete_asset(path):
                    raise RuntimeError('existing montage could not be deleted: ' + path)
                # The in-memory asset registry can lag the deletion and make
                # the factory refuse the same-package create; force a
                # synchronous rescan and require the deletion to be visible.
                unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([MONTAGE_DIR], True)
                if assets.does_asset_exist(path) or unreal.load_asset(path) is not None:
                    raise RuntimeError('deleted montage is still resolvable: ' + path)
                record['replaced_wrong_content'] = True

            factory = factory_class()
            factory.set_editor_property('source_animation', anim)
            montage = tools.create_asset(name, MONTAGE_DIR, unreal.AnimMontage, factory)
            if montage is None:
                raise RuntimeError('AnimMontageFactory returned no montage for ' + path)
            montage_length = read_length(montage)
            if montage_length is not None and montage_length <= 0.0:
                raise RuntimeError('created montage has a non-positive composite length: ' + path)
            info = read_segment_info(montage)
            if not segment_matches(info, animation_path):
                raise RuntimeError('created montage does not reference the source animation: ' + path)
            assets.save_loaded_asset(montage, only_if_is_dirty=False)
            record['status'] = record.get('replaced_wrong_content') and 'recreated' or 'created'
            record['montage_length'] = montage_length
            record['segment'] = info
            if record.get('replaced_wrong_content'):
                report['recreated'].append(path)
            else:
                report['created'].append(path)
            unreal.log('UEMMO_M5_017 created montage ' + path)
        except Exception as error:  # noqa: BLE001 - abort before further writes
            record['status'] = 'failed'
            record['error'] = str(error)
            report['failed'].append(name)
            report['success'] = False
            write_report(report)
            raise RuntimeError('montage generation failed for %s; earlier results were saved. See %s' % (name, REPORT_PATH))

    report['success'] = True
    write_report(report)
    unreal.log('UEMMO_M5_017_CREATE_SUCCESS created=%d verified=%d recreated=%d' % (
        len(report['created']), len(report['verified']), len(report['recreated'])))


def verify_mode():
    """Fresh-process reload check: load the saved montages from disk only."""
    report = {'mode': 'verify', 'entries': [], 'failed': []}
    for name, animation_path in ROWS:
        record = {'montage': name, 'montage_path': montage_path(name), 'animation_path': animation_path}
        try:
            montage = unreal.load_asset(montage_path(name))
            if montage is None:
                raise RuntimeError('saved montage failed to load from disk: ' + montage_path(name))
            info = read_segment_info(montage)
            if not segment_matches(info, animation_path):
                raise RuntimeError('saved montage does not reference the source animation: ' + str(info))
            length = read_length(montage)
            if length is None or length <= 0.0:
                raise RuntimeError('saved montage length unreadable or non-positive: ' + montage_path(name))
            record['status'] = 'verified'
            record['montage_length'] = length
            record['segment'] = info
            report['entries'].append(record)
            unreal.log('UEMMO_M5_017 verify ok ' + montage_path(name))
        except Exception as error:  # noqa: BLE001 - collected into the report
            record['error'] = str(error)
            report['failed'].append(name)
            report['entries'].append(record)
    report['success'] = len(report['failed']) == 0
    write_report(report)
    if not report['success']:
        raise RuntimeError('verification failed for: ' + ', '.join(report['failed']))
    unreal.log('UEMMO_M5_017_VERIFY_SUCCESS verified=%d' % len(report['entries']))


def main():
    try:
        if MODE == 'verify':
            verify_mode()
        else:
            create_mode()
    except Exception:
        unreal.log_error('UEMMO_M5_017 failed:\n' + traceback.format_exc())
        raise


main()
