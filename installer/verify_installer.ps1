param(
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$SkipUninstall,
    [switch]$RunUiFlow
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$outDir = Join-Path $repoRoot "Temp\installer"
$msiPath = Join-Path $outDir "bh_agent_debugger.msi"
$verifyDir = Join-Path $outDir "verify"

New-Item -ItemType Directory -Path $verifyDir -Force | Out-Null
$wix = & (Join-Path $scriptDir 'find_wix.ps1')

if (-not $SkipBuild) {
    Write-Host "[verify] Building MSI"
    & $wix.Exe build (Join-Path $scriptDir "Product.wxs") -ext $wix.UiExtension -o $msiPath
    if ($LASTEXITCODE -ne 0) {
        throw "wix build failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path $msiPath)) {
    throw "MSI output not found: $msiPath"
}

$decompiledPath = Join-Path $verifyDir "decompiled.wxs"
Write-Host "[verify] Decompiling MSI"
& $wix.Exe msi decompile $msiPath -o $decompiledPath

if (-not (Test-Path $decompiledPath)) {
    throw "Decompiled MSI output not found: $decompiledPath"
}

$decompiled = Get-Content -Raw -Path $decompiledPath

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Label
    )
    if ($Text -notmatch [regex]::Escape($Needle)) {
        throw "Missing expected UI route: $Label"
    }
}

function Get-MsiProperty {
    param(
        [string]$Path,
        [string]$Name
    )
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
    $query = 'SELECT Value FROM Property WHERE Property=''' + $Name + ''''
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($query))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
    $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
    $value = $null
    if ($null -ne $record) {
        $value = $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
    }
    $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
    foreach ($com in @($record, $view, $database, $installer)) {
        if ($null -ne $com) { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($com) }
    }
    return $value
}

function Get-MsiWordCount {
    param(
        [string]$Path
    )
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $summary = $installer.GetType().InvokeMember('SummaryInformation', 'GetProperty', $null, $installer, @($Path, 0))
    $value = [int]$summary.GetType().InvokeMember('Property', 'GetProperty', $null, $summary, @(15))
    foreach ($com in @($summary, $installer)) {
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($com)
    }
    return $value
}

Write-Host "[verify] Validating dialog routes"
Assert-Contains -Text $decompiled -Needle 'Dialog Id="LicenseAgreementDlg"' -Label "License dialog present"
Assert-Contains -Text $decompiled -Needle 'Value="BhCustomizeDlg"' -Label "License -> Features"
Assert-Contains -Text $decompiled -Needle 'Dialog Id="BhCustomizeDlg"' -Label "Features dialog present"
Assert-Contains -Text $decompiled -Needle 'Value="InstallDirDlg"' -Label "Features -> Path"
Assert-Contains -Text $decompiled -Needle 'Dialog Id="InstallDirDlg"' -Label "Path dialog present"
Assert-Contains -Text $decompiled -Needle 'Value="VerifyReadyDlg"' -Label "Path -> Ready"

Write-Host "[verify] Validating the Claude Code feature"
Assert-Contains -Text $decompiled -Needle 'Feature Id="ClaudeCodeFeature"' -Label "Claude Code feature present"
Assert-Contains -Text $decompiled -Needle 'DllEntry="SyncClaudeCode"' -Label "Claude Code sync action present"
Assert-Contains -Text $decompiled -Needle 'DllEntry="CleanupClaudeCode"' -Label "Claude Code cleanup action present"

Write-Host "[verify] Validating the Codex features"
Assert-Contains -Text $decompiled -Needle 'Feature Id="CodexFeature"' -Label "Codex feature present"
Assert-Contains -Text $decompiled -Needle 'Feature Id="CodexWslFeature"' -Label "Codex WSL feature present"
foreach ($entry in 'SyncCodex', 'CleanupCodex', 'SyncCodexWsl', 'CleanupCodexWsl') {
    Assert-Contains -Text $decompiled -Needle "DllEntry=`"$entry`"" -Label "Codex action $entry present"
}

Write-Host "[verify] Validating the per-user install scope"
$allUsers = Get-MsiProperty -Path $msiPath -Name 'ALLUSERS'
$perUser = Get-MsiProperty -Path $msiPath -Name 'MSIINSTALLPERUSER'
if ($null -ne $allUsers -or $null -ne $perUser) {
    throw "MSI must install per user only (no ALLUSERS or MSIINSTALLPERUSER property); found ALLUSERS=$allUsers MSIINSTALLPERUSER=$perUser"
}
# Summary information word count bit 8: elevated privileges are not required (Package Scope="perUser").
$wordCount = Get-MsiWordCount -Path $msiPath
if (($wordCount -band 8) -eq 0) {
    throw "MSI summary information must mark the package as not needing elevation; word count=$wordCount"
}

if (-not $SkipInstall) {
    $installLog = Join-Path $verifyDir "install.log"
    Write-Host "[verify] Running silent per-user install"
    $installArgs = @(
        "/i", "`"$msiPath`"",
        "/qn",
        "/l*v", "`"$installLog`""
    )
    $p = Start-Process -FilePath "msiexec.exe" -ArgumentList $installArgs -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) {
        throw "Install failed with exit code $($p.ExitCode). Log: $installLog"
    }

    $logText = Get-Content -Raw -Path $installLog
    if ($logText -match "Return value 3") {
        throw "Install log indicates failure (Return value 3). Log: $installLog"
    }
    if (($logText -notmatch "Installation success or error status:\s*0") -and ($logText -notmatch "MainEngineThread is returning 0")) {
        throw "Install log does not show success status 0 or engine return 0. Log: $installLog"
    }

    if (-not $SkipUninstall) {
        $uninstallLog = Join-Path $verifyDir "uninstall.log"
        Write-Host "[verify] Running silent uninstall"
        $uninstallArgs = @(
            "/x", "`"$msiPath`"",
            "/qn",
            "/l*v", "`"$uninstallLog`""
        )
        $u = Start-Process -FilePath "msiexec.exe" -ArgumentList $uninstallArgs -Wait -PassThru -NoNewWindow
        if ($u.ExitCode -ne 0) {
            throw "Uninstall failed with exit code $($u.ExitCode). Log: $uninstallLog"
        }

        $uninstallText = Get-Content -Raw -Path $uninstallLog
        if ($uninstallText -match "Return value 3") {
            throw "Uninstall log indicates failure (Return value 3). Log: $uninstallLog"
        }
        if (($uninstallText -notmatch "Removal success or error status:\s*0") -and ($uninstallText -notmatch "MainEngineThread is returning 0")) {
            throw "Uninstall log does not show success status 0 or engine return 0. Log: $uninstallLog"
        }
    }
}

if ($RunUiFlow) {
    Write-Host "[verify] Running interactive UI flow check"
    & (Join-Path $scriptDir "verify_installer_ui.ps1") -MsiPath $msiPath
    if ($LASTEXITCODE -ne 0) {
        throw "Interactive UI flow check failed with exit code $LASTEXITCODE"
    }
}

Write-Host "[verify] PASS"
