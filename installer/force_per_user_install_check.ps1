$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$msi = Join-Path $repoRoot 'Temp\installer\bh_agent_debugger.msi'
$log = Join-Path $repoRoot 'Temp\installer\force_per_user_install.log'

Start-Process -FilePath msiexec.exe -ArgumentList @('/x', $msi, '/qn') -Wait | Out-Null

$p = Start-Process -FilePath msiexec.exe -ArgumentList @(
    '/i',
    $msi,
    '/qn',
    '/l*v',
    $log
) -Wait -PassThru

Write-Output ("EXIT={0}" -f $p.ExitCode)

$perUser = Join-Path $env:LOCALAPPDATA 'Programs\BH Agent Debugger\extension\bh_agent_debugger.vsix'
$machine = 'C:\Program Files\BH Agent Debugger\extension\bh_agent_debugger.vsix'

if (Test-Path -LiteralPath $perUser) {
    Write-Output 'PER_USER_SRC_OK'
}
else {
    Write-Output 'PER_USER_SRC_MISSING'
}

if (Test-Path -LiteralPath $machine) {
    Write-Output 'MACHINE_SRC_PRESENT'
}
else {
    Write-Output 'MACHINE_SRC_ABSENT'
}
