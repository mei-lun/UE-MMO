param([switch]$Json, [string]$ProjectRoot = '')
$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$Board = Join-Path $ProjectRoot 'TASKS.md'
$Problems = New-Object 'System.Collections.Generic.List[string]'
$Rows = New-Object 'System.Collections.Generic.List[object]'
$ById = @{}
$Allowed = @('TODO','IN_PROGRESS','PAUSED','BLOCKED','REVIEW','DONE','DEFERRED','SUPERSEDED')
$IdPattern = '^M[0-4]-(?:[0-9]{3}[A-Z]?|H[0-9]{2})$'
$EmptyCell = [string][char]0x2014

function Get-LinkPath([string]$Cell) {
    if ($Cell -match '\[[^\]]+\]\(([^)]+)\)') { return ($Matches[1] -split '#',2)[0] }
    return ''
}
function Test-LocalLink([string]$Link, [string]$Label) {
    if (-not $Link) { return $false }
    if ($Link -match '^[a-z]+://' -or [IO.Path]::IsPathRooted($Link)) {
        $Problems.Add("$Label must use a repository-relative path."); return $false
    }
    $Absolute = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $Link))
    $Prefix = $ProjectRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $Absolute.StartsWith($Prefix,[StringComparison]::OrdinalIgnoreCase)) {
        $Problems.Add("$Label leaves the repository."); return $false
    }
    if (-not (Test-Path -LiteralPath $Absolute -PathType Leaf)) {
        $Problems.Add("$Label file does not exist: $Link"); return $false
    }
    return $true
}
function Get-ReportField([string]$Content, [string]$Name) {
    $Pattern = '(?m)^- ' + [regex]::Escape($Name) + ':\s*(.+?)\s*$'
    if ($Content -match $Pattern) { return $Matches[1].Trim() }
    return ''
}

