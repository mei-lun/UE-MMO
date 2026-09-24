. "$PSScriptRoot\Common.ps1"
$Source = Join-Path $EngineRoot 'Templates\TemplateResources\High\Characters\Content\Mannequins'
$Destination = Join-Path $ProjectRoot 'Content\Characters\Mannequins'
if (-not (Test-Path -LiteralPath $Destination)) {
    if (-not (Test-Path -LiteralPath $Source)) { throw 'Install Unreal Engine templates and feature packs before preparing assets.' }
    New-Item -ItemType Directory -Path (Split-Path $Destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse
}
if(Test-Path -LiteralPath $Source) {
    foreach($File in Get-ChildItem -LiteralPath $Source -Recurse -File) {
        $Relative = $File.FullName.Substring($Source.Length).TrimStart('\')
        $TargetFile = Join-Path $Destination $Relative
        if(-not (Test-Path -LiteralPath $TargetFile)) {
            New-Item -ItemType Directory -Path (Split-Path $TargetFile) -Force | Out-Null
            Copy-Item -LiteralPath $File.FullName -Destination $TargetFile
        }
    }
}
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\create_foundation.py'
$Report = Join-Path $ArtifactRoot 'foundation-report.json'
if(Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }
$Arguments = @($ProjectFile, '-run=pythonscript', "-script=$Script", '-unattended', '-nosplash', '-nosound', '-nullrhi', "-abslog=$ArtifactRoot\Logs\prepare-content.log")
Invoke-UEProcess $Editor $Arguments 'prepare-content' 600
if(-not (Test-Path -LiteralPath $Report)) { throw 'UE did not write foundation-report.json.' }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if(-not $Result.success) { throw 'Foundation resource verification failed.' }
Write-Output 'Foundation resources verified and arena saved.'
