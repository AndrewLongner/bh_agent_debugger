param([Parameter(Mandatory=$true)][string]$Pipe, [Parameter(Mandatory=$true)][uint32]$TargetPid, [string]$Output)
$ErrorActionPreference = 'Stop'
$cli = Join-Path (Split-Path $PSScriptRoot -Parent) 'Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe'
$target = Get-Process -Id $TargetPid
if ($target.ProcessName -ne 'test_vs_profiler') { throw 'This integration test only accepts the test_vs_profiler workload fixture' }
function Invoke-Profile([string[]]$Arguments) {
    $raw = & $cli profile @Arguments --pipe $Pipe --no-auto-start-service
    if ($LASTEXITCODE -ne 0) { throw "profile failed: $raw" }
    $r = $raw | ConvertFrom-Json
    if (!$r.ok) { throw "profile error: $raw" }
    return $r.result.text
}
function Wait-Profile([string]$State, [int]$Seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $status = Invoke-Profile @('status')
        if ($status -match "state=$State(?: |$)") { return $status }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "timed out: $status"
}
$started = $false
try {
    $startArgs = @('start','--pid',"$TargetPid",'--seconds','60')
    if ($Output) { $startArgs += @('--output', $Output) }
    $accepted = Invoke-Profile $startArgs
    if ($Output -and $accepted -notmatch 'caller_owned=1') { throw "caller-selected destination not reported: $accepted" }
    $started = $true
    Wait-Profile 'running' 75
    Start-Sleep -Seconds 2
    Invoke-Profile @('stop')
    $status = Wait-Profile 'ready' 75
    $started = $false
    $status
    $report = Invoke-Profile @('report')
    if ($report -notmatch 'output="([^"]+\.diagsession)"') { throw 'missing report path' }
    $path = $Matches[1]
    if ($status -notmatch [regex]::Escape("output=`"$path`"")) { throw 'status and report paths differ' }
    if ($Output) {
        $expectedDir = if ($Output -like '*.diagsession') { Split-Path $Output -Parent } else { $Output.TrimEnd('\') }
        if ($Output -like '*.diagsession' -and $path -ne $Output) { throw "explicit output path not honored: $path" }
        if ((Split-Path $path -Parent) -ne $expectedDir) { throw "capture not in requested directory: $path" }
        if (Get-ChildItem -LiteralPath $expectedDir -Filter '.bh_partial_*' -Force) { throw 'temporary capture left behind' }
        $stream = [IO.File]::OpenRead($path); $stream.Dispose()
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        if (!($archive.Entries | Where-Object { $_.FullName -like '*.etl' -and $_.Length -gt 0 })) { throw 'report contains no ETL data' }
    } finally { $archive.Dispose() }
    if (!(Get-Process -Id $TargetPid -ErrorAction SilentlyContinue)) { throw 'profiler must not terminate target' }
    'PASS: CLI/broker capture, explicit stop, nonempty ETL in diagsession; target still alive'
} finally {
    if ($started) {
        & $cli profile stop --pipe $Pipe --no-auto-start-service
        Write-Warning 'Test failed; poll profile status to confirm cleanup before stopping broker'
    }
}
