param([switch]$Editor, [switch]$Menu)
. "$PSScriptRoot\Common.ps1"
if ($Editor) {
    Start-Process -FilePath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe') -ArgumentList @('"' + $ProjectFile + '"') -WorkingDirectory $ProjectRoot
} else {
    if (-not (Test-Path -LiteralPath $DevelopmentPackageExecutable)) { throw 'Development package missing. Run Scripts/Package.ps1 first.' }
    $Arguments = @('-windowed','-ResX=1280','-ResY=720','-nosplash')
    if ($Menu) { $Arguments += '-UEMMOExperienceMenu' }
    Start-Process -FilePath $DevelopmentPackageExecutable -ArgumentList $Arguments -WorkingDirectory $DevelopmentPackageDirectory
}
