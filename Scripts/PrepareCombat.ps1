# M1-009: generate the four /Game/UEMMO/Combat/Definitions attack data assets
# from Data/combat-attacks.json via the UE editor Python script, then verify
# the saved assets in a second fresh UE process against the same JSON.
# ASCII only: PowerShell 5.1 parses BOM-less UTF-8 as GBK.
param(
    # Optional path to a sample JSON for the red-line failure-path test.
    # The sample is only read; the official Data/combat-attacks.json is never
    # touched. Empty means the official source data.
    [string]$SampleJson = ''
)
. "$PSScriptRoot\Common.ps1"

$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\create_combat_assets.py'
$GenerateReport = Join-Path $ArtifactRoot 'Logs\combat-assets-report.json'
$VerifyReport = Join-Path $ArtifactRoot 'Logs\combat-assets-verify.json'
$SourceJson = Join-Path $ProjectRoot 'Data\combat-attacks.json'
if ($SampleJson -ne '') {
    if (-not [IO.Path]::IsPathRooted($SampleJson)) { $SampleJson = Join-Path $ProjectRoot $SampleJson }
    $ResolvedSample = [IO.Path]::GetFullPath($SampleJson)
    if (-not (Test-Path -LiteralPath $ResolvedSample)) { throw "Sample JSON not found: $ResolvedSample" }
    $SourceJson = $ResolvedSample
}

$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$EvidenceDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' (Join-Path 'M1-009' $Stamp))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null

foreach ($Report in @($GenerateReport, $VerifyReport)) {
    if (Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }
}

# Pass 1: validate-first generation. Any structural error or animation load
# failure aborts before the first asset is created; the report then says
# success=false and no asset is written.
$env:UEMMOCombatDataJson = $SourceJson
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\prepare-combat-generate.log"
) 'prepare-combat-generate' 900
if (-not (Test-Path -LiteralPath $GenerateReport)) { throw 'UE did not write combat-assets-report.json.' }
$Result = Get-Content -LiteralPath $GenerateReport -Raw | ConvertFrom-Json
if (-not $Result.success) { throw 'Combat asset generation failed; inspect combat-assets-report.json and Artifacts/Logs/prepare-combat-generate.log.' }
Copy-Item -LiteralPath $GenerateReport -Destination (Join-Path $EvidenceDir 'combat-assets-report.json')

# Pass 2: a fresh editor process reloads the saved assets from disk and
# compares every field against the same JSON (reload + idempotency proof).
$env:UEMMOCombatVerifyDir = $EvidenceDir
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-UEMMOCombatAssetsVerify',
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\prepare-combat-verify.log"
) 'prepare-combat-verify' 900
if (-not (Test-Path -LiteralPath $VerifyReport)) { throw 'UE did not write combat-assets-verify.json.' }
$Verification = Get-Content -LiteralPath $VerifyReport -Raw | ConvertFrom-Json
if (-not $Verification.success) { throw 'Combat asset verification failed; inspect combat-assets-verify.json and Artifacts/Logs/prepare-combat-verify.log.' }
Copy-Item -LiteralPath $VerifyReport -Destination (Join-Path $EvidenceDir 'json-vs-assets.json')

$CreatedCount = @($Result.created).Count
$UpdatedCount = @($Result.updated).Count
Write-Output ("Combat definitions ready: created=" + $CreatedCount + " updated=" + $UpdatedCount + " assets=" + $Verification.asset_count)
Write-Output ("Directory listing: " + (($Verification.directory_listing | ForEach-Object { [string]$_ }) -join ', '))
Write-Output ("JSON vs assets comparison report: " + (Join-Path $EvidenceDir 'json-vs-assets.json'))
Write-Output 'Combat data assets verified.'
exit 0
