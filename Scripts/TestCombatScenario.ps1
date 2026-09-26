# M1-037 + M1-038 entry: runs the full-combo scenario automation suites
# through the shared TestAutomation.ps1 harness and re-checks each exported
# result summary. Thin wrapper on purpose: the default run covers both the
# M1-037 scenario tests (UEMMO.Tasks.M1_037.*) and the M1-038 timing
# regression tests (UEMMO.Tasks.M1_038.*); each run's summary check enforces
# its card's acceptance gate (matched > 0, failed = 0, incomplete = 0,
# success = true). Pass -AdditionalFilter '' to run a single filter.
param(
    [string]$Filter = 'UEMMO.Tasks.M1_037',
    [string]$TaskId = 'M1-037',
    [int]$TimeoutSeconds = 600,
    [string]$AdditionalFilter = 'UEMMO.Tasks.M1_038',
    [string]$AdditionalTaskId = 'M1-038'
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($TaskId) -or $TaskId -match '[/\\]') {
    throw "TestCombatScenario: TaskId must be a non-empty identifier without path separators."
}
if ([string]::IsNullOrWhiteSpace($Filter)) {
    throw "TestCombatScenario: Filter must not be empty."
}
if (-not [string]::IsNullOrWhiteSpace($AdditionalFilter)) {
    if ([string]::IsNullOrWhiteSpace($AdditionalTaskId) -or $AdditionalTaskId -match '[/\\]') {
        throw "TestCombatScenario: AdditionalTaskId must be a non-empty identifier without path separators when AdditionalFilter is set."
    }
}

$Harness = Join-Path $PSScriptRoot 'TestAutomation.ps1'
if (-not (Test-Path -LiteralPath $Harness)) {
    throw ("TestCombatScenario: harness script not found: " + $Harness)
}

function Invoke-ScenarioRun {
    param([string]$RunFilter, [string]$RunTaskId)
    Write-Host ("TestCombatScenario: running filter '" + $RunFilter + "' for task '" + $RunTaskId + "'.")
    & $Harness -Filter $RunFilter -TaskId $RunTaskId -TimeoutSeconds $TimeoutSeconds
    $RunExitCode = $LASTEXITCODE

    # Locate the summary the harness just wrote (newest run directory for the task).
    $TaskArtifactRoot = Join-Path (Join-Path $PSScriptRoot '..') (Join-Path 'Artifacts' (Join-Path 'Tasks' $RunTaskId))
    $SummaryPath = $null
    if (Test-Path -LiteralPath $TaskArtifactRoot) {
        $NewestRun = Get-ChildItem -LiteralPath $TaskArtifactRoot -Directory |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($NewestRun -ne $null) {
            $Candidate = Join-Path $NewestRun.FullName 'automation-summary.json'
            if (Test-Path -LiteralPath $Candidate) {
                $SummaryPath = $Candidate
            }
        }
    }

    if ([string]::IsNullOrEmpty($SummaryPath)) {
        throw "TestCombatScenario: no automation-summary.json found under '" + $TaskArtifactRoot + "'."
    }

    $Summary = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json

    # The summary check: the card requires matched > 0, failed = 0, incomplete = 0.
    $Problems = @()
    if ($Summary.success -ne $true) { $Problems += 'success is not true' }
    if ([int]$Summary.matched -le 0) { $Problems += 'matched test count is not greater than 0' }
    if ([int]$Summary.failed -ne 0) { $Problems += 'failed test count is not 0' }
    if ([int]$Summary.incomplete -ne 0) { $Problems += 'incomplete test count is not 0' }

    Write-Host ("TestCombatScenario summary [" + $RunTaskId + "]: matched=" + $Summary.matched +
        " passed=" + $Summary.passed +
        " failed=" + $Summary.failed +
        " incomplete=" + $Summary.incomplete +
        " output_dir=" + $Summary.output_dir)

    if ($Problems.Count -gt 0) {
        throw ("TestCombatScenario: summary check failed for task '" + $RunTaskId + "': " + ($Problems -join '; '))
    }

    return $RunExitCode
}

$FirstExitCode = Invoke-ScenarioRun -RunFilter $Filter -RunTaskId $TaskId
$SecondExitCode = 0
if (-not [string]::IsNullOrWhiteSpace($AdditionalFilter)) {
    $SecondExitCode = Invoke-ScenarioRun -RunFilter $AdditionalFilter -RunTaskId $AdditionalTaskId
}

Write-Host "TestCombatScenario: all scenario tests passed the summary gate."
exit $(if ($FirstExitCode -ne 0) { $FirstExitCode } else { $SecondExitCode })
