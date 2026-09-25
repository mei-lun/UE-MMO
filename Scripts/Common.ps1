$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ProjectFile = Join-Path $ProjectRoot 'UEMMO.uproject'
$EngineRoot = if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { 'D:\Epic\UE_5.8' }
$LocalSettings = Join-Path $PSScriptRoot 'local.settings.json'
$CompilerVersion = ''
if (Test-Path -LiteralPath $LocalSettings) {
    $Settings = Get-Content -LiteralPath $LocalSettings -Raw | ConvertFrom-Json
    if ($Settings.EngineRoot) { $EngineRoot = $Settings.EngineRoot }
    if ($Settings.CompilerVersion) { $CompilerVersion = $Settings.CompilerVersion }
}
if (-not (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'))) {
    throw "Unreal Engine not found at $EngineRoot. Set UE_ENGINE_ROOT or Scripts/local.settings.json."
}
$ArtifactRoot = Join-Path $ProjectRoot 'Artifacts'
New-Item -ItemType Directory -Path (Join-Path $ArtifactRoot 'Logs') -Force | Out-Null
function Assert-NativeSuccess([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE. Inspect Artifacts/Logs." }
}
function ConvertFrom-AutomationReportJson {
    # Parses UE5.8 -ReportExportPath index.json (FAutomatedTestPassResults).
    # State exports as the enum name string; ReportCreatedOn exports as an ISO-8601 string.
    param(
        [Parameter(Mandatory=$true)][string]$ReportJson,
        [Parameter(Mandatory=$true)][string]$Filter,
        [object]$NowUtc = $null,
        [int]$MaxAgeMinutes = 15
    )
    $Result = [pscustomobject]@{
        Ok = $false
        ErrorKind = ''   # '' | structure | zero_tests | failures | incomplete | stale
        Error = ''
        Matched = 0
        Passed = 0
        Failed = 0
        Incomplete = 0
        TestNames = @()
        ReportAgeMinutes = $null
    }
    function Set-ReportError([string]$Kind, [string]$Message) {
        $Result.Ok = $false
        $Result.ErrorKind = $Kind
        $Result.Error = $Message
    }
    if ([string]::IsNullOrWhiteSpace($ReportJson)) {
        Set-ReportError 'structure' 'Report content is empty.'
        return $Result
    }
    try { $Report = $ReportJson | ConvertFrom-Json }
    catch {
        Set-ReportError 'structure' ("Report is not valid JSON: " + $_.Exception.Message)
        return $Result
    }
    if ($null -eq $Report -or $null -eq $Report.Tests) {
        Set-ReportError 'structure' 'Report is missing the Tests array.'
        return $Result
    }
    $ValidStates = @('NotRun','InProcess','Fail','Success','Skipped')
    $MatchedNames = @()
    $FailedNames = @()
    $IncompleteNames = @()
    foreach ($Test in @($Report.Tests)) {
        if ($null -eq $Test) {
            Set-ReportError 'structure' 'Tests array contains a null element.'
            return $Result
        }
        $Path = ''
        if ($Test.PSObject.Properties['FullTestPath'] -and $Test.FullTestPath) { $Path = [string]$Test.FullTestPath }
        elseif ($Test.PSObject.Properties['TestDisplayName'] -and $Test.TestDisplayName) { $Path = [string]$Test.TestDisplayName }
        else {
            Set-ReportError 'structure' 'Test result is missing both FullTestPath and TestDisplayName.'
            return $Result
        }
        $StateName = ''
        if ($Test.PSObject.Properties['State'] -and $null -ne $Test.State) {
            $RawState = $Test.State
            if ($RawState -is [string]) {
                $StateName = $RawState.Trim()
            }
            else {
                switch ("" + $RawState) {
                    '0' { $StateName = 'NotRun' }
                    '1' { $StateName = 'InProcess' }
                    '2' { $StateName = 'Fail' }
                    '3' { $StateName = 'Success' }
                    '4' { $StateName = 'Skipped' }
                    default { $StateName = '' }
                }
            }
        }
        if ($ValidStates -notcontains $StateName) {
            Set-ReportError 'structure' ("Test '" + $Path + "' has no recognizable State field.")
            return $Result
        }
        if ($Path -eq $Filter -or $Path -like ($Filter + '.*')) {
            switch ($StateName) {
                'Success' { $Result.Passed++ }
                'Fail'    { $Result.Failed++ }
                default   { $Result.Incomplete++ }
            }
            $MatchedNames += $Path
            if ($StateName -eq 'Fail') { $FailedNames += $Path }
            elseif ($StateName -ne 'Success') { $IncompleteNames += ($Path + ' [' + $StateName + ']') }
        }
    }
    $Result.TestNames = $MatchedNames
    $Result.Matched = $Result.Passed + $Result.Failed + $Result.Incomplete
    if ($Result.Matched -eq 0) {
        Set-ReportError 'zero_tests' ("No tests match Filter '" + $Filter + "' (parsed " + @($Report.Tests).Count + " results).")
        return $Result
    }
    if ($Result.Failed -gt 0) {
        Set-ReportError 'failures' ($Result.Failed.ToString() + " matched test(s) failed: " + ($FailedNames -join ', '))
        return $Result
    }
    if ($Result.Incomplete -gt 0) {
        Set-ReportError 'incomplete' ("Incomplete or skipped tests: " + ($IncompleteNames -join ', '))
        return $Result
    }
    $DateProp = $Report.PSObject.Properties['ReportCreatedOn']
    if (-not $DateProp -or $null -eq $DateProp.Value) {
        Set-ReportError 'structure' 'Report is missing ReportCreatedOn.'
        return $Result
    }
    $CreatedUtc = $null
    $RawDate = $DateProp.Value
    try {
        if ($RawDate -is [string]) {
            # UE 5.8 exports FDateTime as 'yyyy.MM.dd-HH.mm.ss' (UTC, no zone suffix); accept ISO-8601 too.
            $Parsed = $null
            try { $Parsed = [DateTime]::ParseExact($RawDate.Trim(), 'yyyy.MM.dd-HH.mm.ss', [Globalization.CultureInfo]::InvariantCulture) } catch { $Parsed = $null }
            if ($null -eq $Parsed) {
                $Parsed = [DateTime]::Parse($RawDate, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)
            }
            if ($Parsed.Kind -eq [DateTimeKind]::Unspecified) { $Parsed = [DateTime]::SpecifyKind($Parsed, [DateTimeKind]::Utc) }
            $CreatedUtc = $Parsed.ToUniversalTime()
        }
        else {
            $TicksProp = $RawDate.PSObject.Properties['Ticks']
            if ($TicksProp) { $CreatedUtc = [DateTime]::new([int64]$TicksProp.Value, [DateTimeKind]::Utc).ToUniversalTime() }
        }
    }
    catch { $CreatedUtc = $null }
    if ($null -eq $CreatedUtc) {
        Set-ReportError 'structure' 'ReportCreatedOn is not a parseable date.'
        return $Result
    }
    $Now = if ($null -ne $NowUtc) { [DateTime]$NowUtc } else { [DateTime]::UtcNow }
    if ($Now.Kind -ne [DateTimeKind]::Utc) { $Now = $Now.ToUniversalTime() }
    $AgeMinutes = ($Now - $CreatedUtc).TotalMinutes
    $Result.ReportAgeMinutes = [math]::Round($AgeMinutes, 2)
    if ($AgeMinutes -gt $MaxAgeMinutes) {
        Set-ReportError 'stale' ("Report was created " + [math]::Round($AgeMinutes, 1) + " minutes ago, exceeding the " + $MaxAgeMinutes + " minute limit.")
        return $Result
    }
    if ($AgeMinutes -lt -5) {
        Set-ReportError 'stale' ("ReportCreatedOn is in the future by " + [math]::Round(-$AgeMinutes, 1) + " minutes; stale directory reuse or clock skew.")
        return $Result
    }
    $Result.Ok = $true
    return $Result
}

function Invoke-UEProcess([string]$Executable, [string[]]$Arguments, [string]$Name, [int]$TimeoutSeconds=600) {
    $Quoted = @($Arguments | ForEach-Object {
        if($_.Contains('"')) { throw 'Embedded quote is not allowed in UE process arguments.' }
        '"' + $_ + '"'
    })
    $Info = New-Object System.Diagnostics.ProcessStartInfo
    $Info.FileName = $Executable
    $Info.Arguments = $Quoted -join ' '
    $Info.WorkingDirectory = $ProjectRoot
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $Info
    if(-not $Process.Start()) { throw "Could not start $Name." }
    $OutputTask = $Process.StandardOutput.ReadToEndAsync()
    $ErrorTask = $Process.StandardError.ReadToEndAsync()
    if(-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stdout.log"), $OutputTask.Result)
        [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stderr.log"), $ErrorTask.Result)
        $Process.Dispose()
        throw "$Name timed out after $TimeoutSeconds seconds; only the process started by this invocation was stopped."
    }
    $Process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stdout.log"), $OutputTask.Result)
    [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stderr.log"), $ErrorTask.Result)
    $ExitCode = $Process.ExitCode
    $Process.Dispose()
    if($ExitCode -ne 0) { throw "$Name failed with exit code $ExitCode. Inspect Artifacts/Logs." }
}
