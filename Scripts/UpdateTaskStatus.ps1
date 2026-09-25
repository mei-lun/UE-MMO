param(
    [Parameter(Mandatory=$true)][string]$Id,
    [Parameter(Mandatory=$true)][ValidateSet('TODO','IN_PROGRESS','PAUSED','BLOCKED','REVIEW','DONE','DEFERRED','SUPERSEDED')][string]$Status,
    [string]$Owner = '',
    [string]$Report = '',
    [string]$UpdatedAt = '',
    [string]$ProjectRoot = ''
)
$ErrorActionPreference = 'Stop'
if(-not $ProjectRoot) { $ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$BoardPath = Join-Path $ProjectRoot 'TASKS.md'
if(-not (Test-Path -LiteralPath $BoardPath)) { throw "TASKS.md not found: $BoardPath" }
if($Id -notmatch '^M[0-4]-(?:[0-9]{3}[A-Z]?|H[0-9]{2})$') { throw "Invalid task ID: $Id" }
if(-not $UpdatedAt) { $UpdatedAt = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz') }
$ParsedTime = [DateTimeOffset]::MinValue
if(-not [DateTimeOffset]::TryParse($UpdatedAt,[ref]$ParsedTime) -or $UpdatedAt -notmatch '(Z|[+-][0-9]{2}:[0-9]{2})$') { throw 'UpdatedAt must be an ISO-8601 timestamp with timezone.' }
$Lines = [Collections.Generic.List[string]](Get-Content -LiteralPath $BoardPath -Encoding UTF8)
$RowIndices = @()
for($Index=0; $Index -lt $Lines.Count; $Index++) {
    if($Lines[$Index] -match ('^\|\s*' + [regex]::Escape($Id) + '\s*\|')) { $RowIndices += $Index }
}
if($RowIndices.Count -ne 1) { throw "Expected one row for $Id, found $($RowIndices.Count)." }
$Index = $RowIndices[0]
$Cells = @($Lines[$Index].Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
if($Cells.Count -ne 7) { throw "Task row must contain seven columns: $($Lines[$Index])" }
$EmptyCell = [string][char]0x2014
if($Status -eq 'TODO') {
    $OwnerCell=$EmptyCell; $UpdatedCell=$EmptyCell; $ReportCell=$EmptyCell
} else {
    if(-not $Owner) { throw "Owner is required for status $Status." }
    $OwnerCell=$Owner; $UpdatedCell=$UpdatedAt
    if($Report) {
        if([IO.Path]::IsPathRooted($Report) -or $Report -match '^[a-z]+://') { throw 'Report must be repository-relative.' }
        $ReportAbsolute=[IO.Path]::GetFullPath((Join-Path $ProjectRoot $Report))
        if(-not $ReportAbsolute.StartsWith($ProjectRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Report leaves repository.' }
        if(-not (Test-Path -LiteralPath $ReportAbsolute -PathType Leaf)) { throw "Report does not exist: $Report" }
        $ReportCell="[report]($Report)"
    } elseif($Cells[6] -ne $EmptyCell) {
        $ReportCell=$Cells[6]
    } else {
        throw "Report is required for status $Status."
    }
}
$Cells[2]=$Status; $Cells[4]=$OwnerCell; $Cells[5]=$UpdatedCell; $Cells[6]=$ReportCell
$Lines[$Index]='| ' + ($Cells -join ' | ') + ' |'
[IO.File]::WriteAllLines($BoardPath,$Lines,(New-Object Text.UTF8Encoding($false)))
& (Join-Path $PSScriptRoot 'CheckTaskBoard.ps1') -ProjectRoot $ProjectRoot
if($LASTEXITCODE -ne 0) { throw 'Task board was updated, but structural validation failed.' }
Write-Output "$Id -> $Status"
