# M8-042: map placement audit gate (read-only audit + count assertions).
# Usage:
#   Scripts/CheckMapPlacement.ps1                        # record: audit all maps, print counts
#   Scripts/CheckMapPlacement.ps1 -Maps L_DNFM_Volcano01 # audit selected maps only
#   Scripts/CheckMapPlacement.ps1 -Gate                  # exit 1 when any audited map has floating/buried/placeholder > 0
#   Scripts/CheckMapPlacement.ps1 -Gate -SkipMaps L_DNFM_ResourceGallery
# Output: Artifacts/CheckMapPlacement/<timestamp>/ (detail_*.json, summary_*.json, _summary.json)

param(
    [string]$Maps = '',
    [string[]]$SkipMaps = @(),
    [switch]$Gate,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $OutDir) { $OutDir = "Artifacts/CheckMapPlacement/$stamp" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$env:UEMMO_AUDIT_OUT = (Join-Path (Get-Location) $OutDir)
if ($Maps) { $env:UEMMO_AUDIT_MAPS = $Maps } else { Remove-Item Env:UEMMO_AUDIT_MAPS -ErrorAction SilentlyContinue }

$scriptPath = Join-Path $PSScriptRoot 'Editor/audit_map_placement.py'
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $Editor $ProjectFile -run=pythonscript "-script=$scriptPath" `
    -unattended -nopause -nosplash -nosound -nullrhi -stdout *>&1 | Tee-Object -FilePath (Join-Path $OutDir 'run.log') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "audit commandlet exit=$LASTEXITCODE (see $OutDir/run.log)" }

$summary = Get-Content (Join-Path $OutDir '_summary.json') -Raw | ConvertFrom-Json
$rows = @()
$fail = $false
foreach ($m in $summary) {
    if ($SkipMaps -contains $m.slug) { continue }
    $c = $m.counts
    $bad = ($c.floating -gt 0) -or ($c.buried -gt 0) -or ($c.placeholder_slots -gt 0) -or ($c.rule_violations -gt 0)
    if ($bad -and $Gate) { $fail = $true }
    $rows += [pscustomobject]@{
        Map = $m.slug; Actors = $m.actors; Slots = $m.material_slots
        Floating = $c.floating; Buried = $c.buried; Placeholder = $c.placeholder_slots
        Rules = $c.rule_violations
        Gate = $(if ($bad) { 'FAIL' } else { 'ok' })
    }
}
$rows | Format-Table -AutoSize | Tee-Object -FilePath (Join-Path $OutDir 'table.txt')

if ($Gate) {
    if ($fail) {
        Write-Output "CHECKMAPPLACEMENT: GATE FAIL (evidence: $OutDir)"
        exit 1
    }
    Write-Output "CHECKMAPPLACEMENT: GATE PASS (evidence: $OutDir)"
} else {
    Write-Output "CHECKMAPPLACEMENT: recorded (evidence: $OutDir)"
}
