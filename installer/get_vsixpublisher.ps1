<#
.SYNOPSIS
Downloads the pinned Microsoft.VSSDK.BuildTools package and returns the path of its VsixPublisher.exe.

.DESCRIPTION
Used by .github\workflows\publish-marketplace.yml, whose runner has no Visual Studio SDK. Fails closed:
the package must match the pinned SHA-512 and VsixPublisher.exe must carry a valid Microsoft signature
and support -oidc (trusted publishing).

The package ships newer Microsoft.Identity.Client and Microsoft.IdentityModel.Abstractions assemblies
than some of its own assemblies reference, and VsixPublisher.exe.config has no binding redirect for
them, so the publisher fails to load them (VsixPub0031). This script adds the redirects to the
versions the package ships.

The only pipeline output is the VsixPublisher.exe path.
#>
param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$version = '18.12.1606'
$sha512 = 'USqvVK+43nKAtT9xx5FCH00xUPudLreGa4GMc886MnUwj98avOvhWgdQPSxZrxaFK75Ubj5q8J69Wa3sYAG2DA=='
$id = 'microsoft.vssdk.buildtools'
$url = "https://api.nuget.org/v3-flatcontainer/$id/$version/$id.$version.nupkg"
$redirectAssemblies = @('Microsoft.Identity.Client', 'Microsoft.IdentityModel.Abstractions')

function Get-Sha512Base64([string]$Path)
{
    $algorithm = [Security.Cryptography.SHA512]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try
    {
        return [Convert]::ToBase64String($algorithm.ComputeHash($stream))
    }
    finally
    {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Test-VsixPublisherOidc([string]$Path)
{
    # VsixPublisher prints its options for an unknown argument and exits nonzero.
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

function Set-BindingRedirect([xml]$Config, [string]$Name, [string]$PublicKeyToken, [string]$NewVersion)
{
    $namespace = 'urn:schemas-microsoft-com:asm.v1'
    $manager = New-Object Xml.XmlNamespaceManager $Config.NameTable
    $manager.AddNamespace('asm', $namespace)

    $runtime = $Config.SelectSingleNode('/configuration/runtime')
    if (-not $runtime)
    {
        $runtime = $Config.CreateElement('runtime')
        [void]$Config.DocumentElement.AppendChild($runtime)
    }
    $binding = $runtime.SelectSingleNode('asm:assemblyBinding', $manager)
    if (-not $binding)
    {
        $binding = $Config.CreateElement('assemblyBinding', $namespace)
        [void]$runtime.AppendChild($binding)
    }
    $dependency = $binding.SelectSingleNode("asm:dependentAssembly[asm:assemblyIdentity/@name='$Name']", $manager)
    if (-not $dependency)
    {
        $dependency = $Config.CreateElement('dependentAssembly', $namespace)
        $identity = $Config.CreateElement('assemblyIdentity', $namespace)
        $identity.SetAttribute('name', $Name)
        $identity.SetAttribute('publicKeyToken', $PublicKeyToken)
        $identity.SetAttribute('culture', 'neutral')
        [void]$dependency.AppendChild($identity)
        [void]$binding.AppendChild($dependency)
    }
    $redirect = $dependency.SelectSingleNode('asm:bindingRedirect', $manager)
    if (-not $redirect)
    {
        $redirect = $Config.CreateElement('bindingRedirect', $namespace)
        [void]$dependency.AppendChild($redirect)
    }
    $redirect.SetAttribute('oldVersion', "0.0.0.0-$NewVersion")
    $redirect.SetAttribute('newVersion', $NewVersion)
}

if (Test-Path -LiteralPath $Destination)
{
    Remove-Item -LiteralPath $Destination -Recurse -Force
}
New-Item -ItemType Directory -Path $Destination | Out-Null

$nupkg = Join-Path $Destination "$id.$version.nupkg"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing
if ((Get-Sha512Base64 $nupkg) -ne $sha512)
{
    throw "Downloaded Microsoft.VSSDK.BuildTools $version failed the pinned SHA-512 check: $url"
}

$extracted = Join-Path $Destination 'package'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($nupkg, $extracted)

$exe = Join-Path $extracted 'tools\VSSDK\bin\VsixPublisher.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf))
{
    throw "VsixPublisher.exe not found in Microsoft.VSSDK.BuildTools $version"
}
$signature = Get-AuthenticodeSignature -LiteralPath $exe
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike 'CN=Microsoft Corporation,*')
{
    throw "VsixPublisher.exe does not carry a valid Microsoft signature: $exe"
}

$configPath = "$exe.config"
$config = New-Object Xml.XmlDocument
$config.PreserveWhitespace = $true
$config.Load($configPath)
foreach ($name in $redirectAssemblies)
{
    $dll = Join-Path (Split-Path -Parent $exe) "lib\$name.dll"
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf))
    {
        continue
    }
    $assemblyName = [Reflection.AssemblyName]::GetAssemblyName($dll)
    $token = -join ($assemblyName.GetPublicKeyToken() | ForEach-Object { $_.ToString('x2') })
    Set-BindingRedirect $config $name $token $assemblyName.Version.ToString()
    Write-Host "Binding redirect: $name -> $($assemblyName.Version)"
}
$config.Save($configPath)

if (-not (Test-VsixPublisherOidc $exe))
{
    throw "VsixPublisher.exe from Microsoft.VSSDK.BuildTools $version does not support -oidc"
}
Write-Host "VsixPublisher $((Get-Item -LiteralPath $exe).VersionInfo.FileVersion) from Microsoft.VSSDK.BuildTools $version"
$exe
