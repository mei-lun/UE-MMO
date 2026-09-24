. "$PSScriptRoot\Common.ps1"
$Arguments = @('BuildCookRun', "-project=$ProjectFile", '-noP4', '-platform=Win64', '-clientconfig=Development', '-build', '-cook', '-stage', '-pak', '-iostore', '-archive', "-archivedirectory=$ArtifactRoot\Package", '-unattended', '-utf8output')
if($CompilerVersion) { $Arguments += "-UbtArgs=-CompilerVersion=$CompilerVersion" }
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat') @Arguments
Assert-NativeSuccess 'Packaging'
