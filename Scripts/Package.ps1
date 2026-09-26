. "$PSScriptRoot\Common.ps1"
$Arguments = @('BuildCookRun', "-project=$ProjectFile", '-noP4', '-platform=Win64', '-clientconfig=Development', '-build', '-cook', '-stage', '-pak', '-iostore', '-archive', "-archivedirectory=$ArtifactRoot\Package", '-unattended', '-utf8output')
# M1-039: attack montages are resolved at runtime by naming convention
# (/Game/UEMMO/Animation/Montages/MNT_<AttackId>) and nothing references them
# statically, so the unreferenced montage assets must be cooked explicitly.
# -cookall cooks every project content asset without touching
# ProjectPackagingSettings; the M0 cook result stays a subset of this output.
$Arguments += '-cookall'
if($CompilerVersion) { $Arguments += "-UbtArgs=-CompilerVersion=$CompilerVersion" }
& (Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat') @Arguments
Assert-NativeSuccess 'Packaging'
