param(
    [string]$Filter = '',
    [string]$TaskId = '',
    [switch]$SelfTest,
    [int]$TimeoutSeconds = 600
)
. "$PSScriptRoot\Common.ps1"

if (-not $SelfTest) {
    if ([string]::IsNullOrWhiteSpace($TaskId) -or $TaskId -match '[/\\]') {
        throw "TestAutomation: TaskId must be a non-empty identifier without path separators."
    }
    if ([string]::IsNullOrWhiteSpace($Filter)) {
        throw "TestAutomation: Filter is required unless -SelfTest is used."
    }
}

function Get-FileSafeStamp {
    return [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':', '')
}

function New-SelfTestEntry([string]$Name, [object]$State, [bool]$IncludeState = $true) {
    $Object = [ordered]@{
        TestDisplayName = $Name
        FullTestPath = $Name
        Tags = @()
        DeviceInstance = @('Win64')
        Duration = 0.1
    }
    if ($IncludeState) { $Object.State = $State }
    return [pscustomobject]$Object
}

function New-SelfTestReport([object[]]$Tests, [object]$CreatedOn, [bool]$IncludeDate = $true) {
    $Object = [ordered]@{
        Devices = @()
        Succeeded = 0
        SucceededWithWarnings = 0
        Failed = 0
        NotRun = 0
        InProcess = 0
        TotalDuration = 0.1
        ComparisonExported = $false
        ComparisonExportDirectory = ''
        Tests = $Tests
        IsRequired = $false
    }
    if ($IncludeDate) { $Object.ReportCreatedOn = $CreatedOn }
    return ($Object | ConvertTo-Json -Depth 6)
}

