param([ValidateSet('import','verify')][string]$Mode='import')
. "$PSScriptRoot\Common.ps1"
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = 'M6-014'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$env:UEMMO_SPINE_FIGHTER_MODE = $Mode
$RunId = [Guid]::NewGuid().ToString('N')
$env:UEMMO_SPINE_FIGHTER_RUN_ID = $RunId
$Report = Join-Path $Evidence "ue-$Mode.json"
if (Test-Path $Report) {
    $History = Join-Path $Evidence 'history'
    New-Item -ItemType Directory -Path $History -Force | Out-Null
    Move-Item -LiteralPath $Report -Destination (Join-Path $History "ue-$Mode-$RunId.json")
}
try {
    $Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $Arguments = @($ProjectFile,'-unattended','-nosplash','-nosound','-NoSourceControl','-ScriptErrorsAreFatal',"-abslog=$Evidence\ue-$Mode.log","-ExecutePythonScript=$PSScriptRoot\Editor\import_spine_fighter.py")
    if ($Mode -ne 'verify') { $Arguments += '-nullrhi' }
    Invoke-UEProcess $Editor $Arguments "spine-fighter-$Mode" 1800
    if (-not (Test-Path $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    # Stale-report guard: the JSON must carry this launch's run_id.
    if (-not $Result.success -or $Result.run_id -ne $RunId) { throw "Spine fighter $Mode failed or stale; inspect $Report and the UE log." }
    @{success=$true; mode=$Mode; run_id=$RunId; report=$Report} | ConvertTo-Json
} finally {
    Remove-Item Env:UEMMO_SPINE_FIGHTER_MODE,Env:UEMMO_SPINE_FIGHTER_RUN_ID -ErrorAction SilentlyContinue
}
