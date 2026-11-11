<#
.SYNOPSIS
Publishes the release MSI to the Visual Studio Marketplace listing BoilingLavaHotLLC.BHAgentDebugger.

.DESCRIPTION
1. Reads the MSI ProductVersion and checks it against installer\Product.wxs and
   vsix\source.extension.vsixmanifest.
2. Unless -SkipGitHubCheck, checks that the MSI is byte-identical to the bh_agent_debugger.msi asset of
   GitHub release v<version>, so the Marketplace and GitHub ship the same file.
3. Compares with the version the public Marketplace serves: an identical upload is a no-op, and an
   older or same-version-but-different MSI is refused.
4. Stages marketplace\publishManifest.json (with the MSI version filled in), overview.md and the icon
   under Temp\marketplace_publish\<version>, then runs VsixPublisher.exe publish.
5. Waits until the public Marketplace serves the new version and checks that its download is
   byte-identical to the MSI.

-DryRun runs steps 1-4 without publishing.

Authentication: releases are published by .github\workflows\publish-marketplace.yml, which runs this
script with -Oidc (trusted publishing from GitHub Actions; needs a VsixPublisher that supports -oidc,
see installer\get_vsixpublisher.ps1). Without -Oidc, VsixPublisher uses the publisher credentials
saved on this PC by
  VsixPublisher.exe login -publisherName BoilingLavaHotLLC -personalAccessToken <PAT>
This script never takes or prints a token. The GitHub release check sends $env:GITHUB_TOKEN when it
is set (GitHub Actions) to avoid the anonymous API rate limit.

Exit codes: 0 published and verified (or already published), 1 error, 2 uploaded but the public
Marketplace did not serve the new version within -VerifyTimeoutSeconds.
#>
param(
    [string]$MsiPath,
    [switch]$DryRun,
    [switch]$Oidc,
    [switch]$SkipGitHubCheck,
    [string]$VsixPublisherPath,
    [int]$VerifyTimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $MsiPath)
{
    $MsiPath = Join-Path $repoRoot 'Temp\installer\bh_agent_debugger.msi'
}

$publisherName = 'BoilingLavaHotLLC'
$extensionName = 'BHAgentDebugger'
$githubRepo = 'AndrewLongner/bh_agent_debugger'
$msiAssetName = 'bh_agent_debugger.msi'
$galleryQueryUri = 'https://marketplace.visualstudio.com/_apis/public/gallery/extensionquery'
$marketplaceDir = Join-Path $repoRoot 'marketplace'
$stageRoot = Join-Path $repoRoot 'Temp\marketplace_publish'

function Get-Sha256([string]$Path)
{
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-MsiProductVersion([string]$Path)
{
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    $view = $null
    try
    {
        $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database,
            @("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='ProductVersion'"))
        [void]$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $record)
        {
            throw "MSI has no ProductVersion: $Path"
        }
        return $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
    }
    finally
    {
        if ($view)
        {
            [void]$view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
        }
        if ($database)
        {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database)
        }
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
}

function Get-RegexVersion([string]$Path, [string]$Pattern)
{
    $text = [IO.File]::ReadAllText($Path)
    $match = [regex]::Match($text, $Pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success)
    {
        throw "No version found in $Path"
    }
    return $match.Groups[1].Value
}

function Get-LiveMarketplaceVersion
{
    $body = @{
        filters = @(@{ criteria = @(@{ filterType = 7; value = "$publisherName.$extensionName" }); pageNumber = 1; pageSize = 1 })
        flags = 0x1
    } | ConvertTo-Json -Depth 6
    $headers = @{ Accept = 'application/json;api-version=3.0-preview.1' }
    $result = Invoke-RestMethod -Method Post -Uri $galleryQueryUri -Body $body -ContentType 'application/json' -Headers $headers
    $extensions = $result.results[0].extensions
    if (-not $extensions)
    {
        return $null
    }
    return $extensions[0].versions[0].version
}

function Get-LiveMarketplaceSha256([string]$Version, [string]$OutDir)
{
    if (-not (Test-Path -LiteralPath $OutDir))
    {
        New-Item -ItemType Directory -Path $OutDir | Out-Null
    }
    $out = Join-Path $OutDir "marketplace_$Version.msi"
    $uri = "https://marketplace.visualstudio.com/_apis/public/gallery/publishers/$publisherName/vsextensions/$extensionName/$Version/vspackage"
    Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $out
    $hash = Get-Sha256 $out
    Remove-Item -LiteralPath $out -Force
    return $hash
}

