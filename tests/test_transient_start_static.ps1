$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$servicePath = Join-Path $root "bh_agent_debugger_service\bh_agent_debugger_service\named_pipe_transport.cpp"
$serviceSource = Get-Content -Raw -Path $servicePath
$startIndex = $serviceSource.IndexOf('if (method == "debug.startHint")')
$attachIndex = $serviceSource.IndexOf('if (method == "debug.attachPid")', $startIndex)
if ($startIndex -lt 0 -or $attachIndex -le $startIndex) {
    throw "could not locate debug.startHint service handler"
}
$startSource = $serviceSource.Substring($startIndex, $attachIndex - $startIndex)
if ($startSource -match 'capture_startup_project_arguments|set_startup_project_arguments|restore_startup_project|vcxproj\.user|schedule_deferred' -or
    $startSource -notmatch '!has_launch_arguments && bh_agent_dte::execute_command_for_pid\(process_id, "Debug\.Start"' -or
    $startSource -notmatch 'bh_startup_arguments::command_prefix\) \+ encoded_launch_arguments' -or
    $startSource -notmatch 'package_response\.rfind\("ok:", 0\)') {
    throw "argument-bearing service start is not fail-closed and transient"
}

$packagePath = Join-Path $root "bh_agent_debugger_package\bh_agent_debugger_package.h"
$packageSource = Get-Content -Raw -Path $packagePath
$dispatchIndex = $packageSource.IndexOf('TryTransientStartWithArguments(message, response)')
$dkmIndex = $packageSource.IndexOf('bh_agent_dkm_thread_scope dkm_scope;', $dispatchIndex)
$handlerIndex = $packageSource.IndexOf('bool TryTransientStartWithArguments(')
$handlerEnd = $packageSource.IndexOf('bool TryParseUnsigned(', $handlerIndex)
if ($dispatchIndex -lt 0 -or $dkmIndex -le $dispatchIndex -or $handlerIndex -lt 0 -or $handlerEnd -le $handlerIndex) {
    throw "could not locate transient package launch integration"
}
$handlerSource = $packageSource.Substring($handlerIndex, $handlerEnd - $handlerIndex)
$buildIndex = $handlerSource.IndexOf('solution_build->BuildProject(')
$beforeIndex = $handlerSource.IndexOf('debug_cfg->OnBeforeDebugLaunch(', $buildIndex)
$queryIndex = $handlerSource.IndexOf('query_debug_cfg->QueryDebugTargets(', $beforeIndex)
$overrideIndex = $handlerSource.IndexOf('target.bstrArg = replacement;', $queryIndex)
$launchIndex = $handlerSource.IndexOf('shell_debugger->LaunchDebugTargets4(', $overrideIndex)
if ($buildIndex -lt 0 -or $beforeIndex -le $buildIndex -or $queryIndex -le $beforeIndex -or
    $overrideIndex -le $queryIndex -or $launchIndex -le $overrideIndex -or
    $handlerSource -match 'CommandArguments|vcxproj\.user|Debug\.Start|set_startup_project_arguments|restore_startup_project' -or
    $handlerSource -notmatch 'transient launch requires exactly one startup project' -or
    $handlerSource -notmatch 'try_decode\(' -or
    $handlerSource -notmatch 'build_state != vsBuildStateDone' -or
    $handlerSource -notmatch 'VSHPROPID_ConfigurationProvider' -or
    $handlerSource -notmatch '__uuidof\(IVsDebuggableProjectCfg\)' -or
    $handlerSource -notmatch 'debug_cfg_base->QueryInterface\(' -or
    $handlerSource -notmatch '__uuidof\(IVsDebuggableProjectCfg2\)' -or
    $handlerSource -notmatch '__uuidof\(IVsQueryDebuggableProjectCfg2\)' -or
    $handlerSource -notmatch 'hr != E_NOINTERFACE' -or
    $handlerSource -match 'QueryDebugTargets\(launch_flags, 0, nullptr' -or
    $handlerSource -notmatch 'constexpr ULONG maximum_target_count = 32' -or
    ([regex]::Matches($handlerSource, 'CheckTransientLaunchState\(debugger, response\)').Count -ne 3)) {
    throw "package launch order or persistence invariant is broken"
}

Write-Output "PASS transient startup arguments remain bounded, fail-closed, and in-memory only"
