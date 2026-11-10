# Downloads the pinned nuget.exe that WiX's own source build needs (nuget restore / nuget pack).
# Fails closed: the file must match the pinned SHA-256 and carry a valid Microsoft signature.
param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$version = '7.9.0'
$sha256 = '992D70CAC5B06C38EFEC91806CABA64CDCC07E6D963A0959DBBBAF264D33B800'
$url = "https://dist.nuget.org/win-x86-commandline/v$version/nuget.exe"

function Test-NuGetExe([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $sha256) { return $false }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    return ($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -like 'CN=Microsoft Corporation,*')
}

if (Test-NuGetExe $Destination) {
    "nuget.exe $version already present: $Destination"
    exit 0
}

$directory = Split-Path -Parent $Destination
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$download = "$Destination.download"
if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Force }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
if (-not (Test-NuGetExe $download)) {
    Remove-Item -LiteralPath $download -Force
    throw "Downloaded nuget.exe failed the pinned SHA-256 or Microsoft signature check: $url"
}
Move-Item -LiteralPath $download -Destination $Destination -Force
"Downloaded nuget.exe $version to $Destination"
