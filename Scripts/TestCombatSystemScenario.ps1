# M5-018 entry: the Segment-A production reaction scenario. Runs the
# UEMMO.Tasks.M5_018.Reaction.* scenario suites through the shared
# TestAutomation.ps1 harness, validates the exported scenario JSON evidence
# (the 018A-saved events under Artifacts/Tasks/M5-018/scenario-json) and, with
# -Render, additionally runs the capture test in a rendered offscreen game
# process and requires fresh screenshots.
#
# Sections (the card's extension interface - later segments register their own):
#   Reaction (M5-018) - UEMMO.Tasks.M5_018.Reaction
#   Weapons  (M5-035) - UEMMO.Tasks.M5_035 (melee/hitscan/projectile/policy chains)
#   Vehicles (M5-044) - UEMMO.Tasks.M5_044 (the enter/drive/fire/exit production chain)
#   Full - not implemented yet; the refusal is explicit
#   (the capability arrives with the owning segment cards).
#
# -Stress (M5-050): after the selected section's passes (which run first and
# unchanged), runs the UEMMO.Tasks.M5_050 projectile stress and lifecycle
# budget suites in a rendered offscreen 1920x1080 game process (the ~60 second
# steady load is carried by the test itself), then copies the test-exported
# Artifacts/Tasks/M5-050/<stamp>/stress-report.json into this run's -TaskId
# evidence folder as stress-report.json next to a hardware.json snapshot of
# this machine's CPU/memory/GPU and the sample time. -Stress composes with any
# implemented -Section and does NOT require -Render (the stress pass always
# renders offscreen on its own so the frame budget numbers are measured with a
# real RHI). Usage:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/TestCombatSystemScenario.ps1 -Section Weapons -Render -Stress -TaskId M5-050
param(
    [ValidateSet('Reaction', 'Weapons', 'Vehicles', 'Full')]
    [string]$Section = 'Reaction',
    [switch]$Render,
    [switch]$Stress,
    [string]$TaskId = 'M5-018',
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($TaskId) -or $TaskId -match '[/\\]') {
    throw "TestCombatSystemScenario: TaskId must be a non-empty identifier without path separators."
}

# Per-section registration: the automation filter, the artifact task folder that
# owns the exported scenario JSON, the required evidence files, the capture test
# suffix and the required rendered screenshots.
$SectionConfigs = @{
    'Reaction' = @{
        'Filter' = 'UEMMO.Tasks.M5_018.Reaction'
        'ArtifactTask' = 'M5-018'
        'ExpectedFiles' = @('reaction-light-hit.json', 'reaction-depth-whiff.json',
            'reaction-launch-heavy-normal.json', 'reaction-death.json')
        'CaptureSuffix' = '.ReactionCapture'
        'Screenshots' = @('scenario-targets.png', 'scenario-float.png')
    }
    'Weapons' = @{
        'Filter' = 'UEMMO.Tasks.M5_035'
        'ArtifactTask' = 'M5-035'
        'ExpectedFiles' = @('weapons-melee.json', 'weapons-hitscan.json',
            'weapons-straight.json', 'weapons-parabolic.json', 'weapons-homing.json',
            'weapons-explosion.json', 'weapons-pierce.json', 'weapons-burst-reload.json',
            'weapons-death-retry.json', 'weapons-config-change.json')
        'CaptureSuffix' = '.WeaponScenarioCapture'
        'Screenshots' = @('capture-targets.png', 'capture-flight.png', 'capture-blast.png')
    }
    'Vehicles' = @{
        'Filter' = 'UEMMO.Tasks.M5_044'
        'ArtifactTask' = 'M5-044'
        'ExpectedFiles' = @('vehicle-scenario.json', 'vehicle-cleanup.json')
        'CaptureSuffix' = '.VehicleScenarioCapture'
        'Screenshots' = @('capture-vehicle.png', 'capture-driving.png')
    }
}
$Config = $SectionConfigs[$Section]
if ($null -eq $Config) {
    throw ("TestCombatSystemScenario: section '" + $Section + "' is not implemented yet (the owning segment card must register its filter first).")
}
$Filter = $Config['Filter']

