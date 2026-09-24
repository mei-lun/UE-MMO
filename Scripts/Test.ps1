param([switch]$Render)
. "$PSScriptRoot\Common.ps1"
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Report = Join-Path $ArtifactRoot 'runtime-smoke.json'
$Screenshot = Join-Path $ArtifactRoot 'prototype-smoke.png'
if(Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }
if($Render -and (Test-Path -LiteralPath $Screenshot)) { Remove-Item -LiteralPath $Screenshot }
$Arguments = @($ProjectFile, '-game', '-UEMMOSmoke', '-unattended', '-nosplash', '-nosound', '-NoVSync', '-ResX=1280','-ResY=720', "-abslog=$ArtifactRoot\Logs\runtime-smoke.log")
if($Render) { $Arguments += '-RenderOffscreen' } else { $Arguments += '-nullrhi' }
Invoke-UEProcess $Editor $Arguments 'runtime-smoke' 600
if(-not (Test-Path -LiteralPath $Report)) { throw 'Runtime smoke test did not produce a report.' }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if(-not $Result.success) { throw 'Movement / depth / jump smoke test failed.' }
if($Render) {
    if(-not $Result.rendering) { throw 'Rendered test reported rendering=false.' }
    if(-not (Test-Path -LiteralPath $Screenshot)) { throw 'Rendered test did not produce a fresh screenshot.' }
    $Bytes = [IO.File]::ReadAllBytes($Screenshot)
    if($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Screenshot is empty or not a PNG.' }
}
$ModeReport = if($Render) { 'runtime-smoke-render.json' } else { 'runtime-smoke-null.json' }
Copy-Item -LiteralPath $Report -Destination (Join-Path $ArtifactRoot $ModeReport) -Force
$Result | ConvertTo-Json
