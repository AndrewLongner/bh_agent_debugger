<#
.SYNOPSIS
Upgrade the installed BH Agent Debugger to the MSI built by installer\build_installer.bat.

.DESCRIPTION
One command per release update:
  1. Reads the MSI version/ProductCode and finds the installed product (same UpgradeCode).
  2. Refuses same-version, downgrade or ambiguous states. Requires Visual Studio closed, no other
     process running from a Visual Studio install folder (VSIXInstaller refuses to change extensions
     while, for example, idle MSBuild nodes from a build run), and the broker stopped or -StopBroker.
     The MSI stops running broker/CLI processes.
  3. Installs per user. The MSI's MajorUpgrade removes an older per-user version in the same
     transaction. A per-machine install left by 0.12.x is invisible to that upgrade, so it is
     uninstalled first; only that step asks for administrator rights (UAC).
  4. Runs msiexec with a verbose log, /norestart REBOOT=ReallySuppress. Never reboots.
  5. Validates registration (ProductState=5, version, scope, old ProductCodes gone), the log
     (status 0, MainEngineThread 0, the VSIXInstaller and devenv /updateconfiguration result for
     each Visual Studio instance) and the installed CLI version, then prints the slowest installer
     actions so slow installs can be diagnosed.

Use -DryRun to print the plan and checks without changing anything.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File installer\update.ps1 -DryRun
powershell -NoProfile -ExecutionPolicy Bypass -File installer\update.ps1
#>
[CmdletBinding()]
param(
    [string]$MsiPath,
    [string]$LogPath,
    [string]$InstallDir,
    [switch]$Quiet,
    [switch]$StopBroker,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$upgradeCode = '{54A0909D-6B5C-4BE1-BE8E-6CD1DD4B2B75}'
if (!$MsiPath) { $MsiPath = Join-Path $repo 'Temp\installer\bh_agent_debugger.msi' }
$MsiPath = (Resolve-Path -LiteralPath $MsiPath).Path
$script:summary = $null

function Say([string]$text) {
    Write-Host $text
    if ($script:summary) { Add-Content -LiteralPath $script:summary -Value $text }
}

$wi = New-Object -ComObject WindowsInstaller.Installer
function Invoke-Com($target, [string]$name, [string]$kind, [object[]]$arguments) {
    # Unary comma: keep COM collections (StringList) from being unrolled by the pipeline.
    return , $target.GetType().InvokeMember($name, [Reflection.BindingFlags]$kind, $null, $target, $arguments)
}

function Get-MsiProperty([string]$path, [string]$name) {
    $db = Invoke-Com $wi 'OpenDatabase' 'InvokeMethod' @($path, 0)
    try {
        $view = Invoke-Com $db 'OpenView' 'InvokeMethod' @("SELECT Value FROM Property WHERE Property='$name'")
        [void](Invoke-Com $view 'Execute' 'InvokeMethod' $null)
        $record = Invoke-Com $view 'Fetch' 'InvokeMethod' $null
        $value = $null
        if ($record) { $value = Invoke-Com $record 'StringData' 'GetProperty' @(1) }
        [void](Invoke-Com $view 'Close' 'InvokeMethod' $null)
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        return $value
    }
    finally {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($db)
    }
}

function Get-ProductInfo([string]$code, [string]$attribute) {
    try { return [string](Invoke-Com $wi 'ProductInfo' 'GetProperty' @($code, $attribute)) } catch { return '' }
}

function Get-ProductState([string]$code) {
    return [int](Invoke-Com $wi 'ProductState' 'GetProperty' @($code))
}

function Get-RelatedProducts {
    $list = Invoke-Com $wi 'RelatedProducts' 'GetProperty' @($upgradeCode)
    $count = [int](Invoke-Com $list 'Count' 'GetProperty' $null)
    for ($i = 0; $i -lt $count; ++$i) {
        $code = [string](Invoke-Com $list 'Item' 'GetProperty' @($i))
        $scope = 'User'
        if ((Get-ProductInfo $code 'AssignmentType') -eq '1') { $scope = 'Machine' }
        [pscustomobject]@{ ProductCode = $code; Version = (Get-ProductInfo $code 'VersionString'); Scope = $scope }
    }
}

# Install folders of the VS 2022 (17.x) and VS 2026 (18.x) instances, each ending in a backslash.
function Get-VsInstallRoots {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { return @() }
    return @(& $vswhere -all -prerelease -version '[17.0,19.0)' -property installationPath | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') + '\' })
}

# Processes other than devenv running from those folders. VSIXInstaller refuses extension changes
# (exit code 2004) while any of them runs, for example idle MSBuild nodes left by a build.
function Get-VsFolderProcesses([string[]]$roots) {
    if (!$roots -or $roots.Count -eq 0) { return @() }
    return @(Get-CimInstance Win32_Process | Where-Object {
        $exe = $_.ExecutablePath
        $_.Name -ne 'devenv.exe' -and $exe -and @($roots | Where-Object { $exe.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
    })
}

# Failed per-instance VSIXInstaller and devenv /updateconfiguration runs, as the custom action logs them.
function Get-ExtensionFailures([string]$text) {
    foreach ($m in [regex]::Matches($text, 'SyncVsExtensions vsixinstaller_exit_code=(-?\d+) instance=(\w+)')) {
        "VSIXInstaller exited $($m.Groups[1].Value) for VS instance $($m.Groups[2].Value) (2004: processes from that Visual Studio's install folder were running)"
    }
    foreach ($m in [regex]::Matches($text, 'SyncVsExtensions updateconfiguration exit_code=(-?\d+) instance=(\w+)')) {
        if ($m.Groups[1].Value -ne '0') { "devenv /updateconfiguration exited $($m.Groups[1].Value) for VS instance $($m.Groups[2].Value)" }
    }
}

# ---- Plan -------------------------------------------------------------------------------
$msiVersion = Get-MsiProperty $MsiPath 'ProductVersion'
$msiCode = Get-MsiProperty $MsiPath 'ProductCode'
$msiUpgrade = Get-MsiProperty $MsiPath 'UpgradeCode'
if ($msiUpgrade -ne $upgradeCode) { throw "Not a BH Agent Debugger MSI (UpgradeCode $msiUpgrade)" }

$related = @(Get-RelatedProducts)
$existing = $null
if ($related.Count -eq 1) { $existing = $related[0] }

# The MSI installs per user only. A per-machine install left by 0.12.x is invisible to the
# per-user MajorUpgrade, so it is uninstalled first; only that step needs administrator rights.
$legacyMachine = [bool]($existing -and $existing.Scope -eq 'Machine')

if (!$LogPath) {
    $LogPath = Join-Path $repo ("Temp\installer\update_{0}_{1}.log" -f $msiVersion, (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
$LogPath = [IO.Path]::GetFullPath($LogPath)
$uninstallLog = [IO.Path]::ChangeExtension($LogPath, '.uninstall.log')
$summaryPath = [IO.Path]::ChangeExtension($LogPath, '.summary.txt')

$ui = '/passive'
if ($Quiet) { $ui = '/qn' }
$common = "$ui /norestart REBOOT=ReallySuppress"
$installArgs = "/i `"$MsiPath`" $common /L*v `"$LogPath`""
$uninstallArgs = $null
if ($legacyMachine) { $uninstallArgs = "/x $($existing.ProductCode) $common /L*v `"$uninstallLog`"" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$needAdmin = $legacyMachine

Say "MSI:       $MsiPath"
Say "           version=$msiVersion productCode=$msiCode"
if ($related.Count -eq 0) { Say 'Installed: none (fresh install)' }
foreach ($r in $related) { Say "Installed: version=$($r.Version) scope=$($r.Scope) productCode=$($r.ProductCode)" }
Say "Target:    scope=User legacyMachineUninstallFirst=$legacyMachine elevationNeeded=$($needAdmin -and !$isAdmin)"

$problems = @()
if ($related.Count -gt 1) { $problems += 'Multiple BH Agent Debugger products are registered; remove the extras first (AGENTS.md MSI section).' }
if ($existing -and $existing.ProductCode -eq $msiCode) {
    Say "Already installed: $msiVersion ($msiCode). Nothing to do."
    exit 0
}
if ($existing -and [version]$existing.Version -ge [version]$msiVersion) {
    $problems += "Installed $($existing.Version) is not older than MSI $msiVersion. Bump the version (installer\Product.wxs, bh_agent_version.h, VSIX manifest) and rebuild (a same-version MSI would install side by side)."
}
$vs = @(Get-Process devenv -ErrorAction SilentlyContinue)
if ($vs.Count -gt 0) { $problems += "Visual Studio is running (devenv PID $(($vs | ForEach-Object Id) -join ', ')). Close all instances first." }
$vsTools = @(Get-VsFolderProcesses @(Get-VsInstallRoots))
if ($vsTools.Count -gt 0) {
    $problems += "Processes from a Visual Studio install folder are running: $(($vsTools | ForEach-Object { '{0} ({1})' -f $_.Name, $_.ProcessId }) -join ', '). VSIXInstaller refuses to change extensions while they run; close them (idle MSBuild nodes left by a build can be stopped)."
}
$brokers = @(Get-Process bh_agent_debugger_service -ErrorAction SilentlyContinue)
if ($brokers.Count -gt 0 -and !$StopBroker) {
    $problems += "bh_agent_debugger_service is running (PID $(($brokers | ForEach-Object Id) -join ', ')). The installer stops it; re-run with -StopBroker once no debugging/profiling work depends on it."
}
if (!$DryRun -and (Test-Path -LiteralPath $LogPath)) { $problems += "Log already exists: $LogPath" }

if ($uninstallArgs) { Say "Step:      msiexec $uninstallArgs" }
Say "Step:      msiexec $installArgs"
foreach ($p in $problems) { Say "BLOCKED:   $p" }
if ($DryRun) {
    Say 'Dry run: nothing was changed.'
    if ($problems.Count -gt 0) { exit 1 }
    exit 0
}
if ($problems.Count -gt 0) { exit 1 }

# ---- Install ------------------------------------------------------------------------------
$script:summary = $summaryPath
Set-Content -LiteralPath $summaryPath -Value "BH Agent Debugger update to $msiVersion at $(Get-Date -Format s)"

function Invoke-Msi([string]$arguments, [switch]$Elevate) {
    Say "Running:   msiexec $arguments"
    if ($Elevate -and !$isAdmin) {
        Write-Host 'Removing the per-machine install needs administrator rights (UAC prompt)...'
        $process = Start-Process msiexec.exe -ArgumentList $arguments -Wait -PassThru -Verb RunAs
    }
    else {
        $process = Start-Process msiexec.exe -ArgumentList $arguments -Wait -PassThru
    }
    return $process.ExitCode
}

$clock = [Diagnostics.Stopwatch]::StartNew()
if ($uninstallArgs) {
    $code = Invoke-Msi $uninstallArgs -Elevate:$needAdmin
    if ($code -ne 0) { Say "FAILED:    uninstall of $($existing.ProductCode) exited $code; see $uninstallLog"; exit 1 }
}
$code = Invoke-Msi $installArgs
$seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
Say "msiexec exit=$code elapsed=${seconds}s log=$LogPath"

# ---- Validate -----------------------------------------------------------------------------
$failures = @()
$rebootRequested = $code -eq 3010 -or $code -eq 1641
if ($code -ne 0 -and !$rebootRequested) { $failures += "msiexec exit code $code" }
if ($rebootRequested) { $failures += 'Windows Installer requested a reboot (not rebooting); check files in use' }

$state = Get-ProductState $msiCode
if ($state -ne 5) { $failures += "ProductState=$state (expected 5)" }
$installedVersion = Get-ProductInfo $msiCode 'VersionString'
if ($installedVersion -ne $msiVersion) { $failures += "registered version '$installedVersion' (expected $msiVersion)" }
$assignment = Get-ProductInfo $msiCode 'AssignmentType'
if ($assignment -ne '0') { $failures += "AssignmentType=$assignment (expected 0, per user)" }
foreach ($r in $related) {
    if ($r.ProductCode -ne $msiCode -and (Get-ProductState $r.ProductCode) -eq 5) { $failures += "old product $($r.ProductCode) ($($r.Version)) is still registered" }
}

$logText = ''
if (Test-Path -LiteralPath $LogPath) { $logText = Get-Content -LiteralPath $LogPath -Raw }
if ($logText -notmatch 'Installation success or error status: 0') { $failures += 'log lacks "Installation success or error status: 0"' }
$engine = @([regex]::Matches($logText, 'MainEngineThread is returning (\d+)'))
if ($engine.Count -eq 0 -or $engine[$engine.Count - 1].Groups[1].Value -ne '0') { $failures += 'log lacks "MainEngineThread is returning 0"' }
$failedActions = @([regex]::Matches($logText, '(?m)^Action ended [\d:]+: ([^.\r\n]+)\. Return value 3\.') | ForEach-Object { $_.Groups[1].Value })
if ($failedActions.Count -gt 0) { $failures += "actions returned 3: $($failedActions -join ', ')" }
foreach ($m in [regex]::Matches($logText, 'SyncVsExtensions installed_via_vsixinstaller instance=(\w+)')) { Say "Extension: installed for VS instance $($m.Groups[1].Value)" }
# Claude Code results are informational: the feature skips cleanly without Claude Code, and a
# bh_agent_debugger MCP server the user added is kept (claude mcp add then exits non-zero).
foreach ($m in [regex]::Matches($logText, 'SyncClaudeCode ([^\r\n]+)')) { Say "Claude:    $($m.Groups[1].Value.Trim())" }
foreach ($m in [regex]::Matches($logText, 'SyncClaudeCode mcp_add exit_code=(-?\d+)')) {
    if ($m.Groups[1].Value -ne '0') { Say "WARNING:   claude mcp add exited $($m.Groups[1].Value); check 'claude mcp get bh_agent_debugger' (a server of that name you added yourself is kept)" }
}
# Codex results are informational too: both features skip cleanly without Codex, and a bh_agent_debugger
# server the user added is kept.
foreach ($m in [regex]::Matches($logText, 'SyncCodex ([^\r\n]+)')) { Say "Codex:     $($m.Groups[1].Value.Trim())" }
foreach ($m in [regex]::Matches($logText, 'SyncCodexWsl ([^\r\n]+)')) { Say "Codex WSL: $($m.Groups[1].Value.Trim())" }
$repairHint = $null
$extensionFailures = @(Get-ExtensionFailures $logText)
if ($extensionFailures.Count -gt 0) {
    $failures += $extensionFailures
    $repairHint = "close what blocked it, then rerun the extension step: msiexec /fomus $msiCode /passive /norestart REBOOT=ReallySuppress /L*v <log>"
}

# Slowest actions, for diagnosing slow installs.
$starts = @{}
$durations = @()
foreach ($m in [regex]::Matches($logText, '(?m)^Action (start|ended) (\d\d:\d\d:\d\d): ([^.\r\n]+)\.')) {
    $time = [TimeSpan]::Parse($m.Groups[2].Value)
    $name = $m.Groups[3].Value
    if ($m.Groups[1].Value -eq 'start') { $starts[$name] = $time; continue }
    if (!$starts.ContainsKey($name)) { continue }
    $delta = ($time - $starts[$name]).TotalSeconds
    if ($delta -lt 0) { $delta += 86400 }
    $starts.Remove($name)
    if ($name -ne 'INSTALL') { $durations += [pscustomobject]@{ Action = $name; Seconds = $delta } }
}
foreach ($d in @($durations | Where-Object { $_.Seconds -ge 1 } | Sort-Object Seconds -Descending | Select-Object -First 6)) {
    Say ("Slow:      {0,4}s {1}" -f $d.Seconds, $d.Action)
}

$root = $InstallDir
if (!$root) { $root = Get-ProductInfo $msiCode 'InstallLocation' }
if (!$root) { $root = Join-Path $env:LOCALAPPDATA 'Programs\BH Agent Debugger' }
$cli = Join-Path $root 'bin\bh_agent_debugger_cli.exe'
if (Test-Path -LiteralPath $cli) {
    $versionText = (& $cli --version 2>&1 | Out-String).Trim()
    if ($versionText -notmatch ('^bh_agent_debugger_cli ' + [regex]::Escape($msiVersion) + '( |$)')) { $failures += "installed CLI reports '$versionText'" }
    else { Say "CLI:       $versionText" }
    foreach ($pair in @(@('bin\bh_agent_debugger_cli.exe', 'Temp\bh_agent_debugger_cli_x64_Release\bh_agent_debugger_cli.exe'), @('bin\bh_agent_debugger_service.exe', 'Temp\bh_agent_debugger_service_x64_Release\bh_agent_debugger_service.exe'))) {
        $installed = Join-Path $root $pair[0]
        $built = Join-Path $repo $pair[1]
        if ((Test-Path -LiteralPath $installed) -and (Test-Path -LiteralPath $built)) {
            $same = (Get-FileHash -LiteralPath $installed).Hash -eq (Get-FileHash -LiteralPath $built).Hash
            Say "Info:      $($pair[0]) matches current repo build: $same"
        }
    }
}
else {
    $failures += "installed CLI not found at $cli (pass -InstallDir)"
}

foreach ($f in $failures) { Say "FAILED:    $f" }
if ($repairHint) { Say "Fix:       $repairHint" }
if ($failures.Count -gt 0) {
    if ($rebootRequested -and $failures.Count -eq 1) { exit 2 }
    exit 1
}
Say "Update to $msiVersion verified in ${seconds}s."
exit 0
