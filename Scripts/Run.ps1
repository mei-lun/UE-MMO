param([switch]$Editor)
. "$PSScriptRoot\Common.ps1"
$Arguments = @('"' + $ProjectFile + '"')
if (-not $Editor) { $Arguments += @('-game','-windowed','-ResX=1280','-ResY=720','-nosplash') }
Start-Process -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe') -ArgumentList $Arguments -WorkingDirectory $ProjectRoot
