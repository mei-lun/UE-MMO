param(
    [ValidateSet('swordman','gunner')][string]$Role,
    [ValidateSet('import','verify')][string]$Mode='import'
)
. "$PSScriptRoot\Common.ps1"
# M8-017: import/verify a DNFM character visual set (swordman/gunner).
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = 'M8-017'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$env:UEMMO_DNFM_VISUAL_ROLE = $Role
$env:UEMMO_DNFM_VISUAL_MODE = $Mode
$RunId = [Guid]::NewGuid().ToString('N')
$env:UEMMO_DNFM_VISUAL_RUN_ID = $RunId
$Report = Join-Path $Evidence "ue-$Mode-$Role.json"
try {
    $Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $Arguments = @($ProjectFile,'-unattended','-nosplash','-nosound','-NoSourceControl','-ScriptErrorsAreFatal',"-abslog=$Evidence\ue-$Mode-$Role.log","-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_character_visuals_v2.py")
    if ($Mode -ne 'verify') { $Arguments += '-nullrhi' }
    Invoke-UEProcess $Editor $Arguments "dnfm-visual-v2-$Role-$Mode" 1800
    if (-not (Test-Path $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    if (-not $Result.success -or $Result.run_id -ne $RunId) { throw "DNFM visual v2 $Role $Mode failed or stale; inspect $Report and the UE log." }
    @{success=$true; role=$Role; mode=$Mode; run_id=$RunId; report=$Report} | ConvertTo-Json
} finally {
    Remove-Item Env:UEMMO_DNFM_VISUAL_ROLE,Env:UEMMO_DNFM_VISUAL_MODE,Env:UEMMO_DNFM_VISUAL_RUN_ID -ErrorAction SilentlyContinue
}
