"""Create /Game/UEMMO/Professions/DA_ProfessionConfig from Data/Professions.

Run inside the UE editor only (UnrealEditor-Cmd -run=pythonscript). This is
the M7-001 editor entry: it calls the C++ builder
UProfessionDataAssetBuilderLibrary.build_from_source_directory, which parses
the six profession tables (manifest + professions + level_curves + skills +
skill_levels + loadouts), validates the full cross-table gate and saves one
config data asset under /Game/UEMMO/Professions. The heavy lifting (strict
schema, local row validators, cross-table gate, revision digest) lives in
Source/UEMMO/Profession/*; this script is the editor wrapper that reports the
outcome to Artifacts/Logs/profession-assets-report.json:
  {mode, source_dir, success, created, updated, revision, errors}

Modes:
  generate (default) - build/update the asset; write the report.
  verify             - UEMMOProfessionAssetsVerify env var: a FRESH process
                       reloads the saved asset from disk and compares every
                       field against the same JSON rows plus the revision
                       recorded by the last generate run; writes
                       profession-assets-verify.json. No asset is written.

Failure contract: everything is validated before the first write, so any
parse/validation/save failure leaves the existing assets untouched and
reports success=false with the error text naming the offending table, row and
field. The script raises after writing the report so the UE process exits
nonzero.

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir())
SOURCE_DIR = ROOT / 'Data' / 'Professions'
DEST = '/Game/UEMMO/Professions'
ASSET = DEST + '/DA_ProfessionConfig'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'profession-assets-report.json'
VERIFY_REPORT = ROOT / 'Artifacts' / 'Logs' / 'profession-assets-verify.json'

MODE = os.environ.get('UEMMOProfessionAssetsMode', 'generate').lower()

TABLE_FILES = ('professions', 'level_curves', 'skills', 'skill_levels', 'loadouts')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def generate():
    builder = unreal.ProfessionDataAssetBuilderLibrary
    # UFUNCTION out-params come back as a tuple after the return value:
    # (bool, OutError, OutRevision, OutCreated, OutUpdated)
    result = builder.build_from_source_directory(str(SOURCE_DIR), DEST)
    # UE python binding variants: 4-tuple (error, revision, created, updated)
    # or 5-tuple prefixed with the bool return. Unpack tolerantly.
    if len(result) == 5:
        ok, error, revision, created, updated = result
    else:
        error, revision, created, updated = result
        ok = (not error)
    report = {
        'mode': 'generate',
        'source_dir': str(SOURCE_DIR),
        'success': bool(ok),
        'created': bool(created),
        'updated': bool(updated),
        'revision': revision,
        'errors': error,
    }
    if ok:
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    GENERATE_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    unreal.log_warning('[ProfessionAssets] generate success=%s revision=%s' % (ok, revision))
    return report


def verify():
    errors = []
    tables = {name: read_json(SOURCE_DIR / (name + '.json')) for name in TABLE_FILES}
    asset = unreal.load_asset(ASSET)
    if not asset:
        errors.append('asset missing: %s' % ASSET)
        asset = None

    revision = ''
    if asset:
        revision = asset.get_editor_property('config_revision')
        gen_report_path = GENERATE_REPORT
        if gen_report_path.exists():
            gen = json.loads(gen_report_path.read_text(encoding='utf-8'))
            if gen.get('revision') and gen.get('revision') != revision:
                errors.append('revision mismatch: asset=%s generate=%s' % (revision, gen.get('revision')))
        else:
            errors.append('generate report missing (run generate first)')

    def rows_of(name):
        return tables[name]['rows']

    if asset:
        # professions: id -> (display_name, curve_id, placeholder)
        expected_prof = {r['profession_id']: r for r in rows_of('professions')}
        got_prof = {}
        for row in asset.get_editor_property('professions'):
            got_prof[str(row.profession_id)] = {
                'display_name': row.display_name,
                'curve_id': str(row.curve_id),
                'asset_placeholder': row.asset_placeholder,
            }
        if set(got_prof) != set(expected_prof):
            errors.append('professions id set mismatch: %s vs %s' % (sorted(got_prof), sorted(expected_prof)))
        for pid, want in expected_prof.items():
            got = got_prof.get(pid)
            if not got:
                continue
            if got['display_name'] != want['display_name'] or got['curve_id'] != want['curve_id'] \
                    or got['asset_placeholder'] != want['asset_placeholder']:
                errors.append('professions[%s] field mismatch' % pid)

        # level_curves: (curve_id, level) -> all numeric fields
        expected_curve = {(r['curve_id'], r['level']): r for r in rows_of('level_curves')}
        got_curve = {}
        for row in asset.get_editor_property('level_curves'):
            got_curve[(str(row.curve_id), row.level)] = {
                'xp_to_next': row.xp_to_next,
                'base_max_hp': row.base_max_hp,
                'base_attack': row.base_attack,
                'base_defense': row.base_defense,
                'base_max_mp': row.base_max_mp,
                'base_mp_regen': row.base_mp_regen,
                'skill_points': row.skill_points,
            }
        if set(got_curve) != set(expected_curve):
            errors.append('level_curves key set mismatch: %d vs %d' % (len(got_curve), len(expected_curve)))
        for key, want in expected_curve.items():
            got = got_curve.get(key)
            if not got:
                continue
            for field in ('xp_to_next', 'base_max_hp', 'base_attack', 'base_defense',
                          'base_max_mp', 'base_mp_regen', 'skill_points'):
                if got[field] != want[field]:
                    errors.append('level_curves%s %s: %s != %s' % (key, field, got[field], want[field]))

        # skills: id -> (profession, max_rank, policy, attack_pending)
        expected_skill = {r['skill_id']: r for r in rows_of('skills')}
        got_skill = {}
        for row in asset.get_editor_property('skills'):
            got_skill[str(row.skill_id)] = {
                'profession_id': str(row.profession_id),
                'max_rank': row.max_rank,
                'attack_id': str(row.attack_id),
                'attack_pending': row.attack_pending,
                'placeholder': row.placeholder,
            }
        if set(got_skill) != set(expected_skill):
            errors.append('skills id set mismatch')
        for sid, want in expected_skill.items():
            got = got_skill.get(sid)
            if not got:
                continue
            if got['profession_id'] != want['profession_id'] or got['max_rank'] != want['max_rank'] \
                    or got['attack_id'] != want['attack_id'] or got['attack_pending'] != want['attack_pending'] \
                    or got['placeholder'] != want['placeholder']:
                errors.append('skills[%s] field mismatch' % sid)

        # skill_levels: (skill_id, rank) -> numbers
        expected_level = {(r['skill_id'], r['rank']): r for r in rows_of('skill_levels')}
        got_level = {}
        for row in asset.get_editor_property('skill_levels'):
            got_level[(str(row.skill_id), row.rank)] = {
                'required_character_level': row.required_character_level,
                'sp_cost': row.sp_cost,
                'cooldown_s': round(float(row.cooldown_s), 4),
                'mp_cost': row.mp_cost,
                'damage_coefficient': round(float(row.damage_coefficient), 4),
            }
        if set(got_level) != set(expected_level):
            errors.append('skill_levels key set mismatch')
        for key, want in expected_level.items():
            got = got_level.get(key)
            if not got:
                continue
            for field in ('required_character_level', 'sp_cost', 'mp_cost'):
                if got[field] != want[field]:
                    errors.append('skill_levels%s %s: %s != %s' % (key, field, got[field], want[field]))
            if abs(got['cooldown_s'] - want['cooldown_s']) > 1e-4:
                errors.append('skill_levels%s cooldown_s mismatch' % (key,))
            if abs(got['damage_coefficient'] - want['damage_coefficient']) > 1e-4:
                errors.append('skill_levels%s damage_coefficient mismatch' % (key,))

        # loadouts: (profession, slot) -> skill
        expected_load = {(r['profession_id'], r['slot_index']): r['skill_id'] for r in rows_of('loadouts')}
        got_load = {(str(row.profession_id), row.slot_index): str(row.skill_id)
                    for row in asset.get_editor_property('loadouts')}
        if got_load != expected_load:
            errors.append('loadouts mismatch: %s vs %s' % (got_load, expected_load))

    report = {
        'mode': 'verify',
        'success': len(errors) == 0,
        'revision': revision,
        'errors': errors,
    }
    VERIFY_REPORT.write_text(json.dumps(report, indent=2), encoding='utf-8')
    unreal.log_warning('[ProfessionAssets] verify success=%s errors=%d' % (report['success'], len(errors)))
    return report


def main():
    report = generate() if MODE == 'generate' else verify()
    if not report['success']:
        raise RuntimeError('profession assets %s failed: %s' % (MODE, report.get('errors')))


main()