if ($SelfTest) {
    $Harness = 'UEMMO.Tasks.M1_002.Harness'
    $Other = 'UEMMO.Tasks.Other.Harness'
    $FreshIso = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fff'Z'")
    $StaleIso = [DateTime]::UtcNow.AddHours(-2).ToString("yyyy-MM-ddTHH:mm:ss.fff'Z'")
    $FutureIso = [DateTime]::UtcNow.AddMinutes(90).ToString("yyyy-MM-ddTHH:mm:ss.fff'Z'")
    $NowUtc = [DateTime]::UtcNow
    $TwoEntries = @(New-SelfTestEntry $Harness 'Success') + @(New-SelfTestEntry $Other 'Success')
    $CamelFresh = '{"devices":[],"reportCreatedOn":"' + [DateTime]::UtcNow.AddMinutes(-1).ToString('yyyy.MM.dd-HH.mm.ss') + '","succeeded":1,"failed":0,"notRun":0,"inProcess":0,"tests":[{"testDisplayName":"Harness","fullTestPath":"UEMMO.Tasks.M1_002.Harness","state":"Success","duration":0.1}]}'
    $CamelStale = '{"devices":[],"reportCreatedOn":"' + [DateTime]::UtcNow.AddHours(-2).ToString('yyyy.MM.dd-HH.mm.ss') + '","succeeded":1,"failed":0,"notRun":0,"inProcess":0,"tests":[{"testDisplayName":"Harness","fullTestPath":"UEMMO.Tasks.M1_002.Harness","state":"Success","duration":0.1}]}'
    $Cases = @(
        @{ Name = 'good_single_success'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Success') $FreshIso; ExpectOk = $true; ExpectKind = ''; ExpectMatched = 1 },
        @{ Name = 'good_counts_only_matching'; Json = New-SelfTestReport $TwoEntries $FreshIso; ExpectOk = $true; ExpectKind = ''; ExpectMatched = 1 },
        @{ Name = 'good_numeric_state'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 3) $FreshIso; ExpectOk = $true; ExpectKind = ''; ExpectMatched = 1 },
        @{ Name = 'good_camelcase_ue_date'; Json = $CamelFresh; ExpectOk = $true; ExpectKind = ''; ExpectMatched = 1 },
        @{ Name = 'reject_zero_tests_empty'; Json = New-SelfTestReport @() $FreshIso; ExpectOk = $false; ExpectKind = 'zero_tests' },
        @{ Name = 'reject_zero_tests_no_match'; Json = New-SelfTestReport (New-SelfTestEntry $Other 'Success') $FreshIso; ExpectOk = $false; ExpectKind = 'zero_tests' },
        @{ Name = 'reject_fail'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Fail') $FreshIso; ExpectOk = $false; ExpectKind = 'failures' },
        @{ Name = 'reject_inprocess'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'InProcess') $FreshIso; ExpectOk = $false; ExpectKind = 'incomplete' },
        @{ Name = 'reject_notrun'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'NotRun') $FreshIso; ExpectOk = $false; ExpectKind = 'incomplete' },
        @{ Name = 'reject_skipped'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Skipped') $FreshIso; ExpectOk = $false; ExpectKind = 'incomplete' },
        @{ Name = 'reject_stale'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Success') $StaleIso; ExpectOk = $false; ExpectKind = 'stale' },
        @{ Name = 'reject_stale_ue_date'; Json = $CamelStale; ExpectOk = $false; ExpectKind = 'stale' },
        @{ Name = 'reject_future_date'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Success') $FutureIso; ExpectOk = $false; ExpectKind = 'stale' },
        @{ Name = 'reject_broken_json'; Json = '{ "Tests": [ not json'; ExpectOk = $false; ExpectKind = 'structure' },
        @{ Name = 'reject_missing_tests'; Json = '{ "Succeeded": 1 }'; ExpectOk = $false; ExpectKind = 'structure' },
        @{ Name = 'reject_missing_state'; Json = New-SelfTestReport (New-SelfTestEntry $Harness $null $false) $FreshIso; ExpectOk = $false; ExpectKind = 'structure' },
        @{ Name = 'reject_missing_date'; Json = New-SelfTestReport (New-SelfTestEntry $Harness 'Success') $null $false; ExpectOk = $false; ExpectKind = 'structure' }
    )
    $Failures = 0
    foreach ($Case in $Cases) {
        $Parsed = ConvertFrom-AutomationReportJson -ReportJson $Case.Json -Filter 'UEMMO.Tasks.M1_002' -NowUtc $NowUtc
        $Problems = @()
        if ($Parsed.Ok -ne $Case.ExpectOk) { $Problems += ("Ok=" + $Parsed.Ok + " expected " + $Case.ExpectOk) }
        if ($Parsed.ErrorKind -ne $Case.ExpectKind) { $Problems += ("ErrorKind=" + $Parsed.ErrorKind + " expected " + $Case.ExpectKind) }
        if (-not $Case.ExpectOk -and [string]::IsNullOrEmpty($Parsed.Error)) { $Problems += 'missing concrete error message' }
        if ($Case.ContainsKey('ExpectMatched') -and $Parsed.Matched -ne $Case.ExpectMatched) { $Problems += ("Matched=" + $Parsed.Matched + " expected " + $Case.ExpectMatched) }
        if ($Problems.Count -gt 0) {
            $Failures++
            Write-Host ("FAIL " + $Case.Name + ": " + ($Problems -join '; ') + " (actual Error: " + $Parsed.Error + ")")
        }
        else {
            Write-Host ("PASS " + $Case.Name)
        }
    }
    if ($Failures -gt 0) { throw "Parser self-test failed: $Failures case(s) did not match expectations." }
    Write-Host "SelfTest: all parser fixture cases passed."
    exit 0
}

$OutputRoot = Join-Path $ArtifactRoot (Join-Path 'Tasks' $TaskId)
$OutputDir = Join-Path $OutputRoot (Get-FileSafeStamp)
if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$ProcessName = 'automation-' + ($TaskId -replace '[^A-Za-z0-9._-]', '_')
$LogPath = Join-Path $ArtifactRoot ("Logs\" + $ProcessName + ".log")
$Arguments = @(
    $ProjectFile,
    '-game',
    '-unattended',
    '-nosplash',
    '-nosound',
    '-nullrhi',
    ("-ExecCmds=Automation RunTests " + $Filter),
    "-TestExit=Automation Test Queue Empty",
    ("-ReportExportPath=" + $OutputDir),
    ("-abslog=" + $LogPath)
)

$Summary = [ordered]@{
    success = $false
    task_id = $TaskId
    filter = $Filter
    output_dir = $OutputDir
    report_path = ''
    zero_tests = $false
    matched = 0
    passed = 0
    failed = 0
    incomplete = 0
    error_kind = ''
    error = ''
}

$ProcessOk = $true
$ProcessError = ''
try { Invoke-UEProcess $Editor $Arguments $ProcessName $TimeoutSeconds }
catch { $ProcessOk = $false; $ProcessError = $_.Exception.Message }

foreach ($Suffix in @('-stdout.log', '-stderr.log')) {
    $SideLog = Join-Path $ArtifactRoot ("Logs\" + $ProcessName + $Suffix)
    if (Test-Path -LiteralPath $SideLog) { Copy-Item -LiteralPath $SideLog -Destination (Join-Path $OutputDir ([IO.Path]::GetFileName($SideLog))) -Force }
}
if (Test-Path -LiteralPath $LogPath) { Copy-Item -LiteralPath $LogPath -Destination (Join-Path $OutputDir 'ue-automation.log') -Force }

if (-not $ProcessOk) {
    $Summary.error_kind = 'process'
    $Summary.error = $ProcessError
}
else {
    $ReportPath = Join-Path $OutputDir 'index.json'
    if (-not (Test-Path -LiteralPath $ReportPath)) {
        # UE writes no index.json when zero tests matched; classify via the queue-empty log line.
        $ZeroMatched = $false
        if (Test-Path -LiteralPath $LogPath) {
            $LogText = Get-Content -LiteralPath $LogPath -Raw
            $QueueMatch = [regex]::Match($LogText, 'Automation Test Queue Empty\s+(\d+) tests performed')
            if ($QueueMatch.Success -and [int]$QueueMatch.Groups[1].Value -eq 0) { $ZeroMatched = $true }
        }
        if ($ZeroMatched) {
            $Summary.error_kind = 'zero_tests'
            $Summary.zero_tests = $true
            $Summary.error = ("UE matched 0 automation tests for filter '" + $Filter + "'; no report is exported for an empty queue.")
        }
        else {
            $Summary.error_kind = 'no_report'
            $Summary.error = 'UE process exited but did not write the index.json export report.'
        }
    }
    else {
        $Summary.report_path = $ReportPath
        $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $ReportPath -Raw) -Filter $Filter
        $Summary.matched = $Parsed.Matched
        $Summary.passed = $Parsed.Passed
        $Summary.failed = $Parsed.Failed
        $Summary.incomplete = $Parsed.Incomplete
        $Summary.error_kind = $Parsed.ErrorKind
        $Summary.error = $Parsed.Error
        $Summary.zero_tests = ($Parsed.ErrorKind -eq 'zero_tests')
        $Summary.success = $Parsed.Ok
    }
}

$SummaryJson = $Summary | ConvertTo-Json -Depth 4
Set-Content -LiteralPath (Join-Path $OutputDir 'automation-summary.json') -Value $SummaryJson -Encoding UTF8
$SummaryJson
if (-not $Summary.success) { exit 1 }
exit 0
