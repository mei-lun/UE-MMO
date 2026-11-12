# M8-043: single-frame side-view render probe (offscreen cmd editor, self-quits).
# Usage: Scripts/RenderMapProbe.ps1 -Map /Game/... -Out Artifacts/.../shot.png -Cam "x,y,z" [-Rot "-15,-90,0"] [-Fov 55]
param(
    [Parameter(Mandatory=$true)][string]$Map,
    [Parameter(Mandatory=$true)][string]$Out,
    [Parameter(Mandatory=$true)][string]$Cam,
    [string]$Rot = '-15,-90,0',
    [double]$Fov = 55
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running.' }

if (-not $Map.StartsWith('/')) { $Map = '/' + $Map }   # tolerate MSYS path mangling
$env:UEMMO_PROBE_MAP = $Map
$env:UEMMO_PROBE_OUT = (Join-Path $ProjectRoot $Out)
$env:UEMMO_PROBE_CAM = $Cam
$env:UEMMO_PROBE_ROT = $Rot
$env:UEMMO_PROBE_FOV = "$Fov"

$outFull = Join-Path $ProjectRoot $Out
New-Item -ItemType Directory -Force -Path (Split-Path $outFull) | Out-Null
$log = [IO.Path]::ChangeExtension($outFull, '.log')

$probe = Join-Path $PSScriptRoot 'Editor/render_map_probe.py'
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
Invoke-UEProcess $Editor @(
    $ProjectFile, '-NoSourceControl', '-RenderOffscreen',
    "-abslog=$log",
    "-ExecutePythonScript=$probe"
) 'map-render-probe' 600

if (-not (Test-Path -LiteralPath $outFull)) { throw "Missing render output $outFull" }
$Bytes = [IO.File]::ReadAllBytes($outFull)
if ($Bytes.Length -lt 10240) { throw "Render output too small: $($Bytes.Length)" }
Write-Output "RENDERMAPPROBE: saved $Out ($($Bytes.Length) bytes)"
