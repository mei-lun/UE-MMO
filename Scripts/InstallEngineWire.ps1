param([switch]$SkipDownload)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Pin = Get-Content -Raw -Encoding UTF8 "$PSScriptRoot/EngineWire/version.json" | ConvertFrom-Json
$LocalRoot = Join-Path $ProjectRoot '.agent-local/enginewire'
$Upstream = Join-Path $LocalRoot 'upstream'
$Server = Join-Path $LocalRoot 'server'
$Node = (Get-Command node.exe -ErrorAction Stop).Source
$Utf8 = [Text.UTF8Encoding]::new($false)
$Running = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^UnrealEditor' -and $_.CommandLine -like "*$ProjectRoot*" })
if ($Running.Count) { throw 'Close this project editor before installing or updating EngineWire.' }
$NodeVersion = (& $Node --version).TrimStart('v')
if ([version]$NodeVersion -lt [version]'20.19.0') { throw 'EngineWire requires Node.js 20.19 or later.' }
New-Item -ItemType Directory -Force -Path $LocalRoot,$Server | Out-Null
if (-not (Test-Path "$Upstream/.git")) {
    if ($SkipDownload) { throw 'Pinned source checkout is missing.' }
    & git clone --depth 1 --branch $Pin.tag $Pin.repository $Upstream
    if ($LASTEXITCODE -ne 0) { throw 'EngineWire source download failed.' }
}
$Revision = (& git -C $Upstream rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $Revision -ne $Pin.commit) { throw 'Existing EngineWire checkout does not match version.json; preserve and inspect it before upgrading.' }
$SourceChanges = @(& git -C $Upstream status --porcelain --untracked-files=no)
if ($LASTEXITCODE -ne 0 -or $SourceChanges.Count) { throw 'EngineWire source checkout contains tracked changes.' }
$Descriptor = Get-Content -Raw -Encoding UTF8 "$Upstream/plugins/$($Pin.plugin)/$($Pin.plugin).uplugin" | ConvertFrom-Json
if ($Descriptor.VersionName -ne $Pin.npmVersion) { throw 'Plugin and Node version pins do not match.' }
if (-not $SkipDownload) {
    Copy-Item -LiteralPath "$PSScriptRoot/EngineWire/package.json","$PSScriptRoot/EngineWire/package-lock.json" -Destination $Server -Force
    & npm.cmd ci --prefix $Server --ignore-scripts --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw 'EngineWire Node dependency installation failed.' }
}
$Cli = Join-Path $Server 'node_modules/unreal-engine-mcp-server/dist/cli.js'
if (-not (Test-Path -LiteralPath $Cli)) { throw 'EngineWire Node entry point is missing.' }
$NodePackage = Get-Content -Raw -Encoding UTF8 "$Server/node_modules/unreal-engine-mcp-server/package.json" | ConvertFrom-Json
if ($NodePackage.version -ne $Pin.npmVersion) { throw 'Installed Node server does not match the pinned plugin.' }

$ProjectFile = Join-Path $ProjectRoot 'UEMMO.uproject'
$Project = Get-Content -Raw -Encoding UTF8 $ProjectFile | ConvertFrom-Json
$PluginDir = '.agent-local/enginewire/upstream/plugins'
if (-not $Project.PSObject.Properties['AdditionalPluginDirectories']) { $Project | Add-Member NoteProperty AdditionalPluginDirectories @($PluginDir) }
elseif ($Project.AdditionalPluginDirectories -notcontains $PluginDir) { $Project.AdditionalPluginDirectories = @($Project.AdditionalPluginDirectories) + $PluginDir }
$Entry = @($Project.Plugins | Where-Object Name -eq $Pin.plugin)
if (-not $Entry.Count) { $Project.Plugins += [pscustomobject]@{Name=$Pin.plugin;Enabled=$true;TargetAllowList=@('Editor')} }
elseif ($Entry.Count -ne 1 -or -not $Entry[0].Enabled -or @($Entry[0].TargetAllowList).Count -ne 1 -or $Entry[0].TargetAllowList[0] -ne 'Editor') { throw 'Existing MCP plugin declaration differs; inspect before changing it.' }
$ProjectText = ($Project | ConvertTo-Json -Depth 16)+"`n"

