. "$PSScriptRoot\Common.ps1"
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$Instances = if(Test-Path -LiteralPath $VsWhere) { @(& $VsWhere -all -products '*' -format json | ConvertFrom-Json) } else { @() }
$Toolchains = @($Instances | ForEach-Object {
    $Directory = Join-Path $_.installationPath 'VC\Tools\MSVC'
    if(Test-Path -LiteralPath $Directory) {
        Get-ChildItem -LiteralPath $Directory -Directory | ForEach-Object {
            $Compiler = Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe'
            if(Test-Path -LiteralPath $Compiler) { @{Folder=$_.FullName; FileVersion=(Get-Item -LiteralPath $Compiler).VersionInfo.FileVersion} }
        }
    }
})
$Report = @{
    CheckedAt=(Get-Date).ToString('o'); EngineRoot=$EngineRoot
    EngineVersion=(Get-Content -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') -Raw | ConvertFrom-Json)
    CompilerOverride=$CompilerVersion; Toolchains=$Toolchains
    ProjectExists=(Test-Path -LiteralPath $ProjectFile)
    EditorModuleExists=(Test-Path -LiteralPath (Join-Path $ProjectRoot 'Binaries\Win64\UnrealEditor-UEMMO.dll'))
    ArenaExists=(Test-Path -LiteralPath (Join-Path $ProjectRoot 'Content\UEMMO\Maps\L_PrototypeArena.umap'))
}
$Report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $ArtifactRoot 'environment.json') -Encoding UTF8
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') '-Mode=ValidatePlatforms' '-Platforms=Win64' '-OutputSDKs' "-Log=$ArtifactRoot\Logs\platform-check.log"
Assert-NativeSuccess 'Platform validation'
if(-not (Select-String -LiteralPath (Join-Path $ArtifactRoot 'Logs\platform-check.log') -SimpleMatch 'Win64 VALID' -Quiet)) { throw 'Win64 SDK is not valid.' }
$Report | ConvertTo-Json -Depth 6
