[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VsixPath,

    [Parameter(Mandatory = $true)]
    [string]$PackageDllPath,

    [Parameter(Mandatory = $true)]
    [string]$SourceManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$ProductWxsPath,

    [string]$MsiPath = "",
    [string]$AnalyzerDirectory = "",

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RequiredFile {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-Sha256Text {
    param([byte[]]$Bytes)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha256.ComputeHash($Bytes))).Replace("-", "")
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-ZipEntryBytes {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string]$EntryName
    )

    $entries = @($Archive.Entries | Where-Object {
        [string]::Equals($_.FullName, $EntryName, [System.StringComparison]::OrdinalIgnoreCase)
    })
    if ($entries.Count -ne 1) {
        throw "expected exactly one VSIX entry '$EntryName', found $($entries.Count)"
    }

    $stream = $entries[0].Open()
    $memory = New-Object System.IO.MemoryStream
    try {
        $stream.CopyTo($memory)
        return $memory.ToArray()
    }
    finally {
        $memory.Dispose()
        $stream.Dispose()
    }
}

function Get-RequiredXmlAttribute {
    param(
        [xml]$Document,
        [string]$XPath,
        [string]$Attribute,
        [string]$Label
    )

    $node = $Document.SelectSingleNode($XPath)
    if (-not $node) {
        throw "$Label XML node not found"
    }
    $value = $node.GetAttribute($Attribute)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Label attribute '$Attribute' is missing"
    }
    return $value
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$resolvedVsix = Resolve-RequiredFile $VsixPath "VSIX"
$resolvedPackage = Resolve-RequiredFile $PackageDllPath "package DLL"
$resolvedSourceManifest = Resolve-RequiredFile $SourceManifestPath "source VSIX manifest"
$resolvedProductWxs = Resolve-RequiredFile $ProductWxsPath "installer source"

[xml]$sourceManifest = Get-Content -LiteralPath $resolvedSourceManifest -Raw
[xml]$productWxs = Get-Content -LiteralPath $resolvedProductWxs -Raw
$sourceVersion = Get-RequiredXmlAttribute $sourceManifest "//*[local-name()='Identity']" "Version" "source VSIX manifest"
$productVersion = Get-RequiredXmlAttribute $productWxs "//*[local-name()='Package']" "Version" "installer package"
if ($sourceVersion -ne $productVersion) {
    throw "release version mismatch: source VSIX=$sourceVersion installer=$productVersion"
}
$versionHeader = Resolve-RequiredFile (Join-Path $PSScriptRoot '..\bh_agent_version.h') "runtime version header"
$headerMatch = [regex]::Match((Get-Content -LiteralPath $versionHeader -Raw), 'static const char version\[\] = "([0-9.]+)";')
if (!$headerMatch.Success -or $headerMatch.Groups[1].Value -ne $productVersion) {
    throw "runtime version mismatch: bh_agent_version.h=$($headerMatch.Groups[1].Value) installer=$productVersion"
}

$packageBytes = [System.IO.File]::ReadAllBytes($resolvedPackage)
$transientMarker = "__bh_transient_start_hex "
if (-not [System.Text.Encoding]::ASCII.GetString($packageBytes).Contains($transientMarker)) {
    throw "built package DLL lacks transient-launch marker"
}
$packageHash = Get-Sha256Text $packageBytes

$archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedVsix)
try {
    $manifestBytes = Get-ZipEntryBytes $archive "extension.vsixmanifest"
    $embeddedPackageBytes = Get-ZipEntryBytes $archive "bh_agent_debugger_package.dll"
}
finally {
    $archive.Dispose()
}

$embeddedManifestText = [System.Text.Encoding]::UTF8.GetString($manifestBytes)
[xml]$embeddedManifest = $embeddedManifestText
$embeddedVersion = Get-RequiredXmlAttribute $embeddedManifest "//*[local-name()='Identity']" "Version" "embedded VSIX manifest"
if ($embeddedVersion -ne $sourceVersion) {
    throw "VSIX manifest version mismatch: source=$sourceVersion embedded=$embeddedVersion"
}

