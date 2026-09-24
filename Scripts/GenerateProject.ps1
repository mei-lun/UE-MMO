. "$PSScriptRoot\Common.ps1"
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat') '-projectfiles' "-project=$ProjectFile" '-game' '-engine' '-2022'
Assert-NativeSuccess 'Project file generation'
