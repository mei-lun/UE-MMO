param(
    [string]$TaskId = 'M8-034'
)
. "$PSScriptRoot\Common.ps1"
# M8-034: the attack-chain regression acceptance (the user's "attacks
# completely failed" package report). Two guards:
# - TeardownNeverLocksThePhaseMachine: a mid-Active teardown (jump cancel /
#   hit interrupt / ResetCombat) must never leave the phase machine in a
#   state that refuses a later attack start.
# - ChainWindowStartAcceptsTheFollowUp: the buffered follow-up press must be
#   consumed AND started on the first cancel-window frame (the eaten-press
#   regression from the user's package log).
if ($TaskId -notmatch '^[A-Za-z0-9_-]+$') { throw 'TaskId must be a plain identifier.' }
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$Evidence = Join-Path $ArtifactRoot "Tasks\$TaskId\$Stamp-attack-lifecycle"
if (Test-Path -LiteralPath $Evidence) { throw 'Evidence directory already exists.' }
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Filter = 'UEMMO.Tasks.M8_031'
$Arguments = @(
    $ProjectFile,
    '/Game/UEMMO/Maps/L_TrainingArena',
    '-game', '-unattended', '-nosplash', '-nosound', '-RenderOffscreen',
    '-ResX=1280', '-ResY=720',
    "-ExecCmds=Automation RunTests $Filter",
    '-TestExit=Automation Test Queue Empty',
    "-ReportExportPath=$Evidence",
    "-UserDir=$Evidence\User",
    "-abslog=$Evidence\ue.log"
)
$Executable = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Summary = [ordered]@{success=$false;map='L_TrainingArena';matched=0;passed=0;failed=0;error='';evidence=$Evidence}
try {
    Invoke-UEProcess $Executable $Arguments "attack-lifecycle" 300
    $Index = Join-Path $Evidence 'index.json'
    if (-not (Test-Path -LiteralPath $Index)) { throw 'Attack lifecycle acceptance did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $Index -Raw) -Filter $Filter
    $Summary.matched = $Parsed.Matched
    $Summary.passed = $Parsed.Passed
    $Summary.failed = $Parsed.Failed
    if (-not $Parsed.Ok -or $Parsed.Matched -ne 2) { throw "Attack lifecycle acceptance failed: $($Parsed.Error)" }
    $Summary.success = $true
}
catch { $Summary.error = $_.Exception.Message }
$Summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'summary.json') -Encoding UTF8
$Summary | ConvertTo-Json
if (-not $Summary.success) { exit 1 }