$embeddedPackageHash = Get-Sha256Text $embeddedPackageBytes
if ($embeddedPackageHash -ne $packageHash) {
    throw "VSIX package DLL is stale: built=$packageHash embedded=$embeddedPackageHash"
}
if (-not [System.Text.Encoding]::ASCII.GetString($embeddedPackageBytes).Contains($transientMarker)) {
    throw "VSIX package DLL lacks transient-launch marker"
}

$vsixHash = (Get-FileHash -LiteralPath $resolvedVsix -Algorithm SHA256).Hash
if (-not [string]::IsNullOrWhiteSpace($MsiPath)) {
    $resolvedMsi = Resolve-RequiredFile $MsiPath "MSI"
    $temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("bh_agent_debugger_verify_" + [guid]::NewGuid().ToString("N"))
    $payloadRoot = Join-Path $temporaryRoot "payload"
    $decompiledWxs = Join-Path $temporaryRoot "decompiled.wxs"
    try {
        New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
        $wix = & (Join-Path $PSScriptRoot 'find_wix.ps1')
        $wixOutput = & $wix.Exe msi decompile $resolvedMsi -x $payloadRoot -o $decompiledWxs 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "failed to decompile MSI for verification: $($wixOutput -join [Environment]::NewLine)"
        }

        [xml]$decompiledInstaller = Get-Content -LiteralPath $decompiledWxs -Raw
        $msiVersion = Get-RequiredXmlAttribute $decompiledInstaller "//*[local-name()='Package']" "Version" "decompiled MSI"
        if ($msiVersion -ne $productVersion) {
            throw "MSI ProductVersion mismatch: source=$productVersion built=$msiVersion"
        }

        $expectedUpgrade = Get-RequiredXmlAttribute $productWxs "//*[local-name()='Package']" 'UpgradeCode' 'installer source'
        $actualUpgrade = Get-RequiredXmlAttribute $decompiledInstaller "//*[local-name()='Package']" 'UpgradeCode' 'decompiled MSI'
        if ($expectedUpgrade.Trim('{}') -ne $actualUpgrade.Trim('{}')) { throw 'MSI UpgradeCode mismatch' }
        # ProductCode is generated per build unless the source pins one.
        $actualProductCode = Get-RequiredXmlAttribute $decompiledInstaller "//*[local-name()='Package']" 'ProductCode' 'decompiled MSI'
        $guid = [guid]::Empty
        if (![guid]::TryParse($actualProductCode, [ref]$guid) -or $guid -eq [guid]::Empty) { throw "MSI ProductCode invalid: $actualProductCode" }
        $pinnedProductCode = $productWxs.SelectSingleNode("//*[local-name()='Package']").GetAttribute('ProductCode')
        if ($pinnedProductCode -and $pinnedProductCode.Trim('{}') -ne $actualProductCode.Trim('{}')) { throw 'MSI ProductCode mismatch' }
        $repo = Split-Path (Split-Path $resolvedProductWxs -Parent) -Parent
        $knownPayload = @{
            'filCli' = (Join-Path $repo "Temp\bh_agent_debugger_cli_x64_$Configuration\bh_agent_debugger_cli.exe")
            'filService' = (Join-Path $repo "Temp\bh_agent_debugger_service_x64_$Configuration\bh_agent_debugger_service.exe")
            'filLicense' = (Join-Path $repo 'LICENSE')
            'filThirdPartyNotices' = (Join-Path $repo 'THIRD_PARTY_NOTICES.txt')
            'filOpenCodeToolPayload' = (Join-Path $repo 'opencode\tools\bh_agent_debugger.ts')
            'filOpenCodeSkillPayload' = (Join-Path $repo 'opencode\skills\bh-agent-debugger\SKILL.md')
        }
        foreach ($id in $knownPayload.Keys) {
            $embedded = Resolve-RequiredFile (Join-Path $payloadRoot "File\$id") $id
            if ((Get-Item -LiteralPath $embedded).Length -le 0 -or
                (Get-FileHash -LiteralPath $embedded -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $knownPayload[$id] -Algorithm SHA256).Hash) {
                throw "MSI payload stale or empty: $id"
            }
        }
        if ($AnalyzerDirectory) {
            # Reconstruct destination paths, not merely basenames: native symbol
            # helpers may have the same filename in architecture subdirectories.
            $directories = @{}
            foreach ($dir in $decompiledInstaller.SelectNodes("//*[local-name()='Directory' or local-name()='StandardDirectory']")) {
                $directories[$dir.GetAttribute('Id')] = $dir
            }
            $binFiles = @{}
            foreach ($file in $decompiledInstaller.SelectNodes("//*[local-name()='File']")) {
                $component = $file.ParentNode
                $dirId = $component.GetAttribute('Directory')
                if (!$dirId) { $dirId = $component.ParentNode.GetAttribute('Id') }
                $relative = $file.GetAttribute('Name')
                for ($depth = 0; $depth -lt 32 -and $dirId -ne 'BINFOLDER' -and $directories.ContainsKey($dirId); ++$depth) {
                    $dir = $directories[$dirId]
                    $name = $dir.GetAttribute('Name')
                    if (!$name) { break } # reached a standard folder outside bin
                    $relative = Join-Path $name $relative
                    $dirId = $dir.ParentNode.GetAttribute('Id')
                }
                if ($dirId -eq 'BINFOLDER') {
                    if ($binFiles.ContainsKey($relative)) { throw "Duplicate MSI bin destination: $relative" }
                    $binFiles[$relative] = Join-Path $payloadRoot ('File\' + $file.GetAttribute('Id'))
                }
            }
            $analyzerRoot = (Resolve-Path -LiteralPath $AnalyzerDirectory).Path.TrimEnd('\')
            $files = @(Get-ChildItem -LiteralPath $analyzerRoot -Recurse -File)
            if ($files.Count -lt 5) { throw 'Analyzer staging is incomplete' }
            $runRoot = Join-Path $temporaryRoot 'analyzer-smoke'
            New-Item -ItemType Directory -Path $runRoot | Out-Null
            foreach ($file in $files) {
                $relative = $file.FullName.Substring($analyzerRoot.Length + 1)
                if (!$binFiles.ContainsKey($relative)) { throw "Analyzer file not installed beside CLI: $relative" }
                $embedded = Resolve-RequiredFile $binFiles[$relative] "analyzer $relative"
                if ($file.Length -le 0 -or (Get-FileHash -LiteralPath $embedded -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) {
                    throw "MSI analyzer mismatch: $relative"
                }
                $destination = Join-Path $runRoot $relative
                New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
                Copy-Item -LiteralPath $embedded -Destination $destination
            }
            & (Join-Path $runRoot 'bh_profile_analyzer.exe') --self-test
            if ($LASTEXITCODE -ne 0) { throw 'MSI-extracted analyzer self-test failed' }
            Write-Output "MSI analyzer verified: files=$($files.Count), destination=bin, extracted self-test passed"
        }

        $embeddedVsixPath = Resolve-RequiredFile (Join-Path $payloadRoot "File\filExtensionVsix") "MSI-embedded VSIX"
        $embeddedVsixHash = (Get-FileHash -LiteralPath $embeddedVsixPath -Algorithm SHA256).Hash
        if ($embeddedVsixHash -ne $vsixHash) {
            throw "MSI contains stale VSIX: built=$vsixHash embedded=$embeddedVsixHash"
        }
    }
    finally {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Output "release payload verified: version=$sourceVersion packageSha256=$packageHash vsixSha256=$vsixHash"
