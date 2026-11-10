param([string]$InputFile, [int]$RecordedPid, [string]$Symbols, [long]$ExpectedSamples = 0, [string]$Function, [long]$ExpectedFunctionSamples = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $root 'Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe'
$helper = Join-Path $root 'Temp\bh_agent_debugger_cli_x64_Debug\bh_profile_analyzer.exe'
function Invoke-Analysis([string[]]$Arguments, [bool]$Success) {
    $text = (& $cli profile analyze @Arguments | Out-String).Trim()
    $code = $LASTEXITCODE
    if (($code -eq 0) -ne $Success) { throw "unexpected exit=$code output=$text" }
    $r = $text | ConvertFrom-Json
    if ([bool]$r.ok -ne $Success) { throw 'exit/JSON status disagreement' }
    return $r
}
& $helper --self-test
if ($LASTEXITCODE) { throw 'analyzer deterministic tests failed' }
[void](Invoke-Analysis @('--input','missing.etl','--pid','1') $false)
[void](Invoke-Analysis @('--input','missing.etl','--pid','0') $false)
[void](Invoke-Analysis @('--input','missing.etl','--pid','1','--top','51') $false)
[void](Invoke-Analysis @('--input','missing.etl','--pid','1','--pid','2') $false)
[void](Invoke-Analysis @('--input','missing.etl','--pid','1','--pipe','invalid') $false)
[void](Invoke-Analysis @('--input','missing.etl','--pid','1','--symbols','srv*C:\cache*https://example.com') $false)
if ($InputFile) {
    if ($RecordedPid -le 0) { throw 'RecordedPid required with InputFile' }
    $before = (Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash
    $args = @('--input',$InputFile,'--pid',"$RecordedPid",'--top','5')
    if ($Symbols) { $args += @('--symbols',$Symbols) }
    $r = Invoke-Analysis $args $true
    if ($r.process.pid -ne $RecordedPid -or $r.metrics.samples -le 0) { throw 'incorrect process/samples' }
    if ($ExpectedSamples -and $r.metrics.samples -ne $ExpectedSamples) { throw 'independent reference sample count mismatch' }
    if ($r.analysis.functionsBySelf.Count -gt 5 -or $r.analysis.callTree.Count -gt 5 -or $r.analysis.hotPaths.Count -gt 5) { throw 'unbounded report' }
    if ($r.analysis.callTree[0].inclusiveSamples -ne $r.metrics.samples) { throw 'tree root/sample total mismatch' }
    if ($r.quality.missingStacks -gt $r.metrics.samples -or $r.quality.unresolvedLeafSamples -lt $r.quality.missingStacks) { throw 'quality counters inconsistent' }
    if ($Symbols -and !($r.analysis.functionsBySelf | Where-Object { $_.frame.resolved })) { throw 'local symbols did not resolve' }
    [void](Invoke-Analysis @('--input',$InputFile,'--pid','2147483647') $false)
    $noSymbols = Invoke-Analysis @('--input',$InputFile,'--pid',"$RecordedPid",'--top','1') $true
    if ($noSymbols.metrics.samples -ne $r.metrics.samples -or $noSymbols.quality.symbols -ne 'not_requested') { throw 'symbols change sample count' }
    if ($Function) {
        $detail = Invoke-Analysis ($args + @('--function',$Function)) $true
        $d = $detail.sourceDrilldown
        if ($d.status -ne 'available' -or $d.selectedFunction.name -ne $Function) { throw 'function scope failed' }
        if ($ExpectedFunctionSamples -and $d.selectedSelfSamples -ne $ExpectedFunctionSamples) { throw 'function self count mismatch' }
        if ($d.rows.Count -gt 5 -or $d.resolvedLineSamples + $d.unresolvedLineSamples -ne $d.selectedSelfSamples) { throw 'source coverage/bound mismatch' }
        $shown = ($d.rows | Measure-Object selfSamples -Sum).Sum
        if ($shown + $d.samplesInOmittedRows -ne $d.selectedSelfSamples) { throw 'source rows double-count or discard samples' }
        if ($d.missingStackSamplesInProcess -ne $r.quality.missingStacks -or $d.unresolvedLeafSamplesInProcess -ne $r.quality.unresolvedLeafSamples) { throw 'source scope hides unknown samples' }
        if ($d.resolvedLineSamples -le 0 -or $d.sourceTextVerified) { throw 'invalid source-coverage claim' }
        $byId = Invoke-Analysis ($args + @('--function',$d.selectedFunction.id)) $true
        if ($byId.sourceDrilldown.selectedSelfSamples -ne $d.selectedSelfSamples) { throw 'function id scope mismatch' }
        $unresolved = Invoke-Analysis @('--input',$InputFile,'--pid',"$RecordedPid",'--top','1','--function',$Function) $true
        if ($unresolved.sourceDrilldown.status -ne 'function_unresolved_or_absent' -or $unresolved.metrics.samples -ne $r.metrics.samples) { throw 'missing-PDB degradation failed' }
        $missingStack = Invoke-Analysis ($args + @('--function','[missing-stack]')) $true
        if ($missingStack.sourceDrilldown.selectedSelfSamples -ne $r.quality.missingStacks -or $missingStack.sourceDrilldown.resolvedLineSamples -ne 0) { throw 'explicit missing-stack bucket failed' }
        "PASS: source drill-down function=$Function self=$($d.selectedSelfSamples) resolvedLines=$($d.resolvedLineSamples)"
        $d.rows | Select-Object @{n='File';e={$_.location.file}},@{n='Line';e={$_.location.line}},selfSamples,inlineStatus | Format-Table -AutoSize
    }
    # Exercise Unicode/space path forwarding through the native CLI (not shell arguments).
    $copy = Join-Path $root ('Temp\tests\analysis ' + [char]0x03A9 + ' ' + [guid]::NewGuid().ToString() + [IO.Path]::GetExtension($InputFile))
    try {
        Copy-Item -LiteralPath $InputFile -Destination $copy
        $unicode = Invoke-Analysis @('--input',$copy,'--pid',"$RecordedPid",'--top','1') $true
        if ($unicode.metrics.samples -ne $r.metrics.samples) { throw 'Unicode input forwarding failed' }
    } finally { Remove-Item -LiteralPath $copy -ErrorAction SilentlyContinue }
    if ((Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash -ne $before) { throw 'analyzer modified original capture' }
    "PASS: archived capture pid=$RecordedPid samples=$($r.metrics.samples) missingStacks=$($r.quality.missingStacks)"
    $r.analysis.functionsBySelf | Select-Object @{n='Function';e={$_.frame.name}},selfSamples,inclusiveSamples | Format-Table -AutoSize
} else { 'Archived-capture validation NOT RUN (supply InputFile and RecordedPid)' }
'PASS: analyzer CLI/errors/deterministic tests'
