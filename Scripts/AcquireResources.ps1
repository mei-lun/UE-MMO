. "$PSScriptRoot\Common.ps1"
$Downloads = Join-Path $ProjectRoot 'SourceAssets\Downloads'
$Unpacked = Join-Path $ProjectRoot 'SourceAssets\Unpacked\KenneyImpact'
New-Item -ItemType Directory -Path $Downloads -Force | Out-Null
$Zip = Join-Path $Downloads 'kenney_impact-sounds.zip'
$Expected = '029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8'
if(-not (Test-Path -LiteralPath $Zip)) {
    $Partial = "$Zip.partial"
    try {
        & curl.exe '--fail' '-L' '--connect-timeout' '10' '--max-time' '120' 'https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip' '-o' $Partial
        Assert-NativeSuccess 'Sound archive download'
        $InputStream = [IO.File]::OpenRead($Partial)
        try { $PartialHash=[BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($InputStream)).Replace('-','').ToLowerInvariant() } finally { $InputStream.Dispose() }
        if($PartialHash -ne $Expected) { throw 'Downloaded archive hash differs from the recorded publisher archive.' }
        Move-Item -LiteralPath $Partial -Destination $Zip
    } finally {
        if(Test-Path -LiteralPath $Partial) { Remove-Item -LiteralPath $Partial }
    }
}
$Stream = [IO.File]::OpenRead($Zip)
try { $Hash=[BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($Stream)).Replace('-','').ToLowerInvariant() } finally { $Stream.Dispose() }
if($Hash -ne $Expected) { throw "Archive hash changed: $Hash. Inspect publisher update before using it." }
$RequiredSources = @('License.txt','Audio\impactPunch_medium_000.ogg','Audio\impactPunch_heavy_000.ogg','Audio\footstep_concrete_000.ogg','Audio\impactSoft_heavy_000.ogg')
if(@($RequiredSources | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Unpacked $_)) }).Count -gt 0) {
    Expand-Archive -LiteralPath $Zip -DestinationPath $Unpacked -Force
}
if(@($RequiredSources | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Unpacked $_)) }).Count -gt 0) { throw 'Archive extraction is missing required sources.' }
New-Item -ItemType Directory -Path (Join-Path $ProjectRoot 'SourceAssets\Licenses') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Unpacked 'License.txt') -Destination (Join-Path $ProjectRoot 'SourceAssets\Licenses\KenneyImpact-CC0.txt') -Force
Write-Output 'Kenney source archive hash verified and source files ready.'
