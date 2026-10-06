"""Create /Game/UEMMO/Combat/Data combat data assets from Data/CombatSystem.

Run inside the UE editor only (UnrealEditor-Cmd -run=pythonscript). This is the
M5-008 editor entry: it calls the C++ builder
UCombatDataAssetBuilderLibrary.build_from_source_directory, which parses every
required table of the source directory, validates it with the full cross-table
reference gate (including the presentations table) and then builds one data
asset per row of the six tables under /Game/UEMMO/Combat/Data
(DA_Damage_*, DA_Reaction_*, DA_Target_*, DA_Weapon_*, DA_Ammo_*,
DA_Projectile_*), created or updated in place, never duplicated.

The heavy lifting (strict table schema, local row validators, reference gate,
idempotent asset build, package save) lives in
Source/UEMMO/Combat/Data/CombatDataAssetBuilder.h/.cpp; this script is the
editor wrapper that reports the outcome to
Artifacts/Logs/combat-system-assets-report.json:
  {mode, source_dir, success, created, updated, config_revision, errors}

Optional environment overrides:
  UEMMOCombatDataSourceDir - source directory. Default Data/CombatSystem
                             (project-relative paths resolve against the
                             project directory).
  UEMMOCombatReportDir     - extra directory that receives a report copy.

Failure contract: the builder validates everything before touching anything,
so any parse/validation/save failure leaves the existing assets untouched and
reports success=false with the builder's error text naming the offending
table, row and field. The script raises after writing the report so the UE
process exits nonzero.

ASCII only: this file must not contain non-ASCII characters.
"""
import json
import os
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir())
DEFAULT_SOURCE_DIR = 'Data/CombatSystem'
GENERATE_REPORT = ROOT / 'Artifacts' / 'Logs' / 'combat-system-assets-report.json'


def source_dir():
    override = os.environ.get('UEMMOCombatDataSourceDir')
    if override:
        return override
    return DEFAULT_SOURCE_DIR


def write_report(report):
    GENERATE_REPORT.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(report, indent=2)
    GENERATE_REPORT.write_text(payload, encoding='utf-8')
    extra = os.environ.get('UEMMOCombatReportDir')
    if extra:
        extra_dir = Path(extra)
        extra_dir.mkdir(parents=True, exist_ok=True)
        (extra_dir / GENERATE_REPORT.name).write_text(payload, encoding='utf-8')


def main():
    # The UFUNCTION returns the error text (empty on success); the out
    # parameters come back with it as one tuple:
    # (errors, created, updated, config_revision).
    errors, created, updated, revision = (
        unreal.CombatDataAssetBuilderLibrary.build_from_source_directory(source_dir()))
    success = not (errors or '').strip()
    report = {
        'mode': 'generate',
        'source_dir': source_dir(),
        'success': bool(success),
        'created': int(created),
        'updated': int(updated),
        'config_revision': revision or '',
        'errors': errors or '',
    }
    write_report(report)
    if not success:
        unreal.log_error(
            'combat system asset generation failed; no asset was touched: ' + (errors or 'unknown error'))
        raise RuntimeError('combat system asset generation failed')
    unreal.log('combat system asset generation succeeded: created=%d updated=%d revision=%s'
               % (int(created), int(updated), revision or ''))


main()
