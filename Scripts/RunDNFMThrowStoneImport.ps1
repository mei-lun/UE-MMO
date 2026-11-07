. "$PSScriptRoot\Common.ps1"
# M8-019B: import the throwstone projectile texture + material (editor session).
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = 'M8-019B'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Arguments = @($ProjectFile,'-unattended','-nosplash','-nosound','-NoSourceControl','-RenderOffscreen','-ScriptErrorsAreFatal',
    "-abslog=$Evidence\ue-throwstone-import.log",
    "-ExecutePythonScript=$PSScriptRoot\Editor\import_dnfm_throwstone.py")
Invoke-UEProcess $Editor $Arguments "dnfm-throwstone-import" 900
$Report = Join-Path $Evidence "ue-throwstone-import.json"
if (-not (Test-Path -LiteralPath $Report)) { throw "Missing $Report" }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if (-not $Result.success) { throw "Throwstone import failed; inspect $Report and the UE log." }
@{success=$true; report=$Report} | ConvertTo-Json
