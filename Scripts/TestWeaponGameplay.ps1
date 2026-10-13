param(
    [string]$PackageRoot = '',
    [switch]$Editor,
    [string[]]$WeaponIds = @('weapon_hitscan_sample','weapon_burst_sample','weapon_pellet_sample','weapon_projectile_sample','weapon_parabolic_sample','weapon_homing_sample','weapon_explosive_sample','weapon_training_sword')
)
. "$PSScriptRoot\Common.ps1"
if (-not $Editor -and [string]::IsNullOrWhiteSpace($PackageRoot)) { $PackageRoot = Join-Path $ArtifactRoot 'Package' }
if ($WeaponIds.Count -lt 1 -or @($WeaponIds | Select-Object -Unique).Count -ne $WeaponIds.Count) { throw 'Provide a nonempty set of distinct weapon ids.' }
foreach ($WeaponId in $WeaponIds) { if ($WeaponId -notmatch '^weapon_[a-z_]+$') { throw "Invalid weapon id: $WeaponId" } }
$Executable = if ($Editor) { Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' } else { Join-Path $PackageRoot 'Windows\UEMMO\Binaries\Win64\UEMMO.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw 'The requested built executable is missing.' }
$Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-ss-fffzzz').Replace(':','')
$EvidenceRoot = Join-Path $ArtifactRoot "Tasks\M5-060\$Stamp-gameplay"
if (Test-Path -LiteralPath $EvidenceRoot) { throw 'Refusing to reuse gameplay evidence.' }
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$Filter = 'UEMMO.Acceptance.M5_060.RoomWeaponGameplay'
$Results = @()
$Summary = [ordered]@{ success=$false; executable=$Executable; filter=$Filter; editor=[bool]$Editor; evidence_root=$EvidenceRoot; cases=@(); error='' }
try {
    foreach ($WeaponId in $WeaponIds) {
        $CaseDir = Join-Path $EvidenceRoot $WeaponId
        New-Item -ItemType Directory -Path $CaseDir | Out-Null
        $StartedUtc = [DateTime]::UtcNow
        $GameArgs = @()
        if ($Editor) { $GameArgs += $ProjectFile }
        $GameArgs += @('-game','-unattended','-nosplash','-nosound','-RenderOffscreen','-ResX=1280','-ResY=720','-LogCmds=LogTemp Verbose',
            "-UserDir=$CaseDir\user-data", "-ExecCmds=Automation RunTests $Filter", '-TestExit=Automation Test Queue Empty',
            "-ReportExportPath=$CaseDir", "-UEMMOAcceptanceWeapon=$WeaponId", "-UEMMOAcceptanceOutput=$CaseDir", "-abslog=$CaseDir\ue.log")
        Invoke-UEProcess $Executable $GameArgs "gameplay-$WeaponId" 120
        $Index = Join-Path $CaseDir 'index.json'
        $Evidence = Join-Path $CaseDir 'gameplay.json'
        $Screenshot = Join-Path $CaseDir 'gameplay.png'
        foreach ($Path in @($Index,$Evidence,$Screenshot)) {
            if (-not (Test-Path -LiteralPath $Path) -or (Get-Item -LiteralPath $Path).LastWriteTimeUtc -lt $StartedUtc) { throw "Missing or stale gameplay evidence: $Path" }
        }
        $Parsed = ConvertFrom-AutomationReportJson -ReportJson (Get-Content -LiteralPath $Index -Raw) -Filter $Filter
        if (-not $Parsed.Ok -or $Parsed.Matched -ne 1) { throw "Gameplay automation failed: $($Parsed.Error)" }
        $Observed = Get-Content -LiteralPath $Evidence -Raw | ConvertFrom-Json
        if (-not $Observed.success -or -not $Observed.menu_travel_completed -or $Observed.weapon -ne $WeaponId -or $Observed.health_after -ge $Observed.health_before -or $Observed.run_id -le 0) {
            throw "Gameplay did not prove menu travel and this shot's damage: $WeaponId"
        }
        # EAmmoModelError::NoReserveAmmo is frozen as 13. State remaining
        # unchanged alone cannot prove that the real T callback ran.
        if ($Observed.reload_result -eq 'refused_no_reserve') {
            $ReloadLog = @(Select-String -LiteralPath (Join-Path $CaseDir 'ue.log') -SimpleMatch 'UEMMO M5-033: the T reload refused (13): BeginReload refused: the shared reserve pool is empty')
            if ($ReloadLog.Count -eq 0) { throw "The real T callback did not report NoReserveAmmo: $WeaponId" }
        }
        $Bytes = [IO.File]::ReadAllBytes($Screenshot)
        if ($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid current PNG: $Screenshot" }
        $Results += [ordered]@{ weapon=$WeaponId; success=$true; report=$Index; screenshot=$Screenshot; health_before=$Observed.health_before; health_after=$Observed.health_after; shots=$Observed.shots_committed; loaded_before=$Observed.loaded_before; loaded_after=$Observed.loaded_after_fire; reserve=$Observed.reserve_before_reload; reload=$Observed.reload_result }
        Write-Host ("PASS {0}: HP {1} -> {2}; shots {3}" -f $WeaponId,$Observed.health_before,$Observed.health_after,$Observed.shots_committed)
    }
    $Summary.success = $true
}
catch {
    $Summary.error = $_.Exception.Message
    throw
}
finally {
    $Summary.cases = $Results
    $Summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'gameplay-summary.json') -Encoding UTF8
    $Summary | ConvertTo-Json -Depth 6
}
