. "$PSScriptRoot\Common.ps1"
# M8-021: import the slice's victim-voice WAVs (editor session), then a
# fresh-process reload verify.
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = 'M8-021'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\import_dnfm_slice_audio.py'
Invoke-UEProcess $Editor @(
    $ProjectFile,'-run=pythonscript',"-script=$Script",
    '-unattended','-nosplash','-nosound','-nullrhi',
    "-abslog=$Evidence\slice-audio-generate.log"
) 'slice-audio-generate' 900
Invoke-UEProcess $Editor @(
    $ProjectFile,'-run=pythonscript',"-script=$Script",'-UEMMOSliceAudioVerify',
    '-unattended','-nosplash','-nosound','-nullrhi',
    "-abslog=$Evidence\slice-audio-verify.log"
) 'slice-audio-verify' 900
foreach ($Name in @('slice-audio-import.json','slice-audio-verify.json')) {
    $Report = Join-Path $Evidence $Name
    if (-not (Test-Path -LiteralPath $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    if (-not $Result.success) { throw "Slice audio $Name reports failure; inspect the UE logs." }
}
@{success=$true; task=$TaskId} | ConvertTo-Json
