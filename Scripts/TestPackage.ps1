. "$PSScriptRoot\Common.ps1"
$PackageProject = Join-Path $ArtifactRoot 'Package\Windows\UEMMO'
$Executable = Join-Path $PackageProject 'Binaries\Win64\UEMMO.exe'
if(-not (Test-Path -LiteralPath $Executable)) { throw 'Package not found. Run Package.ps1 first.' }
$PackageArtifacts = Join-Path $PackageProject 'Artifacts'
foreach($Name in @('runtime-smoke.json','prototype-smoke.png')) {
    $File = Join-Path $PackageArtifacts $Name
    if(Test-Path -LiteralPath $File) { Remove-Item -LiteralPath $File }
}
Invoke-UEProcess $Executable @('-UEMMOSmoke','-unattended','-RenderOffscreen','-nosound','-ResX=1280','-ResY=720',"-abslog=$ArtifactRoot\Logs\package-smoke.log") 'package-smoke' 600
$Report = Join-Path $PackageArtifacts 'runtime-smoke.json'
$Screenshot = Join-Path $PackageArtifacts 'prototype-smoke.png'
if(-not (Test-Path -LiteralPath $Report)) { throw 'Packaged executable did not produce a runtime report.' }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if(-not $Result.success -or -not $Result.rendering) { throw 'Packaged runtime / rendering test failed.' }
if(-not (Test-Path -LiteralPath $Screenshot)) { throw 'Packaged test screenshot missing.' }
$Bytes = [IO.File]::ReadAllBytes($Screenshot)
if($Bytes.Length -lt 1024 -or [BitConverter]::ToString($Bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Packaged screenshot is not a valid PNG header.' }
Copy-Item -LiteralPath $Report -Destination (Join-Path $ArtifactRoot 'package-smoke.json') -Force
Copy-Item -LiteralPath $Screenshot -Destination (Join-Path $ArtifactRoot 'package-smoke.png') -Force
$Result | ConvertTo-Json
