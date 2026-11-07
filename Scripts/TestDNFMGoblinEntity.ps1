param(
    [string]$TaskId = 'M8-019A'
)
. "$PSScriptRoot\Common.ps1"
# M8-019A: rendered acceptance of the goblin entity (display states + death).
if ($TaskId -notmatch '^[A-Za-z0-9_-]+$') { throw 'TaskId must be a plain identifier.' }
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$Evidence = Join-Path $ArtifactRoot "Tasks\$TaskId\$Stamp-goblin-entity"
if (Test-Path -LiteralPath $Evidence) { throw 'Evidence directory already exists.' }
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Filter = 'UEMMO.Acceptance.M8_019A.GoblinEntity'
$Arguments = @(
    $ProjectFile,
    '/Game/UEMMO/Maps/L_TrainingArena',
    '-game', '-unattended', '-nosplash', '-nosound', '-RenderOffscreen',
    '-ResX=1280', '-ResY=720',
    "-ExecCmds=Automation RunTests $Filter",
    '-TestExit=Automation Test Queue Empty',
    "-ReportExportPath=$Evidence",
    "-DNFMGoblinOutput=$Evidence",
    '-DPCVars=UEMMO.PlayerDNFMVisual=1',
    "-UserDir=$Evidence\User",
    "-abslog=$Evidence\ue.log"
)
$Executable = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Summary = [ordered]@{success=$false;map='L_TrainingArena';matched=0;passed=0;failed=0;error='';evidence=$Evidence}
try {
    Invoke-UEProcess $Executable $Arguments "dnfm-goblin-entity" 300
    $Index = Join-Path $Evidence 'index.json'
    if (-not (Test-Path -LiteralPath $Index)) { throw 'Goblin entity acceptance did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $Index -Raw) -Filter $Filter
    $Summary.matched = $Parsed.Matched
    $Summary.passed = $Parsed.Passed
    $Summary.failed = $Parsed.Failed
    if (-not $Parsed.Ok -or $Parsed.Matched -ne 1) { throw "Goblin entity acceptance failed: $($Parsed.Error)" }
    foreach ($Name in @('goblin-alive.png','goblin-dead.png')) {
        $Shot = Join-Path $Evidence $Name
        if (-not (Test-Path -LiteralPath $Shot)) { throw "Missing screenshot: $Name" }
        $Bytes = [IO.File]::ReadAllBytes($Shot)
        if ($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') {
            throw "Invalid screenshot: $Name"
        }
    }
    $Summary.success = $true
}
catch { $Summary.error = $_.Exception.Message }
$Summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Evidence 'summary.json') -Encoding UTF8
$Summary | ConvertTo-Json
if (-not $Summary.success) { exit 1 }
