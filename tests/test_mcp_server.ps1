<#
.SYNOPSIS
JSON-RPC test for the MCP stdio server (bh_agent_debugger_cli mcp).

.DESCRIPTION
Runs the CLI as an MCP server and checks the initialize handshake, that tools/list mirrors the
OpenCode wrapper (opencode/tools/bh_agent_debugger.ts), JSON-RPC error handling, strict JSON
parsing and tool argument validation. One tool call runs the CLI as a child process (offline
profile analysis of a missing file). A second server uses --pipe with a stand-in broker pipe that the
test serves, to check how broker replies become tool results. The default run never contacts the
real broker or Visual Studio.
-Live adds read-only doctor, targets and dte calls against a broker that is already running.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_mcp_server.ps1
#>
param(
    [string]$CliPath,
    [switch]$Live
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $CliPath) { $CliPath = Join-Path $repo 'Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe' }
if (-not (Test-Path -LiteralPath $CliPath)) { throw "CLI not found: $CliPath (run build.bat first)" }

$failures = New-Object System.Collections.Generic.List[string]
function Check([bool]$condition, [string]$label) {
    if ($condition) { Write-Host "ok   $label" } else { Write-Host "FAIL $label"; $failures.Add($label) }
}

function New-CliStartInfo([string]$arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $CliPath
    $info.Arguments = $arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $info.CreateNoWindow = $true
    return $info
}

# An unknown server option fails closed before any protocol traffic.
$bad = [System.Diagnostics.Process]::Start((New-CliStartInfo 'mcp --bogus'))
$bad.StandardInput.Close()
$badError = $bad.StandardError.ReadToEnd()
Check ($bad.WaitForExit(10000) -and $bad.ExitCode -eq 1 -and $badError -like '*invalid mcp option*') 'unknown mcp option is rejected'

# Never start a broker from this test.
$server = [System.Diagnostics.Process]::Start((New-CliStartInfo 'mcp --no-auto-start-service'))
$stderrTask = $server.StandardError.ReadToEndAsync()

function Send-Line([string]$line) {
    $server.StandardInput.WriteLine($line)
    $server.StandardInput.Flush()
}

function Receive([int]$timeoutMs = 60000) {
    $task = $server.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($timeoutMs)) { throw 'no response from the MCP server' }
    $line = $task.Result
    if ($null -eq $line) { throw 'the MCP server closed stdout' }
    try { return ($line | ConvertFrom-Json) } catch { throw "stdout line is not JSON: $line" }
}

$script:nextId = 0
function Request([string]$method, $params, $id) {
    if ($null -eq $id) { $script:nextId++; $id = $script:nextId }
    $message = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $method }
    if ($null -ne $params) { $message.params = $params }
    Send-Line (ConvertTo-Json $message -Compress -Depth 10)
    $response = Receive
    Check ($response.id -eq $id) "$method reply carries id $id"
    if ($response.id -ne $id) { Write-Host ('     reply: ' + (ConvertTo-Json $response -Compress -Depth 10)) }
    return $response
}

function ToolText($response) {
    if ($response.result -and $response.result.content) { return [string]$response.result.content[0].text }
    return ''
}

function Invalid([string]$tool, $arguments, [string]$expect, [string]$label) {
    $response = Request 'tools/call' @{ name = $tool; arguments = $arguments }
    $text = ToolText $response
    Check ($response.result.isError -eq $true -and $text -like "invalid arguments:*$expect*") "$label [$text]"
}

