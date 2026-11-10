param([Parameter(Mandatory=$true)][string]$InputFile, [Parameter(Mandatory=$true)][int]$RecordedPid, [Parameter(Mandatory=$true)][string]$Symbols, [Parameter(Mandatory=$true)][string]$Subtree)
$ErrorActionPreference = 'Stop'
$cli = Join-Path (Split-Path $PSScriptRoot -Parent) 'Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe'
function Analyze([string[]]$Filter = @(), [bool]$Success = $true) {
    $text = (& $cli profile analyze --input $InputFile --pid $RecordedPid --symbols $Symbols --top 50 @Filter | Out-String).Trim()
    $exit = $LASTEXITCODE
    $r = $text | ConvertFrom-Json
    if (($exit -eq 0) -ne $Success -or [bool]$r.ok -ne $Success) { throw "unexpected result exit=$exit $text" }
    if (!$Success) { return $r }
    $f = $r.filters
    if ($f.processSamples -ne $f.retainedSamples + $f.excludedSamples + $f.indeterminateSubtreeSamples) { throw 'filter partition mismatch' }
    if ($f.excludedSamples -ne $f.excludedByTime + $f.excludedByThread + $f.excludedBySubtree) { throw 'exclusion totals mismatch' }
    $treeTotal = if ($null -ne $r.analysis.subtreeScope) { $r.analysis.subtreeScope.samplesUnderCallerContexts } else { $r.analysis.callTree[0].inclusiveSamples }
    if ($r.metrics.samples -ne $f.retainedSamples -or $treeTotal -ne $f.retainedSamples) { throw 'analysis denominator mismatch' }
    return $r
}
$before = (Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash
$baseline = Analyze
$thread = $baseline.filters.threads[0].threadId
if ($baseline.filters.processSamples -ne $baseline.metrics.samples -or $baseline.filters.excludedSamples -ne 0) { throw 'unfiltered regression' }
[double]$split = ($baseline.metrics.firstSampleMs + $baseline.metrics.lastSampleMs) / 2
$boundary = $split.ToString('R',[Globalization.CultureInfo]::InvariantCulture)
$left = Analyze @('--end-ms',$boundary)
$right = Analyze @('--start-ms',$boundary)
if ($left.metrics.samples + $right.metrics.samples -ne $baseline.metrics.samples) { throw 'half-open windows do not partition capture' }
$oneThread = Analyze @('--thread',"$thread")
if ($oneThread.metrics.samples -ne $baseline.filters.threads[0].processSamples) { throw 'native thread filter count mismatch' }
$sub = Analyze @('--subtree',$Subtree)
$root = @($baseline.analysis.functionsByInclusive | Where-Object { $_.frame.name -eq $Subtree })
if ($root.Count -ne 1 -or $sub.metrics.samples -ne $root[0].inclusiveSamples) { throw 'subtree count must match flat inclusive count, including recursion' }
if (@($sub.analysis.hotPaths | Where-Object { $_.frames[0].id -ne $root[0].id }).Count -ne 0) { throw 'subtree hot paths must start at the subtree frame' }
if (@($sub.analysis.callTree | Where-Object { $null -eq $_.parentId -and $_.frame.id -ne $root[0].id }).Count -ne 0) { throw 'subtree call tree roots must be the subtree frame' }
$compact = (& $cli profile analyze --input $InputFile --pid $RecordedPid --symbols $Symbols --top 30 --subtree $Subtree | Out-String)
if ([Text.Encoding]::UTF8.GetByteCount($compact) -ge 61440) { throw "subtree top=30 output still exceeds 60 KiB: $([Text.Encoding]::UTF8.GetByteCount($compact))" }
$sections = Analyze @('--subtree',$Subtree,'--sections','hotPaths')
if ($null -ne $sections.analysis.callTree -or $null -ne $sections.analysis.functionsBySelf -or @($sections.analysis.sectionsOmitted).Count -ne 3) { throw 'section selection not honored' }
[void](Analyze @('--sections','nope') $false)
$compactBytes = [Text.Encoding]::UTF8.GetByteCount($compact)
$byId = Analyze @('--subtree',$root[0].id)
if ($byId.metrics.samples -ne $sub.metrics.samples) { throw 'subtree id/name mismatch' }
$combined = Analyze @('--start-ms',$boundary,'--thread',"$thread",'--subtree',$Subtree,'--function',$Subtree)
if ($combined.metrics.samples -gt $right.metrics.samples -or $combined.metrics.samples -gt $oneThread.metrics.samples -or $combined.metrics.samples -gt $sub.metrics.samples) { throw 'filters do not compose with AND' }
$d = $combined.sourceDrilldown
if ($d.processSamples -ne $baseline.metrics.samples -or $d.retainedSamples -ne $combined.metrics.samples) { throw 'source denominators inconsistent with filters' }
if ($d.missingStackSamplesInProcess -ne $baseline.quality.missingStacks) { throw 'source filtering hid process missing stacks' }
if ($d.selectedSelfSamples -ne ($d.rows | Measure-Object selfSamples -Sum).Sum + $d.samplesInOmittedRows) { throw 'filtered source rows do not conserve samples' }
$empty = Analyze @('--start-ms','999999999999','--end-ms','1000000000000','--function',$Subtree)
if ($empty.metrics.samples -ne 0 -or $null -ne $empty.metrics.firstSampleMs -or $null -ne $empty.metrics.lastSampleMs) { throw 'empty valid window not represented explicitly' }
if ($empty.filters.status -ne 'empty_selection' -or $empty.sourceDrilldown.status -ne 'no_retained_samples') { throw 'empty filter/source status misleading' }
foreach ($bad in @(@('--start-ms','NaN'),@('--end-ms','Infinity'),@('--start-ms','-1'),@('--start-ms','1','--end-ms','1'),@('--thread','0'),@('--thread','2147483647'),@('--subtree','not_a_function_in_this_trace'),@('--start-ms','1','--start-ms','2'))) { [void](Analyze $bad $false) }
if ((Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash -ne $before) { throw 'source capture changed' }
"PASS: time partitions=$($left.metrics.samples)+$($right.metrics.samples), thread=$thread samples=$($oneThread.metrics.samples), subtree=$($sub.metrics.samples), indeterminate=$($sub.filters.indeterminateSubtreeSamples), combined=$($combined.metrics.samples), subtreeTop30Bytes=$compactBytes"