$Harness = Join-Path $PSScriptRoot 'TestAutomation.ps1'
$ProjectRoot = Join-Path $PSScriptRoot '..'
$ProjectFile = Join-Path $ProjectRoot 'UEMMO.uproject'
$ArtifactRoot = Join-Path $ProjectRoot 'Artifacts'
$ScenarioJsonDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' $Config['ArtifactTask'])

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
$ExpectedFiles = $Config['ExpectedFiles']
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
    $CaptureFilter = $Filter + $Config['CaptureSuffix']
    $ScreenshotDir = Join-Path $ScenarioJsonDir 'scenario-json/render'
    foreach ($Stale in $Config['Screenshots']) {
        $StalePath = Join-Path $ScreenshotDir $Stale
        if (Test-Path -LiteralPath $StalePath) { Remove-Item -LiteralPath $StalePath }
    }
    & $Editor $ProjectFile '-game' '-RenderOffscreen' '-ResX=1280' '-ResY=720' '-NoVSync' `
        '-unattended' '-nosplash' '-nosound' `
        ('-ExecCmds=Automation RunTests ' + $CaptureFilter) `
        '-TestExit=Automation Test Queue Empty' `
        ('-abslog=' + (Join-Path $ArtifactRoot ('Logs\' + $Config['ArtifactTask'].ToLower() + '-capture.log'))) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "TestCombatSystemScenario: the rendered capture run failed (exit $LASTEXITCODE)."
    }
    foreach ($Name in $Config['Screenshots']) {
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

# Pass 4 (optional, -Stress, M5-050): the projectile stress and lifecycle
# budget. A rendered offscreen game process at 1920x1080 runs the
# UEMMO.Tasks.M5_050 suites (the ~60 second steady load lives inside the
# test); the run must exit 0 and export a fresh stress-report.json with
# "success": true, which is copied into this run's -TaskId evidence folder
# next to a hardware.json snapshot of the machine the numbers were measured
# on. The stress pass never reuses a previous run's report: the copied file
# is removed first and the picked report must be newer than this pass.
if ($Stress) {
    . (Join-Path $PSScriptRoot 'Common.ps1')
    $Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $StressSourceRoot = Join-Path $ArtifactRoot (Join-Path 'Tasks' 'M5-050')
    $TaskEvidenceDir = Join-Path $ArtifactRoot (Join-Path 'Tasks' $TaskId)
    $CopiedReportPath = Join-Path $TaskEvidenceDir 'stress-report.json'
    $HardwarePath = Join-Path $TaskEvidenceDir 'hardware.json'
    if (-not (Test-Path -LiteralPath $TaskEvidenceDir)) {
        New-Item -ItemType Directory -Path $TaskEvidenceDir -Force | Out-Null
    }
    foreach ($Stale in @($CopiedReportPath, $HardwarePath)) {
        if (Test-Path -LiteralPath $Stale) { Remove-Item -LiteralPath $Stale }
    }

    Write-Host ("TestCombatSystemScenario: stress pass starting (UEMMO.Tasks.M5_050, 1920x1080, RenderOffscreen).")
    $StressRunStarted = Get-Date
    & $Editor $ProjectFile '-game' '-RenderOffscreen' '-ResX=1920' '-ResY=1080' '-NoVSync' `
        '-unattended' '-nosplash' '-nosound' `
        '-ExecCmds=Automation RunTests UEMMO.Tasks.M5_050' `
        '-TestExit=Automation Test Queue Empty' `
        ('-abslog=' + (Join-Path $ArtifactRoot 'Logs\m5-050-stress.log')) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "TestCombatSystemScenario: the stress automation run failed (exit $LASTEXITCODE)."
    }

    # The test exports Artifacts/Tasks/M5-050/<stamp>/stress-report.json; pick
    # the newest export and require it to be from this pass.
    $StressReport = $null
    if (Test-Path -LiteralPath $StressSourceRoot) {
        $StressReport = Get-ChildItem -LiteralPath $StressSourceRoot -Directory | ForEach-Object {
            $Candidate = Join-Path $_.FullName 'stress-report.json'
            if (Test-Path -LiteralPath $Candidate) { Get-Item -LiteralPath $Candidate }
        } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }
    if ($null -eq $StressReport) {
        throw "TestCombatSystemScenario: the stress report was not exported under Artifacts/Tasks/M5-050."
    }
    if ($StressReport.LastWriteTime -lt $StressRunStarted) {
        throw ("TestCombatSystemScenario: the stress report is stale (written before this pass): " + $StressReport.FullName)
    }
    $StressPayload = Get-Content -LiteralPath $StressReport.FullName -Raw | ConvertFrom-Json
    if ($StressPayload.success -ne $true) {
        throw ("TestCombatSystemScenario: the stress report holds success=false: " + $StressReport.FullName)
    }
    Copy-Item -LiteralPath $StressReport.FullName -Destination $CopiedReportPath -Force

    # The hardware snapshot of the machine the budget numbers were measured
    # on (AdapterRAM is a uint32 WMI field and under-reports GPUs above 4 GB;
    # the name/driver pair stays the identifying information).
    $Cpu = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
    $Gpus = @(Get-CimInstance -ClassName Win32_VideoController | Where-Object { $_.Name })
    $MemorySticks = @(Get-CimInstance -ClassName Win32_PhysicalMemory)
    $Os = Get-CimInstance -ClassName Win32_OperatingSystem | Select-Object -First 1
    $TotalPhysicalBytes = ($MemorySticks | Measure-Object -Property Capacity -Sum).Sum
    $Hardware = [ordered]@{
        sampled_at_utc8   = (Get-Date).ToUniversalTime().AddHours(8).ToString('yyyy-MM-ddTHH:mm:ss')
        stress_filter     = 'UEMMO.Tasks.M5_050'
        stress_resolution = '1920x1080'
        render_mode       = 'RenderOffscreen'
        task_id           = $TaskId
        cpu    = [ordered]@{
            name                = $Cpu.Name
            cores               = $Cpu.NumberOfCores
            logical_processors  = $Cpu.NumberOfLogicalProcessors
            max_clock_mhz       = $Cpu.MaxClockSpeed
        }
        memory = [ordered]@{
            total_physical_mb = [math]::Round($TotalPhysicalBytes / 1MB)
            os_visible_mb     = [math]::Round($Os.TotalVisibleMemorySize / 1KB)
            sticks            = $MemorySticks.Count
        }
        gpus   = @($Gpus | ForEach-Object {
            [ordered]@{
                name            = $_.Name
                driver_version  = $_.DriverVersion
                adapter_ram_mb  = if ($_.AdapterRAM) { [math]::Round($_.AdapterRAM / 1MB) } else { $null }
            }
        })
        os     = [ordered]@{
            caption  = $Os.Caption
            version  = $Os.Version
        }
    }
    $Hardware | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $HardwarePath -Encoding UTF8
    Write-Host ("TestCombatSystemScenario: stress evidence ok (report: " + $CopiedReportPath + ", hardware: " + $HardwarePath + ").")
}

Write-Host "TestCombatSystemScenario: section '" + $Section + "' PASSED."
