param([ValidateSet('import','verify','gallery','capture','guildagit','guildagit_verify','guildagit_capture','guildagit_wire')][string]$Mode='import',[int]$Limit=0,[switch]$Render,[string]$LayoutDir='')
. "$PSScriptRoot\Common.ps1"
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = if ($Mode -eq 'guildagit_wire') { 'M6-013' } elseif ($Mode -like 'guildagit*') { 'M6-012' } else { 'M6-009' }
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$env:UEMMO_DNFM_MODE=$Mode
$env:UEMMO_DNFM_LIMIT=[string]$Limit
if ($Mode -like 'guildagit*') {
    $env:UEMMO_DNFM_LAYOUT_DIR = if ($LayoutDir) { $LayoutDir } else { Join-Path $ProjectRoot 'Artifacts\Tasks\M6-011\2026-11-04T0215_0800\intermediate' }
}
$RunId=[Guid]::NewGuid().ToString('N')
$env:UEMMO_DNFM_RUN_ID=$RunId
$Report = Join-Path $Evidence "ue-$Mode.json"
if (Test-Path $Report) {
    $History = Join-Path $Evidence 'history'
    New-Item -ItemType Directory -Path $History -Force | Out-Null
    Move-Item -LiteralPath $Report -Destination (Join-Path $History "ue-$Mode-$RunId.json")
}
try {
    $Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    $Arguments = @($ProjectFile,'-unattended','-nosplash','-nosound','-NoSourceControl','-ScriptErrorsAreFatal',"-abslog=$Evidence\ue-$Mode.log")
    if ($Mode -like '*capture') { $Arguments += @("-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_static.py",'-RenderOffscreen') }
    else { $Arguments += "-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_static.py"; if (-not $Render) { $Arguments += '-nullrhi' } }
    $Timeout = if ($Mode -like 'guildagit*') { 3600 } else { 2400 }
    Invoke-UEProcess $Editor $Arguments "dnfm-$Mode" $Timeout
    if (-not (Test-Path $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    if (-not $Result.success -or $Result.run_id -ne $RunId) { throw "DNFM $Mode failed or stale; inspect $Report and UE log." }
    @{success=$Result.success; mode=$Mode; report=$Report} | ConvertTo-Json
} finally {
    Remove-Item Env:UEMMO_DNFM_MODE,Env:UEMMO_DNFM_LIMIT,Env:UEMMO_DNFM_RUN_ID,Env:UEMMO_DNFM_LAYOUT_DIR -ErrorAction SilentlyContinue
}
