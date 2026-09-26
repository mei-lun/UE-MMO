# M1-039: without -IncludeCombat this script is the unchanged M0 package
# smoke. With -IncludeCombat the SAME packaged executable gets two extra
# passes after the M0 smoke (Development builds compile WITH_DEV_AUTOMATION_TESTS
# and the engine's Launch loop loads the AutomationController module in every
# non-shipping game, so the packed exe can run UEMMO.Tasks automation itself):
#   1. in-package automation: -ExecCmds="Automation RunTests <CombatFilter>"
#      with -TestExit and -ReportExportPath; index.json is parsed with the
#      shared report parser and gated: matched > 0, incomplete = 0, and every
#      failed test must belong to DevOnlySourceJsonFilter. The default filter
#      is the full UEMMO.Tasks suite. The only tolerated failures are the
#      seven UEMMO.Tasks.M1_008 tests: they validate the development source
#      Data/combat-attacks.json by reading it from FPaths::ProjectDir()/Data,
#      a directory the package does not ship (M1-010: the runtime catalog
#      loads cooked assets by config path and never reads that JSON; the same
#      contract values are re-validated against the cooked assets by
#      UEMMO.Tasks.M1_009, which passes in-package). The tolerated failure
#      names are reported in the summary, never silently dropped.
#   2. rendered staged combat: the M0 smoke entry (-UEMMOSmoke) launched on
#      CombatMap (default /Game/UEMMO/Maps/L_TrainingArena - the only map
#      with training enemies) plus the existing M1-035 HUD exec
#      "UEMMODebugCombatOverlay 3", so the captured screenshot shows real
#      in-package hits (enemy HP bar drop, damage number, combo counter).
#      Staged evidence choreography, never gameplay.
param(
    [switch]$IncludeCombat,
    [string]$CombatFilter = 'UEMMO.Tasks',
    [string]$DevOnlySourceJsonFilter = 'UEMMO.Tasks.M1_008.',
    [string]$CombatMap = '/Game/UEMMO/Maps/L_TrainingArena'
)
. "$PSScriptRoot\Common.ps1"
$PackageProject = Join-Path $ArtifactRoot 'Package\Windows\UEMMO'
$Executable = Join-Path $PackageProject 'Binaries\Win64\UEMMO.exe'
if(-not (Test-Path -LiteralPath $Executable)) { throw 'Package not found. Run Package.ps1 first.' }
$PackageArtifacts = Join-Path $PackageProject 'Artifacts'
foreach($Name in @('runtime-smoke.json','prototype-smoke.png')) {
    $File = Join-Path $PackageArtifacts $Name
    if(Test-Path -LiteralPath $File) { Remove-Item -LiteralPath $File }
}
Invoke-UEProcess $Executable @('-UEMMOSmoke','-unattended','-RenderOffscreen','-nosound','-ResX=1280','-ResY=720',"-abslog=$ArtifactRoot\Logs\package-smoke.log") 'package-smoke' 600
$Report = Join-Path $PackageArtifacts 'runtime-smoke.json'
$Screenshot = Join-Path $PackageArtifacts 'prototype-smoke.png'
if(-not (Test-Path -LiteralPath $Report)) { throw 'Packaged executable did not produce a runtime report.' }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if(-not $Result.success -or -not $Result.rendering) { throw 'Packaged runtime / rendering test failed.' }
if(-not (Test-Path -LiteralPath $Screenshot)) { throw 'Packaged test screenshot missing.' }
$Bytes = [IO.File]::ReadAllBytes($Screenshot)
if($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Packaged screenshot is not a valid PNG header.' }
Copy-Item -LiteralPath $Report -Destination (Join-Path $ArtifactRoot 'package-smoke.json') -Force
Copy-Item -LiteralPath $Screenshot -Destination (Join-Path $ArtifactRoot 'package-smoke.png') -Force
$Result | ConvertTo-Json

if($IncludeCombat) {
    # Pass 1: in-package automation over CombatFilter (default: full suite).
    $Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
    $ReportDir = Join-Path $ArtifactRoot (Join-Path 'Tasks\M1-039-Package' $Stamp)
    New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
    Invoke-UEProcess $Executable @('-unattended', '-nosplash', '-nosound', '-nullrhi',
        "-ExecCmds=Automation RunTests $CombatFilter",
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$ReportDir",
        "-abslog=$ArtifactRoot\Logs\package-automation.log") 'package-automation' 900
    $IndexJson = Join-Path $ReportDir 'index.json'
    if(-not (Test-Path -LiteralPath $IndexJson)) { throw 'Packaged automation did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $IndexJson -Raw) -Filter $CombatFilter
    if(-not $Parsed.Ok -and $Parsed.ErrorKind -ne 'failures') { throw 'Packaged automation gate failed: ' + $Parsed.Error }
    if($Parsed.Passed -lt 1) { throw 'Packaged automation gate failed: zero tests passed.' }
    if($Parsed.Incomplete -ne 0) { throw 'Packaged automation gate failed: incomplete tests: ' + $Parsed.Error }
    # Name-level check of the only tolerated failure family: every failed test
    # must be a UEMMO.Tasks.M1_008 dev-source-JSON validator (see header).
    $RawReport = Get-Content -LiteralPath $IndexJson -Raw | ConvertFrom-Json
    $FailedNames = @()
    foreach($Test in @($RawReport.tests)) {
        $Path = ''
        if($Test.PSObject.Properties['fullTestPath'] -and $Test.fullTestPath) { $Path = [string]$Test.fullTestPath }
        elseif($Test.PSObject.Properties['testDisplayName'] -and $Test.testDisplayName) { $Path = [string]$Test.testDisplayName }
        if($Path -eq '' -or ($Path -ne $CombatFilter -and -not $Path.StartsWith($CombatFilter + '.'))) { continue }
        $State = $Test.state
        if($State -isnot [string]) { $State = switch ("" + $State) { '2' { 'Fail' } default { '' } } }
        if($State -eq 'Fail') { $FailedNames += $Path }
    }
    if($FailedNames.Count -ne $Parsed.Failed) { throw 'Packaged automation gate failed: failed-test count mismatch between parser and report.' }
    $UnexpectedFailed = @($FailedNames | Where-Object { -not $_.StartsWith($DevOnlySourceJsonFilter) })
    if($UnexpectedFailed.Count -gt 0) { throw 'Packaged automation gate failed: unexpected failures: ' + ($UnexpectedFailed -join ', ') }
    Copy-Item -LiteralPath $IndexJson -Destination (Join-Path $ArtifactRoot 'package-automation-index.json') -Force
    $AutomationSummary = [ordered]@{
        success = $true
        filter = $CombatFilter
        report_path = $IndexJson
        matched = $Parsed.Matched
        passed = $Parsed.Passed
        failed = $Parsed.Failed
        incomplete = $Parsed.Incomplete
        allowed_dev_only_failures = $FailedNames
        allowed_dev_only_filter = $DevOnlySourceJsonFilter
    }
    $AutomationSummary | ConvertTo-Json

    # Pass 2: rendered staged combat - real strikes through the real combat
    # component while the M0 smoke runner drives movement and captures the
    # screenshot at ~8 s (strikes start at 6.6 s, so at least one hit with its
    # damage number, HP bar drop and combo counter is on screen).
    foreach($Name in @('runtime-smoke.json','prototype-smoke.png')) {
        $File = Join-Path $PackageArtifacts $Name
        if(Test-Path -LiteralPath $File) { Remove-Item -LiteralPath $File }
    }
    Invoke-UEProcess $Executable @($CombatMap, '-UEMMOSmoke', '-unattended', '-RenderOffscreen', '-nosound',
        '-ResX=1280', '-ResY=720',
        "-abslog=$ArtifactRoot\Logs\package-combat.log",
        '-ExecCmds=UEMMODebugCombatOverlay 3') 'package-combat' 600
    $CombatReport = Join-Path $PackageArtifacts 'runtime-smoke.json'
    $CombatScreenshot = Join-Path $PackageArtifacts 'prototype-smoke.png'
    if(-not (Test-Path -LiteralPath $CombatReport)) { throw 'Packaged combat pass did not produce a runtime report.' }
    $CombatResult = Get-Content -LiteralPath $CombatReport -Raw | ConvertFrom-Json
    if(-not $CombatResult.success -or -not $CombatResult.rendering) { throw 'Packaged combat pass failed the movement/render smoke.' }
    if(-not (Test-Path -LiteralPath $CombatScreenshot)) { throw 'Packaged combat screenshot missing.' }
    $CombatBytes = [IO.File]::ReadAllBytes($CombatScreenshot)
    if($CombatBytes.Length -lt 1024 -or [BitConverter]::ToString($CombatBytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Packaged combat screenshot is not a valid PNG header.' }
    $CombatLog = Join-Path $ArtifactRoot 'Logs\package-combat.log'
    if(-not (Test-Path -LiteralPath $CombatLog)) { throw 'Packaged combat pass log missing.' }
    $StrikeCount = ([regex]::Matches((Get-Content -LiteralPath $CombatLog -Raw), 'staged real strike')).Count
    if($StrikeCount -lt 1) { throw 'Packaged combat pass staged no real strikes; screenshot would not show combat.' }
    Copy-Item -LiteralPath $CombatReport -Destination (Join-Path $ArtifactRoot 'package-combat.json') -Force
    Copy-Item -LiteralPath $CombatScreenshot -Destination (Join-Path $ArtifactRoot 'package-combat.png') -Force
    [ordered]@{ success = $true; staged_strike_log_lines = $StrikeCount } | ConvertTo-Json
}
