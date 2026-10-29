# M6-004: clone one combat-room umap (the new-level production step; the DA
# generation and the trigger rewiring stay with PrepareRooms.ps1). Two editor
# passes: clone, then a fresh-process reload verification.
# ASCII only: PowerShell 5.1 parses BOM-less UTF-8 as GBK.
param(
    # Evidence task directory under Artifacts/Tasks.
    [string]$TaskId = 'M6-004',
    # Source map package (default: the shipped wave-combat room).
    [string]$SourceMap = '/Game/UEMMO/Maps/L_CombatRoom01',
    # Target map package (default: the M6-004 second combat room).
    [string]$TargetMap = '/Game/UEMMO/Maps/L_CombatRoom02',
    # M6-005: strip the RoomTrigger/RoomExit actors (the pure menu-surface
    # clone). The parameters are the single source of configuration - the
    # script always overwrites the python env with them.
    [switch]$StripRoomActors
)
. "$PSScriptRoot\Common.ps1"

$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\clone_room_map.py'
$GenerateReport = Join-Path $ArtifactRoot 'Logs\room-map-clone-report.json'
$VerifyReport = Join-Path $ArtifactRoot 'Logs\room-map-clone-verify.json'

$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$EvidenceDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' (Join-Path $TaskId $Stamp))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null

foreach ($Report in @($GenerateReport, $VerifyReport)) {
    if (Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }
}

# Pass 1: the clone (idempotent; an existing target re-strips/re-saves
# untouched). The params overwrite the python env unconditionally - the
# caller-facing configuration is THESE parameters, never stale env state.
$env:UEMMOCloneSourceMap = $SourceMap
$env:UEMMOCloneTargetMap = $TargetMap
$env:UEMMOCloneStripRoomActors = if ($StripRoomActors) { '1' } else { '' }
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\clone-room-map-generate.log"
) 'clone-room-map-generate' 900
if (-not (Test-Path -LiteralPath $GenerateReport)) { throw 'UE did not write room-map-clone-report.json.' }
$Result = Get-Content -LiteralPath $GenerateReport -Raw | ConvertFrom-Json
if (-not $Result.success) { throw 'Room map clone failed; inspect room-map-clone-report.json and Artifacts/Logs/clone-room-map-generate.log.' }
Copy-Item -LiteralPath $GenerateReport -Destination (Join-Path $EvidenceDir 'room-map-clone-report.json')

# Pass 2: a fresh editor process reloads the clone from disk and re-checks
# the orchestration (the reload proof - the M5-070 two-pass shape).
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    '-UEMMORoomMapCloneVerify',
    "-abslog=$ArtifactRoot\Logs\clone-room-map-verify.log"
) 'clone-room-map-verify' 900
if (-not (Test-Path -LiteralPath $VerifyReport)) { throw 'UE did not write room-map-clone-verify.json.' }
$VerifyResult = Get-Content -LiteralPath $VerifyReport -Raw | ConvertFrom-Json
if (-not $VerifyResult.success) { throw 'Room map clone verification failed; inspect room-map-clone-verify.json.' }
Copy-Item -LiteralPath $VerifyReport -Destination (Join-Path $EvidenceDir 'room-map-clone-verify.json')

$Checks = ($VerifyResult.checks | ForEach-Object { "$($_.check)=$($_.passed)" }) -join ', '
Write-Output "Room map clone ready: target=$TargetMap created=$($Result.created)"
Write-Output "Fresh-process verification checks: $Checks"
Write-Output "Evidence: $EvidenceDir"
