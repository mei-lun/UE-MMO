# M1-008: read-only structural validation of Data/combat-attacks.json.
# Rules:
#   - Exit code 0 when the catalog is valid; exit code 1 with concrete reasons otherwise.
#   - Strictly read-only: this script never writes to the JSON file, never touches
#     any other file, and never creates or modifies UE assets. A failed validation
#     leaves the sample file byte-for-byte identical.
#   - Use -SamplePath to point at a temporary bad sample (duplicate id, unknown
#     next id, wrong schema, ...) without polluting the real Data/ file.
# NOTE: PS 5.1 parses BOM-less UTF-8 as GBK, so this file must stay pure ASCII.

param(
    [string]$SamplePath = ''
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($SamplePath)) {
    $SamplePath = Join-Path $ProjectRoot 'Data\combat-attacks.json'
}
elseif (-not [IO.Path]::IsPathRooted($SamplePath)) {
    $SamplePath = Join-Path $ProjectRoot $SamplePath
}

$script:Problems = New-Object System.Collections.Generic.List[string]
$script:CheckedEntryCount = 0

function Add-Problem([string]$Message) {
    $script:Problems.Add($Message) | Out-Null
}

function Add-EntryProblem([string]$Id, [string]$Field, [string]$Reason) {
    $script:Problems.Add(("FAIL attack '" + $Id + "' field '" + $Field + "': " + $Reason)) | Out-Null
}

# Case-sensitive JSON member lookup (PowerShell property access is case-insensitive,
# which would silently accept "Attack_ID" instead of "attack_id").
# The comma before $Value is load-bearing: it stops PowerShell from unrolling the
# returned value on the pipeline (a 1-element JSON array would otherwise reach the
# caller as a scalar and an empty array as $null).
function Get-Field([object]$Object, [string]$Name) {
    if ($null -eq $Object -or $Object -isnot [pscustomobject]) { return , $null }
    foreach ($Property in $Object.PSObject.Properties) {
        if ([string]::Equals($Property.Name, $Name, [StringComparison]::Ordinal)) {
            return , $Property.Value
        }
    }
    return , $null
}

function Test-IsNumber([object]$Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [bool]) { return $false }
    return ($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [single] -or $Value -is [decimal])
}

function Convert-ToDouble([object]$Value) {
    return [double]$Value
}

function Test-IsIntegral([object]$Value) {
    if (-not (Test-IsNumber $Value)) { return $false }
    $D = Convert-ToDouble $Value
    return ([math]::Floor($D) -eq $D)
}

function Test-IsString([object]$Value) {
    return ($null -ne $Value -and $Value -is [string])
}

function Test-IsBool([object]$Value) {
    return ($null -ne $Value -and $Value -is [bool])
}

# True when the value is a JSON array (anything enumerable that is not an object or scalar).
function Test-IsArray([object]$Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [string] -or $Value -is [bool] -or (Test-IsNumber $Value) -or $Value -is [pscustomobject]) { return $false }
    return ($Value -is [System.Collections.IEnumerable])
}

# ---- Load ----

