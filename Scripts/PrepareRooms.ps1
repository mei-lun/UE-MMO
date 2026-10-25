# M5-070: generate /Game/UEMMO/Rooms/DA_room_combat_01 from Data/rooms.json via
# the UE editor Python script, wire the L_CombatRoom01 ARoomTrigger to it, then
# verify the saved asset and the map wiring in a second fresh UE process.
# ASCII only: PowerShell 5.1 parses BOM-less UTF-8 as GBK.
param(
    # Optional path override for the source JSON (red-line failure-path test).
    [string]$SampleJson = ''
)
. "$PSScriptRoot\Common.ps1"

$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\create_room_definition_assets.py'
$GenerateReport = Join-Path $ArtifactRoot 'Logs\room-definition-assets-report.json'
$VerifyReport = Join-Path $ArtifactRoot 'Logs\room-definition-assets-verify.json'
$SourceJson = Join-Path $ProjectRoot 'Data\rooms.json'
if ($SampleJson -ne '') {
    if (-not [IO.Path]::IsPathRooted($SampleJson)) { $SampleJson = Join-Path $ProjectRoot $SampleJson }
    $ResolvedSample = [IO.Path]::GetFullPath($SampleJson)
    if (-not (Test-Path -LiteralPath $ResolvedSample)) { throw "Sample JSON not found: $ResolvedSample" }
    $SourceJson = $ResolvedSample
}

$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$EvidenceDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' (Join-Path 'M5-070' $Stamp))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null

foreach ($Report in @($GenerateReport, $VerifyReport)) {
    if (Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }
}

# Pass 1: validate-first generation + trigger wiring. Any structural error
# aborts before the first write; the report then says success=false.
$env:UEMMORoomDataJson = $SourceJson
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\prepare-rooms-generate.log"
) 'prepare-rooms-generate' 900
if (-not (Test-Path -LiteralPath $GenerateReport)) { throw 'UE did not write room-definition-assets-report.json.' }
$Result = Get-Content -LiteralPath $GenerateReport -Raw | ConvertFrom-Json
if (-not $Result.success) { throw 'Room definition generation failed; inspect room-definition-assets-report.json and Artifacts/Logs/prepare-rooms-generate.log.' }
Copy-Item -LiteralPath $GenerateReport -Destination (Join-Path $EvidenceDir 'room-definition-assets-report.json')

# Pass 2: a fresh editor process reloads the saved asset and the maps from
# disk and re-checks every field plus the production wiring (reload proof).
Remove-Item env:UEMMORoomDataJson -ErrorAction SilentlyContinue
$env:UEMMORoomDataJson = $SourceJson
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-UEMMORoomAssetVerify',
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\prepare-rooms-verify.log"
) 'prepare-rooms-verify' 900
if (-not (Test-Path -LiteralPath $VerifyReport)) { throw 'UE did not write room-definition-assets-verify.json.' }
$Verification = Get-Content -LiteralPath $VerifyReport -Raw | ConvertFrom-Json
if (-not $Verification.success) { throw 'Room definition verification failed; inspect room-definition-assets-verify.json and Artifacts/Logs/prepare-rooms-verify.log.' }
Copy-Item -LiteralPath $VerifyReport -Destination (Join-Path $EvidenceDir 'room-definition-assets-verify.json')

Write-Output ("Room definition asset ready: " + $Result.asset + " created=" + $Result.created + " wave_counts=" + ($Result.wave_counts -join ','))
Write-Output ("Fresh-process verification checks: " + (($Verification.checks | ForEach-Object { [string]$_.check + '=' + [string]$_.passed }) -join ', '))
Write-Output ("Evidence: " + $EvidenceDir)
exit 0
