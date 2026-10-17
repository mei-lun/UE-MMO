param([switch]$Editor)
. "$PSScriptRoot\Common.ps1"
if ($Editor) {
    Start-Process -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe') -ArgumentList @('"' + $ProjectFile + '"') -WorkingDirectory $ProjectRoot
} else {
    if (-not (Test-Path -LiteralPath $DevelopmentPackageExecutable)) { throw 'Development package missing. Run Scripts/Package.ps1 first.' }
    Start-Process -FilePath $DevelopmentPackageExecutable -ArgumentList @('-windowed','-ResX=1280','-ResY=720','-nosplash') -WorkingDirectory $DevelopmentPackageDirectory
}
