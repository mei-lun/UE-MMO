param(
    [string]$RoomId = 'room_training_01',
    [string]$TaskId = 'M6-022'
)
. "$PSScriptRoot\Common.ps1"
if ($TaskId -notmatch '^[A-Za-z0-9_-]+$' -or $RoomId -notmatch '^[a-z0-9_]+$') {
    throw 'TaskId and RoomId must be plain identifiers.'
}
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$Evidence = Join-Path $ArtifactRoot "Tasks\$TaskId\$Stamp-dnfm-visual"
if (Test-Path -LiteralPath $Evidence) { throw 'Evidence directory already exists.' }
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Filter = 'UEMMO.Acceptance.M6_022.DNFMVisual'
$Arguments = @('-game', '-unattended', '-nosplash', '-nosound', '-RenderOffscreen',
    '-ResX=1280', '-ResY=720', "-ExecCmds=Automation RunTests $Filter",
    '-TestExit=Automation Test Queue Empty', "-ReportExportPath=$Evidence",
    "-DNFMVisualRoom=$RoomId", "-DNFMVisualOutput=$Evidence",
    '-DPCVars=UEMMO.PlayerDNFMVisual=1',
    "-UserDir=$Evidence\User", "-abslog=$Evidence\ue.log")
$Executable = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Arguments = @($ProjectFile) + $Arguments
$Summary = [ordered]@{success=$false;room_id=$RoomId;matched=0;passed=0;failed=0;error='';evidence=$Evidence}
try {
    Invoke-UEProcess $Executable $Arguments "dnfm-import-visual" 300
    $Index = Join-Path $Evidence 'index.json'
    if (-not (Test-Path -LiteralPath $Index)) { throw 'Visual acceptance did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $Index -Raw) -Filter $Filter
    $Summary.matched = $Parsed.Matched
    $Summary.passed = $Parsed.Passed
    $Summary.failed = $Parsed.Failed
    if (-not $Parsed.Ok -or $Parsed.Matched -ne 1) { throw "Visual acceptance failed: $($Parsed.Error)" }
    foreach ($Name in @('menu.png','dnfm-idle.png','dnfm-right.png','dnfm-attack.png')) {
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
$Summary | ConvertTo-Json | Set-Content (Join-Path $Evidence 'summary.json') -Encoding UTF8
$Summary | ConvertTo-Json
if (-not $Summary.success) { exit 1 }
