param([ValidateSet('Editor','Game')] [string]$Target='Editor')
. "$PSScriptRoot\Common.ps1"
$TargetName = if($Target -eq 'Editor') { 'UEMMOEditor' } else { 'UEMMO' }
$BuildArgs = @($TargetName, 'Win64', 'Development', "-Project=$ProjectFile", '-NoHotReloadFromIDE', '-WaitMutex', '-MaxParallelActions=4', '-NoUBA', "-Log=$ArtifactRoot\Logs\build-$Target.log")
if ($CompilerVersion) { $BuildArgs += "-CompilerVersion=$CompilerVersion" }
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') @BuildArgs
Assert-NativeSuccess 'UnrealBuildTool'
