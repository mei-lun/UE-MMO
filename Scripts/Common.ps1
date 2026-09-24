$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ProjectFile = Join-Path $ProjectRoot 'UEMMO.uproject'
$EngineRoot = if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { 'D:\Epic\UE_5.8' }
$LocalSettings = Join-Path $PSScriptRoot 'local.settings.json'
$CompilerVersion = ''
if (Test-Path -LiteralPath $LocalSettings) {
    $Settings = Get-Content -LiteralPath $LocalSettings -Raw | ConvertFrom-Json
    if ($Settings.EngineRoot) { $EngineRoot = $Settings.EngineRoot }
    if ($Settings.CompilerVersion) { $CompilerVersion = $Settings.CompilerVersion }
}
if (-not (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'))) {
    throw "Unreal Engine not found at $EngineRoot. Set UE_ENGINE_ROOT or Scripts/local.settings.json."
}
$ArtifactRoot = Join-Path $ProjectRoot 'Artifacts'
New-Item -ItemType Directory -Path (Join-Path $ArtifactRoot 'Logs') -Force | Out-Null
function Assert-NativeSuccess([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE. Inspect Artifacts/Logs." }
}
function Invoke-UEProcess([string]$Executable, [string[]]$Arguments, [string]$Name, [int]$TimeoutSeconds=600) {
    $Quoted = @($Arguments | ForEach-Object {
        if($_.Contains('"')) { throw 'Embedded quote is not allowed in UE process arguments.' }
        '"' + $_ + '"'
    })
    $Info = New-Object System.Diagnostics.ProcessStartInfo
    $Info.FileName = $Executable
    $Info.Arguments = $Quoted -join ' '
    $Info.WorkingDirectory = $ProjectRoot
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $Info
    if(-not $Process.Start()) { throw "Could not start $Name." }
    $OutputTask = $Process.StandardOutput.ReadToEndAsync()
    $ErrorTask = $Process.StandardError.ReadToEndAsync()
    if(-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stdout.log"), $OutputTask.Result)
        [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stderr.log"), $ErrorTask.Result)
        $Process.Dispose()
        throw "$Name timed out after $TimeoutSeconds seconds; only the process started by this invocation was stopped."
    }
    $Process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stdout.log"), $OutputTask.Result)
    [IO.File]::WriteAllText((Join-Path $ArtifactRoot "Logs\$Name-stderr.log"), $ErrorTask.Result)
    $ExitCode = $Process.ExitCode
    $Process.Dispose()
    if($ExitCode -ne 0) { throw "$Name failed with exit code $ExitCode. Inspect Artifacts/Logs." }
}
