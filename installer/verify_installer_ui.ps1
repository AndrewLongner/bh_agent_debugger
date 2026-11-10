param(
    [string]$MsiPath = "",
    [int]$TimeoutSeconds = 180,
    [switch]$SkipUninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

if ([string]::IsNullOrWhiteSpace($MsiPath)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $repoRoot = Resolve-Path (Join-Path $scriptDir "..")
    $MsiPath = Join-Path $repoRoot "Temp\installer\bh_agent_debugger.msi"
}

if (-not (Test-Path $MsiPath)) {
    throw "MSI not found: $MsiPath"
}

function Get-InstallerWindow {
    param([int]$ProcessId)

    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $ProcessId
    )
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
}

function Find-VisibleElementById {
    param(
        [System.Windows.Automation.AutomationElement]$Window,
        [string]$AutomationId
    )

    $all = $Window.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            $AutomationId
        )
    )

    foreach ($node in $all) {
        if (-not $node.Current.IsOffscreen) {
            return $node
        }
    }

    return $null
}

function Wait-VisibleElementById {
    param(
        [System.Windows.Automation.AutomationElement]$Window,
        [string]$AutomationId,
        [int]$TimeoutSec = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $node = Find-VisibleElementById -Window $Window -AutomationId $AutomationId
        if ($null -ne $node) {
            return $node
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for control '$AutomationId'"
}

function Invoke-Button {
    param([System.Windows.Automation.AutomationElement]$Button)

    $patternObj = $Button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern = [System.Windows.Automation.InvokePattern]$patternObj
    $pattern.Invoke()
}

function Set-CheckboxOn {
    param([System.Windows.Automation.AutomationElement]$Checkbox)

    $patternObj = $Checkbox.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $pattern = [System.Windows.Automation.TogglePattern]$patternObj
    if ($pattern.Current.ToggleState -ne [System.Windows.Automation.ToggleState]::On) {
        $pattern.Toggle()
    }
}

function Test-NameExists {
    param(
        [System.Windows.Automation.AutomationElement]$Scope,
        [string]$Fragment
    )

    $allText = $Scope.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Text
        )
    )

    foreach ($node in $allText) {
        $name = $node.Current.Name
        if (-not [string]::IsNullOrWhiteSpace($name) -and $name.IndexOf($Fragment, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }

    return $false
}

Write-Host "[ui] Starting interactive installer"
$process = Start-Process -FilePath "msiexec.exe" -ArgumentList @("/i", "`"$MsiPath`"") -PassThru

try {
    $deadline = (Get-Date).AddSeconds(30)
    $window = $null
    while ((Get-Date) -lt $deadline) {
        $window = Get-InstallerWindow -ProcessId $process.Id
        if ($null -ne $window) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if ($null -eq $window) {
        throw "Installer UI window did not appear"
    }

    Write-Host "[ui] Welcome -> License"
    Invoke-Button (Wait-VisibleElementById -Window $window -AutomationId "Next")

    Write-Host "[ui] Accept license -> Features"
    Set-CheckboxOn (Wait-VisibleElementById -Window $window -AutomationId "LicenseAcceptedCheckBox")
    Invoke-Button (Wait-VisibleElementById -Window $window -AutomationId "Next")

    Write-Host "[ui] Features -> InstallDir"
    Wait-VisibleElementById -Window $window -AutomationId "Tree" | Out-Null
    Invoke-Button (Wait-VisibleElementById -Window $window -AutomationId "Next")

    Write-Host "[ui] InstallDir -> VerifyReady"
    Wait-VisibleElementById -Window $window -AutomationId "Folder" | Out-Null
    Invoke-Button (Wait-VisibleElementById -Window $window -AutomationId "Next")

    Write-Host "[ui] VerifyReady -> Install"
    $installButton = Find-VisibleElementById -Window $window -AutomationId "Install"
    if ($null -eq $installButton) {
        $installButton = Wait-VisibleElementById -Window $window -AutomationId "InstallNoShield"
    }
    Invoke-Button $installButton

    Write-Host "[ui] Waiting for completion or error"
    $installDeadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $sawFinish = $false
    while ((Get-Date) -lt $installDeadline) {
        $window = Get-InstallerWindow -ProcessId $process.Id
        if ($null -eq $window) {
            break
        }

        if (Test-NameExists -Scope $window -Fragment "Error 2343") {
            throw "Detected MSI UI error: Error 2343"
        }

        $finish = Find-VisibleElementById -Window $window -AutomationId "Finish"
        if ($null -ne $finish) {
            Invoke-Button $finish
            $sawFinish = $true
            break
        }

        Start-Sleep -Milliseconds 300
    }

    if (-not $sawFinish) {
        throw "Installer did not reach Finish within timeout"
    }

    Write-Host "[ui] PASS"
}
finally {
    if (-not $process.HasExited) {
        try {
            $process.CloseMainWindow() | Out-Null
            Start-Sleep -Milliseconds 500
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force
            }
        }
        catch {
        }
    }

    if (-not $SkipUninstall) {
        Write-Host "[ui] Running cleanup uninstall"
        $args = @(
            "/x", "`"$MsiPath`"",
            "/qn"
        )
        $u = Start-Process -FilePath "msiexec.exe" -ArgumentList $args -Wait -PassThru -NoNewWindow
        if ($u.ExitCode -ne 0) {
            throw "Cleanup uninstall failed with exit code $($u.ExitCode)"
        }
    }
}
