<#
.SYNOPSIS
Returns the WiX v6.0.2 tools built from source, as located by find_wix.cmd.

.DESCRIPTION
Outputs an object with Exe (wix.exe) and UiExtension (WixToolset.UI.wixext.dll). Honors
BH_WIX_EXE and BH_WIX_UI_EXT like find_wix.cmd. Throws when the source build is missing.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$findWix = Join-Path $PSScriptRoot 'find_wix.cmd'
# cmd.exe removes the outer quotes and runs: call "<find_wix.cmd>" && set BH_WIX_
$output = @(& cmd.exe /d /c "call `"$findWix`" && set BH_WIX_")
if ($LASTEXITCODE -ne 0) { throw (($output | ForEach-Object { "$_" }) -join [Environment]::NewLine) }
$values = @{}
foreach ($line in $output) {
    $text = "$line"
    $separator = $text.IndexOf('=')
    if ($text.StartsWith('BH_WIX_') -and $separator -gt 0) { $values[$text.Substring(0, $separator)] = $text.Substring($separator + 1) }
}
foreach ($name in 'BH_WIX_EXE', 'BH_WIX_UI_EXT') {
    if (-not $values.ContainsKey($name) -or -not (Test-Path -LiteralPath $values[$name] -PathType Leaf)) { throw "find_wix.cmd did not report $name" }
}
[pscustomobject]@{ Exe = $values['BH_WIX_EXE']; UiExtension = $values['BH_WIX_UI_EXT'] }