function Test-VsixPublisherOidc([string]$Path)
{
    # VsixPublisher prints its options for an unknown argument. Windows PowerShell turns redirected
    # native stderr into errors, so don't stop on them here.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try
    {
        $help = & $Path publish -help 2>&1 | Out-String
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
    return ($help -match '(?m)^\s*-oidc\b')
}

function Find-VsixPublisher([bool]$NeedOidc)
{
    if ($VsixPublisherPath)
    {
        if (-not (Test-Path -LiteralPath $VsixPublisherPath))
        {
            throw "VsixPublisher not found: $VsixPublisherPath"
        }
        if ($NeedOidc -and -not (Test-VsixPublisherOidc $VsixPublisherPath))
        {
            throw "VsixPublisher does not support -oidc: $VsixPublisherPath"
        }
        return $VsixPublisherPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere))
    {
        throw "vswhere.exe not found; pass -VsixPublisherPath"
    }

    $candidates = @()
    foreach ($root in (& $vswhere -all -prerelease -products * -property installationPath))
    {
        $exe = Join-Path $root 'VSSDK\VisualStudioIntegration\Tools\Bin\VsixPublisher.exe'
        if (Test-Path -LiteralPath $exe)
        {
            $candidates += [pscustomobject]@{ Path = $exe; Version = [version](Get-Item -LiteralPath $exe).VersionInfo.FileVersion }
        }
    }

    foreach ($candidate in ($candidates | Sort-Object Version -Descending))
    {
        if (-not $NeedOidc -or (Test-VsixPublisherOidc $candidate.Path))
        {
            return $candidate.Path
        }
    }

    if ($NeedOidc)
    {
        throw 'No installed VsixPublisher supports -oidc; pass -VsixPublisherPath'
    }
    throw 'VsixPublisher.exe not found (needs the Visual Studio extension development workload); pass -VsixPublisherPath'
}

# 1. Versions
if (-not (Test-Path -LiteralPath $MsiPath))
{
    throw "MSI not found: $MsiPath"
}
$MsiPath = (Resolve-Path -LiteralPath $MsiPath).Path
if ((Split-Path -Leaf $MsiPath) -ne $msiAssetName)
{
    throw "The payload must be named $msiAssetName (the Marketplace shows the file name): $MsiPath"
}

$version = Get-MsiProductVersion $MsiPath
$msiSha256 = Get-Sha256 $MsiPath
$wxsVersion = Get-RegexVersion (Join-Path $repoRoot 'installer\Product.wxs') '<Package\b[^>]*?\sVersion="([^"]+)"'
$vsixVersion = Get-RegexVersion (Join-Path $repoRoot 'vsix\source.extension.vsixmanifest') '<Identity\b[^>]*?\sVersion="([^"]+)"'
Write-Output "MSI:         $MsiPath"
Write-Output "             version=$version sha256=$msiSha256"
if ($wxsVersion -ne $version -or $vsixVersion -ne $version)
{
    throw "Version mismatch: MSI=$version Product.wxs=$wxsVersion vsixmanifest=$vsixVersion"
}
if ($version -notmatch '^([0-9]+\.){1,3}[0-9]+$')
{
    throw "MSI version '$version' is not a Marketplace version"
}

# 2. GitHub release
if ($SkipGitHubCheck)
{
    Write-Output 'GitHub:      check skipped'
}
else
{
    $releaseUri = "https://api.github.com/repos/$githubRepo/releases/tags/v$version"
    try
    {
        $githubHeaders = @{ 'User-Agent' = 'bh_agent_debugger-publish'; Accept = 'application/vnd.github+json' }
        if ($env:GITHUB_TOKEN)
        {
            $githubHeaders.Authorization = "Bearer $env:GITHUB_TOKEN"
        }
        $release = Invoke-RestMethod -Uri $releaseUri -Headers $githubHeaders
    }
    catch
    {
        throw "GitHub release v$version not found ($releaseUri). Publish the GitHub release first, or pass -SkipGitHubCheck."
    }
    $asset = $release.assets | Where-Object { $_.name -eq $msiAssetName } | Select-Object -First 1
    if (-not $asset)
    {
        throw "GitHub release v$version has no $msiAssetName asset"
    }
    if (-not $asset.digest)
    {
        throw "GitHub release v$version asset $msiAssetName has no digest to compare"
    }
    if ($asset.digest -ne "sha256:$msiSha256")
    {
        throw "MSI differs from GitHub release v$version ($($asset.digest)); publish the same file to both"
    }
    Write-Output "GitHub:      release v$version asset matches"
}

