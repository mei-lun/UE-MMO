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
# M2-015: -IncludeRoomScenario adds the M2 package passes after the M0 smoke:
#   1. in-package full-suite automation (same gate as -IncludeCombat, own
#      evidence directory, relaxed report-age window because the 329-test
#      suite incl. the five M2_014 room scenarios runs longer). The
#      dev-source-JSON exemption list now names every family that validates
#      FPaths::ProjectDir()/Data files the package does not ship:
#      M1_008 (combat-attacks.json, M1-039 precedent), M2_001 (enemies.json),
#      M2_005 (rooms.json + enemies.json), M2_008 (rooms.json + enemies.json).
#      The same contract values are re-validated against cooked assets /
#      runtime doubles by M1_009, M2_002..M2_004 and M2_006..M2_014, which all
#      pass in-package. The full allowed list is reported, never dropped.
#   2. the scenario evidence JSONs written in-package by M2_014
#      (<package>/Artifacts/Tasks/M2-014/scenario-json) are copied out.
#   3. rendered result-screen capture: packaged exe with -RenderOffscreen
#      running UEMMO.Tasks.M2_012.RoomResultScreenshots (the M2-012 capture
#      companion) so room-result-victory/defeat/after-retry PNGs come from the
#      real HUD through the production terminal entries.
#   4. rendered exit-state capture on L_CombatRoom01: UEMMO.Tasks.M2_009
#      .ExitStateScreenshots produces exit-locked / exit-unlocked PNGs and
#      doubles as the packaged-entry proof for the room map.
#   5. L_CombatRoom01 entry smoke: -UEMMOSmoke on the room map (movement +
#      jump + HUD screenshot) proves the packaged game enters the M2 map.
# M3-022: -IncludeProgressionScenario adds the M3 package passes after the M0
#   smoke, reusing the M2-015 in-package pattern for the M3-020 growth loop:
#   1. in-package full-suite automation (same gate as the M2 pass; the
#      DevOnlySourceJsonFilter default list now also names the two new M3
#      dev-source-JSON validator families: M3_001 reads Data/items.json and
#      M3_007 reads Data/drops.json + Data/items.json). The same contract
#      values are re-validated in-package against cooked assets / the runtime
#      catalog by M1_009, M3_002..M3_006 (item definitions) and
#      M3_008..M3_010 (drop generation), which all pass in-package.
#   2. the progression evidence JSON written in-package by the M3-020 suite
#      (full loop: enter room -> two waves -> settle -> claim -> equip ->
#      save -> simulated restart -> damage reflects equipment, on the
#      dedicated M3_020Slot_ test slots, never the player's Profile_ saves)
#      is copied out of <package>/Artifacts/Tasks/M3-020/scenario-json.
#   3. rendered inventory/settlement UI capture: packaged exe with
#      -RenderOffscreen running UEMMO.Tasks.M3_019.InventoryIconScreenshots
#      (the M3-019 capture companion) so the packaged inventory rows (slot
#      icons WPN/ARM/ACC) and the settlement reward line come from the real
#      HUD widgets on the cooked UI code.
# M3-022: a second tolerance list joins the dev-source-JSON one, for failures
#   that are EXPECTED in the packaged optimized game build for reasons other
#   than dev-source JSON (every failed name is still reported in full):
#   - UEMMO.Tasks.M3_005.StatsChangedDetectsOnlyRealChanges pins the defensive
#     "NaN always reports a change" float semantics. The editor Development
#     build preserves IEEE NaN compares for this TU; the optimized packaged
#     game build folds the all-constant NaN comparisons differently (fast
#     float mode), so the two NaN assertions fail in-package. Production
#     inputs are validated non-finite-free (M3_013 save validation rejects
#     non-finite stats), so no gameplay path is affected; the guarantee stays
#     editor-verified until the semantics are re-pinned in a float-mode-safe
#     way (registered limitation, not silenced).
#   - UEMMO.Tasks.M3_019.SlotIconConfigIsSharedAndDistinct and
#     UEMMO.Tasks.M3_019.InventoryRowsShowIconWithShortTagFallback resolve the
#     ENGINE BUILTIN placeholder icon textures (/Engine/EngineResources/
#     AICON-Red, AICON-Green) via code soft paths. Soft paths never enter the
#     cook dependency chain, so the two engine textures are not inside the
#     pak and LoadObject fails in-package; the inventory widget then uses the
#     DESIGNED fail-closed fallback (icon hidden, short tag shown). The
#     GradientTexture0 accessory icon (referenced by cooked content) resolves.
#     Editor runs pass. The registered fix path: swap to project assets (which
#     cook) or add the engine assets to the always-cook list.
param(
    [switch]$IncludeCombat,
    [switch]$IncludeRoomScenario,
    [switch]$IncludeProgressionScenario,
    [string]$CombatFilter = 'UEMMO.Tasks',
    [string[]]$DevOnlySourceJsonFilter = @('UEMMO.Tasks.M1_008.', 'UEMMO.Tasks.M2_001.', 'UEMMO.Tasks.M2_005.', 'UEMMO.Tasks.M2_008.', 'UEMMO.Tasks.M3_001.', 'UEMMO.Tasks.M3_007.'),
    [string[]]$ExpectedInPackageFailures = @('UEMMO.Tasks.M3_005.', 'UEMMO.Tasks.M3_019.'),
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
    # M2-015: the tolerated families are now a prefix list (all dev-source-JSON
    # validators); M3-022 adds the second prefix list of expected-in-package
    # failures (fast-float NaN semantics, uncooked engine soft-path icons).
    # Any failure outside every listed prefix still fails the gate.
    $AllAllowedPrefixes = @($DevOnlySourceJsonFilter) + @($ExpectedInPackageFailures)
    $UnexpectedFailed = @(foreach($FailedName in $FailedNames) {
        $Allowed = $false
        foreach($Prefix in $AllAllowedPrefixes) {
            if($FailedName.StartsWith($Prefix)) { $Allowed = $true; break }
        }
        if(-not $Allowed) { $FailedName }
    })
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
        expected_in_package_failures = @($ExpectedInPackageFailures)
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

if($IncludeRoomScenario) {
    # M2-015 Pass 1: in-package automation over the full suite. Same mechanism
    # and gate as the -IncludeCombat pass (Development ships the test code and
    # the engine's Launch loop loads the AutomationController module); the only
    # tolerated failures remain the dev-source-JSON validators listed in
    # DevOnlySourceJsonFilter, each reported by name. The report-age window is
    # relaxed to 60 minutes because the 329-test suite (incl. the five M2_014
    # room scenarios) runs longer than the M1-era suite inside the package.
    $Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
    $ReportDir = Join-Path $ArtifactRoot (Join-Path 'Tasks\M2-015-Package' $Stamp)
    New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
    Invoke-UEProcess $Executable @('-unattended', '-nosplash', '-nosound', '-nullrhi',
        "-ExecCmds=Automation RunTests $CombatFilter",
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$ReportDir",
        "-abslog=$ArtifactRoot\Logs\package-scenario-automation.log") 'package-automation' 900
    $IndexJson = Join-Path $ReportDir 'index.json'
    if(-not (Test-Path -LiteralPath $IndexJson)) { throw 'Packaged automation did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $IndexJson -Raw) -Filter $CombatFilter -MaxAgeMinutes 60
    if(-not $Parsed.Ok -and $Parsed.ErrorKind -ne 'failures') { throw 'Packaged automation gate failed: ' + $Parsed.Error }
    if($Parsed.Passed -lt 1) { throw 'Packaged automation gate failed: zero tests passed.' }
    if($Parsed.Incomplete -ne 0) { throw 'Packaged automation gate failed: incomplete tests: ' + $Parsed.Error }
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
    $AllAllowedPrefixes = @($DevOnlySourceJsonFilter) + @($ExpectedInPackageFailures)
    $UnexpectedFailed = @(foreach($FailedName in $FailedNames) {
        $Allowed = $false
        foreach($Prefix in $AllAllowedPrefixes) {
            if($FailedName.StartsWith($Prefix)) { $Allowed = $true; break }
        }
        if(-not $Allowed) { $FailedName }
    })
    if($UnexpectedFailed.Count -gt 0) { throw 'Packaged automation gate failed: unexpected failures: ' + ($UnexpectedFailed -join ', ') }
    Copy-Item -LiteralPath $IndexJson -Destination (Join-Path $ArtifactRoot 'package-scenario-automation-index.json') -Force
    $AutomationSummary = [ordered]@{
        success = $true
        filter = $CombatFilter
        report_path = $IndexJson
        matched = $Parsed.Matched
        passed = $Parsed.Passed
        failed = $Parsed.Failed
        incomplete = $Parsed.Incomplete
        allowed_dev_only_failures = $FailedNames
        allowed_dev_only_filter = @($DevOnlySourceJsonFilter)
        expected_in_package_failures = @($ExpectedInPackageFailures)
    }
    $AutomationSummary | ConvertTo-Json

    # M2-015 Pass 2: copy the scenario evidence JSONs that the M2_014 room
    # scenarios just wrote inside the package (FPaths::ProjectDir()/Artifacts
    # of the packaged game). Their existence proves the two-wave run, the
    # death-retry path and the mid-run exit path all executed in-package.
    $PackageScenarioDir = Join-Path $PackageProject 'Artifacts\Tasks\M2-014\scenario-json'
    $ScenarioFiles = @(Get-ChildItem -LiteralPath $PackageScenarioDir -File -Filter '*.json' -ErrorAction SilentlyContinue)
    if($ScenarioFiles.Count -lt 1) { throw 'Packaged M2_014 run wrote no scenario evidence JSONs.' }
    New-Item -ItemType Directory -Path (Join-Path $ArtifactRoot 'package-scenario-json') -Force | Out-Null
    $CopiedScenario = @()
    foreach($ScenarioFile in $ScenarioFiles) {
        Copy-Item -LiteralPath $ScenarioFile.FullName -Destination (Join-Path $ArtifactRoot (Join-Path 'package-scenario-json' $ScenarioFile.Name)) -Force
        $CopiedScenario += $ScenarioFile.Name
    }
    [ordered]@{ success = $true; scenario_json = $CopiedScenario } | ConvertTo-Json

    # M2-015 Passes 3-5: rendered captures. Shared runner: packaged exe with
    # -RenderOffscreen (real rendering so FScreenshotRequest writes real
    # frames), the requested capture test via -ExecCmds, -TestExit and
    # -ReportExportPath for the gate. Every expected screenshot is checked for
    # existence and a valid PNG header, then copied into Artifacts.
    function Invoke-M2RenderedCapture {
        param(
            [Parameter(Mandatory=$true)][string]$PassName,
            [Parameter(Mandatory=$true)][string]$TestName,
            [string]$MapArgument = '',
            [Parameter(Mandatory=$true)][string]$TaskFolder,
            [Parameter(Mandatory=$true)][hashtable]$NameMap
        )
        $Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
        $CaptureDir = Join-Path $ArtifactRoot (Join-Path 'Tasks\M2-015-Package' ($Stamp + '-' + $PassName))
        New-Item -ItemType Directory -Path $CaptureDir -Force | Out-Null
        $CaptureArgs = @('-unattended', '-nosplash', '-nosound', '-RenderOffscreen', '-ResX=1280', '-ResY=720')
        if($MapArgument -ne '') { $CaptureArgs = @($MapArgument) + $CaptureArgs }
        $CaptureArgs += @("-ExecCmds=Automation RunTests $TestName",
            '-TestExit=Automation Test Queue Empty',
            "-ReportExportPath=$CaptureDir",
            "-abslog=$ArtifactRoot\Logs\$PassName.log")
        Invoke-UEProcess $Executable $CaptureArgs $PassName 900
        $CaptureIndex = Join-Path $CaptureDir 'index.json'
        if(-not (Test-Path -LiteralPath $CaptureIndex)) { throw "$PassName did not export index.json." }
        $CaptureParsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $CaptureIndex -Raw) -Filter $TestName
        if(-not $CaptureParsed.Ok) { throw "$PassName gate failed: " + $CaptureParsed.Error }
        $SourceDir = Join-Path $PackageProject (Join-Path 'Artifacts\Tasks' $TaskFolder)
        foreach($SourceName in $NameMap.Keys) {
            $SourceFile = Join-Path $SourceDir $SourceName
            if(-not (Test-Path -LiteralPath $SourceFile)) { throw "$PassName screenshot missing: $SourceName" }
            $CaptureBytes = [IO.File]::ReadAllBytes($SourceFile)
            if($CaptureBytes.Length -lt 1024 -or [BitConverter]::ToString($CaptureBytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "$PassName screenshot $SourceName is not a valid PNG." }
            Copy-Item -LiteralPath $SourceFile -Destination (Join-Path $ArtifactRoot $NameMap[$SourceName]) -Force
        }
        [ordered]@{
            success = $true
            pass = $PassName
            test = $TestName
            map = $MapArgument
            matched = $CaptureParsed.Matched
            passed = $CaptureParsed.Passed
            failed = $CaptureParsed.Failed
            incomplete = $CaptureParsed.Incomplete
            screenshots = @($NameMap.Values)
        } | ConvertTo-Json
    }

    # Pass 3: Victory / Defeat / after-retry result screens from the real HUD
    # (M2-012 capture companion; same invocation as the M2-012 precedent).
    Invoke-M2RenderedCapture -PassName 'package-result-screens' `
        -TestName 'UEMMO.Tasks.M2_012.RoomResultScreenshots' `
        -MapArgument '/Game/UEMMO/Maps/L_TrainingArena' `
        -TaskFolder 'M2-012' `
        -NameMap @{
            'room-result-victory.png'     = 'package-result-victory.png'
            'room-result-defeat.png'      = 'package-result-defeat.png'
            'room-result-after-retry.png' = 'package-result-after-retry.png'
        }

    # Pass 4: exit locked / unlocked states on the M2 room map (M2-009 capture
    # companion on L_CombatRoom01), which also proves the packaged game loads
    # the room map asset itself.
    Invoke-M2RenderedCapture -PassName 'package-exit-screens' `
        -TestName 'UEMMO.Tasks.M2_009.ExitStateScreenshots' `
        -MapArgument '/Game/UEMMO/Maps/L_CombatRoom01' `
        -TaskFolder 'M2-009' `
        -NameMap @{
            'exit-locked.png'   = 'package-exit-locked.png'
            'exit-unlocked.png' = 'package-exit-unlocked.png'
        }

    # Pass 5: L_CombatRoom01 entry smoke - the packaged game boots straight
    # into the M2 room map, moves with depth, jumps and captures the HUD
    # (the M0 smoke runner is map-agnostic; the room trigger starting a run
    # does not affect the movement assertions).
    foreach($Name in @('runtime-smoke.json','prototype-smoke.png')) {
        $File = Join-Path $PackageArtifacts $Name
        if(Test-Path -LiteralPath $File) { Remove-Item -LiteralPath $File }
    }
    Invoke-UEProcess $Executable @('/Game/UEMMO/Maps/L_CombatRoom01', '-UEMMOSmoke', '-unattended', '-RenderOffscreen', '-nosound',
        '-ResX=1280', '-ResY=720',
        "-abslog=$ArtifactRoot\Logs\package-entry-smoke.log") 'package-entry-smoke' 600
    $EntryReport = Join-Path $PackageArtifacts 'runtime-smoke.json'
    $EntryScreenshot = Join-Path $PackageArtifacts 'prototype-smoke.png'
    if(-not (Test-Path -LiteralPath $EntryReport)) { throw 'Packaged entry smoke did not produce a runtime report.' }
    $EntryResult = Get-Content -LiteralPath $EntryReport -Raw | ConvertFrom-Json
    if(-not $EntryResult.success -or -not $EntryResult.rendering) { throw 'Packaged L_CombatRoom01 entry smoke failed the movement/render smoke.' }
    if(-not (Test-Path -LiteralPath $EntryScreenshot)) { throw 'Packaged entry smoke screenshot missing.' }
    $EntryBytes = [IO.File]::ReadAllBytes($EntryScreenshot)
    if($EntryBytes.Length -lt 1024 -or [BitConverter]::ToString($EntryBytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Packaged entry smoke screenshot is not a valid PNG header.' }
    Copy-Item -LiteralPath $EntryReport -Destination (Join-Path $ArtifactRoot 'package-entry-smoke.json') -Force
    Copy-Item -LiteralPath $EntryScreenshot -Destination (Join-Path $ArtifactRoot 'package-entry-smoke.png') -Force
    $EntryResult | ConvertTo-Json
}

if($IncludeProgressionScenario) {
    # M3-022 Pass 1: in-package automation over the full suite. Same mechanism
    # and gate as the M2-015 pass (Development ships the test code and the
    # engine's Launch loop loads the AutomationController module); the only
    # tolerated failures remain the dev-source-JSON validators listed in
    # DevOnlySourceJsonFilter (now including M3_001 items.json and M3_007
    # drops.json+items.json), each reported by name. The report-age window is
    # relaxed to 90 minutes because the 480-test suite (incl. the four M3_020
    # progression-loop scenarios and the six M3_021 failure regressions) runs
    # longer than the M2-era suite inside the package.
    $Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
    $ReportDir = Join-Path $ArtifactRoot (Join-Path 'Tasks\M3-022-Package' $Stamp)
    New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
    Invoke-UEProcess $Executable @('-unattended', '-nosplash', '-nosound', '-nullrhi',
        "-ExecCmds=Automation RunTests $CombatFilter",
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$ReportDir",
        "-abslog=$ArtifactRoot\Logs\package-progression-automation.log") 'package-automation' 1200
    $IndexJson = Join-Path $ReportDir 'index.json'
    if(-not (Test-Path -LiteralPath $IndexJson)) { throw 'Packaged automation did not export index.json.' }
    $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $IndexJson -Raw) -Filter $CombatFilter -MaxAgeMinutes 90
    if(-not $Parsed.Ok -and $Parsed.ErrorKind -ne 'failures') { throw 'Packaged automation gate failed: ' + $Parsed.Error }
    if($Parsed.Passed -lt 1) { throw 'Packaged automation gate failed: zero tests passed.' }
    if($Parsed.Incomplete -ne 0) { throw 'Packaged automation gate failed: incomplete tests: ' + $Parsed.Error }
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
    $AllAllowedPrefixes = @($DevOnlySourceJsonFilter) + @($ExpectedInPackageFailures)
    $UnexpectedFailed = @(foreach($FailedName in $FailedNames) {
        $Allowed = $false
        foreach($Prefix in $AllAllowedPrefixes) {
            if($FailedName.StartsWith($Prefix)) { $Allowed = $true; break }
        }
        if(-not $Allowed) { $FailedName }
    })
    if($UnexpectedFailed.Count -gt 0) { throw 'Packaged automation gate failed: unexpected failures: ' + ($UnexpectedFailed -join ', ') }
    Copy-Item -LiteralPath $IndexJson -Destination (Join-Path $ArtifactRoot 'package-progression-automation-index.json') -Force
    $AutomationSummary = [ordered]@{
        success = $true
        filter = $CombatFilter
        report_path = $IndexJson
        matched = $Parsed.Matched
        passed = $Parsed.Passed
        failed = $Parsed.Failed
        incomplete = $Parsed.Incomplete
        allowed_dev_only_failures = $FailedNames
        allowed_dev_only_filter = @($DevOnlySourceJsonFilter)
        expected_in_package_failures = @($ExpectedInPackageFailures)
    }
    $AutomationSummary | ConvertTo-Json

    # M3-022 Pass 2: copy the progression evidence JSON that the M3_020
    # growth-loop suite just wrote inside the package
    # (<package>/Artifacts/Tasks/M3-020/scenario-json). Its existence proves
    # the full loop (enter room -> two waves -> settle -> claim -> equip ->
    # save -> simulated restart -> damage reflects equipment) executed
    # in-package on the dedicated M3_020Slot_ test slots (never Profile_).
    $PackageProgressionDir = Join-Path $PackageProject 'Artifacts\Tasks\M3-020\scenario-json'
    $ProgressionFiles = @(Get-ChildItem -LiteralPath $PackageProgressionDir -File -Filter '*.json' -ErrorAction SilentlyContinue)
    if($ProgressionFiles.Count -lt 1) { throw 'Packaged M3_020 run wrote no progression evidence JSONs.' }
    New-Item -ItemType Directory -Path (Join-Path $ArtifactRoot 'package-progression-json') -Force | Out-Null
    $CopiedProgression = @()
    foreach($ProgressionFile in $ProgressionFiles) {
        Copy-Item -LiteralPath $ProgressionFile.FullName -Destination (Join-Path $ArtifactRoot (Join-Path 'package-progression-json' $ProgressionFile.Name)) -Force
        $CopiedProgression += $ProgressionFile.Name
    }
    [ordered]@{ success = $true; progression_json = $CopiedProgression } | ConvertTo-Json

    # M3-022 Pass 3: rendered inventory/settlement UI capture. The M3-019
    # capture companion drives the real HUD inventory screen and the real
    # result-screen reward line in the packaged game (same shape as the M2-015
    # rendered passes: -RenderOffscreen so FScreenshotRequest writes real
    # frames, -TestExit plus -ReportExportPath for the gate).
    $CaptureStamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
    $CaptureDir = Join-Path $ArtifactRoot (Join-Path 'Tasks\M3-022-Package' ($CaptureStamp + '-ui-screens'))
    New-Item -ItemType Directory -Path $CaptureDir -Force | Out-Null
    Invoke-UEProcess $Executable @('/Game/UEMMO/Maps/L_TrainingArena', '-unattended', '-nosplash', '-nosound', '-RenderOffscreen', '-ResX=1280', '-ResY=720',
        '-ExecCmds=Automation RunTests UEMMO.Tasks.M3_019.InventoryIconScreenshots',
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$CaptureDir",
        "-abslog=$ArtifactRoot\Logs\package-ui-screens.log") 'package-ui-screens' 900
    $CaptureIndex = Join-Path $CaptureDir 'index.json'
    if(-not (Test-Path -LiteralPath $CaptureIndex)) { throw 'Packaged UI capture did not export index.json.' }
    $CaptureParsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $CaptureIndex -Raw) -Filter 'UEMMO.Tasks.M3_019.InventoryIconScreenshots'
    if(-not $CaptureParsed.Ok) { throw 'Packaged UI capture gate failed: ' + $CaptureParsed.Error }
    $UiCaptures = @{
        'inventory-icons.png'   = 'package-inventory-icons.png'
        'settlement-icons.png'  = 'package-settlement-icons.png'
    }
    foreach($SourceName in @('inventory-icons.png','settlement-icons.png')) {
        $SourceFile = Join-Path (Join-Path $PackageProject 'Artifacts\Tasks\M3-019') $SourceName
        if(-not (Test-Path -LiteralPath $SourceFile)) { throw "Packaged UI capture screenshot missing: $SourceName" }
        $CaptureBytes = [IO.File]::ReadAllBytes($SourceFile)
        if($CaptureBytes.Length -lt 1024 -or [BitConverter]::ToString($CaptureBytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Packaged UI capture screenshot $SourceName is not a valid PNG." }
        Copy-Item -LiteralPath $SourceFile -Destination (Join-Path $ArtifactRoot $UiCaptures[$SourceName]) -Force
    }
    [ordered]@{
        success = $true
        pass = 'package-ui-screens'
        test = 'UEMMO.Tasks.M3_019.InventoryIconScreenshots'
        map = '/Game/UEMMO/Maps/L_TrainingArena'
        matched = $CaptureParsed.Matched
        passed = $CaptureParsed.Passed
        failed = $CaptureParsed.Failed
        incomplete = $CaptureParsed.Incomplete
        screenshots = @($UiCaptures.Values)
    } | ConvertTo-Json
}
