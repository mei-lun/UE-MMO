# M2-014 entry: runs the two-wave room scenario regression suite
# (UEMMO.Tasks.M2_014.*) through the shared TestAutomation.ps1 harness and
# re-checks the exported result summary. Thin wrapper on purpose: the summary
# gate enforces the card's acceptance (success = true, matched > 0, failed = 0,
# incomplete = 0). The scenario evidence JSON files written by the tests
# (Artifacts/Tasks/M2-014/scenario-json/*.json) are copied into the harness
# run directory afterwards, so one run folder carries the complete evidence.
param(
    [string]$Filter = 'UEMMO.Tasks.M2_014',
    [string]$TaskId = 'M2-014',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($TaskId) -or $TaskId -match '[/\\]') {
    throw "TestRoomScenario: TaskId must be a non-empty identifier without path separators."
}
if ([string]::IsNullOrWhiteSpace($Filter)) {
    throw "TestRoomScenario: Filter must not be empty."
}

$Harness = Join-Path $PSScriptRoot 'TestAutomation.ps1'
if (-not (Test-Path -LiteralPath $Harness)) {
    throw ("TestRoomScenario: harness script not found: " + $Harness)
}

Write-Host ("TestRoomScenario: running filter '" + $Filter + "' for task '" + $TaskId + "'.")
& $Harness -Filter $Filter -TaskId $TaskId -TimeoutSeconds $TimeoutSeconds
$RunExitCode = $LASTEXITCODE

# Locate the summary the harness just wrote (newest run directory for the task).
$ProjectRoot = (Get-Item -LiteralPath $PSScriptRoot).Parent.FullName
$TaskArtifactRoot = Join-Path $ProjectRoot (Join-Path 'Artifacts' (Join-Path 'Tasks' $TaskId))
$SummaryPath = $null
$RunDir = $null
if (Test-Path -LiteralPath $TaskArtifactRoot) {
    $NewestRun = Get-ChildItem -LiteralPath $TaskArtifactRoot -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'automation-summary.json') } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($NewestRun -ne $null) {
        $RunDir = $NewestRun.FullName
        $SummaryPath = Join-Path $RunDir 'automation-summary.json'
    }
}

if ([string]::IsNullOrEmpty($SummaryPath)) {
    throw "TestRoomScenario: no automation-summary.json found under '" + $TaskArtifactRoot + "'."
}

$Summary = Get-Content -LiteralPath $SummaryPath -Raw | ConvertFrom-Json

# The summary check: the card requires matched > 0, failed = 0, incomplete = 0.
$Problems = @()
if ($Summary.success -ne $true) { $Problems += 'success is not true' }
if ([int]$Summary.matched -le 0) { $Problems += 'matched test count is not greater than 0' }
if ([int]$Summary.failed -ne 0) { $Problems += 'failed test count is not 0' }
if ([int]$Summary.incomplete -ne 0) { $Problems += 'incomplete test count is not 0' }

Write-Host ("TestRoomScenario summary [" + $TaskId + "]: matched=" + $Summary.matched +
    " passed=" + $Summary.passed +
    " failed=" + $Summary.failed +
    " incomplete=" + $Summary.incomplete +
    " output_dir=" + $Summary.output_dir)

if ($Problems.Count -gt 0) {
    throw ("TestRoomScenario: summary check failed for task '" + $TaskId + "': " + ($Problems -join '; '))
}

# Copy the scenario evidence JSON files into the run directory (best effort:
# a missing evidence folder is reported but does not fail the gate).
$EvidenceSource = Join-Path $ProjectRoot (Join-Path 'Artifacts' (Join-Path 'Tasks' (Join-Path 'M2-014' 'scenario-json')))
if (Test-Path -LiteralPath $EvidenceSource) {
    $EvidenceFiles = Get-ChildItem -LiteralPath $EvidenceSource -File -Filter '*.json' -ErrorAction SilentlyContinue
    foreach ($EvidenceFile in $EvidenceFiles) {
        if ($RunDir -ne $null) {
            Copy-Item -LiteralPath $EvidenceFile.FullName -Destination $RunDir -Force
            Write-Host ("TestRoomScenario: evidence copied: " + $EvidenceFile.Name)
        }
    }
    if ($EvidenceFiles.Count -eq 0) {
        Write-Host "TestRoomScenario: warning - no scenario evidence JSON files found to copy."
    }
}
else {
    Write-Host ("TestRoomScenario: warning - scenario evidence folder not found: " + $EvidenceSource)
}

Write-Host "TestRoomScenario: all room scenario tests passed the summary gate."
exit $RunExitCode