# 3. Live Marketplace version
$liveVersion = Get-LiveMarketplaceVersion
Write-Output "Marketplace: live version=$liveVersion"
if ($liveVersion)
{
    if ([version]$liveVersion -gt [version]$version)
    {
        throw "The Marketplace already serves $liveVersion, newer than $version"
    }
    if ([version]$liveVersion -eq [version]$version)
    {
        $liveSha256 = Get-LiveMarketplaceSha256 $liveVersion (Join-Path $stageRoot 'verify')
        if ($liveSha256 -eq $msiSha256)
        {
            Write-Output "Marketplace: $version is already published with this MSI; nothing to do"
            exit 0
        }
        throw "The Marketplace serves a different $version MSI (sha256=$liveSha256); bump the version instead of overwriting it"
    }
}

# 4. Stage manifest, overview and icon
$stageDir = Join-Path $stageRoot $version
if (Test-Path -LiteralPath $stageDir)
{
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDir | Out-Null

$manifest = Get-Content -LiteralPath (Join-Path $marketplaceDir 'publishManifest.json') -Raw | ConvertFrom-Json
if ($manifest.identity.PSObject.Properties['version'])
{
    throw 'marketplace\publishManifest.json must not set identity.version; this script fills it in from the MSI'
}
if ($manifest.publisher -ne $publisherName -or $manifest.identity.internalName -ne $extensionName)
{
    throw "publishManifest.json names $($manifest.publisher).$($manifest.identity.internalName), expected $publisherName.$extensionName"
}
if ($manifest.identity.description.Length -gt 200)
{
    throw "identity.description is $($manifest.identity.description.Length) characters; the Marketplace allows 200"
}
$manifest.identity | Add-Member -NotePropertyName version -NotePropertyValue $version

foreach ($file in @($manifest.overview, $manifest.identity.icon))
{
    Copy-Item -LiteralPath (Join-Path $marketplaceDir $file) -Destination (Join-Path $stageDir $file)
}
$manifestPath = Join-Path $stageDir 'publishManifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
Write-Output "Staged:      $stageDir"

$publisherExe = Find-VsixPublisher $Oidc.IsPresent
$publishArgs = @('publish', '-payload', $MsiPath, '-publishManifest', $manifestPath)
if ($Oidc)
{
    $publishArgs += '-oidc'
}
Write-Output "Publisher:   $publisherExe"
Write-Output ("Command:     VsixPublisher.exe " + ($publishArgs -join ' '))

if ($DryRun)
{
    Write-Output 'Dry run: nothing was published.'
    exit 0
}

# 5. Publish and verify
& $publisherExe @publishArgs
if ($LASTEXITCODE -ne 0)
{
    throw "VsixPublisher publish failed with exit code $LASTEXITCODE"
}

Write-Output "Uploaded $version; waiting for the public Marketplace (up to $VerifyTimeoutSeconds s)..."
$deadline = (Get-Date).AddSeconds($VerifyTimeoutSeconds)
while ((Get-Date) -lt $deadline)
{
    $liveVersion = Get-LiveMarketplaceVersion
    if ($liveVersion -and [version]$liveVersion -eq [version]$version)
    {
        $liveSha256 = Get-LiveMarketplaceSha256 $liveVersion (Join-Path $stageRoot 'verify')
        if ($liveSha256 -ne $msiSha256)
        {
            throw "The Marketplace serves $version but its download differs (sha256=$liveSha256)"
        }
        Write-Output "Marketplace: $version is live and its download matches the MSI"
        exit 0
    }
    Start-Sleep -Seconds 30
}

Write-Output "Marketplace: still serving $liveVersion after $VerifyTimeoutSeconds s; Microsoft's verification may take longer. Check later with -DryRun, which reports 'already published' once it is live."
exit 2
