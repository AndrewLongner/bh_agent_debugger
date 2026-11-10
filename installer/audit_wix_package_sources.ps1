# Fails when the WiX source build got a WiX package from anywhere but its own build output.
# NuGet's HTTP cache records every remote feed request (list_<id>.dat, nupkg_<id>.<version>.dat),
# and each package in the global packages folder records its source in .nupkg.metadata.
param(
    [Parameter(Mandatory = $true)][string]$WixHome,
    [Parameter(Mandatory = $true)][string]$Artifacts
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$artifactsFull = [IO.Path]::GetFullPath($Artifacts).TrimEnd('\')
$problems = @()
$httpCache = Join-Path $WixHome '.nuget\http-cache'
if (Test-Path -LiteralPath $httpCache) {
    foreach ($file in Get-ChildItem -LiteralPath $httpCache -Recurse -File -Force) {
        if ($file.Name -match '^(list|nupkg)_wix') { $problems += "remote feed request $($file.FullName)" }
    }
}
$packages = Join-Path $WixHome '.nuget\packages'
if (Test-Path -LiteralPath $packages) {
    foreach ($id in Get-ChildItem -LiteralPath $packages -Directory -Force | Where-Object { $_.Name -like 'wix*' }) {
        foreach ($version in Get-ChildItem -LiteralPath $id.FullName -Directory -Force) {
            $metadata = Join-Path $version.FullName '.nupkg.metadata'
            $source = $null
            if (Test-Path -LiteralPath $metadata) { $source = (Get-Content -Raw -LiteralPath $metadata | ConvertFrom-Json).source }
            if (!$source -or $source.TrimEnd('\') -ne $artifactsFull) { $problems += "$($id.Name) $($version.Name) from '$source'" }
        }
    }
}
if ($problems) {
    foreach ($problem in $problems) { Write-Host "error: WiX package not from the source build: $problem" }
    exit 1
}
'Every WiX package came from the source build; none was requested from a remote feed.'
