. "$PSScriptRoot\Common.ps1"
# M8-024: complete the guild scene's placeholder materials (editor session,
# then a fresh-process verify).
$Existing = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like '*UEMMO.uproject*' })
if ($Existing.Count -gt 0) { throw 'An editor for this project is already running. Do not run two resource writers.' }
$TaskId = 'M8-024'
$Evidence = Join-Path $ProjectRoot ("Artifacts\Tasks\" + $TaskId)
New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\complete_guild_materials.py'
Invoke-UEProcess $Editor @(
    $ProjectFile,'-run=pythonscript',"-script=$Script",
    '-unattended','-nosplash','-nosound','-nullrhi',
    "-abslog=$Evidence\guild-materials-generate.log"
) 'guild-materials-generate' 1800
Invoke-UEProcess $Editor @(
    $ProjectFile,'-run=pythonscript',"-script=$Script",'-UEMMOGuildMaterialVerify',
    '-unattended','-nosplash','-nosound','-nullrhi',
    "-abslog=$Evidence\guild-materials-verify.log"
) 'guild-materials-verify' 1800
foreach ($Name in @('ue-guild-materials-generate.json','ue-guild-materials-verify.json')) {
    $Report = Join-Path $Evidence $Name
    if (-not (Test-Path -LiteralPath $Report)) { throw "Missing $Report" }
    $Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    if (-not $Result.success) { throw "Guild materials $Name reports failure; inspect the UE logs." }
}
@{success=$true; task=$TaskId} | ConvertTo-Json
