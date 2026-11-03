param([switch]$Verify)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Blender = 'D:\DOF\blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release\blender-5.2.4-candidate+v52.4f5026e99892-windows.amd64-release\blender.exe'
$Evidence = Join-Path $ProjectRoot 'Artifacts\Tasks\M6-009'
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Mode = if ($Verify) { 'verify' } else { 'export' }
$env:DNFM_BLENDER_VERIFY = if ($Verify) { '1' } else { '0' }
try {
    & $Blender --background --factory-startup --python-exit-code 1 --python "$PSScriptRoot\blender_dnfm_static.py" *> "$Evidence\blender-$Mode.log"
    if ($LASTEXITCODE -ne 0) { throw "Blender $Mode failed; inspect $Evidence\blender-$Mode.log" }
    $Result = Get-Content "$Evidence\blender-$Mode.json" -Raw | ConvertFrom-Json
    if (-not $Result.success) { throw 'Blender result is not successful.' }
    @{success=$true; mode=$Mode; meshes=$Result.meshes.Count} | ConvertTo-Json
} finally { Remove-Item Env:DNFM_BLENDER_VERIFY -ErrorAction SilentlyContinue }