try {
    $r = Request 'ping' $null 'before-init'
    Check ($null -ne $r.result -and $null -eq $r.error) 'ping works before initialize'
    $r = Request 'tools/list' $null
    Check ($r.error.code -eq -32002) 'tools/list before initialize is rejected'

    $r = Request 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'test_mcp_server'; version = '1' } } 'init'
    Check ($r.result.protocolVersion -eq '2025-06-18') 'initialize echoes a supported protocol version'
    Check ($r.result.serverInfo.name -eq 'bh_agent_debugger') 'serverInfo.name'
    Check ([string]$r.result.serverInfo.version -match '^\d+\.\d+\.\d+$') 'serverInfo.version'
    Check ($null -ne $r.result.capabilities.tools) 'tools capability is advertised'
    Check ([string]$r.result.instructions -like '*run code*') 'instructions carry the code-execution warning'
    $r = Request 'initialize' @{ protocolVersion = '2025-06-18' }
    Check ($r.error.code -eq -32600) 'a second initialize is rejected'

    # A notification gets no reply: the next reply must belong to the next request.
    Send-Line '{"jsonrpc":"2.0","method":"notifications/initialized"}'

    $r = Request 'tools/list' @{}
    $tools = @($r.result.tools)
    $wrapper = Get-Content -Raw -LiteralPath (Join-Path $repo 'opencode\tools\bh_agent_debugger.ts')
    $expected = @{}
    foreach ($match in [regex]::Matches($wrapper, 'export const (\w+) = tool\(\{\s*description:[^\n]*\n\s*args: \{(?<args>[\s\S]*?)\n  \},')) {
        $expected['bh_agent_debugger_' + $match.Groups[1].Value] = @([regex]::Matches($match.Groups['args'].Value, '(?m)^    (\w+):') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    }
    Check ($expected.Count -eq 12) "the OpenCode wrapper defines 12 tools (found $($expected.Count))"
    $names = @($tools | ForEach-Object { $_.name } | Sort-Object)
    Check (($names -join ',') -eq ((@($expected.Keys) | Sort-Object) -join ',')) 'tools/list names mirror the OpenCode wrapper'
    foreach ($tool in $tools) {
        $schema = $tool.inputSchema
        $properties = @($schema.properties.PSObject.Properties | ForEach-Object { $_.Name } | Sort-Object)
        Check ($schema.type -eq 'object' -and $schema.additionalProperties -eq $false) "$($tool.name) schema is a closed object"
        if ($expected.ContainsKey($tool.name)) {
            Check (($properties -join ',') -eq ($expected[$tool.name] -join ',')) "$($tool.name) arguments mirror the wrapper ($($properties -join ','))"
        }
    }

    $r = Request 'resources/list' @{}
    Check ($r.error.code -eq -32601) 'an unknown method is -32601'
    Send-Line '{"jsonrpc":"2.0","id":900,"method":"ping"'
    $r = Receive
    Check ($r.error.code -eq -32700 -and $null -eq $r.id) 'truncated JSON is a parse error with id null'
    Send-Line '{"jsonrpc":"2.0","id":901,"id":902,"method":"ping"}'
    $r = Receive
    Check ($r.error.code -eq -32700) 'duplicate member names are rejected'
    Send-Line '{"jsonrpc":"2.0","id":903,"method":"tools/call","params":{"name":"bh_agent_debugger_send","arguments":{"targetId":"vs_1","command":"\ud800"}}}'
    $r = Receive
    Check ($r.error.code -eq -32700) 'a lone surrogate is rejected'
    Send-Line '[{"jsonrpc":"2.0","id":904,"method":"ping"}]'
    $r = Receive
    Check ($r.error.code -eq -32600) 'batches are rejected'
    Send-Line '{"jsonrpc":"1.0","id":905,"method":"ping"}'
    $r = Receive
    Check ($r.error.code -eq -32600 -and $r.id -eq 905) 'jsonrpc must be 2.0'

    $r = Request 'tools/call' @{ name = 'bh_agent_debugger_nope'; arguments = @{} }
    Check ($r.error.code -eq -32602) 'an unknown tool is -32602'
    $r = Request 'tools/call' @{ name = 'bh_agent_debugger_doctor'; arguments = @(1) }
    Check ($r.error.code -eq -32602) 'arguments must be an object'

    Invalid 'bh_agent_debugger_send' @{ targetId = 'vs_1'; command = 'status'; extra = 1 } 'unknown argument' 'unknown arguments are rejected'
    Invalid 'bh_agent_debugger_send' @{ targetId = 'vs_1' } 'command is required' 'required arguments are enforced'
    Invalid 'bh_agent_debugger_send' @{ targetId = 'vs_1'; command = '--timeout-ms' } "must not start with '-'" 'option-like positional values are rejected'
    Invalid 'bh_agent_debugger_send' @{ targetId = 'vs_1'; command = ("status" + [char]1) } 'control characters' 'control characters are rejected'
    Invalid 'bh_agent_debugger_send' @{ targetId = 'vs 1'; command = 'status' } 'letters, digits' 'target ids are restricted'
    Invalid 'bh_agent_debugger_doctor' @{ timeoutMs = 0 } 'integer from 1' 'timeoutMs lower bound'
    Invalid 'bh_agent_debugger_doctor' @{ timeoutMs = 1.5 } 'integer' 'timeoutMs must be an integer'
    Invalid 'bh_agent_debugger_doctor' @{ timeoutMs = '5000' } 'integer' 'timeoutMs must be a number'
    Invalid 'bh_agent_debugger_profile' @{ action = 'start' } 'processId is required only for start' 'profile start needs processId'
    Invalid 'bh_agent_debugger_profile' @{ action = 'status'; seconds = 5 } 'start-only' 'seconds is start-only'
    Invalid 'bh_agent_debugger_profile' @{ action = 'open' } 'targetId is required only for open' 'profile open needs targetId'
    Invalid 'bh_agent_debugger_profile' @{ action = 'open'; targetId = 'vs_x' } 'vs_<pid>' 'profile open needs vs_<pid>'
    Invalid 'bh_agent_debugger_profile' @{ action = 'pause' } 'action must be' 'profile action is an enum'
    Invalid 'bh_agent_debugger_start' @{ hint = 'app'; arguments = '--pipe' } 'CLI option name' 'launch arguments cannot be a CLI option name'
    Invalid 'bh_agent_debugger_chain' @{ hints = @() } 'hints must be' 'chain needs at least one hint'
    Invalid 'bh_agent_debugger_open_dump' @{ targetId = 'vs_1'; path = 'C:\a"b.dmp' } 'must not contain' 'dump paths cannot contain quotes'
    Invalid 'bh_agent_debugger_profile_analyze' @{ input = 'C:\x.etl'; processId = 4; sections = @('callTree', 'callTree') } 'distinct' 'analysis sections must be distinct'
    if ([System.Text.Encoding]::Default.CodePage -eq 1252) {
        Send-Line '{"jsonrpc":"2.0","id":"acp","method":"tools/call","params":{"name":"bh_agent_debugger_send","arguments":{"targetId":"vs_1","command":"status \u4e2d"}}}'
        $r = Receive
        Check ($r.id -eq 'acp' -and $r.result.isError -eq $true -and (ToolText $r) -like '*ANSI code page*') 'text the ANSI code page cannot carry is rejected'
    }

    # A real child CLI call that needs no broker: offline analysis of a file that does not exist.
    $missing = Join-Path ([System.IO.Path]::GetTempPath()) ('bh_mcp_missing_' + [guid]::NewGuid().ToString('N') + '.etl')
    $r = Request 'tools/call' @{ name = 'bh_agent_debugger_profile_analyze'; arguments = @{ input = $missing; processId = 4 } }
    $text = ToolText $r
    Check ($r.result.isError -eq $true -and $text -like 'bh_agent_debugger_cli failed (code=*') "a failing CLI call is a tool error [$text]"

    if ($Live) {
        foreach ($tool in 'bh_agent_debugger_doctor', 'bh_agent_debugger_targets', 'bh_agent_debugger_dte') {
            $r = Request 'tools/call' @{ name = $tool; arguments = @{ timeoutMs = 5000 } }
            $text = ToolText $r
            Check ($r.result.isError -eq $false -and $text.Length -gt 0) "$tool succeeds against the running broker [$text]"
        }
    }

    $server.StandardInput.Close()
    Check ($server.WaitForExit(10000)) 'the server exits when stdin closes'
    Check ($server.ExitCode -eq 0) 'the server exit code is 0'
}
finally {
    if (-not $server.HasExited) { $server.Kill() }
}
$stderr = $stderrTask.Result
if ($stderr) { Write-Host "server stderr: $stderr" }

