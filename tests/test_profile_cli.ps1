param([string]$Pipe)
$ErrorActionPreference = 'Stop'
$cli = Join-Path (Split-Path $PSScriptRoot -Parent) 'Temp\bh_agent_debugger_cli_x64_Debug\bh_agent_debugger_cli.exe'
function Invoke-Checked([string[]]$Arguments, [bool]$Success) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $text = (& $cli @Arguments 2>&1 | Out-String).Trim()
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    if (($code -eq 0) -ne $Success) { throw "unexpected exit=$code args=$Arguments output=$text" }
    return $text
}
$r = (Invoke-Checked @('profile','start','--pid','123','--seconds','2','--print-request') $true) | ConvertFrom-Json
if ($r.method -ne 'profile.command' -or $r.message -ne 'profile start 123 2') { throw 'incorrect start request' }
if ($r.PSObject.Properties.Name -contains 'output') { throw 'default start must not send output' }
foreach ($out in @('C:\cap tures\run 1.diagsession', 'D:\work\profiles\', "C:\$([char]0x00E9)t$([char]0x00E9)\cap.diagsession")) {
    $r = (Invoke-Checked @('profile','start','--pid','123','--output',$out,'--print-request') $true) | ConvertFrom-Json
    if ($r.output -ne $out -or $r.message -ne 'profile start 123 10') { throw "incorrect output serialization: $($r.output)" }
}
$r = (Invoke-Checked @('profile','open','--target-id','vs_123','--print-request') $true) | ConvertFrom-Json
if ($r.targetId -ne 'vs_123' -or $r.message -ne 'profile open') { throw 'incorrect open request' }
foreach ($verb in @('status','stop','report')) {
    $r = (Invoke-Checked @('profile',$verb,'--print-request') $true) | ConvertFrom-Json
    if ($r.message -ne "profile $verb") { throw 'incorrect control request' }
}
$invalid = @(
    @('start'), @('start','--pid','0'), @('start','--pid','-1'), @('start','--pid','4294967296'),
    @('start','--pid','123','--seconds','0'), @('start','--pid','123','--seconds','61'),
    @('start','--pid','123','--pid','456'), @('status','--pid','123'), @('open'),
    @('open','--target-id','123'), @('open','--target-id','vs_0'), @('status','--target-id','vs_123'),
    @('status','--timeout-ms','abc'), @('status','--typo','1'), @('functions'),
    @('status','--output','C:\x'), @('stop','--output','C:\x'), @('start','--pid','123','--output',''),
    @('start','--pid','123','--output','C:\a','--output','C:\b'), @('start','--pid','123','--output',('C:\' + ('a' * 200)))
)
foreach ($args in $invalid) { [void](Invoke-Checked (@('profile') + $args + @('--print-request')) $false) }
if ($Pipe) {
    $r = (Invoke-Checked @('profile','status','--pipe',$Pipe,'--no-auto-start-service') $true) | ConvertFrom-Json
    if (!$r.ok -or $r.result.text -notmatch 'backend=vs_cpu_usage state=idle') { throw 'unexpected broker state' }
    [void](Invoke-Checked @('profile','report','--pipe',$Pipe) $false)
    $r = (Invoke-Checked @('profile','stop','--pipe',$Pipe) $true) | ConvertFrom-Json
    if (!$r.ok) { throw 'idle stop failed' }
}
'PASS: profile CLI validation (live capture is tested separately)'
