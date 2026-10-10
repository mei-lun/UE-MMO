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
# M5-036: the weapon trial package ships the Data/CombatSystem runtime
# config (the eight source tables the production mount reads per run from
# FPaths::ProjectDir()/Data/CombatSystem, M5-020). Without this carry the
# packaged mount refuses and weapon firing stays disabled; the cook itself
# never reads these files (the isolation check proves it), so the tables ride
# as runtime config exactly like Data/test_room.json above.
$RepoCombatDataDir = Join-Path $ProjectRoot 'Data\CombatSystem'
if (Test-Path -LiteralPath $RepoCombatDataDir) {
    $StagedCombatDataDir = Join-Path $ArchiveDirectory 'Windows\UEMMO\Data\CombatSystem'
    New-Item -ItemType Directory -Path $StagedCombatDataDir -Force | Out-Null
    Copy-Item -Path (Join-Path $RepoCombatDataDir '*.json') -Destination $StagedCombatDataDir -Force
}
else {
    Write-Warning 'Data/CombatSystem not found; the packaged weapon trial will have no combat config and the weapon mount will refuse.'
}
# M5-056: the items source ships as runtime config too (LoadProductionItems
# reads Data/items.json per run; the staging grant and the weapon mappings
# come from it). Without this carry the package stages nothing and every
# weapon bind refuses - the M5-H02 "Q does nothing" root cause.
$RepoItemsSource = Join-Path $ProjectRoot 'Data\items.json'
if (Test-Path -LiteralPath $RepoItemsSource) {
    $StagedItemsSource = Join-Path $ArchiveDirectory 'Windows\UEMMO\Data\items.json'
    New-Item -ItemType Directory -Path (Split-Path -Parent $StagedItemsSource) -Force | Out-Null
    Copy-Item -LiteralPath $RepoItemsSource -Destination $StagedItemsSource -Force
}
else {
    Write-Warning 'Data/items.json not found; the packaged inventory will stage no items and weapon binds will refuse.'
}