# Broker replies through a stand-in broker pipe that only this test serves: "ok": false is a tool
# error carrying error.message, and "ok": true returns result.text.
$brokerName = 'bh_mcp_test_' + [guid]::NewGuid().ToString('N')
$brokerUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().User

function Read-PipeLine($pipe) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = $pipe.ReadByte()
        if ($b -lt 0) { throw 'the CLI closed the stand-in broker pipe before sending a request line' }
        if ($b -eq 10) { break }
        $bytes.Add([byte]$b)
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

# Sends one tools/call, answers the CLI's broker request with $reply and returns both sides.
function Invoke-WithBroker([string]$tool, $arguments, [string]$reply) {
    # The CLI refuses a broker pipe that another account owns, so set the owner explicitly.
    $security = New-Object System.IO.Pipes.PipeSecurity
    $security.SetOwner($brokerUser)
    $security.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule($brokerUser, [System.IO.Pipes.PipeAccessRights]::FullControl, [System.Security.AccessControl.AccessControlType]::Allow)))
    $pipe = New-Object System.IO.Pipes.NamedPipeServerStream($brokerName, [System.IO.Pipes.PipeDirection]::InOut, 1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous, 4096, 4096, $security)
    try {
        $script:nextId++
        $id = $script:nextId
        Send-Line (ConvertTo-Json ([ordered]@{ jsonrpc = '2.0'; id = $id; method = 'tools/call'; params = @{ name = $tool; arguments = $arguments } }) -Compress -Depth 10)
        $wait = $pipe.BeginWaitForConnection($null, $null)
        if (-not $wait.AsyncWaitHandle.WaitOne(30000)) { throw 'the CLI did not connect to the stand-in broker' }
        $pipe.EndWaitForConnection($wait)
        $request = Read-PipeLine $pipe
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($reply + "`n")
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        # The CLI exits after reading the reply, so the MCP reply follows.
        $response = Receive
        Check ($response.id -eq $id) "tools/call reply carries id $id"
        return [pscustomobject]@{ Request = $request; Response = $response }
    }
    finally { $pipe.Dispose() }
}

