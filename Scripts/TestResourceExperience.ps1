param([switch]$Package, [string]$TaskId = 'M6-026')
. "$PSScriptRoot\Common.ps1"
if ($TaskId -notmatch '^[A-Za-z0-9_-]+$') { throw 'TaskId must be a plain identifier.' }
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':','')
$Kind = if ($Package) { 'package' } else { 'editor' }
$Evidence = Join-Path $ArtifactRoot "Tasks\$TaskId\$Stamp-$Kind-experience"
New-Item -ItemType Directory -Path $Evidence | Out-Null
$Filter = 'UEMMO.Acceptance.M6_026.ResourceExperience'
$Summary = [ordered]@{success=$false;kind=$Kind;evidence=$Evidence;passes=@();error=''}
try {
    foreach ($Pass in @('first','restart')) {
        $PassDir = Join-Path $Evidence $Pass
        New-Item -ItemType Directory -Path $PassDir | Out-Null
        # Intentionally no -unattended, visual override, map override, experience
        # enable flag or skill-demo command: exercise the normal public boot.
        $Arguments = @('-game','-nosplash','-nosound','-RenderOffscreen','-ResX=1280','-ResY=720',
            "-ExecCmds=Automation RunTests $Filter", '-TestExit=Automation Test Queue Empty',
            "-ReportExportPath=$PassDir", "-ExperienceOutput=$PassDir",
            "-UserDir=$Evidence\User", "-abslog=$PassDir\ue.log")
        if ($Pass -eq 'restart') { $Arguments += '-ExperienceExpectRestore' }
        $Executable = $DevelopmentPackageExecutable
        if (-not $Package) {
            $Executable = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
            $Arguments = @($ProjectFile) + $Arguments
        }
        Invoke-UEProcess $Executable $Arguments "resource-experience-$Kind-$Pass" 180
        $Index = Join-Path $PassDir 'index.json'
        if (-not (Test-Path -LiteralPath $Index)) { throw "Missing automation index: $Pass" }
        $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -Raw -LiteralPath $Index) -Filter $Filter
        $Summary.passes += @{name=$Pass;matched=$Parsed.Matched;passed=$Parsed.Passed;failed=$Parsed.Failed}
        if (-not $Parsed.Ok -or $Parsed.Matched -ne 1) { throw "Experience acceptance failed ($Pass): $($Parsed.Error)" }
        foreach ($Name in @('startup','right','depth-front','depth-back','jump','launcher','singlekick','damagelowkick','throwsand','earthbreak','stomp','menu')) {
            $Path = Join-Path $PassDir "$Name.png"
            if (-not (Test-Path -LiteralPath $Path)) { throw "Missing screenshot: $Pass/$Name" }
            $Bytes = [IO.File]::ReadAllBytes($Path)
            if ($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid screenshot: $Path" }
        }
    }
    $FirstId = Get-Content -Raw -LiteralPath (Join-Path $Evidence 'first/character-id.txt')
    $SecondId = Get-Content -Raw -LiteralPath (Join-Path $Evidence 'restart/character-id.txt')
    if ($FirstId -ne $SecondId) { throw 'Restart created a new identity instead of restoring the experience save.' }
    $Summary.success = $true
}
catch { $Summary.error = $_.Exception.Message }
$Summary | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Evidence 'summary.json') -Encoding UTF8
$Summary | ConvertTo-Json -Depth 5
if (-not $Summary.success) { exit 1 }
