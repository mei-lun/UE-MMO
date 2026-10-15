param([string]$EvidenceRoot = '')
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $EvidenceRoot) {
    $Stamp = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH-mm-sszzz').Replace(':','')
    $EvidenceRoot = Join-Path $ProjectRoot "Artifacts/Tasks/M0-006/$Stamp"
}
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
& node "$PSScriptRoot/EngineWire/verify.mjs" $ProjectRoot $EvidenceRoot
if ($LASTEXITCODE -ne 0) { throw "EngineWire verification failed. Inspect $EvidenceRoot/enginewire-summary.json and enginewire-stderr.log." }
$Report = Get-Content -Raw -Encoding UTF8 "$EvidenceRoot/enginewire-summary.json" | ConvertFrom-Json
if (-not $Report.success) { throw 'EngineWire did not produce a successful current report.' }
Write-Output "EngineWire verified: $EvidenceRoot/enginewire-summary.json"
