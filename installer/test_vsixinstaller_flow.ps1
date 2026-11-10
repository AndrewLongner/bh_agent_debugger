$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$msi = Join-Path $repoRoot 'Temp\installer\bh_agent_debugger.msi'
$log = Join-Path $repoRoot 'Temp\installer\vsixinstaller_flow.log'

Get-Process devenv -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Start-Process -FilePath msiexec.exe -ArgumentList @('/x', $msi, '/qn') -Wait | Out-Null

$proc = Start-Process -FilePath msiexec.exe -ArgumentList @(
    '/i',
    $msi,
    '/qn',
    '/l*v',
    $log
) -Wait -PassThru

Write-Output ("EXIT={0}" -f $proc.ExitCode)

Select-String -Path $log -Pattern 'caSyncVsExtensions|SyncVsExtensions' | ForEach-Object {
    Write-Output $_.Line
}
