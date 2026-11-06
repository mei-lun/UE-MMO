param(
    [string]$TaskId = 'M8-012'
)
. "$PSScriptRoot\Common.ps1"
# M8-012: rendered in-game acceptance of the bamboo P02 segment (walk band,
# walls, jump, occlusion) on L_DNFM_Bamboo01 with the DNFM 2D visual enabled.
if ($TaskId -notmatch '^[A-Za-z0-9_-]+$') { throw 'TaskId must be a plain identifier.' }
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
$Evidence = Join-Path $ArtifactRoot "Tasks\$TaskId\$Stamp-bamboo-segment"
if (Test-Path -LiteralPath $Evidence) { throw 'Evidence directory already exists.' }
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Filter = 'UEMMO.Acceptance.M8_012.BambooSegment'
$Arguments = @(
    $ProjectFile,
    '/Game/UEMMO/DNFM/Bamboo/Maps/L_DNFM_Bamboo01',
    '-game', '-unattended', '-nosplash', '-nosound', '-RenderOffscreen',
    '-ResX=1280', '-ResY=720',
    "-ExecCmds=Automation RunTests $Filter",
    '-TestExit=Automation Test Queue Empty',
    "-ReportExportPath=$Evidence",
    "-DNFMBambooOutput=$Evidence",
    '-DPCVars=UEMMO.PlayerDNFMVisual=1',
    "-UserDir=$Evidence\User",
    "-abslog=$Evidence\ue.log"
)
$Executable = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Summary = [ordered]@{success=$false;room='L_DNFM_Bamboo01';matched=0;passed=0;failed=0;error='';evidence=$Evidence}
try {
    Invoke-UEProcess $Executable $Arguments "dnfm-bamboo-segment" 300
    $Index = Join-Path $Evidence 'index.json'
    if (-not (Test-Path -LiteralPath $Index)) { throw 'Bamboo segment acceptance did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $Index -Raw) -Filter $Filter
    $Summary.matched = $Parsed.Matched
    $Summary.passed = $Parsed.Passed
    $Summary.failed = $Parsed.Failed
    if (-not $Parsed.Ok -or $Parsed.Matched -ne 1) { throw "Bamboo segment acceptance failed: $($Parsed.Error)" }
    foreach ($Name in @('feet-ground.png','band-near-block.png','band-far-block.png','jump-air.png','occlusion.png')) {
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
