param (
    [string]$message = "status",
    [string]$pipeName = "bh_agent_debugger_agent",
    [int]$connectTimeoutMs = 5000,
    [switch]$startDebugging,
    [string]$solution = "",
    [string]$startupProject = "",
    [string]$configuration = "",
    [string]$platform = "",
    [string]$expSolution = "",
    [string]$expStartupProject = "",
    [string]$expConfiguration = "",
    [string]$expPlatform = "",
    [string]$experimentalSuffix = "",
    [switch]$launchExpIfMissing,
    [switch]$strictDteMatch,
    [int]$timeoutSeconds = 15,
    [switch]$clean,
    [switch]$waitForBreak,
    [int]$breakTimeoutSeconds = 30,
    [string[]]$queue = @(),
    [int]$queueDelayMs = 0,
    [switch]$diagnostics,
    [switch]$useAgentCtl,
    [string]$servicePipeName = "bh_agent_ctl",
    [string]$agentTargetId = ""
)

if ($Host.Runspace.ApartmentState -ne "STA")
{
    $forwardArgs = @()
    foreach ($entry in $PSBoundParameters.GetEnumerator())
    {
        $name = [string]$entry.Key
        $value = $entry.Value

        if ($value -is [System.Management.Automation.SwitchParameter])
        {
            if ($value.IsPresent)
            {
                $forwardArgs += "-$name"
            }
            continue
        }

        if ($value -is [array])
        {
            if ($value.Count -gt 0)
            {
                $forwardArgs += "-$name"
                foreach ($item in $value)
                {
                    $forwardArgs += [string]$item
                }
            }
            continue
        }

        $forwardArgs += "-$name"
        $forwardArgs += [string]$value
    }

    & powershell.exe -STA -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath @forwardArgs
    exit $LASTEXITCODE
}

function Get-DevenvPath
{
    # Newest Visual Studio 2022 (17.x) or 2026 (18.x) install, located through vswhere.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere)
    {
        $paths = & $vswhere -nologo -prerelease -products * -version "[17.0,19.0)" -sort -property productPath
        foreach ($path in @($paths))
        {
            if (-not [string]::IsNullOrWhiteSpace($path) -and $path.EndsWith("devenv.exe", [System.StringComparison]::OrdinalIgnoreCase) -and (Test-Path $path))
            {
                return $path
            }
        }
    }

    throw "Could not locate devenv.exe for Visual Studio 2022 or 2026"
}

function Get-DteFromRot([string]$pattern)
{
    if (-not ("RotHelper" -as [type]))
    {
        $rotSource = @"
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text.RegularExpressions;
using System.Collections.Generic;

public static class RotHelper
{
    [DllImport("ole32.dll")]
    private static extern int GetRunningObjectTable(int reserved, out IRunningObjectTable prot);

    [DllImport("ole32.dll")]
    private static extern int CreateBindCtx(int reserved, out IBindCtx ppbc);

    public static object GetObject(string pattern)
    {
        IRunningObjectTable rot;
        if (GetRunningObjectTable(0, out rot) != 0 || rot == null)
        {
            return null;
        }

        IEnumMoniker enumMoniker;
        rot.EnumRunning(out enumMoniker);
        if (enumMoniker == null)
        {
            return null;
        }

        var monikers = new IMoniker[1];
        var fetched = IntPtr.Zero;
        while (enumMoniker.Next(1, monikers, fetched) == 0)
        {
            IBindCtx bindCtx;
            CreateBindCtx(0, out bindCtx);
            string name;
            monikers[0].GetDisplayName(bindCtx, null, out name);
            if (Regex.IsMatch(name ?? string.Empty, pattern))
            {
                object obj;
                rot.GetObject(monikers[0], out obj);
                return obj;
            }
        }

        return null;
    }

    public static string[] GetMonikerNames(string pattern)
    {
        IRunningObjectTable rot;
        if (GetRunningObjectTable(0, out rot) != 0 || rot == null)
        {
            return new string[0];
        }

        IEnumMoniker enumMoniker;
        rot.EnumRunning(out enumMoniker);
        if (enumMoniker == null)
        {
            return new string[0];
        }

        var names = new List<string>();
        var monikers = new IMoniker[1];
        var fetched = IntPtr.Zero;
        while (enumMoniker.Next(1, monikers, fetched) == 0)
        {
            IBindCtx bindCtx;
            CreateBindCtx(0, out bindCtx);
            string name;
            monikers[0].GetDisplayName(bindCtx, null, out name);
            if (Regex.IsMatch(name ?? string.Empty, pattern))
            {
                names.Add(name);
            }
        }

        return names.ToArray();
    }
}
"@
        Add-Type -TypeDefinition $rotSource
    }

    return [RotHelper]::GetObject($pattern)
}

