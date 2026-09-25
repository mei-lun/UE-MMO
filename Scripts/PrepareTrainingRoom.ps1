# M1-016: build and verify the training arena map via the UE editor Python script.
# ASCII only: PowerShell 5.1 parses BOM-less UTF-8 as GBK.
. "$PSScriptRoot\Common.ps1"
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Script = Join-Path $PSScriptRoot 'Editor\create_training_room.py'
$Report = Join-Path $ArtifactRoot 'training-room-report.json'
if (Test-Path -LiteralPath $Report) { Remove-Item -LiteralPath $Report }

# Pass 1: duplicate L_PrototypeArena into L_TrainingArena, edit, save.
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    "-abslog=$ArtifactRoot\Logs\prepare-training-room-create.log"
) 'prepare-training-room-create' 600

# Pass 2: a fresh editor process reloads both maps and asserts the actor
# inventory, then writes Artifacts/training-room-report.json.
Invoke-UEProcess $Editor @(
    $ProjectFile,
    '-run=pythonscript',
    "-script=$Script",
    '-unattended', '-nosplash', '-nosound', '-nullrhi',
    '-UEMMOTrainingRoomVerify',
    "-abslog=$ArtifactRoot\Logs\prepare-training-room-verify.log"
) 'prepare-training-room-verify' 600

if (-not (Test-Path -LiteralPath $Report)) { throw 'UE did not write training-room-report.json.' }
$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if (-not $Result.success) { throw 'Training room verification failed; inspect training-room-report.json.' }
$Training = $Result.training_arena
$Prototype = $Result.prototype_arena
Write-Output ('Training arena verified: ' + $Training.actor_count + ' actors, training enemies=' + $Training.training_enemy_count + ', previews=' + $Training.preview_actor_count + ', player_start_present=' + ($Training.actors | Where-Object { $_.label -eq 'PlayerStart' } | Measure-Object).Count)
Write-Output ('Prototype arena unchanged: ' + $Prototype.actor_count + ' actors, training enemies=' + $Prototype.training_enemy_count + ', previews=' + $Prototype.preview_actor_count)
Write-Output 'Training room ready.'
