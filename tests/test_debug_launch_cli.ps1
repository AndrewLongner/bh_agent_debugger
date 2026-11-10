param(
    [string]$Configuration = "Debug",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $root "Temp\bh_agent_debugger_cli_${Platform}_${Configuration}\bh_agent_debugger_cli.exe"
$service = Join-Path $root "Temp\bh_agent_debugger_service_${Platform}_${Configuration}\bh_agent_debugger_service.exe"

if (-not (Test-Path $cli)) { throw "CLI not found: $cli" }
if (-not (Test-Path $service)) { throw "Service not found: $service" }

$packageHeaderPath = Join-Path $root "bh_agent_debugger_package\bh_agent_debugger_package.h"
$packageHeader = Get-Content -Raw -Path $packageHeaderPath
$executeCommandIndex = $packageHeader.IndexOf('hr = dte->ExecuteCommand(CComBSTR(command_name), CComBSTR(L""));')
$waitForStepIndex = $packageHeader.IndexOf('!WaitForVsStepCompletion(', $executeCommandIndex)
$stepSuccessIndex = $packageHeader.IndexOf('response = "ok: executed " + std::string(message);', $executeCommandIndex)
if ($executeCommandIndex -lt 0 -or
    $waitForStepIndex -le $executeCommandIndex -or
    $stepSuccessIndex -le $waitForStepIndex -or
    $packageHeader -notmatch 'run_generation > initial_run_generation' -or
    $packageHeader -notmatch 'break_generation > initial_break_generation' -or
    $packageHeader -notmatch 'ForwardMessageToRemotePipe\("status", engine_status\)' -or
    $packageHeader -notmatch 'engine_status\.find\("reason=step"\)' -or
    $packageHeader -notmatch 'source=%s' -or
    $packageHeader -notmatch 'const DWORD timeout_ms = 10000' -or
    $packageHeader -notmatch 'step stabilization timeout waiting for stable break state') {
    throw "step commands are not serialized through a bounded run-to-break stabilization wait"
}

$breakWaitStart = $packageHeader.IndexOf('bool WaitForVsBreakReadiness(')
$breakCommandStart = $packageHeader.IndexOf('if (is_break_command)', $packageHeader.IndexOf('bool TryExecuteVsStepCommand('))
$executeBreakIndex = $packageHeader.IndexOf('hr = dte->ExecuteCommand(CComBSTR(command_name), CComBSTR(L""));', $breakCommandStart)
$breakWaitAfterExecute = $packageHeader.IndexOf('!WaitForVsBreakReadiness(', $executeBreakIndex)
$breakSuccessIndex = $packageHeader.IndexOf('response = "ok: executed " + std::string(message);', $breakWaitAfterExecute)
$breakWaitEnd = $packageHeader.IndexOf('static DWORD WINAPI AgentPipeThreadProc(', $breakWaitStart)
$breakWaitSource = $packageHeader.Substring($breakWaitStart, $breakWaitEnd - $breakWaitStart)
$finalModeIndex = $breakWaitSource.IndexOf('hr = debugger->get_CurrentMode(&verified_mode);')
$verifiedGenerationIndex = $breakWaitSource.IndexOf('const unsigned long long verified_run_generation', $finalModeIndex)
if ($breakWaitStart -lt 0 -or
    $breakCommandStart -lt 0 -or
    $executeBreakIndex -le $breakCommandStart -or
    $breakWaitAfterExecute -le $executeBreakIndex -or
    $breakSuccessIndex -le $breakWaitAfterExecute -or
    $breakWaitEnd -le $breakWaitStart -or
    $finalModeIndex -lt 0 -or
    $verifiedGenerationIndex -le $finalModeIndex -or
    $packageHeader -notmatch '!require_new_break \|\| break_generation > initial_break_generation' -or
    $packageHeader -notmatch 'engine_status\.rfind\("ok: paused break ", 0\) == 0' -or
    $packageHeader -notmatch 'verified_run_generation == run_generation' -or
    $packageHeader -notmatch 'verified_break_generation == break_generation' -or
    $packageHeader -notmatch 'verified_design_generation == design_generation' -or
    $packageHeader -notmatch 'stable_engine_status != engine_status' -or
    $packageHeader -notmatch 'unable to recheck debugger mode before break' -or
    $packageHeader -notmatch 'debugger mode events became unavailable while waiting for stable break state' -or
    $packageHeader -notmatch 'break command completed despite ExecuteCommand failure' -or
    $packageHeader -notmatch 'error: break stabilization timeout waiting for stable DTE/DKM break session' -or
    $packageHeader -notmatch 'response = "ok: already break"') {
    throw "break commands are not serialized through stable DTE/DKM break-session readiness"
}

$chainStart = $packageHeader.IndexOf('std::string combined = "ok: chain count="')
$chainEnd = $packageHeader.IndexOf('ExecuteSingleMessage(buffer, response);', $chainStart)
if ($chainStart -lt 0 -or $chainEnd -le $chainStart) {
    throw "could not locate package command-chain aggregation"
}
$chainSource = $packageHeader.Substring($chainStart, $chainEnd - $chainStart)
if ($chainSource -notmatch 'error: chain failed at #' -or
    $chainSource -notmatch 'skipped=' -or
    $chainSource -notmatch 'commands\.size\(\) - i - 1' -or
    $chainSource -match 'response = combined;\s*chain_error = true;') {
    throw "failed command chains do not return explicit overall errors with skipped-step counts"
}

if ($packageHeader -notmatch 'const unsigned long long parsed = _strtoui64\(text, &end_ptr, 10\)' -or
    $packageHeader -notmatch 'parsed > 0xFFFFFFFFULL') {
    throw "unsigned command arguments are narrowed before strict 32-bit range validation"
}

$serviceSourcePath = Join-Path $root "bh_agent_debugger_service\bh_agent_debugger_service\named_pipe_transport.cpp"
$serviceSource = Get-Content -Raw -Path $serviceSourcePath
$startHandlerIndex = $serviceSource.IndexOf('if (method == "debug.startHint")')
$attachHandlerIndex = $serviceSource.IndexOf('if (method == "debug.attachPid")', $startHandlerIndex)
if ($startHandlerIndex -lt 0 -or $attachHandlerIndex -le $startHandlerIndex) {
    throw "could not locate debug.startHint service handler"
}
$startHandlerSource = $serviceSource.Substring($startHandlerIndex, $attachHandlerIndex - $startHandlerIndex)
if ($startHandlerSource -match 'capture_startup_project_arguments|set_startup_project_arguments|restore_startup_project|vcxproj\.user|schedule_deferred' -or
    $startHandlerSource -notmatch '!has_launch_arguments && bh_agent_dte::execute_command_for_pid\(process_id, "Debug\.Start"' -or
    $startHandlerSource -notmatch 'bh_startup_arguments::command_prefix\) \+ encoded_launch_arguments' -or
    $startHandlerSource -notmatch 'package_response\.rfind\("ok:", 0\)') {
    throw "argument-bearing start is not isolated to the fail-closed transient package route"
}

$transientDispatchIndex = $packageHeader.IndexOf('TryTransientStartWithArguments(message, response)')
$dkmDispatchIndex = $packageHeader.IndexOf('bh_agent_dkm_thread_scope dkm_scope;', $transientDispatchIndex)
$transientStartIndex = $packageHeader.IndexOf('bool TryTransientStartWithArguments(')
$transientEndIndex = $packageHeader.IndexOf('bool TryParseUnsigned(', $transientStartIndex)
if ($transientDispatchIndex -lt 0 -or $dkmDispatchIndex -le $transientDispatchIndex -or
    $transientStartIndex -lt 0 -or $transientEndIndex -le $transientStartIndex) {
    throw "could not locate transient package launch integration"
}
$transientSource = $packageHeader.Substring($transientStartIndex, $transientEndIndex - $transientStartIndex)
$buildIndex = $transientSource.IndexOf('solution_build->BuildProject(')
$beforeLaunchIndex = $transientSource.IndexOf('debug_cfg->OnBeforeDebugLaunch(', $buildIndex)
$queryTargetsIndex = $transientSource.IndexOf('query_debug_cfg->QueryDebugTargets(', $beforeLaunchIndex)
$overrideArgumentsIndex = $transientSource.IndexOf('target.bstrArg = replacement;', $queryTargetsIndex)
$launchTargetsIndex = $transientSource.IndexOf('shell_debugger->LaunchDebugTargets4(', $overrideArgumentsIndex)
if ($buildIndex -lt 0 -or $beforeLaunchIndex -le $buildIndex -or
    $queryTargetsIndex -le $beforeLaunchIndex -or $overrideArgumentsIndex -le $queryTargetsIndex -or
    $launchTargetsIndex -le $overrideArgumentsIndex -or
    $transientSource -match 'CommandArguments|vcxproj\.user|Debug\.Start|set_startup_project_arguments|restore_startup_project' -or
    $transientSource -notmatch 'transient launch requires exactly one startup project') {
    throw "package transient launch does not preserve the build/optional-prelaunch/query/in-memory-override/launch contract"
}

$stopHandlerIndex = $serviceSource.IndexOf('if (method == "debug.stopHint")')
$designNoOpIndex = $serviceSource.IndexOf('initial_mode == "design"', $stopHandlerIndex)
$stopCommandIndex = $serviceSource.IndexOf('execute_command_for_pid(process_id, "Debug.StopDebugging"', $stopHandlerIndex)
if ($stopHandlerIndex -lt 0 -or
    $designNoOpIndex -le $stopHandlerIndex -or
    $stopCommandIndex -le $designNoOpIndex -or
    $serviceSource.IndexOf('debug.stopHint ok alreadyStopped=true via=dte', $designNoOpIndex) -lt 0) {
    throw "stop-debugging does not return an immediate success when DTE is already in design mode"
}

$installerCaPath = Join-Path $root "installer\bh_agent_debugger_installer_ca.cpp"
$installerCaSource = Get-Content -Raw -Path $installerCaPath
$productWxsPath = Join-Path $root "installer\Product.wxs"
$productWxsSource = Get-Content -Raw -Path $productWxsPath
if ($installerCaSource -notmatch 'StopAgentProcesses' -or
    $installerCaSource -notmatch 'bh_agent_debugger_service\.exe' -or
    $installerCaSource -notmatch 'bh_agent_debugger_cli\.exe' -or
    $productWxsSource -notmatch 'Custom Action="caStopAgentProcesses" Before="InstallValidate"') {
    throw "installer does not stop CLI and service processes before files-in-use validation"
}

$installerBuildPath = Join-Path $root "installer\build_installer.bat"
$installerBuildSource = Get-Content -Raw -Path $installerBuildPath
$payloadVerifierPath = Join-Path $root "installer\verify_release_payload.ps1"
$payloadVerifierSource = Get-Content -Raw -Path $payloadVerifierPath
if ($installerBuildSource -notmatch '/t:vsix:Rebuild' -or
    $installerBuildSource -notmatch 'verify_release_payload\.ps1' -or
    $installerBuildSource -notmatch '-MsiPath' -or
    $payloadVerifierSource -notmatch 'extension\.vsixmanifest' -or
    $payloadVerifierSource -notmatch 'bh_agent_debugger_package\.dll' -or
    $payloadVerifierSource -notmatch '__bh_transient_start_hex ' -or
    $payloadVerifierSource -notmatch 'File\\filExtensionVsix' -or
    $payloadVerifierSource -notmatch 'MSI ProductVersion mismatch' -or
    $payloadVerifierSource -notmatch 'MSI contains stale VSIX') {
    throw "installer builds do not rebuild and verify nested release payload freshness"
}

$localWorkerPath = Join-Path $root "bh_agent_debugger_engine\bh_agent_local_worker_component.cpp"
$localWorkerSource = Get-Content -Raw -Path $localWorkerPath
$qualifierParseIndex = $localWorkerSource.IndexOf("requested_symbol.find(L'!')")
$qualifiedFailureIndex = $localWorkerSource.IndexOf('if (module_qualified)', $qualifierParseIndex)
$unqualifiedScanIndex = $localWorkerSource.IndexOf('runtime->GetNativeModuleInstances(&native_modules)', $qualifiedFailureIndex)
if ($qualifierParseIndex -lt 0 -or
    $localWorkerSource -notmatch 'requested_module = requested_symbol\.substr\(0, qualifier_separator\)' -or
    $localWorkerSource -notmatch 'bh_resolve_symbol_in_module\(\s*module_instance,\s*requested_symbol\.c_str\(\),\s*search_budget' -or
    $localWorkerSource -notmatch 'bh_agent_symbol_search::budget search_budget' -or
    $localWorkerSource -notmatch 'symbol_search_limited' -or
    $qualifiedFailureIndex -le $qualifierParseIndex -or
    $unqualifiedScanIndex -le $qualifiedFailureIndex) {
    throw "module-qualified symbols are not restricted to the explicitly named native module"
}

$remoteEnginePath = Join-Path $root "bh_agent_debugger_engine\bh_agent_remote_component.cpp"
$remoteEngineSource = Get-Content -Raw -Path $remoteEnginePath
$setTrapStart = $remoteEngineSource.IndexOf('std::string bh_set_trap_at_address(UINT64 absolute_address)')
$setTrapEnd = $remoteEngineSource.IndexOf('std::string bh_remove_trap_by_id(UINT64 trap_id)', $setTrapStart)
if ($setTrapStart -lt 0 -or $setTrapEnd -le $setTrapStart) {
    throw "could not locate native trap implementation"
}
$setTrapSource = $remoteEngineSource.Substring($setTrapStart, $setTrapEnd - $setTrapStart)
if ($remoteEngineSource -notmatch 'snapshot\.module_instance && !module_qualified' -or
    $setTrapSource -notmatch 'runtime_instance->GetModuleInstances\(&modules\)' -or
    $setTrapSource -notmatch 'absolute_address - module_base < module_size' -or
    $setTrapSource -notmatch 'trap address outside all loaded native modules' -or
    $setTrapSource -match 'trap address outside current target module') {
    throw "address traps are not validated against all loaded native modules"
}

$listThreadsStart = $remoteEngineSource.IndexOf('std::string bh_list_threads_command(')
$selectThreadStart = $remoteEngineSource.IndexOf('std::string bh_select_thread_command(', $listThreadsStart)
$handleCommandStart = $remoteEngineSource.IndexOf('std::string bh_handle_script_command(', $selectThreadStart)
if ($listThreadsStart -lt 0 -or $selectThreadStart -le $listThreadsStart -or
    $handleCommandStart -le $selectThreadStart) {
    throw "could not locate native thread command implementations"
}
$listThreadsSource = $remoteEngineSource.Substring($listThreadsStart, $selectThreadStart - $listThreadsStart)
$selectThreadSource = $remoteEngineSource.Substring($selectThreadStart, $handleCommandStart - $selectThreadStart)
if ($listThreadsSource -notmatch 'process->GetThreads\(&threads\)' -or
    $listThreadsSource -notmatch 'thread->SystemPart\(\)' -or
    $listThreadsSource -notmatch 'left\.thread_id < right\.thread_id' -or
    $listThreadsSource -notmatch "entry\.thread_id == trap_session\.thread_id \? '\*' : '-'" -or
    $selectThreadSource -notmatch 'process->FindSystemThread\(thread_id' -or
    $selectThreadSource -notmatch 'process_data->trap_session\.thread_id = thread_id' -or
    $selectThreadSource -notmatch 'process_data->trap_session\.instruction_pointer = instruction_pointer' -or
    $selectThreadSource -notmatch 'trap_session\.generation != trap_session\.generation' -or
    $remoteEngineSource -notmatch 'command == command_type::list_threads \|\|' -or
    $remoteEngineSource -notmatch 'command == command_type::select_thread \|\| command == command_type::where') {
    throw "native thread selection is not scoped, deterministic, or applied to the break-session context"
}

$selectPackageStart = $packageHeader.IndexOf('bool TrySelectThread(')
$stepPackageStart = $packageHeader.IndexOf('bool TryExecuteVsStepCommand(', $selectPackageStart)
if ($selectPackageStart -lt 0 -or $stepPackageStart -le $selectPackageStart) {
    throw "could not locate package thread-selection integration"
}
$selectPackageSource = $packageHeader.Substring($selectPackageStart, $stepPackageStart - $selectPackageStart)
$dteSelectIndex = $selectPackageSource.IndexOf('debugger->put_CurrentThread(selected_thread)')
$engineSelectIndex = $selectPackageSource.IndexOf('ForwardMessageToRemotePipe(engine_command.c_str(), response)', $dteSelectIndex)
$rollbackIndex = $selectPackageSource.IndexOf('debugger->put_CurrentThread(previous_thread)', $engineSelectIndex)
if ($packageHeader -notmatch '!TrySelectThread\(message, response\)' -or
    $packageHeader -notmatch 'debugger->get_DebuggedProcesses\(&processes\)' -or
    $packageHeader -notmatch 'thread->get_ID\(&thread_id\)' -or
    $selectPackageSource -notmatch 'mode != dbgBreakMode' -or
    $selectPackageSource -notmatch '"select thread " \+ std::to_string\(requested_thread_id\)' -or
    $dteSelectIndex -lt 0 -or $engineSelectIndex -le $dteSelectIndex -or
    $rollbackIndex -le $engineSelectIndex) {
    throw "package thread selection does not synchronize DTE and engine state atomically"
}

function ConvertTo-NativeArgument {
    param([string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
        }
        elseif ($character -eq '"') {
            if ($backslashes -gt 0) { [void]$builder.Append(('\' * ($backslashes * 2))) }
            [void]$builder.Append('\"')
            $backslashes = 0
        }
        else {
            if ($backslashes -gt 0) { [void]$builder.Append(('\' * $backslashes)) }
            [void]$builder.Append($character)
            $backslashes = 0
        }
    }
    if ($backslashes -gt 0) { [void]$builder.Append(('\' * ($backslashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-Cli {
    param([string[]]$Arguments)
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $cli
    $startInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = ($stdout + $stderr).Trim() }
}

$launchArguments = '--name "two words" --path C:\temp\demo --json {"enabled":true}'
$startResult = Invoke-Cli @("start-debugging", "ExampleSolution", "--args", $launchArguments, "--timeout-ms", "4321", "--print-request")
if ($startResult.ExitCode -ne 0) { throw "start request serialization failed: $($startResult.Output)" }
$startRequest = $startResult.Output | ConvertFrom-Json
if ($startRequest.method -ne "debug.startHint" -or
    $startRequest.solutionHint -ne "ExampleSolution" -or
    $startRequest.arguments -cne $launchArguments -or
    $startRequest.timeoutMs -ne 4321) {
    throw "unexpected start request: $($startResult.Output)"
}

$attachResult = Invoke-Cli @("attach-debugging", "ExampleSolution", "--pid", "4294967295", "--timeout-ms", "9876", "--print-request")
if ($attachResult.ExitCode -ne 0) { throw "attach request serialization failed: $($attachResult.Output)" }
$attachRequest = $attachResult.Output | ConvertFrom-Json
if ($attachRequest.method -ne "debug.attachPid" -or
    $attachRequest.solutionHint -ne "ExampleSolution" -or
    [uint64]$attachRequest.processId -ne 4294967295 -or
    $attachRequest.timeoutMs -ne 9876) {
    throw "unexpected attach request: $($attachResult.Output)"
}

foreach ($invalidPid in @("0", "-1", "12x", "4294967296")) {
    $invalidResult = Invoke-Cli @("attach-debugging", "ExampleSolution", "--pid", $invalidPid, "--print-request")
    if ($invalidResult.ExitCode -eq 0 -or $invalidResult.Output -notmatch "invalid --pid") {
        throw "invalid PID '$invalidPid' was accepted: $($invalidResult.Output)"
    }
}

$pipe = "bh_agent_ctl_test_$PID"
$serviceProcess = Start-Process -FilePath $service -ArgumentList @("--pipe", $pipe, "--target-pipe", "bh_agent_debugger_agent_test_$PID") -PassThru
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        Start-Sleep -Milliseconds 100
        $probe = Invoke-Cli @("targets-list", "--pipe", $pipe, "--timeout-ms", "500", "--no-auto-start-service")
        if ($probe.ExitCode -eq 0) { $ready = $true; break }
    }
    if (-not $ready) { throw "test service did not become ready" }

    $targetsResponse = $probe.Output | ConvertFrom-Json
    if ($targetsResponse.ok -ne $true -or $targetsResponse.result.text -ne "targets none") {
        throw "service advertised a synthetic target: $($probe.Output)"
    }

    $health = Invoke-Cli @("doctor", "--pipe", $pipe, "--timeout-ms", "500", "--no-auto-start-service")
    if ($health.ExitCode -ne 0) { throw "health request failed: $($health.Output)" }
    $healthResponse = $health.Output | ConvertFrom-Json
    if ($healthResponse.result.text -notmatch "targets none" -or
        $healthResponse.result.text -notmatch "probes none" -or
        $healthResponse.result.text -match "id='default'") {
        throw "unexpected empty-target health response: $($health.Output)"
    }

    $sendWithoutTarget = Invoke-Cli @("send", "status", "--pipe", $pipe, "--timeout-ms", "500", "--no-auto-start-service")
    $sendWithoutTargetResponse = $sendWithoutTarget.Output | ConvertFrom-Json
    if ($sendWithoutTargetResponse.ok -ne $false -or $sendWithoutTargetResponse.error.code -ne "target_not_found") {
        throw "target-less send did not reject an empty registry: $($sendWithoutTarget.Output)"
    }

    $sendDefault = Invoke-Cli @("send-target", "default", "status", "--pipe", $pipe, "--timeout-ms", "500", "--no-auto-start-service")
    $sendDefaultResponse = $sendDefault.Output | ConvertFrom-Json
    if ($sendDefaultResponse.ok -ne $false -or $sendDefaultResponse.error.code -ne "target_not_found") {
        throw "legacy default target was accepted: $($sendDefault.Output)"
    }

    $missingHint = "missing_solution_$PID"
    $startFailure = Invoke-Cli @("start-debugging", $missingHint, "--args", "--fixture", "--pipe", $pipe, "--timeout-ms", "1000", "--no-auto-start-service")
    if ($startFailure.ExitCode -ne 0) { throw "start failure request transport failed: $($startFailure.Output)" }
    $startFailureResponse = $startFailure.Output | ConvertFrom-Json
    if ($startFailureResponse.ok -ne $false -or $startFailureResponse.error.code -ne "target_not_found") {
        throw "unexpected start failure response: $($startFailure.Output)"
    }

    $attachFailure = Invoke-Cli @("attach-debugging", $missingHint, "--pid", "1234", "--pipe", $pipe, "--timeout-ms", "1000", "--no-auto-start-service")
    if ($attachFailure.ExitCode -ne 0) { throw "attach failure request transport failed: $($attachFailure.Output)" }
    $attachFailureResponse = $attachFailure.Output | ConvertFrom-Json
    if ($attachFailureResponse.ok -ne $false -or $attachFailureResponse.error.code -ne "target_not_found") {
        throw "unexpected attach failure response: $($attachFailure.Output)"
    }
}
finally {
    if ($serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force
        $serviceProcess.WaitForExit()
    }
}

Write-Output "debug launch CLI/service tests passed"