$server = [System.Diagnostics.Process]::Start((New-CliStartInfo "mcp --pipe $brokerName --no-auto-start-service"))
$stderrTask = $server.StandardError.ReadToEndAsync()
try {
    $r = Request 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'test_mcp_server'; version = '1' } }
    Check ($null -ne $r.result) 'initialize the server that uses the stand-in broker'
    Send-Line '{"jsonrpc":"2.0","method":"notifications/initialized"}'

    $message = 'error: chain failed at #1 skipped=0 | #0 ok: bp count=0 | #1 error: poll break timeout tries=5'
    $call = Invoke-WithBroker 'bh_agent_debugger_send' @{ targetId = 'vs_1'; command = 'status' } ('{"protocolVersion":1,"id":"cli-1","ok":false,"error":{"code":"backend_error","message":' + (ConvertTo-Json $message) + '}}')
    Check ($call.Request -like '*"method":"send"*' -and $call.Request -like '*"targetId":"vs_1"*') "the CLI sends to the --pipe broker [$($call.Request)]"
    $text = ToolText $call.Response
    Check ($call.Response.result.isError -eq $true -and $text -eq $message) "a broker reply with ok false is a tool error carrying error.message [$text]"

    $call = Invoke-WithBroker 'bh_agent_debugger_send' @{ targetId = 'vs_1'; command = 'status' } '{"protocolVersion":1,"id":"cli-1","ok":true,"result":{"text":"ok: vs design mode"}}'
    $text = ToolText $call.Response
    Check ($call.Response.result.isError -eq $false -and $text -eq 'ok: vs design mode') "a broker reply with ok true returns result.text [$text]"

    $reply = '{"protocolVersion":1,"id":"cli-1","ok":false,"error":{"code":"target_not_found"}}'
    $call = Invoke-WithBroker 'bh_agent_debugger_targets' @{ timeoutMs = 5000 } $reply
    $text = ToolText $call.Response
    Check ($call.Response.result.isError -eq $true -and $text -eq $reply) "an ok false reply without a message is a tool error with the raw reply [$text]"

    $server.StandardInput.Close()
    Check ($server.WaitForExit(10000) -and $server.ExitCode -eq 0) 'the stand-in broker server exits cleanly'
}
finally {
    if (-not $server.HasExited) { $server.Kill() }
}
$stderr = $stderrTask.Result
if ($stderr) { Write-Host "server stderr: $stderr" }
if ($failures.Count) {
    Write-Host "$($failures.Count) check(s) failed"
    exit 1
}
Write-Host 'MCP server JSON-RPC test passed'
