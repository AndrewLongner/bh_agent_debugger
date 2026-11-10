$ErrorActionPreference = 'Stop'

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-BhAgentProductCodes {
    $roots = @(
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
    )

    $codes = New-Object System.Collections.Generic.List[string]

    foreach ($root in $roots) {
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($null -eq $p) { return }
            if ($p.DisplayName -notlike 'BH Agent Debugger*') { return }

            $key = $_.PSChildName
            if ($key -match '^\{[0-9A-Fa-f-]{36}\}$') {
                if (-not $codes.Contains($key)) {
                    $codes.Add($key)
                }
            }
        }
    }

    return $codes
}

if (-not (Test-IsAdmin)) {
    Write-Host 'Relaunching elevated...'
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        ('"{0}"' -f $PSCommandPath)
    )
    Start-Process -FilePath 'powershell.exe' -ArgumentList $args -Verb RunAs | Out-Null
    exit 0
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$logDir = Join-Path $repoRoot 'Temp\installer\admin_cleanup'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$codes = Get-BhAgentProductCodes
if ($codes.Count -eq 0) {
    Write-Host 'No BH Agent Debugger MSI entries found.'
    exit 0
}

Write-Host "Found $($codes.Count) BH Agent Debugger entries. Removing..."

$results = @()
foreach ($code in $codes) {
    $safe = $code.Trim('{}')
    $logPath = Join-Path $logDir ("remove_{0}.log" -f $safe)
    $argList = "/x $code /qn /l*v `"$logPath`""

    $proc = Start-Process -FilePath 'msiexec.exe' -ArgumentList $argList -Wait -PassThru
    $results += [PSCustomObject]@{
        ProductCode = $code
        ExitCode = $proc.ExitCode
        Log = $logPath
    }

    Write-Host ("{0} -> {1}" -f $code, $proc.ExitCode)
}

Write-Host ''
Write-Host 'Results:'
$results | Format-Table -AutoSize

$remaining = Get-BhAgentProductCodes
Write-Host ''
Write-Host ("Remaining BH Agent Debugger entries: {0}" -f $remaining.Count)
foreach ($code in $remaining) {
    Write-Host ("  {0}" -f $code)
}

if ($remaining.Count -gt 0) {
    exit 1
}

exit 0
