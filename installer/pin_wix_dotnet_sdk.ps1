# WiX 6.0.2's build writes global.json from a template that does not pin a .NET SDK, so it uses the
# newest SDK installed. The .NET 10 SDK needs MSBuild 18 (Visual Studio 2026), but WiX builds with
# Visual Studio 2022 (MSBuild 17.x), so pin the WiX build to the .NET 9 SDK feature bands.
# This changes only SDK selection in the local WiX clone, never WiX source code.
param([Parameter(Mandatory = $true)][string]$WixSource)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sdks = @(& dotnet --list-sdks 2>$null) | Where-Object { $_ -match '^9\.0\.\d+' }
if (-not $sdks) {
    throw 'Building WiX with Visual Studio 2022 needs a .NET 9 SDK. Install the .NET 9 SDK, then rerun.'
}

$template = Join-Path $WixSource 'src\internal\SetBuildNumber\global.json.pp'
$text = [IO.File]::ReadAllText($template)
$pin = '"version": "9.0.100"'
if ($text.Contains($pin)) {
    'WiX build already pinned to the .NET 9 SDK'
    exit 0
}
$anchor = '"allowPrerelease": false'
if ([regex]::Matches($text, [regex]::Escape($anchor)).Count -ne 1) {
    throw "Unexpected WiX global.json template: $template"
}
$newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$replacement = $pin + ',' + $newline + '    "rollForward": "latestFeature",' + $newline + '    ' + $anchor
[IO.File]::WriteAllText($template, $text.Replace($anchor, $replacement), (New-Object System.Text.UTF8Encoding($false)))
'Pinned the WiX build to the .NET 9 SDK'
