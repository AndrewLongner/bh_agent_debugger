param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'Temp\installer\analyzer'
if (Test-Path -LiteralPath $out) {
    $items = @((Get-Item -LiteralPath $out)) + @(Get-ChildItem -LiteralPath $out -Recurse -Force)
    if ($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw 'Refusing to clean a reparse point in analyzer staging' }
    Remove-Item -LiteralPath $out -Recurse -Force
}
# MSBuild reads environment variables as properties: an inherited OUTDIR (build_installer.bat sets one)
# or OUTPUTPATH would move the build output away from the runtimeconfig checked below.
foreach ($name in @('OutDir', 'OutputPath')) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
# Remove the previous runtimeconfig so the check below cannot read a stale copy.
$runtimeConfig = Join-Path $root "bh_profile_analyzer\bin\$Configuration\net10.0-windows\win-x64\bh_profile_analyzer.runtimeconfig.json"
if (Test-Path -LiteralPath $runtimeConfig) { Remove-Item -LiteralPath $runtimeConfig -Force }
# Single-file publish of the managed code and runtime (about 205 files down to about 7, which makes MSI
# upgrades much faster). Native libraries are NOT bundled (IncludeNativeLibrariesForSelfExtract=false),
# so TraceEvent's amd64\ helpers, msdia140 and bh_profile_dia stay at their normal relative paths.
# No trimming. EnableSingleFileAnalyzer=false avoids an ILLink package that the locked restore
# does not allow.
& dotnet publish (Join-Path $root 'bh_profile_analyzer\bh_profile_analyzer.csproj') -c $Configuration -r win-x64 --self-contained true -o $out -p:RuntimeFrameworkVersion=10.0.14 -p:RestoreLockedMode=true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=false -p:EnableSingleFileAnalyzer=false -p:PublishTrimmed=false --nologo
if ($LASTEXITCODE -ne 0) { throw 'Analyzer publish failed' }
$assets = Get-Content -Raw -LiteralPath (Join-Path $root 'Temp\bh_profile_analyzer_obj\project.assets.json') | ConvertFrom-Json
$runtimePackage = $null
foreach ($folder in $assets.packageFolders.PSObject.Properties.Name) {
    $candidate = Join-Path $folder 'microsoft.netcore.app.runtime.win-x64\10.0.14'
    if (Test-Path -LiteralPath (Join-Path $candidate 'LICENSE.TXT')) { $runtimePackage = $candidate; break }
}
if (!$runtimePackage) { throw 'Runtime package license source not found' }
# The analyzer is installed in bin\ beside the CLI. The dotnet- prefix keeps the .NET runtime's license
# files apart from this product's LICENSE.txt and THIRD_PARTY_NOTICES.txt in the application folder.
Copy-Item -LiteralPath (Join-Path $runtimePackage 'LICENSE.TXT') -Destination (Join-Path $out 'dotnet-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $runtimePackage 'THIRD-PARTY-NOTICES.TXT') -Destination (Join-Path $out 'dotnet-THIRD-PARTY-NOTICES.txt')
foreach ($name in @('bh_profile_analyzer.exe','bh_profile_dia.dll','amd64\msdia140.dll','amd64\KernelTraceControl.dll','dotnet-LICENSE.txt','dotnet-THIRD-PARTY-NOTICES.txt')) {
    $file = Get-Item -LiteralPath (Join-Path $out $name)
    if ($file.Length -le 0) { throw "Empty analyzer payload: $name" }
}
foreach ($name in @('bh_profile_analyzer.dll','coreclr.dll','hostfxr.dll','Microsoft.Diagnostics.Tracing.TraceEvent.dll')) {
    if (Test-Path -LiteralPath (Join-Path $out $name)) { throw "Analyzer was not published single-file: loose $name" }
}
if ((Get-Item -LiteralPath (Join-Path $out 'bh_profile_analyzer.exe')).Length -lt 30MB) { throw 'Analyzer bundle is unexpectedly small; runtime not bundled' }
$nativeBuilt = Join-Path $root 'Temp\bh_profile_native_x64\bh_profile_dia.dll'
if ((Get-FileHash -LiteralPath $nativeBuilt -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $out 'bh_profile_dia.dll') -Algorithm SHA256).Hash) {
    throw 'Published DIA helper does not match the native build'
}
# The runtimeconfig is embedded in the bundle; check the copy that was bundled.
if (-not (Test-Path -LiteralPath $runtimeConfig -PathType Leaf)) { throw "Analyzer publish did not write $runtimeConfig" }
$config = Get-Content -Raw -LiteralPath $runtimeConfig | ConvertFrom-Json
if (!($config.runtimeOptions.includedFrameworks | Where-Object { $_.name -eq 'Microsoft.NETCore.App' -and $_.version -eq '10.0.14' })) {
    throw 'Analyzer must include the pinned self-contained runtime'
}
& (Join-Path $out 'bh_profile_analyzer.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw 'Published analyzer self-tests failed' }
"Published self-contained single-file win-x64 analyzer with .NET 10.0.14 (no system runtime installation); files=$(@(Get-ChildItem -LiteralPath $out -Recurse -File).Count)"