$IniFile = Join-Path $ProjectRoot 'Config/DefaultGame.ini'
$Ini = [IO.File]::ReadAllText($IniFile)
$Settings = @"
[/Script/McpAutomationBridge.McpAutomationBridgeSettings]
bAlwaysListen=True
ListenHost=127.0.0.1
ListenPorts=$($Pin.projectPort)
bMultiListen=False
bRequireCapabilityToken=True
bAllowNonLoopback=False
bEnableNativeMCP=False
"@
$SectionPattern = '(?im)^\s*\[/Script/McpAutomationBridge\.McpAutomationBridgeSettings\]\s*$'
$Sections = [regex]::Matches($Ini,$SectionPattern)
if ($Sections.Count -eq 0) {
    $IniText = $Ini.TrimEnd()+"`n`n; EngineWire is editor-only. The token is generated under Saved/MCP, never in this file.`n"+$Settings+"`n"
} else {
    if ($Sections.Count -ne 1) { throw 'Duplicate MCP settings sections; inspect before changing them.' }
    $SectionTail = $Ini.Substring($Sections[0].Index+$Sections[0].Length)
    $NextSection = [regex]::Match($SectionTail,'(?m)^\s*\[')
    $SectionText = if ($NextSection.Success) { $SectionTail.Substring(0,$NextSection.Index) } else { $SectionTail }
    foreach ($Line in ($Settings -split "`r?`n" | Select-Object -Skip 1)) {
        if (-not $Line) { continue }
        $Parts = $Line.Split('=',2)
        $Values = [regex]::Matches($SectionText,'(?im)^\s*'+[regex]::Escape($Parts[0])+'\s*=\s*([^\r\n]*?)\s*$')
        if ($Values.Count -ne 1 -or $Values[0].Groups[1].Value.Trim() -ne $Parts[1]) {
            throw "MCP setting $($Parts[0]) is missing, duplicated or differs from the installation profile."
        }
    }
    $IniText = $Ini
}

$ConfigDir = Join-Path $ProjectRoot '.codex'
$ConfigFile = Join-Path $ConfigDir 'config.toml'
$Existing = if(Test-Path $ConfigFile) { [IO.File]::ReadAllText($ConfigFile) } else { '' }
$ManagedPattern = '(?ms)^# BEGIN UE-MMO EngineWire\r?\n.*?^# END UE-MMO EngineWire\r?\n?'
$ManagedBlocks = [regex]::Matches($Existing,$ManagedPattern)
if ($ManagedBlocks.Count -gt 1) { throw 'Duplicate managed EngineWire configuration blocks.' }
$Remaining = [regex]::Replace($Existing,$ManagedPattern,'')
if ($Remaining -match '(?m)^\s*\[mcp_servers\.ue_mmo_enginewire(?:\.|\])') { throw 'An unmanaged ue_mmo_enginewire MCP entry already exists.' }
if (($Node+$Cli+$ProjectRoot).Contains("'")) { throw 'Single quotes in installation paths require manual TOML escaping.' }
$Block = @"
# BEGIN UE-MMO EngineWire
[mcp_servers.ue_mmo_enginewire]
command = '$Node'
args = ['$Cli']
cwd = '$ProjectRoot'
startup_timeout_sec = 60
tool_timeout_sec = 120

[mcp_servers.ue_mmo_enginewire.env]
UE_PROJECT_PATH = '$ProjectRoot'
LOG_LEVEL = 'warn'
# END UE-MMO EngineWire
"@
$ConfigText = ($Remaining.TrimEnd()+"`n`n"+$Block+"`n").TrimStart()
# Validate every proposed configuration above before enabling the plugin. Keep exact
# original bytes for rollback if a later write fails (e.g. a locked config file).
$Changes = @(
    @{Path=$ProjectFile;Text=$ProjectText},
    @{Path=$IniFile;Text=$IniText},
    @{Path=$ConfigFile;Text=$ConfigText}
)
$Written = [Collections.Generic.List[object]]::new()
try {
    foreach ($Change in $Changes) {
        $Exists = Test-Path -LiteralPath $Change.Path
        if ($Exists -and [IO.File]::ReadAllText($Change.Path) -eq $Change.Text) { continue }
        $Original = if ($Exists) { [IO.File]::ReadAllBytes($Change.Path) } else { $null }
        New-Item -ItemType Directory -Force -Path (Split-Path $Change.Path) | Out-Null
        $Written.Add(@{Path=$Change.Path;Exists=$Exists;Bytes=$Original})
        [IO.File]::WriteAllText($Change.Path,$Change.Text,$Utf8)
    }
} catch {
    $InstallError = $_
    for ($Index=$Written.Count-1; $Index -ge 0; $Index--) {
        $Backup = $Written[$Index]
        if ($Backup.Exists) { [IO.File]::WriteAllBytes($Backup.Path,$Backup.Bytes) }
        elseif (Test-Path -LiteralPath $Backup.Path) { Remove-Item -LiteralPath $Backup.Path }
    }
    throw $InstallError
}
Write-Output "Installed EngineWire $($Pin.npmVersion), plugin $($Pin.plugin), project MCP ue_mmo_enginewire."
Write-Output 'Next run Scripts/Build.ps1, then Scripts/Run.ps1 -Editor. Open a new Codex session to load the project MCP entry.'
