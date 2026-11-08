. "$PSScriptRoot\Common.ps1"
# M8-024: render the guild map through the fixed side-view capture (visual check).
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running.' }
$TaskId = 'M8-024'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
Invoke-UEProcess $Editor @(
    $ProjectFile,'-NoSourceControl','-RenderOffscreen',
    "-abslog=$Evidence\guild-render-wrapper.log",
    "-ExecutePythonScript=$PSScriptRoot\Editor\render_guild_probe.py"
) 'guild-render-probe' 600
$Out = Join-Path $Evidence 'guild-materials-done.png'
if (-not (Test-Path -LiteralPath $Out)) { throw "Missing render output $Out" }
$Bytes = [IO.File]::ReadAllBytes($Out)
if ($Bytes.Length -lt 10240) { throw "Render output too small: $($Bytes.Length)" }
@{success=$true; output=$Out; bytes=$Bytes.Length} | ConvertTo-Json
