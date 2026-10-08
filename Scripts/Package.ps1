param(
    # M5-018B: optional explicit package root (the timestamped segment
    # package). Empty keeps the legacy default directory so the older
    # Package/TestPackage calls stay byte-compatible.
    [string]$PackageRoot = ''
)
. "$PSScriptRoot\Common.ps1"
$ArchiveDirectory = if ([string]::IsNullOrWhiteSpace($PackageRoot)) { "$ArtifactRoot\Package" } else { $PackageRoot }
$Arguments = @('BuildCookRun', "-project=$ProjectFile", '-noP4', '-platform=Win64', '-clientconfig=Development', '-build', '-cook', '-stage', '-pak', '-iostore', '-archive', "-archivedirectory=$ArchiveDirectory", '-unattended', '-utf8output')
# M1-039: attack montages are resolved at runtime by naming convention
# (/Game/UEMMO/Animation/Montages/MNT_<AttackId>) and nothing references them
# statically, so the unreferenced montage assets must be cooked explicitly.
# -cookall cooks every project content asset without touching
# ProjectPackagingSettings; the M0 cook result stays a subset of this output.
$Arguments += '-cookall'
if($CompilerVersion) { $Arguments += "-UbtArgs=-CompilerVersion=$CompilerVersion" }
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat') @Arguments
Assert-NativeSuccess 'Packaging'
# M5-018C: the system test room is config-driven at runtime (ACombatTestRoomDriver
# reads Data/test_room.json under the packaged project dir), so the real config
# ships inside the package - the M5-H01 trial package stays self-contained for
# play while the M5-018B reward-fallback proof (which targets drops.json
# specifically) keeps its meaning.
$StagedRoomConfig = Join-Path $ArchiveDirectory 'Windows\UEMMO\Data\test_room.json'
$RepoRoomConfig = Join-Path $ProjectRoot 'Data\test_room.json'
if (Test-Path -LiteralPath $RepoRoomConfig) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $StagedRoomConfig) -Force | Out-Null
    Copy-Item -LiteralPath $RepoRoomConfig -Destination $StagedRoomConfig -Force
}
else {
    Write-Warning 'Data/test_room.json not found; the packaged system test room will have no configured targets.'
}
