# M5-018 entry: the Segment-A production reaction scenario. Runs the
# UEMMO.Tasks.M5_018.Reaction.* scenario suites through the shared
# TestAutomation.ps1 harness, validates the exported scenario JSON evidence
# (the 018A-saved events under Artifacts/Tasks/M5-018/scenario-json) and, with
# -Render, additionally runs the capture test in a rendered offscreen game
# process and requires fresh screenshots.
#
# Sections (the card's extension interface - later segments register their own):
#   Reaction (this card) - UEMMO.Tasks.M5_018.Reaction
#   Weapons  / Vehicles / Full - not implemented yet; the refusal is explicit
#   (the capability arrives with the M5-036/M5-045 segment cards).
param(
    [ValidateSet('Reaction', 'Weapons', 'Vehicles', 'Full')]
    [string]$Section = 'Reaction',
    [switch]$Render,
    [string]$TaskId = 'M5-018',
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($TaskId) -or $TaskId -match '[/\\]') {
    throw "TestCombatSystemScenario: TaskId must be a non-empty identifier without path separators."
}

$SectionFilters = @{
    'Reaction' = 'UEMMO.Tasks.M5_018.Reaction'
    'Weapons'  = $null
    'Vehicles' = $null
    'Full'     = $null
}
$Filter = $SectionFilters[$Section]
if ([string]::IsNullOrEmpty($Filter)) {
    throw ("TestCombatSystemScenario: section '" + $Section + "' is not implemented yet (the owning segment card must register its filter first).")
}

$Harness = Join-Path $PSScriptRoot 'TestAutomation.ps1'
$ProjectRoot = Join-Path $PSScriptRoot '..'
$ProjectFile = Join-Path $ProjectRoot 'UEMMO.uproject'
$ArtifactRoot = Join-Path $ProjectRoot 'Artifacts'
$ScenarioJsonDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' 'M5-018')

Write-Host ("TestCombatSystemScenario: section '" + $Section + "' filter '" + $Filter + "' task '" + $TaskId + "'.")

# Pass 1: the scenario automation suites (the shared harness, nullrhi).
& $Harness -Filter $Filter -TaskId $TaskId -TimeoutSeconds $TimeoutSeconds
$RunExitCode = $LASTEXITCODE
if ($RunExitCode -ne 0) {
    throw "TestCombatSystemScenario: the scenario automation run failed (exit $RunExitCode)."
}

# Pass 2: the scenario JSON evidence (the exported reaction event sequences)
# must exist and parse - the card's "读取已保存场景" gate.
$JsonDir = Join-Path $ScenarioJsonDir 'scenario-json'
$ExpectedFiles = @('reaction-light-hit.json', 'reaction-depth-whiff.json',
    'reaction-launch-heavy-normal.json', 'reaction-death.json')
$EventsTotal = 0
foreach ($Name in $ExpectedFiles) {
    $Path = Join-Path $JsonDir $Name
    if (-not (Test-Path -LiteralPath $Path)) {
        throw ("TestCombatSystemScenario: the scenario evidence is missing: " + $Name)
    }
    $Payload = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $Payload.events -or @($Payload.events).Count -eq 0) {
        throw ("TestCombatSystemScenario: the scenario evidence holds no events: " + $Name)
    }
    $EventsTotal += @($Payload.events).Count
}
Write-Host ("TestCombatSystemScenario: scenario evidence ok (" + $EventsTotal + " events across " + $ExpectedFiles.Count + " files).")

# Pass 3 (optional): the rendered capture - a real offscreen game process runs
# the capture test and the fresh screenshots are required to exist.
if ($Render) {
    . (Join-Path $PSScriptRoot 'Common.ps1')
    $Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $CaptureFilter = $Filter + '.ReactionCapture'
    $ScreenshotDir = Join-Path $ScenarioJsonDir 'scenario-json/render'
    foreach ($Stale in @('scenario-targets.png', 'scenario-float.png')) {
        $StalePath = Join-Path $ScreenshotDir $Stale
        if (Test-Path -LiteralPath $StalePath) { Remove-Item -LiteralPath $StalePath }
    }
    & $Editor $ProjectFile '-game' '-RenderOffscreen' '-ResX=1280' '-ResY=720' '-NoVSync' `
        '-unattended' '-nosplash' '-nosound' `
        ('-ExecCmds=Automation RunTests ' + $CaptureFilter) `
        '-TestExit=Automation Test Queue Empty' `
        ('-abslog=' + (Join-Path $ArtifactRoot 'Logs\m5-018-capture.log')) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "TestCombatSystemScenario: the rendered capture run failed (exit $LASTEXITCODE)."
    }
    foreach ($Name in @('scenario-targets.png', 'scenario-float.png')) {
        # The engine flushes the screenshot during shutdown; poll briefly.
        $Path = Join-Path $ScreenshotDir $Name
        $Waited = 0
        while (-not (Test-Path -LiteralPath $Path) -and $Waited -lt 15) {
            Start-Sleep -Milliseconds 500
            $Waited += 1
        }
        if (-not (Test-Path -LiteralPath $Path)) {
            throw ("TestCombatSystemScenario: the rendered capture is missing: " + $Name)
        }
        $Bytes = [IO.File]::ReadAllBytes($Path)
        if ($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes, 0, 8) -ne '89-50-4E-47-0D-0A-1A-0A') {
            throw ("TestCombatSystemScenario: the rendered capture is empty or not a PNG: " + $Name)
        }
    }
    Write-Host "TestCombatSystemScenario: rendered captures ok."
}

Write-Host "TestCombatSystemScenario: section '" + $Section + "' PASSED."