if (-not (Test-Path -LiteralPath $SamplePath -PathType Leaf)) {
    Add-Problem ("FAIL: combat data file not found: " + $SamplePath)
}
else {
    try {
        $RawText = [IO.File]::ReadAllText($SamplePath)
        $Doc = ConvertFrom-Json -InputObject $RawText
    }
    catch {
        $Doc = $null
        Add-Problem ("FAIL: JSON parse error in '" + $SamplePath + "': " + $_.Exception.Message)
    }

    if ($null -ne $Doc) {
        if ($Doc -isnot [pscustomobject]) {
            Add-Problem "FAIL: top-level JSON value must be an object."
        }
        else {
            # ---- Top-level schema ----
            $SchemaVersion = Get-Field $Doc 'schema_version'
            if (-not (Test-IsNumber $SchemaVersion) -or (Convert-ToDouble $SchemaVersion) -ne 1) {
                $Shown = if ($null -eq $SchemaVersion) { '<missing>' } else { [string]$SchemaVersion }
                Add-Problem ("FAIL field 'schema_version': must be the number 1 (got " + $Shown + ").")
            }
            $LogicFps = Get-Field $Doc 'logic_fps'
            if (-not (Test-IsNumber $LogicFps) -or (Convert-ToDouble $LogicFps) -ne 60) {
                $Shown = if ($null -eq $LogicFps) { '<missing>' } else { [string]$LogicFps }
                Add-Problem ("FAIL field 'logic_fps': must be the number 60 (got " + $Shown + ").")
            }

            # ---- attacks array ----
            $Attacks = Get-Field $Doc 'attacks'
            $AttackList = @()
            if (-not (Test-IsArray $Attacks)) {
                Add-Problem "FAIL field 'attacks': must be a JSON array."
            }
            else {
                $AttackList = @($Attacks)
                if ($AttackList.Count -ne 4) {
                    Add-Problem ("FAIL field 'attacks': must hold exactly 4 entries (got " + $AttackList.Count + ").")
                }
            }

            # ---- pass 1: ids, uniqueness, expected set ----
            $IdList = @()
            $Entries = @()
            for ($Index = 0; $Index -lt $AttackList.Count; ++$Index) {
                $Entry = $AttackList[$Index]
                if ($Entry -isnot [pscustomobject]) {
                    Add-Problem ("FAIL attacks[" + $Index + "]: must be a JSON object.")
                    $Entries += $null
                    continue
                }
                $script:CheckedEntryCount++
                $Id = Get-Field $Entry 'attack_id'
                if (-not (Test-IsString $Id) -or [string]::IsNullOrWhiteSpace($Id)) {
                    Add-Problem ("FAIL attacks[" + $Index + "] field 'attack_id': must be a non-empty string.")
                    $Entries += $null
                    continue
                }
                $Entries += $Entry
                $IdList += $Id
            }

            $DuplicateIds = @($IdList | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
            foreach ($DuplicateId in $DuplicateIds) {
                Add-Problem ("FAIL field 'attack_id': duplicate id '" + $DuplicateId + "' (ids must be unique).")
            }
            $ExpectedIds = @('light_01', 'light_02', 'launcher', 'aerial_01')
            foreach ($ExpectedId in $ExpectedIds) {
                if ($IdList -notcontains $ExpectedId) {
                    Add-Problem ("FAIL field 'attack_id': expected attack '" + $ExpectedId + "' is missing from the catalog.")
                }
            }
            foreach ($Id in $IdList) {
                if ($ExpectedIds -notcontains $Id) {
                    Add-Problem ("FAIL field 'attack_id': unexpected attack id '" + $Id + "' (expected set: light_01, light_02, launcher, aerial_01).")
                }
            }

            # ---- pass 2: per-entry fields ----
            $IdSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
            foreach ($Id in $IdList) { $IdSet.Add($Id) | Out-Null }
            $CancelKinds = @('attack', 'jump')

            for ($Index = 0; $Index -lt $Entries.Count; ++$Index) {
                $Entry = $Entries[$Index]
                if ($null -eq $Entry) { continue }
                $Id = Get-Field $Entry 'attack_id'
                if (-not (Test-IsString $Id) -or [string]::IsNullOrWhiteSpace($Id)) { $Id = 'attacks[' + $Index + ']' }

                foreach ($FieldName in @('attack_id', 'animation_path')) {
                    $Value = Get-Field $Entry $FieldName
                    if (-not (Test-IsString $Value)) {
                        Add-EntryProblem $Id $FieldName "required string field is missing or not a string."
                    }
                }
                foreach ($FieldName in @(
                        'duration_frames', 'active_start_frame', 'active_end_frame',
                        'cancel_start_frame', 'cancel_end_frame', 'base_damage', 'attack_coefficient',
                        'knockback_cm_per_s', 'launch_cm_per_s', 'hit_stun_seconds', 'hit_stop_seconds',
                        'clip_start_seconds', 'clip_end_seconds')) {
                    $Value = Get-Field $Entry $FieldName
                    if (-not (Test-IsNumber $Value)) {
                        Add-EntryProblem $Id $FieldName "required number field is missing or not a number."
                    }
                }
                foreach ($FieldName in @('hit_offset_cm', 'hit_half_extent_cm')) {
                    $Value = Get-Field $Entry $FieldName
                    if (-not (Test-IsArray $Value)) {
                        Add-EntryProblem $Id $FieldName "required array field is missing or not an array."
                    }
                    else {
                        $Components = @($Value)
                        if ($Components.Count -ne 3) {
                            Add-EntryProblem $Id $FieldName ("must hold exactly 3 components (got " + $Components.Count + ").")
                        }
                        else {
                            for ($Axis = 0; $Axis -lt 3; ++$Axis) {
                                if (-not (Test-IsNumber $Components[$Axis])) {
                                    Add-EntryProblem $Id $FieldName ("component " + $Axis + " is not a number.")
                                }
                            }
                        }
                    }
                }
                foreach ($FieldName in @('next_attack_ids', 'cancel_window_allows')) {
                    $Value = Get-Field $Entry $FieldName
                    if (-not (Test-IsArray $Value)) {
                        Add-EntryProblem $Id $FieldName "required array field is missing or not an array."
                    }
                    else {
                        foreach ($Element in @($Value)) {
                            if (-not (Test-IsString $Element)) {
                                Add-EntryProblem $Id $FieldName "holds a non-string element."
                            }
                        }
                    }
                }
                $Placeholder = Get-Field $Entry 'placeholder_animation'
                if (-not (Test-IsBool $Placeholder)) {
                    Add-EntryProblem $Id 'placeholder_animation' "required boolean field is missing or not a boolean."
                }

                # ---- numeric range checks (only for well-typed values) ----
                $Duration = Get-Field $Entry 'duration_frames'
                $ActiveStart = Get-Field $Entry 'active_start_frame'
                $ActiveEnd = Get-Field $Entry 'active_end_frame'
                $CancelStart = Get-Field $Entry 'cancel_start_frame'
                $CancelEnd = Get-Field $Entry 'cancel_end_frame'

                if ((Test-IsNumber $Duration)) {
                    if (-not (Test-IsIntegral $Duration)) {
                        Add-EntryProblem $Id 'duration_frames' ("must be an integer (got " + $Duration + ").")
                    }
                    if ((Convert-ToDouble $Duration) -le 0) {
                        Add-EntryProblem $Id 'duration_frames' ("must be > 0 (got " + $Duration + ").")
                    }
                }
                if ((Test-IsNumber $ActiveStart) -and (Convert-ToDouble $ActiveStart) -lt 0) {
                    Add-EntryProblem $Id 'active_start_frame' ("must be >= 0 (got " + $ActiveStart + ").")
                }
                if ((Test-IsNumber $ActiveStart) -and (Test-IsNumber $ActiveEnd) -and
                    (Convert-ToDouble $ActiveEnd) -le (Convert-ToDouble $ActiveStart)) {
                    Add-EntryProblem $Id 'active_end_frame' ("must be > active_start_frame (got active [" + $ActiveStart + ", " + $ActiveEnd + ")).")
                }
                if ((Test-IsNumber $ActiveEnd) -and (Test-IsNumber $Duration) -and
                    (Convert-ToDouble $ActiveEnd) -gt (Convert-ToDouble $Duration)) {
                    Add-EntryProblem $Id 'active_end_frame' ("must be <= duration_frames (got end " + $ActiveEnd + " > duration " + $Duration + ").")
                }
                if ((Test-IsNumber $CancelStart) -and (Convert-ToDouble $CancelStart) -lt 0) {
                    Add-EntryProblem $Id 'cancel_start_frame' ("must be >= 0 (got " + $CancelStart + ").")
                }
                if ((Test-IsNumber $CancelStart) -and (Test-IsNumber $CancelEnd) -and
                    (Convert-ToDouble $CancelEnd) -le (Convert-ToDouble $CancelStart)) {
                    Add-EntryProblem $Id 'cancel_end_frame' ("must be > cancel_start_frame (got cancel [" + $CancelStart + ", " + $CancelEnd + ")).")
                }
                if ((Test-IsNumber $CancelEnd) -and (Test-IsNumber $Duration) -and
                    (Convert-ToDouble $CancelEnd) -gt (Convert-ToDouble $Duration)) {
                    Add-EntryProblem $Id 'cancel_end_frame' ("must be <= duration_frames (got end " + $CancelEnd + " > duration " + $Duration + ").")
                }

                $BaseDamage = Get-Field $Entry 'base_damage'
                if ((Test-IsNumber $BaseDamage) -and (Convert-ToDouble $BaseDamage) -lt 0) {
                    Add-EntryProblem $Id 'base_damage' ("must be >= 0 (got " + $BaseDamage + ").")
                }
                $Coefficient = Get-Field $Entry 'attack_coefficient'
                if ((Test-IsNumber $Coefficient) -and (Convert-ToDouble $Coefficient) -le 0) {
                    Add-EntryProblem $Id 'attack_coefficient' ("must be > 0 (got " + $Coefficient + ").")
                }
                foreach ($FieldName in @('knockback_cm_per_s', 'launch_cm_per_s', 'hit_stun_seconds', 'hit_stop_seconds')) {
                    $Value = Get-Field $Entry $FieldName
                    if ((Test-IsNumber $Value) -and (Convert-ToDouble $Value) -lt 0) {
                        Add-EntryProblem $Id $FieldName ("must be >= 0 (got " + $Value + ").")
                    }
                }

                foreach ($FieldName in @('hit_offset_cm', 'hit_half_extent_cm')) {
                    $Value = Get-Field $Entry $FieldName
                    if (Test-IsArray $Value) {
                        $Components = @($Value)
                        if ($Components.Count -eq 3) {
                            for ($Axis = 0; $Axis -lt 3; ++$Axis) {
                                if (Test-IsNumber $Components[$Axis]) {
                                    $ComponentValue = Convert-ToDouble $Components[$Axis]
                                    if ($FieldName -eq 'hit_half_extent_cm' -and $ComponentValue -le 0) {
                                        Add-EntryProblem $Id $FieldName ("component " + $Axis + " must be > 0 (got " + $ComponentValue + ").")
                                    }
                                    if ([double]::IsNaN($ComponentValue) -or [double]::IsInfinity($ComponentValue)) {
                                        Add-EntryProblem $Id $FieldName ("component " + $Axis + " must be finite (got " + $ComponentValue + ").")
                                    }
                                }
                            }
                        }
                    }
                }

                $ClipStart = Get-Field $Entry 'clip_start_seconds'
                $ClipEnd = Get-Field $Entry 'clip_end_seconds'
                if ((Test-IsNumber $ClipStart) -and (Convert-ToDouble $ClipStart) -lt 0) {
                    Add-EntryProblem $Id 'clip_start_seconds' ("must be >= 0 (got " + $ClipStart + ").")
                }
                if ((Test-IsNumber $ClipEnd) -and (Convert-ToDouble $ClipEnd) -lt 0) {
                    Add-EntryProblem $Id 'clip_end_seconds' ("must be >= 0 (got " + $ClipEnd + ").")
                }
                if ((Test-IsBool $Placeholder) -and (([bool]$Placeholder) -eq $false) -and
                    (Test-IsNumber $ClipStart) -and (Test-IsNumber $ClipEnd) -and
                    (Convert-ToDouble $ClipEnd) -lt (Convert-ToDouble $ClipStart)) {
                    Add-EntryProblem $Id 'clip_end_seconds' ("must be >= clip_start_seconds for a real (non-placeholder) animation (got start " + $ClipStart + " > end " + $ClipEnd + ").")
                }

                $AnimationPath = Get-Field $Entry 'animation_path'
                if ((Test-IsString $AnimationPath)) {
                    if ([string]::IsNullOrWhiteSpace($AnimationPath)) {
                        Add-EntryProblem $Id 'animation_path' "must be a non-empty animation path."
                    }
                    elseif (-not $AnimationPath.StartsWith('/Game/')) {
                        Add-EntryProblem $Id 'animation_path' ("must start with '/Game/' (got '" + $AnimationPath + "').")
                    }
                }

                # ---- set references ----
                $NextIds = Get-Field $Entry 'next_attack_ids'
                if (Test-IsArray $NextIds) {
                    foreach ($Element in @($NextIds)) {
                        if (Test-IsString $Element) {
                            if (-not $IdSet.Contains($Element)) {
                                Add-EntryProblem $Id 'next_attack_ids' ("references unknown attack id '" + $Element + "'.")
                            }
                        }
                    }
                }
                $Allows = Get-Field $Entry 'cancel_window_allows'
                if (Test-IsArray $Allows) {
                    foreach ($Element in @($Allows)) {
                        if ((Test-IsString $Element) -and ($CancelKinds -notcontains $Element)) {
                            Add-EntryProblem $Id 'cancel_window_allows' ("unknown cancel kind '" + $Element + "' (allowed: attack, jump).")
                        }
                    }
                }
            }
        }
    }
}

# ---- Report ----
foreach ($Problem in $script:Problems) {
    Write-Host $Problem
}
Write-Host ("Checked entries: " + $script:CheckedEntryCount + " of 4 expected; sample file: " + $SamplePath)
Write-Host "Read-only validation: no files were written or modified."
if ($script:Problems.Count -gt 0) {
    Write-Host ("VALIDATION FAILED with " + $script:Problems.Count + " problem(s).")
    exit 1
}
Write-Host "VALIDATION PASSED."
exit 0