if (-not (Test-Path -LiteralPath $Board)) { throw "TASKS.md not found at $ProjectRoot" }
foreach ($Line in Get-Content -LiteralPath $Board -Encoding UTF8) {
    if ($Line -notmatch '^\|\s*M[0-9]+-') { continue }
    $Cells = @($Line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
    if ($Cells.Count -ne 7) { $Problems.Add("Expected 7 columns: $Line"); continue }
    $Id = $Cells[0]
    if ($Id -notmatch $IdPattern) { $Problems.Add("Invalid task ID: $Id"); continue }
    if ($ById.ContainsKey($Id)) { $Problems.Add("Duplicate task ID: $Id"); continue }
    $Dependencies = @()
    if ($Cells[3] -ne $EmptyCell) { $Dependencies = @($Cells[3].Split(',') | ForEach-Object { $_.Trim() }) }
    $Row = [pscustomobject]@{Id=$Id; Title=$Cells[1]; Status=$Cells[2]; Dependencies=$Dependencies; Owner=$Cells[4]; UpdatedAt=$Cells[5]; Evidence=$Cells[6]; Stage=$Id.Substring(0,2)}
    $Rows.Add($Row); $ById[$Id]=$Row
    if ($Allowed -notcontains $Row.Status) { $Problems.Add("$Id invalid status: $($Row.Status)") }
    $CardLink = Get-LinkPath $Row.Title
    if ($Id -notmatch '^M0-00[1-5]$') {
        if (-not $CardLink) { $Problems.Add("$Id needs a task card link.") }
        else { $null = Test-LocalLink $CardLink "$Id task card" }
    }
    if ($Row.Status -in @('IN_PROGRESS','PAUSED','BLOCKED','REVIEW','DONE','SUPERSEDED')) {
        if ($Row.Owner -eq $EmptyCell -or -not $Row.Owner) { $Problems.Add("$Id needs an owner.") }
        $EvidenceLink = Get-LinkPath $Row.Evidence
        if (-not $EvidenceLink) { $Problems.Add("$Id needs an evidence report link.") }
        elseif (Test-LocalLink $EvidenceLink "$Id evidence") {
            if ($Id -notmatch '^M0-00[1-5]$') {
                $Report = Get-Content -LiteralPath (Join-Path $ProjectRoot $EvidenceLink) -Raw -Encoding UTF8
                if ((Get-ReportField $Report 'Task-ID') -ne $Id) { $Problems.Add("$Id report Task-ID mismatch.") }
                if ((Get-ReportField $Report 'Owner') -ne $Row.Owner) { $Problems.Add("$Id report Owner differs from board.") }
                if ($Row.Status -eq 'DONE') {
                    if ((Get-ReportField $Report 'Verification') -ne 'PASS') { $Problems.Add("$Id DONE report must contain Verification: PASS.") }
                    $Revision = Get-ReportField $Report 'Implementation-Revision'
                    if ($Revision -notmatch '^[a-fA-F0-9]{7,40}$') { $Problems.Add("$Id DONE needs an implementation commit SHA.") }
                    else {
                        & git -C $ProjectRoot rev-parse --verify --quiet "$Revision`^{commit}" 2>$null | Out-Null
                        if ($LASTEXITCODE -ne 0) { $Problems.Add("$Id implementation commit does not exist: $Revision") }
                        else {
                            & git -C $ProjectRoot merge-base --is-ancestor $Revision HEAD 2>$null
                            if ($LASTEXITCODE -ne 0) { $Problems.Add("$Id implementation commit is not integrated into current HEAD.") }
                        }
                    }
                    if ($Id -match '-H[0-9]{2}$' -and (Get-ReportField $Report 'Human-Approved') -ne 'yes') {
                        $Problems.Add("$Id requires an explicit Human-Approved: yes record.")
                    }
                }
            }
        }
        $Date = [DateTimeOffset]::MinValue
        if (-not [DateTimeOffset]::TryParse($Row.UpdatedAt,[ref]$Date) -or $Row.UpdatedAt -notmatch '(Z|[+-][0-9]{2}:[0-9]{2})$') {
            $Problems.Add("$Id needs an ISO timestamp with timezone.")
        }
    }
}
if ($Rows.Count -eq 0) { $Problems.Add('No tasks were parsed.') }
foreach ($Row in $Rows) {
    foreach ($Dep in $Row.Dependencies) {
        if (-not $ById.ContainsKey($Dep)) { $Problems.Add("$($Row.Id) has unknown dependency: $Dep"); continue }
        if ($Dep -eq $Row.Id) { $Problems.Add("$($Row.Id) depends on itself.") }
        if ($ById[$Dep].Status -eq 'SUPERSEDED') { $Problems.Add("$($Row.Id) must depend on replacement tasks, not $Dep.") }
        if ($Row.Status -in @('IN_PROGRESS','DONE','REVIEW') -and $ById[$Dep].Status -ne 'DONE') {
            $Problems.Add("$($Row.Id) is $($Row.Status) but dependency $Dep is $($ById[$Dep].Status).")
        }
    }
}
$Visiting=@{}; $Visited=@{}
function Visit-Task([string]$Id) {
    if ($Visiting.ContainsKey($Id)) { $Problems.Add("Dependency cycle includes $Id."); return }
    if ($Visited.ContainsKey($Id)) { return }
    $Visiting[$Id]=$true
    foreach($Dep in $ById[$Id].Dependencies) { if($ById.ContainsKey($Dep)) { Visit-Task $Dep } }
    $null = $Visiting.Remove($Id)
    $Visited[$Id]=$true
}
foreach($Row in $Rows) { Visit-Task $Row.Id }
$InProgress=@($Rows | Where-Object Status -eq 'IN_PROGRESS')
if($InProgress.Count -gt 1) { $Problems.Add('Multiple IN_PROGRESS tasks in one shared workspace. Finish/pause before claiming another.') }
$Ready=@($Rows | Where-Object {
    $Row=$_
    $Row.Status -eq 'TODO' -and @($Row.Dependencies | Where-Object { -not $ById.ContainsKey($_) -or $ById[$_].Status -ne 'DONE' }).Count -eq 0
} | ForEach-Object { $_.Id })
$ByStage=@{}
foreach($Stage in @('M0','M1','M2','M3','M4')) {
    $StageRows=@($Rows | Where-Object Stage -eq $Stage)
    $ByStage[$Stage]=@{Total=$StageRows.Count;Done=@($StageRows|Where-Object Status -eq 'DONE').Count}
}
$Result=[ordered]@{
    valid=($Problems.Count -eq 0); total=$Rows.Count
    done=@($Rows|Where-Object Status -eq 'DONE').Count
    todo=@($Rows|Where-Object Status -eq 'TODO').Count
    in_progress=$InProgress.Count; ready=$Ready
    awaiting_human=@($Rows|Where-Object {$_.Id -match '-H[0-9]{2}$' -and $_.Status -eq 'REVIEW'}|ForEach-Object {$_.Id})
    by_stage=$ByStage; errors=@($Problems.ToArray())
    note='Read-only structural check. Does not prove implementation correctness or genuine user approval.'
}
if($Json) { $Result | ConvertTo-Json -Depth 6 }
else {
    Write-Output ("Task board: valid={0}; total={1}; DONE={2}; TODO={3}; IN_PROGRESS={4}" -f $Result.valid,$Result.total,$Result.done,$Result.todo,$Result.in_progress)
    Write-Output ("Ready: " + ($Ready -join ', '))
    Write-Output ("Awaiting human: " + ($Result.awaiting_human -join ', '))
    foreach($Problem in $Problems) { Write-Output ("ERROR: " + $Problem) }
}
if($Problems.Count -gt 0) { exit 1 }
exit 0
