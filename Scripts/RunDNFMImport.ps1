param([ValidateSet('import','verify','gallery','capture')][string]$Mode='import',[int]$Limit=0,[switch]$Render)
. "$PSScriptRoot\Common.ps1"
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$Evidence = Join-Path $ProjectRoot 'Artifacts\Tasks\M6-009'
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$env:UEMMO_DNFM_MODE=$Mode
$env:UEMMO_DNFM_LIMIT=[string]$Limit
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
    if ($Mode -eq 'capture') { $Arguments += @("-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_static.py",'-RenderOffscreen') }
    else { $Arguments += "-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_static.py"; if (-not $Render) { $Arguments += '-nullrhi' } }
    Invoke-UEProcess $Editor $Arguments "dnfm-$Mode" 2400
    if (-not (Test-Path $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    if (-not $Result.success -or $Result.run_id -ne $RunId) { throw "DNFM $Mode failed or stale; inspect $Report and UE log." }
    @{success=$Result.success; mode=$Mode; mesh_count=$Result.meshes.Count; report=$Report} | ConvertTo-Json
} finally {
    Remove-Item Env:UEMMO_DNFM_MODE,Env:UEMMO_DNFM_LIMIT,Env:UEMMO_DNFM_RUN_ID -ErrorAction SilentlyContinue
}