function Get-DteInstance([string[]]$progIds, [string]$rotPattern = "")
{
    foreach ($progId in $progIds)
    {
        try
        {
            $dte = [System.Runtime.InteropServices.Marshal]::GetActiveObject($progId)
            if ($null -ne $dte)
            {
                return $dte
            }
        }
        catch
        {
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($rotPattern))
    {
        return Get-DteFromRot $rotPattern
    }

    return $null
}

function Get-DteByProcessId([int]$processId)
{
    if ($processId -le 0)
    {
        return $null
    }

    $direct = Get-DteFromRot "VisualStudio\.DTE.*:$processId$"
    if ($null -ne $direct)
    {
        return $direct
    }

    [void](Get-DteFromRot "VisualStudio\.DTE")
    $rotNames = [RotHelper]::GetMonikerNames("VisualStudio\.DTE")
    foreach ($rotName in $rotNames)
    {
        if ($rotName -match "VisualStudio\.DTE.*:$processId$")
        {
            try
            {
                return [System.Runtime.InteropServices.Marshal]::BindToMoniker($rotName)
            }
            catch
            {
            }
        }
    }

    return $null
}

function Get-DteByRegistryRoot([string]$registryPattern)
{
    [void](Get-DteFromRot "VisualStudio\.DTE")
    $rotNames = [RotHelper]::GetMonikerNames("VisualStudio\.DTE")
    foreach ($rotName in $rotNames)
    {
        try
        {
            $dte = [System.Runtime.InteropServices.Marshal]::BindToMoniker($rotName)
            if ($dte.RegistryRoot -match $registryPattern)
            {
                return $dte
            }
        }
        catch
        {
        }
    }

    return $null
}

function Find-DteBySolutionHint([string]$hint)
{
    if ([string]::IsNullOrWhiteSpace($hint))
    {
        return $null
    }

    $trimmedHint = $hint.Trim()
    $normalizedHint = $trimmedHint.ToLowerInvariant()
    $normalizedHintNoExt = [System.IO.Path]::GetFileNameWithoutExtension($trimmedHint).ToLowerInvariant()

    $devenvProcs = @()
    try
    {
        $devenvProcs = @(Get-Process devenv -ErrorAction SilentlyContinue)
    }
    catch
    {
        $devenvProcs = @()
    }

    $exactMatch = $null
    $containsMatch = $null
    foreach ($proc in $devenvProcs)
    {
        $dte = Get-DteByProcessId $proc.Id
        if ($null -eq $dte)
        {
            continue
        }

        $solutionPath = Get-DteSolutionPathSafe $dte
        if ([string]::IsNullOrWhiteSpace($solutionPath))
        {
            continue
        }

        $solutionPathLower = $solutionPath.ToLowerInvariant()
        $solutionName = [System.IO.Path]::GetFileNameWithoutExtension($solutionPath).ToLowerInvariant()
        if ($solutionName -eq $normalizedHint -or $solutionName -eq $normalizedHintNoExt)
        {
            $exactMatch = $dte
            break
        }

        if ($solutionPathLower.Contains($normalizedHint) -and $null -eq $containsMatch)
        {
            $containsMatch = $dte
        }
    }

    if ($null -ne $exactMatch)
    {
        return $exactMatch
    }

    if ($null -ne $containsMatch)
    {
        return $containsMatch
    }

    return $null
}

function Start-DebuggingBySolutionHint([string]$hint)
{
    if ([string]::IsNullOrWhiteSpace($hint))
    {
        throw "Missing solution hint"
    }

    $trimmedHint = $hint.Trim()
    $dte = $null
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    while ((Get-Date) -lt $deadline)
    {
        $dte = Find-DteBySolutionHint $trimmedHint
        if ($null -ne $dte)
        {
            break
        }

        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $dte)
    {
        throw "Timed out waiting for Visual Studio instance for '$trimmedHint'"
    }

    $modeBefore = Get-DteDebugModeText $dte
    if ($modeBefore -eq "run" -or $modeBefore -eq "break")
    {
        return "ok: debugger already running for '$trimmedHint'"
    }

    $solutionPath = Get-DteSolutionPathSafe $dte
    $startupCandidate = ""
    if (-not [string]::IsNullOrWhiteSpace($solutionPath))
    {
        try
        {
            $solutionDir = Split-Path -Path $solutionPath -Parent
            $candidate = Join-Path $solutionDir "vsix\vsix.vcxproj"
            if (Test-Path $candidate)
            {
                $startupCandidate = $candidate
            }
        }
        catch
        {
        }
    }

    $attemptedStartupSet = $false
    if (-not [string]::IsNullOrWhiteSpace($startupCandidate))
    {
        try
        {
            Invoke-WithComRetry -timeoutSec $timeoutSeconds -action { $dte.Solution.SolutionBuild.StartupProjects = $startupCandidate }
            $attemptedStartupSet = $true
        }
        catch
        {
        }
    }

    try
    {
        Invoke-DteCommandWithReadiness -dte $dte -commandName "Debug.Start" -timeoutSec $timeoutSeconds -context $trimmedHint
    }
    catch
    {
        throw "Failed to execute Debug.Start for '$trimmedHint'"
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    while ((Get-Date) -lt $deadline)
    {
        $modeAfter = Get-DteDebugModeText $dte
        if ($modeAfter -eq "run" -or $modeAfter -eq "break")
        {
            return "ok: debugger started for '$trimmedHint'"
        }

        Start-Sleep -Milliseconds 200
    }

    if ($attemptedStartupSet)
    {
        throw "Debugger did not enter run/break mode for '$trimmedHint' (startup project tried: '$startupCandidate')"
    }

    throw "Debugger did not enter run/break mode for '$trimmedHint'"
}

function Get-DteRegistryRootSafe([object]$dte)
{
    if ($null -eq $dte)
    {
        return ""
    }

    try { return [string]$dte.RegistryRoot } catch { return "" }
}

function Get-DteSolutionPathSafe([object]$dte)
{
    if ($null -eq $dte)
    {
        return ""
    }

    try
    {
        if ($dte.Solution.IsOpen)
        {
            return [string]$dte.Solution.FullName
        }
    }
    catch
    {
    }

    return ""
}

function Test-IsExperimentalDte([object]$dte, [string]$suffix = "")
{
    $root = Get-DteRegistryRootSafe $dte
    if ([string]::IsNullOrWhiteSpace($root))
    {
        return $false
    }

    if (-not [string]::IsNullOrWhiteSpace($suffix) -and
        $root.EndsWith($suffix, [System.StringComparison]::OrdinalIgnoreCase))
    {
        return $true
    }

    return $root.EndsWith("Exp", [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-DevenvProcessIdByRootSuffix([string]$suffix)
{
    $ids = Get-DevenvProcessIdsByRootSuffix $suffix
    if ($null -eq $ids -or $ids.Count -eq 0)
    {
        return $null
    }

    return [int]$ids[0]
}

function Get-DevenvProcessIdsByRootSuffix([string]$suffix)
{
    if ([string]::IsNullOrWhiteSpace($suffix))
    {
        return @()
    }

    $escapedSuffix = [Regex]::Escape($suffix)
    $commandLinePattern = "(?i)(?:/|-)rootsuffix\s+$escapedSuffix(?:\s|$)"
    $processes = Get-CimInstance Win32_Process -Filter "Name='devenv.exe'"
    $ids = New-Object System.Collections.Generic.List[int]
    foreach ($process in $processes)
    {
        if ($process.CommandLine -match $commandLinePattern)
        {
            $ids.Add([int]$process.ProcessId) | Out-Null
        }
    }

    return @($ids)
}

function Ensure-ExperimentalInstanceRunning([string]$suffix, [int]$timeoutSec, [string]$solutionPath = "")
{
    $devenvPid = Get-DevenvProcessIdByRootSuffix $suffix
    if ($null -ne $devenvPid)
    {
        return $devenvPid
    }

    $devenvPath = Get-DevenvPath
    if (-not [string]::IsNullOrWhiteSpace($solutionPath))
    {
        Start-Process -FilePath $devenvPath -ArgumentList "`"$solutionPath`" /RootSuffix $suffix" | Out-Null
    }
    else
    {
        Start-Process -FilePath $devenvPath -ArgumentList "/RootSuffix $suffix" | Out-Null
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        $devenvPid = Get-DevenvProcessIdByRootSuffix $suffix
        if ($null -ne $devenvPid)
        {
            return $devenvPid
        }

        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for experimental devenv process (suffix '$suffix')"
}

function Stop-ExperimentalInstanceProcess([string]$suffix, [int]$timeoutSec)
{
    $devenvPids = @(Get-DevenvProcessIdsByRootSuffix $suffix)
    if ($devenvPids.Count -eq 0)
    {
        return $true
    }

    foreach ($devenvPid in $devenvPids)
    {
        try
        {
            Stop-Process -Id $devenvPid -Force -ErrorAction Stop
        }
        catch
        {
            return $false
        }
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        $stillThere = @(Get-DevenvProcessIdsByRootSuffix $suffix)
        if ($stillThere.Count -eq 0)
        {
            return $true
        }

        Start-Sleep -Milliseconds 250
    }

    return $false
}

function Start-DebuggerSession
{
    if ([string]::IsNullOrWhiteSpace($solution))
    {
        throw "-solution is required for start debugging"
    }

    if ([string]::IsNullOrWhiteSpace($startupProject))
    {
        throw "-startupProject is required for start debugging"
    }

    if ([string]::IsNullOrWhiteSpace($configuration) -or [string]::IsNullOrWhiteSpace($platform))
    {
        throw "-configuration and -platform are required for start debugging"
    }

    if (-not (Test-Path $solution))
    {
        throw "Solution not found: $solution"
    }

    if (-not (Test-Path $startupProject))
    {
        throw "Startup project not found: $startupProject"
    }

    $mainDte = Get-MainDteInstance
    if ($null -eq $mainDte)
    {
        throw "Failed to connect to main Visual Studio DTE"
    }

    if ($strictDteMatch -and (Test-IsExperimentalDte -dte $mainDte -suffix $experimentalSuffix))
    {
        throw "Resolved DTE appears to be experimental while starting main debugging"
    }

    if (-not (Wait-ForSolutionMatch -dte $mainDte -solutionPath $solution -timeoutSec $timeoutSeconds))
    {
        throw "Main instance did not load expected solution within $timeoutSeconds seconds: $solution"
    }

    [void](Stop-DteDebuggingIfActive -dte $mainDte -timeoutSec $timeoutSeconds)

    try
    {
        Invoke-WithComRetry -timeoutSec $timeoutSeconds -action { $mainDte.Solution.SolutionBuild.StartupProjects = $startupProject }
    }
    catch
    {
        throw "Failed to set startup project: $startupProject"
    }

    Set-DteSolutionConfiguration -dte $mainDte -configName $configuration -platformName $platform

    if ($clean)
    {
        try
        {
            Invoke-DteCommandWithReadiness -dte $mainDte -commandName "Build.CleanSolution" -timeoutSec $timeoutSeconds -context "main instance"
        }
        catch
        {
            throw "Failed to clean solution in main instance"
        }
    }

    try
    {
        Invoke-DteCommandWithReadiness -dte $mainDte -commandName "Debug.Start" -timeoutSec $timeoutSeconds -context "main instance"
    }
    catch
    {
        throw "Failed to execute Debug.Start in main instance"
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    $modeAfter = "unknown"
    while ((Get-Date) -lt $deadline)
    {
        $modeAfter = Get-DteDebugModeText $mainDte
        if ($modeAfter -eq "run" -or $modeAfter -eq "break")
        {
            break
        }

        Start-Sleep -Milliseconds 200
    }

    if ($modeAfter -ne "run" -and $modeAfter -ne "break")
    {
        throw "Main debugger did not enter run/break mode in time"
    }

    if ($waitForBreak)
    {
        $breakDeadline = (Get-Date).AddSeconds([Math]::Max(1, $breakTimeoutSeconds))
        while ((Get-Date) -lt $breakDeadline)
        {
            if ((Get-DteDebugModeText $mainDte) -eq "break")
            {
                break
            }

            Start-Sleep -Milliseconds 200
        }
    }
}

function Stop-DebuggerSession([switch]$forceKillExp)
{
    $effectiveSuffix = if ([string]::IsNullOrWhiteSpace($experimentalSuffix)) { "Exp" } else { $experimentalSuffix }

    if (-not $forceKillExp)
    {
        try
        {
            $stopResponse = Invoke-AgentMessage "stop debugging"
            if (-not [string]::IsNullOrWhiteSpace($stopResponse))
            {
                return $stopResponse
            }
        }
        catch
        {
        }
    }

    if (-not (Stop-ExperimentalInstanceProcess $effectiveSuffix $timeoutSeconds))
    {
        throw "Failed to stop experimental instance process for suffix '$effectiveSuffix'"
    }

    return "ok: stopped debugging (killExp)"
}

function Stop-MainDebuggerSession
{
    $mainDte = Get-MainDteInstance
    if ($null -eq $mainDte)
    {
        throw "Failed to connect to main Visual Studio DTE"
    }

    $result = Stop-DteDebuggingIfActive -dte $mainDte -timeoutSec $timeoutSeconds
    return "ok: main stop result=$result"
}

function Stop-ExperimentalDebuggerSession
{
    $expDte = Get-ExperimentalDteInstance
    if ($null -eq $expDte)
    {
        return (Stop-DebuggerSession -forceKillExp)
    }

    $result = Stop-DteDebuggingIfActive -dte $expDte -timeoutSec $timeoutSeconds
    return "ok: exp stop result=$result"
}

function Get-ExperimentalDteInstance
{
    $effectiveSuffix = if ([string]::IsNullOrWhiteSpace($experimentalSuffix)) { "Exp" } else { $experimentalSuffix }
    $progIds = @("VisualStudio.DTE.17.0:$effectiveSuffix", "VisualStudio.DTE.18.0:$effectiveSuffix", "VisualStudio.DTE:$effectiveSuffix")

    $dte = Get-DteInstance -progIds $progIds -rotPattern "VisualStudio\.DTE.*:$effectiveSuffix"
    if ($null -ne $dte)
    {
        return $dte
    }

    $devenvPid = Get-DevenvProcessIdByRootSuffix $effectiveSuffix
    if ($null -ne $devenvPid)
    {
        $dteByPid = Get-DteByProcessId $devenvPid
        if ($null -ne $dteByPid)
        {
            return $dteByPid
        }
    }

    return (Get-DteByRegistryRoot ("(?i)" + [Regex]::Escape($effectiveSuffix) + "$"))
}

function Get-MainDteInstance
{
    $progIds = @("VisualStudio.DTE", "VisualStudio.DTE.17.0", "VisualStudio.DTE.18.0")
    $dte = Get-DteInstance -progIds $progIds -rotPattern "VisualStudio\.DTE"
    if ($null -ne $dte -and -not (Test-IsExperimentalDte -dte $dte -suffix $experimentalSuffix))
    {
        return $dte
    }

    return (Get-DteByRegistryRoot "VisualStudio\\1[78]\.0")
}

function Wait-ForSolutionMatch([object]$dte, [string]$solutionPath, [int]$timeoutSec)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            $isOpen = Invoke-WithComRetryValue -timeoutSec 2 -action { [bool]$dte.Solution.IsOpen }
            if ($isOpen)
            {
                $fullName = Invoke-WithComRetryValue -timeoutSec 2 -action { [string]$dte.Solution.FullName }
                if (-not [string]::IsNullOrWhiteSpace($fullName) -and $fullName.ToLowerInvariant() -eq $solutionPath.ToLowerInvariant())
                {
                    return $true
                }
            }
        }
        catch
        {
        }

        Start-Sleep -Milliseconds 250
    }

    return $false
}

function Get-DteDebugModeText([object]$dte)
{
    try
    {
        $debugger = Invoke-WithComRetryValue -timeoutSec 2 -action { $dte.Debugger }
        if ($null -eq $debugger)
        {
            return "unknown"
        }

        $mode = [int](Invoke-WithComRetryValue -timeoutSec 2 -action { $debugger.CurrentMode })
        switch ($mode)
        {
            1 { return "design" }
            2 { return "break" }
            3 { return "run" }
            default { return "unknown($mode)" }
        }
    }
    catch
    {
        return "unknown"
    }
}

function Wait-DteCommandAvailable([object]$dte, [string]$commandName, [int]$timeoutSec = 10)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            $command = $dte.Commands.Item($commandName, 0)
            if ($null -ne $command)
            {
                try
                {
                    if ([bool]$command.IsAvailable)
                    {
                        return $true
                    }
                }
                catch
                {
                    return $true
                }
            }
        }
        catch
        {
        }

        Start-Sleep -Milliseconds 200
    }

    return $false
}

function Invoke-DteCommandWithReadiness([object]$dte, [string]$commandName, [string]$args = "", [int]$timeoutSec = 10, [string]$context = "instance")
{
    if (-not (Wait-DteCommandAvailable -dte $dte -commandName $commandName -timeoutSec $timeoutSec))
    {
        throw "Command '$commandName' not available in $context within $timeoutSec seconds"
    }

    Invoke-WithComRetry -timeoutSec $timeoutSec -action { $dte.ExecuteCommand($commandName, $args) }
}

function Stop-DteDebuggingIfActive([object]$dte, [int]$timeoutSec)
{
    $modeBefore = Get-DteDebugModeText $dte
    if ($modeBefore -ne "run" -and $modeBefore -ne "break")
    {
        return "not-running"
    }

    try
    {
        Invoke-DteCommandWithReadiness -dte $dte -commandName "Debug.StopDebugging" -timeoutSec $timeoutSec -context "DTE"
    }
    catch
    {
        return "stop-failed"
    }

    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        $modeNow = Get-DteDebugModeText $dte
        if ($modeNow -eq "design")
        {
            return "stopped"
        }

        Start-Sleep -Milliseconds 200
    }

    return "stop-timeout"
}

function Invoke-WithComRetry([scriptblock]$action, [int]$timeoutSec = 10, [int]$delayMs = 250)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            & $action
            return
        }
        catch
        {
            $hr = 0
            try { $hr = [int]$_.Exception.HResult } catch {}

            if ($hr -eq -2147418111 -or $hr -eq -2147417846)
            {
                Start-Sleep -Milliseconds $delayMs
                continue
            }

            throw
        }
    }

    throw "COM call timed out after ${timeoutSec}s"
}

function Invoke-WithComRetryValue([scriptblock]$action, [int]$timeoutSec = 10, [int]$delayMs = 250)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            return (& $action)
        }
        catch
        {
            $hr = 0
            try { $hr = [int]$_.Exception.HResult } catch {}

            if ($hr -eq -2147418111 -or $hr -eq -2147417846)
            {
                Start-Sleep -Milliseconds $delayMs
                continue
            }

            throw
        }
    }

    throw "COM value call timed out after ${timeoutSec}s"
}

function Set-DteSolutionConfiguration([object]$dte, [string]$configName, [string]$platformName)
{
    if ([string]::IsNullOrWhiteSpace($configName) -or [string]::IsNullOrWhiteSpace($platformName))
    {
        return
    }

    $target = "$configName|$platformName"
    $solutionConfigurations = $dte.Solution.SolutionBuild.SolutionConfigurations
    foreach ($solutionConfiguration in $solutionConfigurations)
    {
        $name = [string]$solutionConfiguration.Name
        if ($name -eq $target)
        {
            $solutionConfiguration.Activate()
            return
        }
    }
}

function Start-ExperimentalDebuggerSession([switch]$forceLaunch)
{
    $effectiveExpConfiguration = $expConfiguration
    if ([string]::IsNullOrWhiteSpace($effectiveExpConfiguration))
    {
        $effectiveExpConfiguration = $configuration
    }

    $effectiveExpPlatform = $expPlatform
    if ([string]::IsNullOrWhiteSpace($effectiveExpPlatform))
    {
        $effectiveExpPlatform = $platform
    }

    if ([string]::IsNullOrWhiteSpace($effectiveExpConfiguration) -or [string]::IsNullOrWhiteSpace($effectiveExpPlatform))
    {
        throw "-expConfiguration and -expPlatform (or -configuration and -platform fallback) are required for start exp debugging"
    }

    $effectiveSuffix = if ([string]::IsNullOrWhiteSpace($experimentalSuffix)) { "Exp" } else { $experimentalSuffix }
    $allowLaunch = $forceLaunch.IsPresent -or $launchExpIfMissing.IsPresent
    if ($allowLaunch)
    {
        $solutionToLaunch = if ([string]::IsNullOrWhiteSpace($expSolution)) { "" } else { $expSolution }
        [void](Ensure-ExperimentalInstanceRunning -suffix $effectiveSuffix -timeoutSec $timeoutSeconds -solutionPath $solutionToLaunch)
    }

    $expDte = $null
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    while ((Get-Date) -lt $deadline)
    {
        $expDte = Get-ExperimentalDteInstance
        if ($null -ne $expDte)
        {
            break
        }

        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $expDte)
    {
        if ($allowLaunch)
        {
            throw "Could not resolve experimental DTE instance"
        }

        throw "Experimental instance not running. Start main debugging first or use 'start exp debugging force'"
    }

    if ($strictDteMatch -and -not (Test-IsExperimentalDte -dte $expDte -suffix $effectiveSuffix))
    {
        throw "Resolved DTE does not appear to be experimental"
    }

    $openedSolutionBefore = ""
    try
    {
        if ($expDte.Solution.IsOpen)
        {
            $openedSolutionBefore = [string]$expDte.Solution.FullName
        }
    }
    catch
    {
    }

    $modeBefore = Get-DteDebugModeText $expDte

    $alreadyExpectedSolution = $true
    if (-not [string]::IsNullOrWhiteSpace($expSolution))
    {
        $alreadyExpectedSolution = (-not [string]::IsNullOrWhiteSpace($openedSolutionBefore) -and
            $openedSolutionBefore.ToLowerInvariant() -eq $expSolution.ToLowerInvariant())
    }

    if ($modeBefore -eq "run" -and $alreadyExpectedSolution -and -not $forceLaunch.IsPresent)
    {
        return "ok: experimental debugger already running (modeBefore=run, solutionChanged=False, modeAfter=run, agentReady=false)"
    }

    $stopResult = Stop-DteDebuggingIfActive -dte $expDte -timeoutSec $timeoutSeconds

    if (-not [string]::IsNullOrWhiteSpace($expSolution))
    {
        $currentExpSolution = Get-DteSolutionPathSafe $expDte
        $alreadyExpected = (-not [string]::IsNullOrWhiteSpace($currentExpSolution) -and
            $currentExpSolution.ToLowerInvariant() -eq $expSolution.ToLowerInvariant())

        if (-not $alreadyExpected)
        {
            try
            {
                Invoke-WithComRetry -timeoutSec $timeoutSeconds -action { $expDte.Solution.Open($expSolution) }
            }
            catch
            {
                throw "Failed to open experimental solution: $expSolution"
            }
        }

        if (-not (Wait-ForSolutionMatch -dte $expDte -solutionPath $expSolution -timeoutSec $timeoutSeconds))
        {
            throw "Experimental instance did not load expected solution within $timeoutSeconds seconds: $expSolution"
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($expStartupProject))
    {
        if (-not (Test-Path $expStartupProject))
        {
            throw "Experimental startup project not found: $expStartupProject"
        }

        $startupCandidates = New-Object System.Collections.Generic.List[string]
        if (-not [string]::IsNullOrWhiteSpace($expSolution))
        {
            try
            {
                $solutionDir = [System.IO.Path]::GetDirectoryName($expSolution)
                $solutionUri = New-Object System.Uri(($solutionDir.TrimEnd('\\') + '\\'))
                $projectUri = New-Object System.Uri($expStartupProject)
                $relative = [System.Uri]::UnescapeDataString($solutionUri.MakeRelativeUri($projectUri).ToString()).Replace('/', '\\')
                if (-not [string]::IsNullOrWhiteSpace($relative))
                {
                    $startupCandidates.Add($relative) | Out-Null
                }
            }
            catch
            {
            }
        }

        $startupCandidates.Add([System.IO.Path]::GetFileNameWithoutExtension($expStartupProject)) | Out-Null
        $startupCandidates.Add($expStartupProject) | Out-Null

        $startupSet = $false
        $startupToken = ""
        foreach ($candidate in $startupCandidates)
        {
            if ([string]::IsNullOrWhiteSpace($candidate))
            {
                continue
            }

            try
            {
                Invoke-WithComRetry -timeoutSec $timeoutSeconds -action { $expDte.Solution.SolutionBuild.StartupProjects = $candidate }
                $startupSet = $true
                $startupToken = $candidate
                break
            }
            catch
            {
            }
        }

        if (-not $startupSet)
        {
            throw "Failed to set experimental startup project: $expStartupProject"
        }

        try
        {
            $actualStartup = Invoke-WithComRetryValue -timeoutSec $timeoutSeconds -action { $expDte.Solution.SolutionBuild.StartupProjects }
            $actualText = [string]$actualStartup
            $expectedName = [System.IO.Path]::GetFileNameWithoutExtension($expStartupProject).ToLowerInvariant()
            if ([string]::IsNullOrWhiteSpace($actualText) -or
                $actualText.ToLowerInvariant() -notlike "*$expectedName*")
            {
                throw "Startup token mismatch"
            }
        }
        catch
        {
            throw "Experimental startup project did not apply: expected '$startupToken' from '$expStartupProject'"
        }
    }

    Set-DteSolutionConfiguration -dte $expDte -configName $effectiveExpConfiguration -platformName $effectiveExpPlatform

    try
    {
        Invoke-DteCommandWithReadiness -dte $expDte -commandName "Debug.Start" -timeoutSec $timeoutSeconds -context "experimental instance"
    }
    catch
    {
        throw "Failed to execute Debug.Start in experimental instance"
    }

    $modeAfter = "unknown"
    $deadlineAfterStart = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    while ((Get-Date) -lt $deadlineAfterStart)
    {
        $modeAfter = Get-DteDebugModeText $expDte
        if ($modeAfter -eq "run" -or $modeAfter -eq "break")
        {
            break
        }

        Start-Sleep -Milliseconds 200
    }

    if ($modeAfter -ne "run" -and $modeAfter -ne "break")
    {
        throw "Experimental debugger did not enter run/break mode in time"
    }

    $agentStatus = Wait-AgentReady -timeoutSec ([Math]::Min(10, [Math]::Max(1, $timeoutSeconds)))
    $agentReady = (-not [string]::IsNullOrWhiteSpace($agentStatus))

    $solutionChanged = $false
    if (-not [string]::IsNullOrWhiteSpace($expSolution) -and -not [string]::IsNullOrWhiteSpace($openedSolutionBefore))
    {
        $solutionChanged = ($openedSolutionBefore.ToLowerInvariant() -ne $expSolution.ToLowerInvariant())
    }
    elseif (-not [string]::IsNullOrWhiteSpace($expSolution))
    {
        $solutionChanged = $true
    }

    if ($agentReady)
    {
        return "ok: experimental debugger started (modeBefore=$modeBefore, stopResult=$stopResult, solutionChanged=$solutionChanged, modeAfter=$modeAfter, agentReady=true, agentStatus='$agentStatus')"
    }

    return "ok: experimental debugger started (modeBefore=$modeBefore, stopResult=$stopResult, solutionChanged=$solutionChanged, modeAfter=$modeAfter, agentReady=false)"
}

function Get-QueueMessages([string[]]$rawQueue, [string]$fallbackMessage)
{
    $messages = New-Object System.Collections.Generic.List[string]

    if ($rawQueue -and $rawQueue.Count -gt 0)
    {
        foreach ($entry in $rawQueue)
        {
            if ([string]::IsNullOrWhiteSpace($entry))
            {
                continue
            }

            $parts = $entry -split ','
            foreach ($part in $parts)
            {
                $trimmedPart = $part.Trim()
                if (-not [string]::IsNullOrWhiteSpace($trimmedPart))
                {
                    $messages.Add($trimmedPart)
                }
            }
        }
    }

    if ($messages.Count -eq 0 -and -not [string]::IsNullOrWhiteSpace($fallbackMessage))
    {
        $messages.Add($fallbackMessage.Trim())
    }

    return $messages
}

function Invoke-AgentMessage([string]$text)
{
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect($connectTimeoutMs)

    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    $writer.WriteLine($text)

    $reader = New-Object System.IO.StreamReader($pipe)
    $response = $reader.ReadLine()

    try { $reader.Dispose() } catch {}
    try { $writer.Dispose() } catch {}
    try { $pipe.Dispose() } catch {}

    return $response
}

function Test-StatusCondition([string]$status, [string]$mode, [string]$needle)
{
    if ([string]::IsNullOrWhiteSpace($status))
    {
        return $false
    }

    switch ($mode)
    {
        "run" { return ($status -like "*vs run mode*" -or $status -like "*running*") }
        "break" { return ($status -like "*vs break mode*" -or $status -like "*paused trap*") }
        "paused" { return ($status -like "*paused trap*") }
        "contains" { return ($status -like "*$needle*") }
        default { return $false }
    }
}

function Wait-Status([string]$mode, [string]$needle, [int]$timeoutSeconds = 20, [int]$pollDelayMs = 250)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    $lastStatus = ""
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            $lastStatus = Invoke-AgentMessage "status"
            if (Test-StatusCondition -status $lastStatus -mode $mode -needle $needle)
            {
                return $lastStatus
            }
        }
        catch
        {
            $lastStatus = "<status read failed>"
        }

        Start-Sleep -Milliseconds $pollDelayMs
    }

    throw "wait status timed out (mode=$mode, timeout=${timeoutSeconds}s, last='$lastStatus')"
}

function Wait-AgentReady([int]$timeoutSec = 10)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        try
        {
            $status = Invoke-AgentMessage "status"
            if (-not [string]::IsNullOrWhiteSpace($status))
            {
                return $status
            }
        }
        catch
        {
        }

        Start-Sleep -Milliseconds 250
    }

    return $null
}

function Open-ExperimentalSolution([switch]$forceLaunch)
{
    if ([string]::IsNullOrWhiteSpace($expSolution))
    {
        throw "-expSolution is required for open exp solution"
    }

    if (-not (Test-Path $expSolution))
    {
        throw "Experimental solution not found: $expSolution"
    }

    $effectiveSuffix = if ([string]::IsNullOrWhiteSpace($experimentalSuffix)) { "Exp" } else { $experimentalSuffix }
    if ($launchExpIfMissing -or $forceLaunch)
    {
        [void](Ensure-ExperimentalInstanceRunning -suffix $effectiveSuffix -timeoutSec $timeoutSeconds -solutionPath $expSolution)
    }

    $expDte = $null
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSeconds))
    while ((Get-Date) -lt $deadline)
    {
        $expDte = Get-ExperimentalDteInstance
        if ($null -ne $expDte)
        {
            break
        }

        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $expDte)
    {
        throw "Experimental instance not running. Start main debugging first or use -launchExpIfMissing"
    }

    if ($strictDteMatch -and -not (Test-IsExperimentalDte -dte $expDte -suffix $effectiveSuffix))
    {
        throw "Resolved DTE does not appear to be experimental"
    }

    $currentSolution = Get-DteSolutionPathSafe $expDte
    if (-not [string]::IsNullOrWhiteSpace($currentSolution) -and
        $currentSolution.ToLowerInvariant() -eq $expSolution.ToLowerInvariant())
    {
        return "ok: exp solution already open"
    }

    try
    {
        Invoke-WithComRetry -timeoutSec $timeoutSeconds -action { $expDte.Solution.Open($expSolution) }
    }
    catch
    {
        throw "Failed to open experimental solution: $expSolution"
    }

    if (-not (Wait-ForSolutionMatch -dte $expDte -solutionPath $expSolution -timeoutSec $timeoutSeconds))
    {
        throw "Experimental instance did not load expected solution within $timeoutSeconds seconds: $expSolution"
    }

    return "ok: exp solution opened"
}

function Invoke-AgentMessageSafe([string]$text)
{
    try
    {
        $response = Invoke-AgentMessage $text
        if ([string]::IsNullOrWhiteSpace($response))
        {
            return "<empty>"
        }

        return $response
    }
    catch
    {
        return "<error: $($_.Exception.Message)>"
    }
}

function Get-SessionDiagnostics
{
    $effectiveSuffix = if ([string]::IsNullOrWhiteSpace($experimentalSuffix)) { "Exp" } else { $experimentalSuffix }
    $expPid = Get-DevenvProcessIdByRootSuffix $effectiveSuffix
    $expRunning = ($null -ne $expPid)

    $mainDte = Get-MainDteInstance
    $mainMode = if ($null -ne $mainDte) { Get-DteDebugModeText $mainDte } else { "missing" }
    $mainRoot = Get-DteRegistryRootSafe $mainDte
    $mainSolution = Get-DteSolutionPathSafe $mainDte

    $expDte = Get-ExperimentalDteInstance
    $expMode = if ($null -ne $expDte) { Get-DteDebugModeText $expDte } else { "missing" }
    $expRoot = Get-DteRegistryRootSafe $expDte
    $expSolutionPath = Get-DteSolutionPathSafe $expDte

    $statusResponse = Invoke-AgentMessageSafe "status"
    $queueListResponse = Invoke-AgentMessageSafe "queue list"

    $statusChannel = "unknown"
    if ($statusResponse -like "ok:*")
    {
        $statusChannel = "available"
    }
    elseif ($statusResponse -like "error:*")
    {
        $statusChannel = "error"
    }

    $queueChannel = "unknown"
    if ($queueListResponse -like "ok: pending queue*")
    {
        $queueChannel = "available"
    }
    elseif ($queueListResponse -like "error:*")
    {
        $queueChannel = "error"
    }

    $activeProcessHint = $false
    if ($statusResponse -like "ok: vs run mode*" -or
        $statusResponse -like "ok: vs break mode*" -or
        $statusResponse -like "ok: paused trap*" -or
        $statusResponse -like "ok: running*" -or
        $statusResponse -like "ok: trap*" )
    {
        $activeProcessHint = $true
    }

    $canStatus = ($statusChannel -eq "available")
    $canQueue = ($queueChannel -eq "available")
    $canSetTrap = ($canQueue -and $activeProcessHint)
    $canReadMem = ($canQueue -and $activeProcessHint)

    $pendingCount = "unknown"
    if ($queueListResponse -match "count=(\d+)")
    {
        $pendingCount = $Matches[1]
    }
    elseif ($queueListResponse -like "ok: pending queue empty*")
    {
        $pendingCount = "0"
    }

    $expPidText = if ($expRunning) { [string]$expPid } else { "none" }
    return "ok: diag session mainMode=$mainMode expMode=$expMode expRunning=$expRunning expPid=$expPidText mainRoot='$mainRoot' expRoot='$expRoot' mainSolution='$mainSolution' expSolution='$expSolutionPath' statusChannel=$statusChannel queueChannel=$queueChannel activeProcessHint=$activeProcessHint canStatus=$canStatus canQueue=$canQueue canSetTrap=$canSetTrap canReadMem=$canReadMem pendingCount=$pendingCount status='$statusResponse' queue='$queueListResponse'"
}

function Get-AllOpenSolutionsInfo
{
    $dteEntries = New-Object System.Collections.Generic.List[object]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'

    $mainDte = Get-MainDteInstance
    if ($null -ne $mainDte)
    {
        [void]$dteEntries.Add($mainDte)
    }

    $expDte = Get-ExperimentalDteInstance
    if ($null -ne $expDte)
    {
        [void]$dteEntries.Add($expDte)
    }

    try
    {
        $devenvProcs = Get-Process devenv -ErrorAction SilentlyContinue
        foreach ($proc in $devenvProcs)
        {
            $dteByPid = Get-DteByProcessId $proc.Id
            if ($null -ne $dteByPid)
            {
                [void]$dteEntries.Add($dteByPid)
            }
        }
    }
    catch
    {
    }

    $items = New-Object System.Collections.Generic.List[string]
    $index = 0
    foreach ($dte in $dteEntries)
    {
        try
        {
            if ($null -eq $dte)
            {
                continue
            }

            $root = Get-DteRegistryRootSafe $dte
            $solutionPath = Get-DteSolutionPathSafe $dte
            $mode = Get-DteDebugModeText $dte
            $key = "$root|$solutionPath|$mode"
            if (-not $seen.Add($key))
            {
                continue
            }

            $isOpen = -not [string]::IsNullOrWhiteSpace($solutionPath)
            $openText = if ($isOpen) { "open" } else { "no-solution" }
            $solutionText = if ($isOpen) { $solutionPath } else { "<none>" }
            $items.Add("#$index mode=$mode root='$root' state=$openText solution='$solutionText'") | Out-Null
            $index += 1
        }
        catch
        {
        }
    }

    if ($items.Count -eq 0)
    {
        return "ok: solutions none"
    }

    return "ok: solutions count=$($items.Count) | " + ($items -join " | ")
}

function Get-RootSuffixFromRegistryRoot([string]$registryRoot)
{
    if ([string]::IsNullOrWhiteSpace($registryRoot))
    {
        return ""
    }

    if ($registryRoot -match "1[78]\.0_[A-Za-z0-9]{8}(?<suffix>[^\\]*)$")
    {
        return [string]$Matches["suffix"]
    }

    return ""
}

function Get-DiscoveredTargets
{
    $targets = @()
    $seenPid = @{}

    try
    {
        $devenvProcs = Get-Process devenv -ErrorAction SilentlyContinue
        foreach ($proc in $devenvProcs)
        {
            $processId = [int]$proc.Id
            if ($seenPid.ContainsKey($processId))
            {
                continue
            }
            $seenPid[$processId] = $true

            $dte = Get-DteByProcessId $processId
            $registryRoot = ""
            $solutionPath = ""
            $mode = "unknown"

            if ($null -ne $dte)
            {
                $registryRoot = Get-DteRegistryRootSafe $dte
                $solutionPath = Get-DteSolutionPathSafe $dte
                $mode = Get-DteDebugModeText $dte
            }

            $suffix = Get-RootSuffixFromRegistryRoot $registryRoot
            $state = if ([string]::IsNullOrWhiteSpace($solutionPath)) { "no-solution" } else { "open" }
            $solutionText = if ([string]::IsNullOrWhiteSpace($solutionPath)) { "<none>" } else { $solutionPath }
            $targetIdPrefix = if ([string]::IsNullOrWhiteSpace($suffix)) { "main" } else { $suffix.ToLowerInvariant() }
            $targetId = "$targetIdPrefix-$processId"

            $targets += [pscustomobject]@{
                targetId = $targetId
                pid = $processId
                suffix = $suffix
                registryRoot = $registryRoot
                solutionPath = $solutionText
                debugMode = $mode
                state = $state
            }
        }
    }
    catch
    {
    }

    return $targets
}

function Get-DiscoveredTargetsInfo([switch]$asJson)
{
    $targets = @(Get-DiscoveredTargets)
    if ($targets.Count -eq 0)
    {
        return "ok: targets none"
    }

    if ($asJson)
    {
        $payload = [pscustomobject]@{
            count = $targets.Count
            targets = $targets
        }

        return "ok: targets json " + ($payload | ConvertTo-Json -Depth 5 -Compress)
    }

    $items = New-Object System.Collections.Generic.List[string]
    $index = 0
    foreach ($target in $targets)
    {
        $suffixText = if ([string]::IsNullOrWhiteSpace($target.suffix)) { "<main>" } else { $target.suffix }
        $items.Add("#$index id='$($target.targetId)' pid=$($target.pid) suffix='$suffixText' mode=$($target.debugMode) state=$($target.state) root='$($target.registryRoot)' solution='$($target.solutionPath)'") | Out-Null
        $index += 1
    }

    return "ok: targets count=$($targets.Count) | " + ($items -join " | ")
}

function Get-PreferredDteInstance
{
    $expDte = Get-ExperimentalDteInstance
    if ($null -ne $expDte)
    {
        return $expDte
    }

    return (Get-MainDteInstance)
}

function Get-DteBreakLocation([object]$dte)
{
    if ($null -eq $dte)
    {
        return "unknown"
    }

    try
    {
        $frame = $dte.Debugger.CurrentStackFrame
        if ($null -eq $frame)
        {
            return "unknown"
        }

        $functionName = ""
        $fileName = ""
        $line = 0
        try { $functionName = [string]$frame.FunctionName } catch {}
        try { $fileName = [string]$frame.FileName } catch {}
        try { $line = [int]$frame.LineNumber } catch {}

        if (-not [string]::IsNullOrWhiteSpace($fileName) -and $line -gt 0)
        {
            if (-not [string]::IsNullOrWhiteSpace($functionName))
            {
                return "${fileName}:$line in $functionName"
            }

            return "${fileName}:$line"
        }

        if (-not [string]::IsNullOrWhiteSpace($functionName))
        {
            return $functionName
        }
    }
    catch
    {
    }

    return "unknown"
}

function Get-DteStackSummary([object]$dte, [int]$maxFrames = 8)
{
    if ($null -eq $dte)
    {
        return "error: no DTE instance"
    }

    if ($maxFrames -le 0)
    {
        $maxFrames = 8
    }

    if ($maxFrames -gt 32)
    {
        $maxFrames = 32
    }

    if ((Get-DteDebugModeText $dte) -ne "break")
    {
        return "error: stack requires break mode"
    }

    try
    {
        $frames = $dte.Debugger.CurrentThread.StackFrames
        if ($null -eq $frames)
        {
            return "error: no stack frames"
        }

        $parts = New-Object System.Collections.Generic.List[string]
        $index = 0
        foreach ($frame in $frames)
        {
            if ($index -ge $maxFrames)
            {
                break
            }

            $functionName = ""
            $fileName = ""
            $line = 0
            try { $functionName = [string]$frame.FunctionName } catch {}
            try { $fileName = [string]$frame.FileName } catch {}
            try { $line = [int]$frame.LineNumber } catch {}

            if ([string]::IsNullOrWhiteSpace($functionName))
            {
                $functionName = "<unknown>"
            }

            if (-not [string]::IsNullOrWhiteSpace($fileName) -and $line -gt 0)
            {
                $parts.Add("#$index $functionName @ ${fileName}:$line") | Out-Null
            }
            else
            {
                $parts.Add("#$index $functionName") | Out-Null
            }

            $index += 1
        }

        if ($parts.Count -eq 0)
        {
            return "error: no stack frames"
        }

        return "ok: stack " + ($parts -join " | ")
    }
    catch
    {
        return "error: failed to read stack frames"
    }
}

function Wait-DteBreak([object]$dte, [int]$timeoutSec = 20)
{
    $deadline = (Get-Date).AddSeconds([Math]::Max(1, $timeoutSec))
    while ((Get-Date) -lt $deadline)
    {
        if ((Get-DteDebugModeText $dte) -eq "break")
        {
            $location = Get-DteBreakLocation $dte
            return "ok: break mode location=$location"
        }

        Start-Sleep -Milliseconds 200
    }

    throw "wait break timed out (${timeoutSec}s)"
}

function Invoke-QueueCommand([string]$queuedMessage)
{
    $text = $queuedMessage.Trim()
    if ([string]::IsNullOrWhiteSpace($text) -or $text.StartsWith("#"))
    {
        return "<skipped>"
    }

    if ($text -ieq "start debugging")
    {
        Start-DebuggerSession
        return "ok: debugger started"
    }

    if ($text -match "^start\s+debugging\s+(.+)$")
    {
        $solutionHint = $Matches[1].Trim()
        if ([string]::IsNullOrWhiteSpace($solutionHint))
        {
            throw "start debugging requires a solution hint"
        }

        return (Start-DebuggingBySolutionHint $solutionHint)
    }

    if ($text -match "^use\s+exp\s+solution\s+(.+)$")
    {
        $script:expSolution = $Matches[1].Trim()
        return "ok: expSolution set to '$script:expSolution'"
    }

    if ($text -match "^use\s+exp\s+startup\s+(.+)$")
    {
        $candidatePath = $Matches[1].Trim()
        if (-not (Test-Path $candidatePath))
        {
            throw "Experimental startup project not found: $candidatePath"
        }

        $script:expStartupProject = $candidatePath
        return "ok: expStartupProject set to '$script:expStartupProject'"
    }

    if ($text -match "^use\s+exp\s+suffix\s+(.+)$")
    {
        $script:experimentalSuffix = $Matches[1].Trim()
        return "ok: experimentalSuffix set to '$script:experimentalSuffix'"
    }

    if ($text -ieq "start exp debugging")
    {
        return (Start-ExperimentalDebuggerSession)
    }

    if ($text -ieq "open exp solution")
    {
        return (Open-ExperimentalSolution)
    }

    if ($text -ieq "open exp solution force")
    {
        return (Open-ExperimentalSolution -forceLaunch)
    }

    if ($text -ieq "start exp debugging force")
    {
        return (Start-ExperimentalDebuggerSession -forceLaunch)
    }

    if ($text -match "^preset\s+restart\s+arm\s+(.+)$")
    {
        $symbol = $Matches[1].Trim()
        if ([string]::IsNullOrWhiteSpace($symbol))
        {
            throw "preset restart arm requires a symbol"
        }

        $steps = @(
            "stop exp debugging",
            "stop main debugging",
            "start debugging",
            "start exp debugging",
            "set trap symbol $symbol",
            "status"
        )

        $last = ""
        foreach ($step in $steps)
        {
            $last = Invoke-QueueCommand $step
        }

        return "ok: preset restart arm completed symbol=$symbol final='$last'"
    }

    if ($text -ieq "preset health")
    {
        $diag = Get-SessionDiagnostics
        $status = Invoke-AgentMessageSafe "status"
        $queueInfo = Invoke-AgentMessageSafe "queue list"
        return "ok: preset health diag='$diag' status='$status' queue='$queueInfo'"
    }

    if ($text -ieq "stop debugging" -or $text -ieq "stop")
    {
        return (Stop-DebuggerSession)
    }

    if ($text -ieq "stop main debugging" -or $text -ieq "stop main")
    {
        return (Stop-MainDebuggerSession)
    }

    if ($text -ieq "stop exp debugging" -or $text -ieq "stop exp")
    {
        return (Stop-ExperimentalDebuggerSession)
    }

    if ($text -ieq "stop debugging force" -or $text -ieq "stop force")
    {
        return (Stop-DebuggerSession -forceKillExp)
    }

    if ($text -match "^wait status\s+(.+)$")
    {
        $spec = $Matches[1].Trim()

        if ($spec -match "^(run|break|paused)(?:\s+(\d+))?$")
        {
            $mode = $Matches[1].ToLowerInvariant()
            $timeout = if ($Matches[2]) { [int]$Matches[2] } else { 20 }
            return Wait-Status -mode $mode -needle "" -timeoutSeconds $timeout
        }

        if ($spec -match "^contains\s+(.+?)(?:\s+(\d+))?$")
        {
            $needle = $Matches[1].Trim()
            $timeout = if ($Matches[2]) { [int]$Matches[2] } else { 20 }
            return Wait-Status -mode "contains" -needle $needle -timeoutSeconds $timeout
        }

        throw "Invalid wait status syntax: '$text'"
    }

    if ($text -match "^wait break(?:\s+(\d+))?$")
    {
        $timeout = if ($Matches[1]) { [int]$Matches[1] } else { 20 }
        $dte = Get-PreferredDteInstance
        if ($null -ne $dte)
        {
            return (Wait-DteBreak -dte $dte -timeoutSec $timeout)
        }

        return (Wait-Status -mode "break" -needle "" -timeoutSeconds $timeout)
    }

    if ($text -ieq "where")
    {
        $dte = Get-PreferredDteInstance
        if ($null -ne $dte -and (Get-DteDebugModeText $dte) -eq "break")
        {
            $location = Get-DteBreakLocation $dte
            return "ok: where $location"
        }

        return (Invoke-AgentMessage "where")
    }

    if ($text -match "^stack(?:\s+(\d+))?$")
    {
        $maxFrames = if ($Matches[1]) { [int]$Matches[1] } else { 8 }
        $dte = Get-PreferredDteInstance
        if ($null -eq $dte)
        {
            return "error: no DTE instance"
        }

        return (Get-DteStackSummary -dte $dte -maxFrames $maxFrames)
    }

    if ($text -ieq "diag session" -or $text -ieq "diag")
    {
        return (Get-SessionDiagnostics)
    }

    if ($text -ieq "list solutions" -or $text -ieq "diag solutions")
    {
        return (Get-AllOpenSolutionsInfo)
    }

    if ($text -ieq "list targets" -or $text -ieq "targets list")
    {
        return (Get-DiscoveredTargetsInfo)
    }

    if ($text -ieq "list targets json" -or $text -ieq "targets list json")
    {
        return (Get-DiscoveredTargetsInfo -asJson)
    }

    if ($text -match "^sleep\s+(\d+)$")
    {
        Start-Sleep -Milliseconds ([int]$Matches[1])
        return "ok: slept ${Matches[1]}ms"
    }

    return Invoke-AgentMessage $text
}

function Find-AgentCtlExe
{
    $candidates = @(
        (Join-Path $PSScriptRoot "Temp\bh_agent_debugger_x64_Debug\bh_agent_debugger.exe"),
        (Join-Path $PSScriptRoot "Temp\bh_agent_debugger_x64_Release\bh_agent_debugger.exe")
    )

    foreach ($candidate in $candidates)
    {
        if (Test-Path $candidate)
        {
            return $candidate
        }
    }

    return ""
}

function Convert-MessageToAgentCtlArgs([string]$text)
{
    $trimmedText = $text.Trim()
    if ($trimmedText -ieq "status")
    {
        return @("status")
    }

    if ($trimmedText -ieq "list solutions" -or $trimmedText -ieq "diag solutions")
    {
        return @("list-solutions")
    }

    if ($trimmedText -ieq "diag session" -or $trimmedText -ieq "diag")
    {
        return @("diag-session")
    }

    if ($trimmedText -ieq "doctor" -or $trimmedText -ieq "health" -or $trimmedText -ieq "diag health")
    {
        return @("doctor")
    }

    if ($trimmedText -ieq "read regs")
    {
        return @("read-regs")
    }

    if ($trimmedText -match "^read\s+stack(?:\s+(\d+))?$")
    {
        if ($Matches[1])
        {
            return @("read-stack", $Matches[1])
        }

        return @("read-stack")
    }

    if ($trimmedText -ieq "local variables" -or $trimmedText -ieq "locals")
    {
        return @("local-vars")
    }

    if ($trimmedText -match "^(?:local\s+variables|locals)\s+(\d+)$")
    {
        return @("local-vars", $Matches[1])
    }

    if ($trimmedText -match "^set\s+trap\s+symbol\s+(.+)$")
    {
        $symbol = $Matches[1].Trim()
        if (-not [string]::IsNullOrWhiteSpace($symbol))
        {
            return @("set-trap", $symbol)
        }
    }

    return $null
}

function Invoke-AgentCtlCommand([string[]]$agentCtlArgs)
{
    $agentCtlExe = Find-AgentCtlExe
    if ([string]::IsNullOrWhiteSpace($agentCtlExe))
    {
        throw "agentctl executable not found (build bh_agent_debugger first)"
    }

    $fullArgs = $agentCtlArgs + @("--timeout-ms", [string]$connectTimeoutMs)
    if (-not [string]::IsNullOrWhiteSpace($agentTargetId))
    {
        $fullArgs += @("--target-id", $agentTargetId)
    }

    $output = & $agentCtlExe @fullArgs 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0)
    {
        $text = [string]::Join("`n", @($output))
        throw "agentctl failed (exit=$exitCode): $text"
    }

    return [string]::Join("`n", @($output))
}

function Try-ForwardToAgentCtl
{
    $preferAgentCtl = $useAgentCtl.IsPresent
    if (-not $preferAgentCtl -and -not $useAgentCtl.IsPresent)
    {
        return $false
    }

    $strictAgentCtl = $useAgentCtl.IsPresent

    $queueMessages = @(Get-QueueMessages -rawQueue $queue -fallbackMessage $null)
    if ($queueMessages.Count -gt 0)
    {
        foreach ($queuedMessage in $queueMessages)
        {
            $translated = Convert-MessageToAgentCtlArgs $queuedMessage
            if ($null -eq $translated)
            {
                if ($strictAgentCtl)
                {
                    throw "agentctl forwarding does not support queued command: $queuedMessage"
                }

                return $false
            }
        }

        foreach ($queuedMessage in $queueMessages)
        {
            $translated = Convert-MessageToAgentCtlArgs $queuedMessage
            $response = ""
            try
            {
                $response = Invoke-AgentCtlCommand $translated
            }
            catch
            {
                if ($strictAgentCtl)
                {
                    throw
                }

                return $false
            }

            Write-Host "Agent response [$queuedMessage]: $response"
            if ($queueDelayMs -gt 0)
            {
                Start-Sleep -Milliseconds $queueDelayMs
            }
        }

        exit 0
    }

    $singleMessages = @(Get-QueueMessages -rawQueue @() -fallbackMessage $message)
    if ($singleMessages.Count -eq 0)
    {
        return $false
    }

    $translatedSingle = Convert-MessageToAgentCtlArgs $singleMessages[0]
    if ($null -eq $translatedSingle)
    {
        if ($strictAgentCtl)
        {
            throw "agentctl forwarding does not support message: $($singleMessages[0])"
        }

        return $false
    }

    $singleResponse = ""
    try
    {
        $singleResponse = Invoke-AgentCtlCommand $translatedSingle
    }
    catch
    {
        if ($strictAgentCtl)
        {
            throw
        }

        return $false
    }

    Write-Host "Agent response: $singleResponse"
    exit 0
}

try
{
    [void](Try-ForwardToAgentCtl)

    if ($startDebugging)
    {
        Start-DebuggerSession
    }

    $queueMessages = @(Get-QueueMessages -rawQueue $queue -fallbackMessage $null)
    if ($queueMessages.Count -gt 0)
    {
        foreach ($queuedMessage in $queueMessages)
        {
            $queuedResponse = Invoke-QueueCommand $queuedMessage
            if ([string]::IsNullOrWhiteSpace($queuedResponse))
            {
                Write-Host "Agent response [$queuedMessage]: <empty>"
            }
            else
            {
                Write-Host "Agent response [$queuedMessage]: $queuedResponse"
            }

            if ($queueDelayMs -gt 0)
            {
                Start-Sleep -Milliseconds $queueDelayMs
            }
        }

        exit 0
    }

    $singleMessages = @(Get-QueueMessages -rawQueue @() -fallbackMessage $message)
    if ($singleMessages.Count -eq 0)
    {
        throw "Missing message"
    }

    $trimmed = $singleMessages[0]
    if ($trimmed -ieq "start debugging")
    {
        Start-DebuggerSession
        Write-Host "Agent response: ok: debugger started"
        exit 0
    }

    if ($trimmed -match "^start\s+debugging\s+(.+)$")
    {
        $solutionHint = $Matches[1].Trim()
        if ([string]::IsNullOrWhiteSpace($solutionHint))
        {
            throw "start debugging requires a solution hint"
        }

        $startByHintResponse = Start-DebuggingBySolutionHint $solutionHint
        Write-Host "Agent response: $startByHintResponse"
        exit 0
    }

    if ($trimmed -ieq "start exp debugging")
    {
        $expStartResponse = Start-ExperimentalDebuggerSession
        Write-Host "Agent response: $expStartResponse"
        exit 0
    }

    if ($trimmed -ieq "open exp solution")
    {
        $openResponse = Open-ExperimentalSolution
        Write-Host "Agent response: $openResponse"
        exit 0
    }

    if ($trimmed -ieq "open exp solution force")
    {
        $openResponse = Open-ExperimentalSolution -forceLaunch
        Write-Host "Agent response: $openResponse"
        exit 0
    }

    if ($trimmed -ieq "start exp debugging force")
    {
        $expStartResponse = Start-ExperimentalDebuggerSession -forceLaunch
        Write-Host "Agent response: $expStartResponse"
        exit 0
    }

    if ($trimmed -ieq "stop debugging" -or $trimmed -ieq "stop")
    {
        $stopResponse = Stop-DebuggerSession
        Write-Host "Agent response: $stopResponse"
        exit 0
    }

    if ($trimmed -ieq "stop main debugging" -or $trimmed -ieq "stop main")
    {
        $stopResponse = Stop-MainDebuggerSession
        Write-Host "Agent response: $stopResponse"
        exit 0
    }

    if ($trimmed -ieq "stop exp debugging" -or $trimmed -ieq "stop exp")
    {
        $stopResponse = Stop-ExperimentalDebuggerSession
        Write-Host "Agent response: $stopResponse"
        exit 0
    }

    if ($trimmed -ieq "stop debugging force" -or $trimmed -ieq "stop force")
    {
        $stopResponse = Stop-DebuggerSession -forceKillExp
        Write-Host "Agent response: $stopResponse"
        exit 0
    }

    if ($trimmed -match "^prime trap symbol\s+(.+)$")
    {
        $symbol = $Matches[1].Trim()
        if ([string]::IsNullOrWhiteSpace($symbol))
        {
            throw "prime trap symbol requires a symbol name"
        }

        $stepResponse = Invoke-AgentMessage "step over"
        if ($diagnostics)
        {
            Write-Host "Step response: $stepResponse"
        }

        Start-Sleep -Milliseconds 200

        $trapResponse = Invoke-AgentMessage "set trap symbol $symbol"
        Write-Host "Agent response: $trapResponse"
        exit 0
    }

    if ($trimmed -match "^preset\s+restart\s+arm\s+(.+)$")
    {
        $presetResponse = Invoke-QueueCommand $trimmed
        Write-Host "Agent response: $presetResponse"
        exit 0
    }

    if ($trimmed -ieq "preset health")
    {
        $presetResponse = Invoke-QueueCommand $trimmed
        Write-Host "Agent response: $presetResponse"
        exit 0
    }

    if ($trimmed -match "^wait break(?:\s+(\d+))?$")
    {
        $timeout = if ($Matches[1]) { [int]$Matches[1] } else { 20 }
        $dte = Get-PreferredDteInstance
        if ($null -ne $dte)
        {
            $waitResult = Wait-DteBreak -dte $dte -timeoutSec $timeout
            Write-Host "Agent response: $waitResult"
            exit 0
        }

        $waitResult = Wait-Status -mode "break" -needle "" -timeoutSeconds $timeout
        Write-Host "Agent response: $waitResult"
        exit 0
    }

    if ($trimmed -ieq "where")
    {
        $dte = Get-PreferredDteInstance
        if ($null -ne $dte -and (Get-DteDebugModeText $dte) -eq "break")
        {
            $location = Get-DteBreakLocation $dte
            Write-Host "Agent response: ok: where $location"
            exit 0
        }
    }

    if ($trimmed -match "^stack(?:\s+(\d+))?$")
    {
        $maxFrames = if ($Matches[1]) { [int]$Matches[1] } else { 8 }
        $dte = Get-PreferredDteInstance
        if ($null -eq $dte)
        {
            Write-Host "Agent response: error: no DTE instance"
            exit 0
        }

        $stackResponse = Get-DteStackSummary -dte $dte -maxFrames $maxFrames
        Write-Host "Agent response: $stackResponse"
        exit 0
    }

    if ($trimmed -ieq "diag session" -or $trimmed -ieq "diag")
    {
        $diagResponse = Get-SessionDiagnostics
        Write-Host "Agent response: $diagResponse"
        exit 0
    }

    if ($trimmed -ieq "list solutions" -or $trimmed -ieq "diag solutions")
    {
        $solutionsResponse = Get-AllOpenSolutionsInfo
        Write-Host "Agent response: $solutionsResponse"
        exit 0
    }

    if ($trimmed -ieq "list targets" -or $trimmed -ieq "targets list")
    {
        $targetsResponse = Get-DiscoveredTargetsInfo
        Write-Host "Agent response: $targetsResponse"
        exit 0
    }

    if ($trimmed -ieq "list targets json" -or $trimmed -ieq "targets list json")
    {
        $targetsResponse = Get-DiscoveredTargetsInfo -asJson
        Write-Host "Agent response: $targetsResponse"
        exit 0
    }

    $response = Invoke-AgentMessage $trimmed

    if ([string]::IsNullOrWhiteSpace($response))
    {
        Write-Host "Sent agent message: $message"
    }
    else
    {
        Write-Host "Agent response: $response"
    }

    exit 0
}
catch
{
    Write-Error "Command failed: $($_.Exception.Message)"
    exit 1
}
